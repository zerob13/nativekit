# nativekit

> Cross-platform native capabilities for Electron — the things Electron doesn't do well.

`nativekit` is a N-API native addon (C++) that gives Electron desktop apps
operating-system-level interaction that the Chromium/Electron layer can't
provide or does poorly:

- **Floating overlay panels** — always-on-top windows anchored to screen edges,
  stacked across all macOS Spaces and the current Windows virtual desktop.
- **Native window awareness** — enumerate, query, and hit-test system windows to
  build context-aware UI.
- **Verified worker channel** — run a dedicated helper process and stream
  length-prefixed output over a private inherited pipe.
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
| Floating panel outside the app window | `BrowserWindow` remains app-owned | OS-level overlay (NSPanel / layered HWND) |
| Knowing what window/app the user is in | Not exposed | Full system window enumeration |
| Dedicated worker isolation | Runs in the renderer/main process | Path-verified helper + private framed pipe |
| Real app icons | Bundled PNGs / guesswork | Native icon extraction |
| Drag a file out to Finder/Explorer | HTML5 DnD breaks outside the app | Native OS drag session |

---

## Install

```bash
npm install @zerob13/nativekit
# or
pnpm add @zerob13/nativekit
```

Prebuilt binaries are shipped per platform/arch — no native toolchain required
for consumers.

```js
// Electron main process
import { overlay, windows, apps, drag, secureChannel } from '@zerob13/nativekit'
```

> `nativekit` is **main-process only**. Calling it from the renderer will throw.

---

## Quick start

```js
const { app, BrowserWindow } = require('electron')
const { overlay, windows } = require('@zerob13/nativekit')

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

## Electron demo

The workspace demo exercises every module through a context-isolated preload.
Its smoke mode also starts and ends a native drag session with synthetic pointer
input.

```bash
pnpm install
pnpm demo
pnpm demo:smoke
```

The interactive demo can choose installed apps and real files for manual icon
and Finder / Explorer drag-out checks.

---

## Modules

| Module | Description | Status |
|---|---|---|
| `overlay` | Floating, always-on-top panel system | P0 — core |
| `windows` | System window enumeration & query | P0 — core |
| `secureChannel` | Verified helper process + framed output pipe | P1 |
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
| Floating overlay | NSPanel on all Spaces | Layered HWND on the current virtual desktop |
| Window enumeration | CGWindowList | `EnumWindows` |
| Frontmost window | NSWorkspace | `GetForegroundWindow` |
| App icon | NSWorkspace.icon | `SHGetFileInfo` / icon resources |
| File drag | NSDraggingSource | `IDropSource` / `IDataObject` (OLE) |
| Worker channel | Inherited pipe + `posix_spawn` | Inherited pipe + restricted token |
| Peer verification | `proc_pidpath` | `QueryFullProcessImageName` |

---

## License

MIT
