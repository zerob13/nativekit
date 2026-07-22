# Architecture

## 1. Design principles

1. **One binding layer, two platform tails.** A single C++ N-API entry point
   compiles platform-specific source files per OS. No scripting-language
   boundary beyond JS↔C++.
2. **Feature modules, not a god-object.** Each capability (`overlay`,
   `windows`, `apps`, `drag`, `secureChannel`) is an independent sub-module with
   its own cross-platform interface and platform implementations.
3. **Idiomatic JS API.** Namespaced exports, camelCase, promises where async,
   EventEmitters for callbacks. No leak of platform/API jargon into JS.
4. **Main-process only.** The addon touches OS windowing — it must run in the
   Electron main process. The JS wrapper enforces this and throws otherwise.
5. **Prebuilt binaries.** Consumers never compile. `prebuildify` ships per-arch
   `.node` artifacts; `node-gyp-build` resolves them at runtime.

---

## 2. Technology choices

### Why C++ (node-addon-api), not Rust (napi-rs)

The headline feature — the **overlay system** — is fundamentally GUI-native:
on macOS it requires NSWindow subclasses, AppKit delegates, and DisplayLink; on
Windows it requires layered HWNDs and DWM. C++ reaches both:

- **macOS**: `.mm` (Objective-C++) files call AppKit / CoreGraphics directly.
- **Windows**: `.cpp` files call Win32 / Shell / OLE directly via `windows.h`.

Rust would require hand-written FFI bridges for *every* ObjC method call and
every Win32 struct — workable for a few enum calls, painful for GUI object
lifecycles, retain cycles, and delegate callbacks. For a GUI-heavy cross-native
addon, C++ remains the pragmatic, honest choice.

### Build system

- **CMake** + **cmake-js** (primary) — cross-platform, integrates with
  Electron's headers, supports Objective-C++ and Swift sources on macOS.
- `binding.gyp` / `node-gyp` as a fallback for environments that prefer it.
- **prebuildify** for distribution; consumers resolve via `node-gyp-build`.

---

## 3. Layered architecture

```
┌───────────────────────────────────────────────────────┐
│  Electron Main Process (JS / TypeScript)              │
│                                                       │
│  ┌─────────┐ ┌────────┐ ┌──────────────┐ ┌────────┐  │
│  │ overlay │ │windows │ │secureChannel │ │ apps   │  │
│  └────┬────┘ └───┬────┘ └──────┬───────┘ └───┬────┘  │
│       └───────────┴─────────────┴─────────────┘       │
│                     js wrapper (index.ts)             │
├───────────────────────────────────────────────────────┤
│  N-API Binding Layer (C++ — node-addon-api)            │
│  binding.cpp  +  per-module ObjectWrap / functions     │
├──────────────────────────┬────────────────────────────┤
│        macOS tail        │        Windows tail         │
│  ┌────────────────────┐  │  ┌────────────────────────┐ │
│  │ AppKit  (ObjC++)   │  │  │ Win32 (C++)            │ │
│  │  - NSWindow lvl    │  │  │  - layered HWND        │ │
│  │  - CGWindowList    │  │  │  - EnumWindows         │ │
│  │  - NSWorkspace     │  │  │  - GetForegroundWindow │ │
│  │  - NSDraggingSrc   │  │  │  - SHGetFileInfo       │ │
│  │  - NSXPCConnection │  │  │  - IDropSource/IDataObj│ │
│  └────────────────────┘  │  │  - named pipe + restr. │ │
└──────────────────────────┴──┴────────────────────────┘┘
```

---

## 4. Repository layout

```
nativekit/
├── package.json
├── README.md
├── docs/
│   ├── architecture.md          ← this file
│   ├── api-reference.md
│   └── roadmap.md
├── src/
│   ├── binding.cpp              # N-API module registration entry
│   ├── common/
│   │   ├── types.h              # shared C++ structs (Rect, Window, etc.)
│   │   ├── json.h               # C++ ↔ JSON helpers for N-API
│   │   └── async.h              # ThreadSafeFunction wrappers
│   ├── overlay/                 # floating panel system
│   │   ├── overlay_manager.h    # platform-neutral interface
│   │   ├── mac/
│   │   │   ├── overlay_window.mm    # NSWindow subclass
│   │   │   ├── overlay_stack.mm     # stack / cascade layout
│   │   │   └── overlay_controls.mm  # hide / relocate controls
│   │   └── win/
│   │       ├── overlay_window.cpp   # layered HWND
│   │       └── overlay_stack.cpp
│   ├── windows/                 # system window query
│   │   ├── window_query.h
│   │   ├── mac/window_query.mm      # CGWindowListCopyWindowInfo
│   │   └── win/window_query.cpp     # EnumWindows + GetWindowRect
│   ├── apps/                    # app icon extraction
│   │   ├── icon.h
│   │   ├── mac/icon.mm              # NSWorkspace.shared.icon
│   │   └── win/icon.cpp             # SHGetFileInfo / ExtractIconEx
│   ├── drag/                    # native file drag-out
│   │   ├── drag_source.h
│   │   ├── mac/drag_source.mm       # NSDraggingSource
│   │   └── win/drag_source.cpp      # OLE IDropSource + IDataObject
│   └── ipc/                     # secure isolated channel
│       ├── secure_channel.h
│       ├── mac/xpc_channel.mm       # NSXPCConnection
│       └── win/pipe_channel.cpp     # named pipe + restricted child proc
├── js/
│   └── index.ts                 # TS wrapper: platform gating, validation
├── CMakeLists.txt               # primary build
├── binding.gyp                  # fallback build
└── prebuilds/
    ├── darwin-arm64/nativekit.node
    ├── darwin-x64/nativekit.node
    └── win32-x64/nativekit.node
```

