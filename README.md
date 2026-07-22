# nativekit

> Cross-platform native capabilities for Electron — the things Electron doesn't do well.

`nativekit` is a N-API native addon (C++) that gives Electron desktop apps
operating-system-level interaction that the Chromium/Electron layer can't
provide or does poorly:

- **Floating overlay panels** — always-on-top windows anchored to screen edges,
  stacked/cascaded, living across all Spaces / virtual desktops.
- **Native window awareness** — enumerate, query, and hit-test system windows to
  build context-aware UI.
- **Secure isolated IPC** — run sensitive work in a sandboxed child process and
  stream its output back, with peer identity verification.
- **App icons** — extract the real OS icon for any installed application.
- **File drag-out** — start a native file drag from an Electron window into
  Finder / Explorer / any app.

Supported platforms: **macOS** (arm64, x64) and **Windows** (x64).

---

## Why

Electron is a webview container. That's great for shipping UI, but several
common desktop interactions fall through the cracks:

| Problem | Electron alone | nativekit |
|---|---|---|
| Floating panel above *all* windows & Spaces | `BrowserWindow` is app-scoped, hides on Space switch | True OS-level overlay (NSWindow / layered HWND) |
| Knowing what window/app the user is in | Not exposed | Full system window enumeration |
| Sensitive automation in isolation | Runs in the renderer/main process | Sandboxed child process + verified IPC |
| Real app icons | Bundled PNGs / guesswork | Native icon extraction |
| Drag a file out to Finder/Explorer | HTML5 DnD breaks outside the app | Native OS drag session |

---

## Install

```bash
npm install nativekit
# or
pnpm add nativekit
```

Prebuilt binaries are shipped per platform/arch — no native toolchain required
for consumers.

```js
// Electron main process
import { overlay, windows, apps, drag, secureChannel } from 'nativekit'
```

> `nativekit` is **main-process only**. Calling it from the renderer will throw.

---

## Quick start

```js
const { app, BrowserWindow } = require('electron')
const { overlay, windows } = require('nativekit')

app.whenReady().then(async () => {
  const win = new BrowserWindow({ width: 800, height: 600 })

  // --- Overlay: show a floating panel ---
  overlay.start({ tooltip: { hide: 'Hide', relocate: 'Move' } })
  overlay.attachHost({
    id: 'main',
    title: 'Assistant',
    bounds: { x: 0, y: 0, width: 320, height: 200 },
    windowHandle: win.getNativeWindowHandle(),
    anchor: { edge: 'trailing', offset: 16 },
  })

  // push an image frame into the overlay
  overlay.pushImage({
    presentationId: 'p1',
    sessionId: 's1',
    imageData: 'data:image/png;base64,iVBOR...',
  })

  // --- Window awareness ---
  const front = await windows.frontmost()
  console.log(front)
  // { bundleId: 'com.google.Chrome', name: 'Google Chrome',
  //   title: 'GitHub', icon: 'data:image/png;base64,...' }

  overlay.on('activate', () => win.show())  // double-click overlay → bring app forward
})

app.on('will-quit', () => overlay.stop())
```

---

## Modules

| Module | Description | Status |
|---|---|---|
| `overlay` | Floating, always-on-top panel system | P0 — core |
| `windows` | System window enumeration & query | P0 — core |
| `secureChannel` | Sandboxed child process + verified IPC | P1 |
| `apps` | Native application icon extraction | P2 |
| `drag` | Native file drag-out from Electron | P2 |
| `animation` | Physics-based window animations | Deferred* |

> *The animation module is deferred unless a comprehensive, reusable physics
> toolkit (spring damping, DisplayLink-driven, multi-interactive-mode) can be
> delivered. A half-baked animation API is worse than none.

See [docs/architecture.md](docs/architecture.md) for the design and
[docs/api-reference.md](docs/api-reference.md) for the full API.

---

## Platform matrix

| Capability | macOS | Windows |
|---|---|---|
| Floating overlay | NSWindow (floating level) | Layered HWND (`WS_EX_LAYERED`) |
| Window enumeration | CGWindowList | `EnumWindows` |
| Frontmost window | NSWorkspace | `GetForegroundWindow` |
| App icon | NSWorkspace.icon | `SHGetFileInfo` / icon resources |
| File drag | NSDraggingSource | `IDropSource` / `IDataObject` (OLE) |
| Secure IPC | NSXPCConnection | Named pipe + restricted child |
| Isolated process | `posix_spawn` | `CreateProcess` (restricted token) |

---

## License

MIT
