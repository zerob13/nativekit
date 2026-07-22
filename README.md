# nativekit

Cross-platform native desktop primitives for the Electron main process.

[简体中文](README.zh-CN.md) · [Electron integration guide（中文）](docs/integration-guide.zh-CN.md)

`nativekit` packages a TypeScript API and a Node-API v8 addon for:

- session-aware floating image overlays;
- system-window enumeration and hit testing;
- exact-size operating-system application icons.

Supported release targets are macOS arm64/x64 and Windows x64.

## Why this library exists

Electron already provides topmost BrowserWindows and cross-workspace options on
macOS. `nativekit` does not pretend otherwise. It provides a narrower
coordinated layer where applications need more state than those primitives
expose.

| Need | `nativekit` behavior |
|---|---|
| Multiple lightweight panels | Draggable native image panels with stack, host, session, suppression, and active-session lifecycle |
| Context-aware desktop UI | Cross-process system-window list, lookup, foreground app, and point hit testing |
| Installed application artwork | Native icon lookup normalized to exact 16×16 or 32×32 PNG |

## Install

After an npm release is published:

```bash
npm install @zerob13/nativekit
# or
pnpm add @zerob13/nativekit
```

The release workflow places these Node-API prebuilds in the npm tarball:

```text
darwin-arm64
darwin-x64
win32-x64
```

`node-gyp-build` selects the matching artifact. A consumer can explicitly build
from source through the included `binding.gyp`, but normal installation should
use a prebuild.

The package is Electron main-process only. Expose specific operations to a
renderer through a context-isolated preload; do not expose the module wholesale.

## Quick start

```ts
import { app, BrowserWindow } from 'electron'
import { overlay, windows } from '@zerob13/nativekit'

await app.whenReady()

const win = new BrowserWindow({ width: 800, height: 600 })

overlay.start({ tooltip: { hide: 'Hide', relocate: 'Move' } })
overlay.attachHost({
  id: 'main',
  title: 'Assistant',
  bounds: win.getContentBounds(),
  windowHandle: win.getNativeWindowHandle(),
  anchor: { edge: 'trailing', offset: 16 },
})

overlay.pushImage({
  presentationId: 'capture-1',
  sessionId: 'task-42',
  imageData: 'data:image/png;base64,iVBORw0KGgo...',
})

overlay.on('activate', () => win.show())

console.log(await windows.frontmost())

app.on('will-quit', () => overlay.stop())
```

See [API reference](docs/api-reference.md) for the complete contract and
[architecture](docs/architecture.md) for native lifecycle and threading.

## Workspace development

Requirements:

- Node.js 20.19 or newer for workspace builds (published runtime: Node 18+);
- pnpm 10;
- CMake 3.22 or newer;
- Xcode command-line tools on macOS; or
- Visual Studio C++ Build Tools on Windows.

```bash
pnpm install
pnpm build:js
pnpm build:native
pnpm check
```

The JavaScript package uses Vite library mode for ESM and CommonJS and `tsc`
for declarations. Native compilation stays in CMake/cmake-js. The pnpm
workspace contains one private Electron example; additional monorepo tooling is
not required.

## Electron demo

The demo calls every public module through a context-isolated preload:

```bash
pnpm demo
pnpm demo:smoke
```

Interactive mode supports direct native-panel movement, multi-image selection
with five-second rotation, and real application selection. Smoke mode validates
window enumeration, icon extraction, and overlay rendering. A successful macOS
run prints one `NATIVEKIT_DEMO_SMOKE` JSON record and exits with status 0.

```text
┌ nativekit demo ───────────────────────────────────────┐
│ Status                              platform / arch   │
├───────────────────┬───────────────────────────────────┤
│ Window awareness  │ Overlay                           │
│ frontmost / list  │ show / hide / drag / image cycle │
├───────────────────┴───────────────────────────────────┤
│ App icon: native icon preview                         │
├───────────────────────────────────────────────────────┤
│ Ordered event log                                     │
└───────────────────────────────────────────────────────┘
```

## Platform behavior

| Capability | macOS | Windows |
|---|---|---|
| Overlay | draggable non-activating `NSPanel`, all Spaces | draggable layered topmost `HWND`, current virtual desktop |
| Window query | CoreGraphics + NSWorkspace | EnumWindows + DWM |
| App icon | NSWorkspace | Shell + WIC |
| Public coordinates | Electron DIP | native pixels normalized to Electron DIP |

macOS arm64 has local native, demo, and visual verification. Windows code is
implemented and statically reviewed; the CI/release matrix makes an MSVC build
and native integration pass mandatory because this repository is developed on
macOS.

## Publish to npm

The source version and tag must match. For `0.4.0`:

1. Configure npm trusted publishing for this GitHub repository and
   `.github/workflows/release.yml`.
2. Confirm the package version in `package.json` is `0.4.0`.
3. Commit the release state and create tag `v0.4.0`.
4. Push the tag.

The release workflow then:

1. builds and tests macOS arm64, macOS x64, and Windows x64 independently;
2. uploads platform-named `nativekit.napi.node` artifacts;
3. assembles and inspects one npm tarball;
4. publishes the exact tarball; and
5. attaches it to the matching GitHub release.

Pushing a matching `v*` tag starts the release; ordinary branch pushes,
including `main`, do not. Manual dispatch can release an existing `vX.Y.Z` tag.
No package version is rewritten inside CI.

## License

MIT
