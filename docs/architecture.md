# Architecture

## 1. Scope and constraints

`nativekit` is a main-process-only Electron library for macOS, Windows, and
Linux X11/XWayland. It combines a small TypeScript API with a Node-API v8 addon.
The public API is the same on every implementation; platform differences that
cannot be removed are called out explicitly.

The project follows five constraints:

1. One Node-API binding with symmetric native platform tails.
2. C++17 for shared, Windows, and Linux code; Objective-C++ for AppKit code.
3. No JavaScript call from a native worker thread. Every callback crosses one
   `Napi::ThreadSafeFunction`.
4. Native handles are owned by module managers and released by explicit stop
   operations or the environment cleanup hook.
5. Published packages contain Node-API prebuilds, while source builds remain a
   supported fallback.

Animation is deliberately outside the current API. Overlay moves are immediate
until a complete cross-platform physics system exists.

## 2. Package and runtime layers

```text
Electron main process
        │
        ▼
dist/index.js / dist/index.cjs        Vite library output + declarations
        │
        ▼
node-gyp-build                        selects local build or N-API prebuild
        │
        ▼
nativekit.node                        node-addon-api, N-API v8
        │
        ├── shared managers           validation, state, event dispatch
        ├── macOS tail                AppKit / CoreGraphics
        ├── Windows tail              Win32 / WIC / Shell
        └── Linux tail                GIO / GdkPixbuf / XCB
```

The TypeScript wrapper performs main-process checks, input validation, Promise
normalization, and Windows physical-pixel/DIP conversion. Native methods remain
synchronous internally; methods that may later move off-thread already expose
Promises at the JavaScript boundary.

Vite only bundles the JavaScript library. CMake remains the primary native build
system because Vite does not compile Node-API C++, Objective-C++, or Win32 code.
`binding.gyp` is kept as the source-build and `prebuildify` fallback.

## 3. Repository layout

```text
src/
  binding.cpp                 Node-API registration and cleanup hook
  common/
    event_callback.*          ordered ThreadSafeFunction adapter
    napi_helpers.h            JS conversion and error helpers
    types.h                   shared Point, Rect, and window records
    mac/image_utils.*         AppKit image encode/decode
    win/image_utils.*         WIC and Shell icon encode/decode
    linux/image_utils.*       GdkPixbuf encode/decode and GIO icon lookup
  overlay/
    overlay_manager.*         host/session/presentation state
    mac/overlay_window.mm     NSPanel renderer and controls
    win/overlay_window.cpp    layered HWND renderer and message pump
    linux/overlay_window.cpp  XCB renderer, controls, and event thread
  windows/
    window_query.*            Node-API module boundary
    mac/window_query.mm       CGWindowList and NSWorkspace
    win/window_query.cpp      EnumWindows, DWM, foreground window
    linux/window_query.cpp    XCB, EWMH stacking, active window
  apps/                       exact-size application icon extraction
js/index.ts                   public TypeScript API
examples/electron/            context-isolated capability demo
tests/                        package and native integration tests
```

The root and `examples/electron` form a pnpm workspace. A larger monorepo tool is
not needed: the demo consumes the library through `workspace:*`, and the root
build remains the only publishable package.

## 4. Module design

### 4.1 `overlay`

The shared manager owns hosts, presentations, session suppression, active
session ordering, global visibility, and maximum image size. It sends immutable
snapshots to the platform renderer.

`pushImage()` is transactional. If native image decoding or rendering fails, the
previous presentation state is restored and re-rendered, so a rejected call does
not make `hasAny()` lie or replace a valid frame with an invalid one.

macOS uses non-activating `NSPanel` instances with transparent content, floating
window level, and these collection behaviors:

```text
canJoinAllSpaces | stationary | ignoresCycle | fullScreenAuxiliary
```

Windows uses one STA message-pump thread and host-owned, per-presentation
layered windows:

```text
WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST
CreateWindowEx(..., host HWND, ...)
UpdateLayeredWindow(PBGRA) + HWND_TOPMOST
```

