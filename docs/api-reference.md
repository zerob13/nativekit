# API reference

`@zerob13/nativekit` must be imported in the Electron main process. Renderer
code should call a narrow, context-isolated preload API instead of importing the
package directly.

```ts
import { apps, overlay, windows } from '@zerob13/nativekit'
```

Promise-returning methods currently wrap synchronous native results or native
session completion. This keeps the JavaScript contract stable if more work moves
off the main thread later.

## Shared coordinates

Every public point and rectangle uses Electron device-independent pixels (DIP)
with a top-left screen origin. macOS points already use this model. On Windows,
the wrapper converts between Electron DIP and native physical pixels. Linux
uses X11 root-window coordinates and therefore requires Electron to run through
X11/XWayland.

```ts
interface Point {
  x: number
  y: number
}

interface Rect extends Point {
  width: number
  height: number
}
```

## `overlay`

Floating image panels with host, presentation, and session lifecycle. macOS
panels join all Spaces. Windows panels are owned by their host window and remain
topmost on their current virtual desktop. Linux uses non-focusing XCB utility
windows on a dedicated X11 event thread and requests keep-above, taskbar/pager
exclusion, and all-workspace placement from the window manager.

Linux overlays do not support native Wayland. Electron must expose an X11
`Window`, normally by starting with `--ozone-platform=x11`; otherwise
`overlay.start()` throws an explicit error. Workspace and keep-above policy can
vary between EWMH window managers.

The user can drag a panel by its visible surface outside the controls.
Manual placement survives host and image updates, remains inside the selected
display's work area, and retains its original anchor-stack slot so other panels
do not jump during a drag.
Clicking the relocate control clears manual placement for that panel and moves
it to the host's next anchor edge. Removing the presentation or stopping the
overlay discards the manual position; it is not persisted across launches.

### Types

```ts
interface AnchorConfig {
  edge: 'leading' | 'trailing' | 'top' | 'bottom'
  offset: number
}

interface HostConfig {
  id: string
  title: string
  bounds: Rect
  windowHandle: Buffer
  anchor: AnchorConfig
}

interface ImageFrame {
  hostId?: string
  presentationId: string
  sessionId: string
  imageData: string
  appIconPath?: string | null
}

interface OverlayOptions {
  tooltip?: {
    hide?: string
    relocate?: string
  }
}
```

Pass `BrowserWindow.getContentBounds()` as `bounds` and
`BrowserWindow.getNativeWindowHandle()` as `windowHandle`. Width and height
constrain panel sizing; the native handle selects the correct display, owns the
panel on Windows, and supplies the X11 transient-parent hint on Linux. Refresh
the host after the BrowserWindow moves, resizes, or changes display.

`imageData` must be a PNG or JPEG base64 data URL no longer than 32 MiB. Decoded
images are limited to 8192 pixels per dimension and 64 MiB of RGBA pixels.
`appIconPath` is a `.app` path on macOS, a Shell-readable path on Windows, or an
absolute `.desktop`, executable, AppImage, or file path on Linux.

### Methods

#### `overlay.start(options?: OverlayOptions): boolean`

Start the platform renderer or update its tooltip options. Repeated calls are
safe. The first Linux X11 renderer draws the controls but does not display
hover tooltips; the strings remain accepted for API symmetry.

#### `overlay.stop(): boolean`

Destroy every host and panel. Repeated calls are safe.

#### `overlay.attachHost(config: HostConfig): boolean`

Add or update a BrowserWindow host. `id` is unique within the process.

```ts
overlay.attachHost({
  id: 'main',
  title: 'Assistant',
  bounds: win.getContentBounds(),
  windowHandle: win.getNativeWindowHandle(),
  anchor: { edge: 'trailing', offset: 16 },
})
```

#### `overlay.detachHost(hostId: string): boolean`

Remove a host and all of its presentations. Returns `false` when the host does
not exist.

#### `overlay.setVisible(visible: boolean): boolean`

Show or hide all presentations without deleting them.

#### `overlay.setMaxSize(size: number): boolean`

Set the maximum rendered panel edge in DIP. Emits `maxSizeChanged` after the new
configured cap has been applied.

#### `overlay.pushImage(frame: ImageFrame): boolean`

