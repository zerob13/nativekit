# API Reference

> `nativekit` is **Electron main-process only**. All methods throw if called
> from the renderer process.

```ts
import { overlay, windows, apps, drag, secureChannel } from 'nativekit'
```

All async methods return Promises. Event subscriptions use the standard
`.on(eventName, listener)` / `.off(eventName, listener)` pattern.

---

## Table of contents

- [overlay](#overlay)
- [windows](#windows)
- [secureChannel](#securechannel)
- [apps](#apps)
- [drag](#drag)

---

## overlay

Floating, always-on-top panel system. Panels persist across Spaces (macOS) /
virtual desktops (Windows) and stack along screen edges.

### Types

```ts
interface AnchorConfig {
  /** screen edge to attach to */
  edge: 'leading' | 'trailing' | 'top' | 'bottom'
  /** distance from the edge in points */
  offset: number
}

interface HostConfig {
  /** unique host id */
  id: string
  /** panel title (for tooltip/accessibility) */
  title: string
  /** content rect relative to the host window, in points */
  bounds: { x: number; y: number; width: number; height: number }
  /** Buffer returned by BrowserWindow.getNativeWindowHandle() */
  windowHandle: Buffer
  /** screen-edge attachment */
  anchor: AnchorConfig
  /** animate appearance (default true) */
  animated?: boolean
}

interface ImageFrame {
  /** unique presentation id */
  presentationId: string
  /** logical grouping; completing a session clears its presentations */
  sessionId: string
  /** 'data:image/png;base64,...' (or jpg) */
  imageData: string
  /** optional app icon path, rendered as a badge (macOS app path / Windows exe) */
  appIconPath?: string | null
}

interface OverlayOptions {
  /** control tooltips */
  tooltip?: { hide?: string; relocate?: string }
}
```

### Methods

#### `overlay.start(options?: OverlayOptions): boolean`

Initialize the overlay host system. Must be called once before `attachHost`.
Returns `true` on success.

```js
overlay.start({ tooltip: { hide: 'Hide', relocate: 'Move' } })
```

#### `overlay.stop(): boolean`

Shut down the overlay system. Destroys all panels and hosts. Safe to call
multiple times.

#### `overlay.attachHost(config: HostConfig): boolean`

Register an Electron `BrowserWindow` as an overlay origin.

```js
overlay.attachHost({
  id: 'main',
  title: 'Assistant',
  bounds: { x: 0, y: 0, width: 320, height: 200 },
  windowHandle: win.getNativeWindowHandle(),
  anchor: { edge: 'trailing', offset: 16 },
})
```

#### `overlay.detachHost(hostId: string): boolean`

Unregister a host and remove its panels.

#### `overlay.setVisible(visible: boolean): boolean`

Show / hide the entire overlay stack.

#### `overlay.setMaxSize(size: number): boolean`

Constrain the maximum display size (longest edge, in points) for panels.

#### `overlay.pushImage(frame: ImageFrame): boolean`

Create or update a presentation with an image frame. Multiple frames with the
same `presentationId` update the same panel.

```js
overlay.pushImage({
  presentationId: 'snap-1',
  sessionId: 'task-42',
  imageData: 'data:image/png;base64,iVBORw0KGgo...',
  appIconPath: '/Applications/Google Chrome.app',
})
```

#### `overlay.removeImage(presentationId: string): boolean`

Remove a single presentation.

#### `overlay.completeSession(sessionId: string): boolean`

Mark a session complete; all presentations belonging to it are cleared.

#### `overlay.invalidateSession(sessionId: string, presentationId: string): boolean`

Invalidate a specific presentation within a session without completing the
whole session.

#### `overlay.suppressSessions(sessionIds: string[]): boolean`

Suppress panels for the given sessions (they exist but are hidden).

#### `overlay.setActiveSession(sessionId: string): boolean`

Set the currently active session. The overlay stack shows the active
session's content on top.

#### `overlay.hasActive(): boolean`

Whether any presentation is currently visible.

#### `overlay.hasAny(): boolean`

Whether any presentation exists (visible or hidden).

### Events

| Event | Listener signature | Fires when |
|---|---|---|
| `maxSizeChanged` | `(size: number) => void` | Max display size changes (e.g. display reconfiguration). |
| `activate` | `() => void` | User interacts to bring the app forward (double-click / tap on panel). |
| `visibilityRequest` | `(visible: boolean) => void` | Overlay requests a visibility change. |
| `cursor` | `(pos: { x: number; y: number; active: boolean }) => void` | Cursor position update from an isolated worker (automation mode). |

```js
overlay.on('activate', () => mainWindow.show())
overlay.on('maxSizeChanged', (size) => console.log('max size now', size))
```

---

## windows

System window enumeration and query. Build context-aware UI by knowing what the
user is looking at.

### Types

```ts
interface SystemWindow {
  /** OS window id */
  id: number
  /** window title, if any */
  name: string | null
  /** frame in global screen coordinates (points mac / px win) */
  bounds: { x: number; y: number; width: number; height: number }
  /** window level (z-order) */
  level: number
  /** owning process id */
  ownerPid: number
  /** owning app name, if any */
  ownerName: string | null
  /** currently on-screen */
  isOnscreen: boolean
}

interface FrontmostWindow {
  /** app bundle id (mac) / exe path (win) */
  bundleId: string
  /** small app icon as data URL, or null */
  icon: string | null
  /** app display name */
  name: string
  /** frontmost window title, if any */
  title: string | null
}
```

### Methods

#### `windows.frontmost(): Promise<FrontmostWindow | null>`

Return information about the currently frontmost application and its window.

```js
const f = await windows.frontmost()
// { bundleId: 'com.google.Chrome', name: 'Google Chrome',
//   title: 'GitHub', icon: 'data:image/png;base64,...' }
```

#### `windows.list(options?: { relativeTo?: number }): Promise<SystemWindow[]>`

Enumerate all system windows. If `relativeTo` (a window id) is given, results
are ordered relative to that window's z-order.

#### `windows.find(id: number): Promise<SystemWindow | null>`

Look up a single window by id.

#### `windows.atPoint(point: { x: number; y: number }, options?: { belowId?: number }): Promise<SystemWindow | null>`

Return the topmost window at the given screen coordinate, optionally excluding
windows at or above `belowId` in z-order.

---

## secureChannel

Spawn a sandboxed child process for sensitive work and stream results back over
a verified channel.

### Methods

#### `secureChannel.spawn(executablePath: string): Promise<number | null>`

Launch an isolated worker process. Resolves with the child PID, or `null` on
failure.

```js
const pid = await secureChannel.spawn('/path/to/worker')
```

#### `secureChannel.verify(pid: number, executablePath: string): Promise<boolean>`

Verify that the given PID is running the expected executable path. Use this to
authenticate an incoming IPC connection (audit-token / path check) before
trusting its data.

#### `secureChannel.wasTerminatedByPrivacy(): boolean`

Returns `true` if the last worker termination was caused by a privacy /
permission gate (e.g. macOS TCC revocation), letting the app show the right
recovery UX instead of a generic crash.

### Events

| Event | Listener signature | Fires when |
|---|---|---|
| `data` | `(payload: Buffer) => void` | A data frame arrives from the worker. |
| `exit` | `(code: number) => void` | Worker process exits. |

```js
secureChannel.on('data', (frame) => {
  // push into an overlay, process, etc.
})
```

---

## apps

Native application icon extraction.

### Methods

#### `apps.icon(appPath: string, options?: { size?: 'small' | 'medium' }): Promise<string | null>`

Return the application's icon as a `data:image/png;base64,...` URL.

- **macOS**: pass a `.app` bundle path (e.g. `/Applications/Safari.app`).
- **Windows**: pass an `.exe` path (e.g. `C:\Program Files\...\chrome.exe`);
  the icon resource is extracted via `SHGetFileInfo`.

```js
const icon = await apps.icon('/Applications/Safari.app', { size: 'medium' })
// 'data:image/png;base64,iVBORw0...'
```

---

## drag

Native file drag-out from an Electron window into Finder / Explorer / any app.

### Types

```ts
interface DragConfig {
  /** absolute file paths to drag */
  files: string[]
  /** native window handle (BrowserWindow.getNativeWindowHandle()) */
  windowHandle: Buffer
  /** drag origin in window-local coordinates */
  position: { x: number; y: number }
}
```

### Methods

#### `drag.start(config: DragConfig): Promise<void>`

Begin a native OS drag session carrying the given files. The promise resolves
when the drag session ends (drop or cancel).

```js
await drag.start({
  files: ['/Users/me/report.pdf'],
  windowHandle: win.getNativeWindowHandle(),
  position: { x: 100, y: 200 },
})
```

### Events

| Event | Listener signature | Fires when |
|---|---|---|
| `ended` | `(info: { dropped: boolean; x: number; y: number }) => void` | Drag session ends. |