All tails preserve image aspect ratio, render an optional application-icon
badge, scale controls, skip suppressed items in stack layout, and hide overflow
items rather than overlap them. A user drag moves the native window directly
without renderer IPC. The platform tail then owns the presentation's manual
origin, clamps it to the current work area, and keeps its anchor-stack slot
stable until relocate clears that state. macOS and Windows provide native
hide/relocate hover tooltips; the first X11 renderer accepts the same strings
but does not draw tooltip windows.

Linux uses one dedicated XCB event thread and one undecorated, non-focusing
utility window per presentation. XCB RandR selects the host monitor; EWMH
properties request transient-parent, keep-above, taskbar/pager exclusion, and
all-workspace behavior. Image decode and scaling use display-independent
GdkPixbuf APIs. This avoids initializing GTK inside Electron and keeps X11
round trips off the Electron UI thread. EWMH window managers may apply
workspace policy differently.

Native Wayland is deliberately excluded from `overlay`. Wayland clients cannot
choose global top-level positions, and Electron's native handle contract for
this integration is an X11 `Window`. Electron must run through X11/XWayland
when the overlay is used.

### 4.2 `windows`

Native enumeration returns front-to-back snapshots. Public `level` is the
z-order position: lower values are nearer the front. `relativeTo` excludes the
reference window and every window above it. Hidden windows remain in `list()`;
`atPoint()` only considers on-screen windows.

CoreGraphics and Win32 use different coordinate spaces. The Electron-facing API
normalizes screen points and rectangles to device-independent pixels (DIP) on
Windows through Electron's `screen` module. macOS points already match Electron
DIP semantics. Linux reads the EWMH client stacking list and X11 root-window
coordinates through XCB, which matches Electron's X11/XWayland window model.
Native Wayland does not provide the required global window model.

### 4.3 `apps`

`apps.icon()` resolves the operating-system icon and rasterizes it to exactly
16×16 or 32×32 PNG pixels. macOS uses `NSWorkspace`; Windows uses Shell icon
lookup plus WIC scaling and PNG encoding. Linux matches `.desktop` entries and
executables through GIO, searches installed freedesktop icon themes, and uses
GdkPixbuf for exact-size PNG output. A generic file icon is the fallback for an
existing path without an application entry.

## 5. Threading and cleanup

```text
Electron main/UI thread
  ├── shared managers and Node-API calls
  ├── AppKit overlay
  └── ThreadSafeFunction delivery

Native worker threads
  ├── Windows overlay message pump (STA)
  └── Linux XCB event loop
```

The Node environment cleanup hook calls independent `noexcept` cleanup
functions. Each module absorbs shutdown-only failures so one native subsystem
cannot prevent the others from releasing resources or unwind an exception
through the C cleanup callback.

## 6. Build and distribution

Local development:

```bash
pnpm install
pnpm build:js       # Vite ESM/CJS + TypeScript declarations
pnpm build:native   # CMake + cmake-js
pnpm check          # package build, typecheck, integration tests
pnpm demo:smoke     # Electron end-to-end capability demo
```

Release CI builds and tests these published targets independently:

```text
prebuilds/darwin-arm64/nativekit.napi.node
prebuilds/darwin-x64/nativekit.napi.node
prebuilds/win32-x64/nativekit.napi.node
```

Build-only CI additionally compiles `linux-x64` and `linux-arm64` on native
Ubuntu 22.04 runners. It starts Xvfb, Openbox, and an X11 fixture client, then
runs both the native integration suite and the Electron demo smoke flow. These
artifacts are uploaded for inspection but are not consumed by `release.yml`.

The publish job assembles the three released artifacts, builds the Vite library,
verifies the npm tarball, publishes that tarball, and attaches the same archive
to the GitHub release. `node-gyp-build` prefers the matching Node-API prebuild
and falls back to `binding.gyp` when a consumer explicitly builds from source.

The renderer demo never imports the addon. A narrow context-isolated preload
exposes domain operations over validated `ipcMain.handle` channels; mutable
native state stays in the Electron main process.