Create or update a presentation. `hostId` may be omitted only when exactly one
host exists. An existing `presentationId` cannot move to another host or
session. Invalid image data rejects the call without replacing the previous
valid presentation.

```ts
overlay.pushImage({
  hostId: 'main',
  presentationId: 'snapshot-1',
  sessionId: 'task-42',
  imageData: 'data:image/png;base64,iVBORw0KGgo...',
  appIconPath: '/Applications/Safari.app',
})
```

#### `overlay.removeImage(presentationId: string): boolean`

Remove one presentation. Returns `false` when it does not exist.

#### `overlay.completeSession(sessionId: string): boolean`

Remove every presentation in a session. Returns whether anything was removed.

#### `overlay.invalidateSession(sessionId: string, presentationId: string): boolean`

Remove one presentation only if it belongs to the supplied session.

#### `overlay.suppressSessions(sessionIds: string[]): boolean`

Replace the set of suppressed sessions. Suppressed items remain in state but do
not occupy stack space.

#### `overlay.setActiveSession(sessionId: string): boolean`

Place a session first in presentation ordering.

#### `overlay.hasActive(): boolean`

Whether at least one unsuppressed presentation is eligible to display while the
overlay is globally visible.

#### `overlay.hasAny(): boolean`

Whether any presentation exists, including suppressed or globally hidden ones.

### Events

| Event | Listener | Meaning |
|---|---|---|
| `maxSizeChanged` | `(size: number) => void` | `setMaxSize()` applied a new configured cap. |
| `activate` | `() => void` | The user double-clicked a panel outside its controls. |
| `visibilityRequest` | `(visible: boolean) => void` | A panel control requested global visibility change; currently hide emits `false`. |

```ts
overlay.on('activate', () => win.show())
overlay.on('visibilityRequest', (visible) => {
  if (!visible) overlay.setVisible(false)
})
```

Overlay movement and transitions are immediate. Dragging does not emit a public
position event, and there is no partial animation API.

## `windows`

### Types

```ts
interface SystemWindow {
  id: number
  name: string | null
  bounds: Rect
  level: number
  ownerPid: number
  ownerName: string | null
  isOnscreen: boolean
}

interface FrontmostWindow {
  bundleId: string
  icon: string | null
  name: string
  title: string | null
}
```

`level` is the front-to-back z-order position; lower values are nearer the
front. OS filtering can leave gaps. `isOnscreen` means visible, not minimized or
cloaked, and intersecting a display. `bundleId` is a bundle identifier (or app
path fallback) on macOS and the executable path on Windows or Linux.

Linux window methods require an X11/XWayland display and an EWMH-compatible
window manager for complete stacking data. The implementation falls back to the
X11 root window tree when `_NET_CLIENT_LIST_STACKING` is unavailable. Native
Wayland window enumeration and hit testing are unsupported.

#### `windows.frontmost(): Promise<FrontmostWindow | null>`

Return the foreground application, its best normal-window title, and a 32×32
PNG icon when available.

#### `windows.list(options?: { relativeTo?: number }): Promise<SystemWindow[]>`

Return a front-to-back snapshot including hidden windows. With `relativeTo`,
the result starts below that window and excludes it and all windows above it.
An unknown reference produces an empty array.

#### `windows.find(id: number): Promise<SystemWindow | null>`

Resolve one current window snapshot by native id.

#### `windows.atPoint(point: Point, options?: { belowId?: number }): Promise<SystemWindow | null>`

Return the first on-screen window containing the screen DIP point. `belowId`
excludes that window and everything above it.

## `apps`

#### `apps.icon(appPath: string, options?: { size?: 'small' | 'medium' }): Promise<string | null>`

Return an exact-size PNG data URL, or `null` when no application icon can be
read. `small` is 16×16 and `medium` is 32×32.

- macOS: absolute `.app` bundle or executable path.
- Windows: absolute executable or file path understood by the Shell.
- Linux: absolute `.desktop`, executable, AppImage, or file path. Installed
  freedesktop icon themes are searched; an existing unmatched path may return
  a generic file icon.

```ts
const icon = await apps.icon('/Applications/Safari.app', { size: 'medium' })
```