---

## 5. Overlay system design (headline feature)

The overlay system shows always-on-top panels that persist across Spaces /
virtual desktops and stack/cascade along screen edges. It is the largest
sub-module.

### 5.1 Layout

```
┌──────────────────────────────────────────────────────┐
│  Desktop (all Spaces / virtual desktops)             │
│                                                      │
│                                       ┌──────────┐   │
│                                       │ Overlay 1│   │
│                          anchor →→    │ (top-    │   │
│                                       │  right)  │   │
│                                       └──────────┘   │
│   ┌──────────┐                                      │
│   │ Overlay 2│  ← cascaded under Overlay 1          │
│   └──────────┘                                      │
│                                                      │
│   ┌─────────────────────────────────┐               │
│   │   Electron Main Window          │               │
│   └─────────────────────────────────┘               │
└──────────────────────────────────────────────────────┘
```

### 5.2 Concepts

- **Host** — an Electron `BrowserWindow` registered as an overlay origin.
  Provides the native window handle, content bounds, title, and anchor.
- **Presentation** — a single visible panel, identified by `presentationId`.
  Lifecycle: `attached → visible → completed → removed`.
- **Session** — a logical grouping (e.g. one assistant conversation). Overlays
  can be shown/suppressed per session; completing a session clears all its
  presentations.
- **Stack** — multiple presentations arranged in a stack (cascade or expand),
  anchored to a screen edge.
- **Anchor** — screen-edge attachment point (`leading`/`trailing`/`top`/
  `bottom`) with an offset.

### 5.3 Window configuration

**macOS**
```
level:            NSFloatingWindowLevel      # above normal windows
collectionBehavior: canJoinAllSpaces         # visible on every Space
                  | stationary               # doesn't participate in Exposé
                  | ignoresCycle             # not in Cmd-` cycle
hasShadow:        false                      # custom shadow if needed
backgroundColor:  NSColor.clearColor         # transparent
ignoresMouseEvents: false                    # interactive
```

**Windows**
```
extended style: WS_EX_LAYERED                # per-pixel alpha
              | WS_EX_NOACTIVATE             # never steals focus
              | WS_EX_TOPMOST                # above normal windows
              | WS_EX_TOOLWINDOW             # no taskbar entry
                + DwmExtendFrameIntoClientArea for translucent edges
```

### 5.4 Frame injection

Frames (screenshots / images) are pushed as data URLs directly through N-API —
no separate producer process needed for the self-contained path:

```
JS  ──pushImage(dataURL)──▶  N-API  ──▶  OverlayWindow render
```

For the **isolated worker** path (see §6), frames stream from the sandboxed
child process over the secure channel and are rendered into the same overlay.

---

## 6. Secure isolated channel design

Spawns a child process for sensitive work (automation, credential handling,
untrusted code execution) and streams results back over a verified channel.

```
┌──────────────┐                         ┌──────────────────┐
│ Electron main│   verified IPC channel   │  sandboxed child │
│  (host)      │◀────────────────────────▶│  process (worker)│
│              │   mac: NSXPCConnection   │                  │
│              │   win: named pipe         │  restricted token│
└──────┬───────┘                         └──────────────────┘
       │
       │  spawn(executablePath) → pid
       │  verify(pid, executablePath) → bool   # audit-token / path check
       │  wasTerminatedByPrivacy() → bool      # privacy-permission death?
       ▼
   overlay.pushImage(...)  # stream worker frames into an overlay
```

### Security model
- **macOS**: `NSXPCConnection` with audit-token verification — only the
  expected child PID with the expected executable path may connect. Entitlements
  scope the child's capabilities.
- **Windows**: `CreateProcess` with a restricted token; named pipe with
  `GetNamedPipeClientProcessId` + full-path verification before accepting data.

---

## 7. Cross-platform capability mapping

```
┌──────────────────────┬────────────────────────┬─────────────────────────────┐
│  Capability          │  macOS                 │  Windows                   │
├──────────────────────┼────────────────────────┼─────────────────────────────┤
│  Floating overlay    │  NSWindow (float level)│  Layered HWND (TOPMOST)    │
│  Window enumeration   │  CGWindowListCopyInfo  │  EnumWindows + GetWindowRect│
│  Frontmost window     │  NSWorkspace.frontmost │  GetForegroundWindow        │
│  App icon             │  NSWorkspace.icon      │  SHGetFileInfo / ExtractIcon│
│  File drag-out        │  NSDraggingSource      │  IDropSource + IDataObject  │
│  Secure IPC           │  NSXPCConnection       │  Named pipe (verified PID) │
│  Isolated process     │  posix_spawn           │  CreateProcess (restr. token)│
└──────────────────────┴────────────────────────┴─────────────────────────────┘
```

---

## 8. Memory & threading

- **N-API ObjectWrap** owns native object lifetimes (overlay window, drag
  session, channel). Destructor releases native handles.
- **ThreadSafeFunction** carries callbacks from native threads (DisplayLink,
  worker process events, window-change notifications) back to the JS event loop
  safely. Never call JS from a non-main thread directly.
- On macOS, overlay window callbacks fire on the main thread already (UI
  thread), so TSF is used only for worker-process notifications.
- On Windows, the overlay message pump runs on a dedicated thread; all JS
  callbacks cross via ThreadSafeFunction.

---

## 9. Distribution

- Each release builds 3 targets: `darwin-arm64`, `darwin-x64`, `win32-x64`.
- `prebuildify` embeds prebuilt `.node` under `prebuilds/<platform>-<arch>/`.
- Runtime resolution via `node-gyp-build`, with `resourcesPath` /
  `app.getAppPath()` fallback search for Electron asar-unpacked binaries.
- TypeScript types ship in the package (`js/index.d.ts`).
