# Electron 集成指南

本指南说明如何把 `@zerob13/nativekit` 接入一个启用了上下文隔离的 Electron
应用。完整类型和边界条件见 [API reference](api-reference.md)。

## 1. 先确定进程边界

`nativekit` 持有原生窗口，只能由 Electron 主进程导入。
Renderer 不应导入它，也不应通过 preload 获得任意 channel 或整个原生模块。

```text
Renderer UI
    │ narrow domain calls
    ▼
context-isolated preload
    │ explicit ipcRenderer.invoke / event subscription
    ▼
Electron main process
    │
    ▼
@zerob13/nativekit → Node-API → AppKit / Win32 / XCB
```

建议目录：

```text
src/
  main.ts
  preload.ts
  renderer.ts
```

安装依赖：

```bash
pnpm add @zerob13/nativekit
```

发布预编译文件支持 Electron 28+、macOS arm64/x64 和 Windows x64。Linux
x64/arm64 当前是 build-only 目标，需要从源码构建，暂不随 release 发布。
Node-API v8 让同一预编译文件可跨兼容的 Electron ABI 使用，不需要为每个
Electron 版本单独编译。

## 2. 主窗口和 Overlay

Overlay host 需要最新的内容区域和原生窗口句柄。创建窗口后启动 Overlay，并在
窗口移动、缩放或跨显示器时合并更新；不要把每个鼠标移动事件发送给 Renderer。

Linux 的 Overlay 与窗口查询要求 X11/XWayland。若系统默认进入原生 Wayland，
必须在创建任何窗口前选择 X11 Ozone backend：

```ts
import { app } from 'electron'

if (process.platform === 'linux') {
  app.commandLine.appendSwitch('ozone-platform', 'x11')
}
```

GNOME、KDE Plasma、Cinnamon、Xfce 与 MATE 的 X11 会话属于支持范围。原生
Wayland 不允许普通客户端读取其他应用窗口或全局坐标，也不允许任意定位顶层窗口，
因此不能用桌面环境专用脚本伪装成统一支持。

```ts
import { fileURLToPath } from 'node:url'

import { app, BrowserWindow, ipcMain } from 'electron'
import { overlay } from '@zerob13/nativekit'

const hostId = 'main'
let win: BrowserWindow | null = null
let syncTimer: NodeJS.Timeout | null = null

function syncOverlayHost(): void {
  if (!win || win.isDestroyed()) return
  overlay.attachHost({
    id: hostId,
    title: 'Assistant',
    bounds: win.getContentBounds(),
    windowHandle: win.getNativeWindowHandle(),
    anchor: { edge: 'trailing', offset: 16 },
  })
}

function scheduleOverlayHostSync(): void {
  if (syncTimer) clearTimeout(syncTimer)
  syncTimer = setTimeout(() => {
    syncTimer = null
    syncOverlayHost()
  }, 80)
}

await app.whenReady()

win = new BrowserWindow({
  width: 960,
  height: 720,
  webPreferences: {
    preload: fileURLToPath(new URL('./preload.js', import.meta.url)),
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: true,
  },
})

overlay.start({
  tooltip: { hide: 'Hide', relocate: 'Move to next anchor' },
})
syncOverlayHost()
win.on('move', scheduleOverlayHostSync)
win.on('resize', scheduleOverlayHostSync)
```

`bounds` 使用 Electron DIP；不要传 `getBounds()` 与手工转换后的物理像素。
`windowHandle` 必须原样使用 `getNativeWindowHandle()` 返回的 `Buffer`。

显示图片：

```ts
overlay.pushImage({
  hostId,
  presentationId: 'preview-1',
  sessionId: 'task-42',
  imageData: pngDataUrl,
  appIconPath: process.execPath,
})
overlay.setActiveSession('task-42')
overlay.setVisible(true)
```

`imageData` 只能是 PNG/JPEG base64 data URL，字符串最大 32 MiB；解码后单边
最大 8192 像素且 RGBA 数据最大 64 MiB。相同 `presentationId` 可以更新图片，
但不能换 host 或 session。

### 用户拖动行为

- 按住面板的可见图片区域即可拖动；hide/relocate 控件仍正常点击。
- 拖动在 AppKit/Win32 内完成，不产生 Renderer `mousemove` IPC，也不会激活面板。
- 松开后，面板保留手动位置；后续 host 同步、显隐和图片尺寸更新不会重新锚定。
- 面板会被限制在当前显示器工作区内，并保留原来的锚点堆叠槽位，避免其他面板
  在拖动时跳位。
- 点击 relocate 控件会清除该面板的手动位置，并按
  `trailing → bottom → leading → top` 循环到下一个 host 锚点。
- `removeImage()`、`completeSession()`、`detachHost()` 或 `stop()` 会删除对应
  原生面板及其手动位置。库不做跨启动位置持久化。

双击同一可见区域会触发 `activate`；hide 控件会请求全局隐藏：

```ts
overlay.on('activate', () => {
  win?.show()
  win?.focus()
})

overlay.on('visibilityRequest', (visible) => {
  overlay.setVisible(visible)
})
```

仓库 demo 的 `Choose images` 会在主进程读取多张图片，并每 5 秒用相同的
`presentationId` 调用一次 `pushImage()`。这样只更新现有面板，不会创建额外浮层，
也会保留用户拖动后的手动位置。轮播定时器属于应用策略，应留在主进程并在窗口关闭
或应用退出时清理，不需要加入 native API。

Renderer 若需要 show/hide，只暴露业务方法：

```ts
// main.ts
ipcMain.handle('overlay:show', (event) => {
  if (!win || event.sender !== win.webContents) {
    throw new Error('Unexpected IPC sender')
  }
  overlay.setVisible(true)
  return { active: overlay.hasActive(), any: overlay.hasAny() }
})

ipcMain.handle('overlay:hide', (event) => {
  if (!win || event.sender !== win.webContents) {
    throw new Error('Unexpected IPC sender')
  }
  overlay.setVisible(false)
  return { active: overlay.hasActive(), any: overlay.hasAny() }
})
```

preload 只暴露业务方法，不要向 Renderer 暴露原始 `ipcRenderer`。

```ts
// preload.ts
import { contextBridge, ipcRenderer } from 'electron'

const overlayApi = Object.freeze({
  show: () => ipcRenderer.invoke('overlay:show'),
  hide: () => ipcRenderer.invoke('overlay:hide'),
})

contextBridge.exposeInMainWorld('desktop', Object.freeze({
  overlay: overlayApi,
}))
```

应用退出和窗口销毁时清理状态：

```ts
win.on('closed', () => {
  overlay.stop()
  win = null
})

app.on('will-quit', () => overlay.stop())
```

## 3. 系统窗口查询

这些方法返回统一的 Electron DIP 坐标；Windows 物理像素转换在库内部完成。
Linux 使用 X11 root-window 坐标，原生 Wayland 下不可用。

```ts
import { windows } from '@zerob13/nativekit'

const [frontmost, visibleOrder] = await Promise.all([
  windows.frontmost(),
  windows.list(),
])

const exact = await windows.find(visibleOrder[0].id)
const underPointer = await windows.atPoint({ x: 320, y: 240 })
```

`list()` 包含隐藏窗口；`atPoint()` 只命中当前在屏幕上的窗口。`level` 越小越靠前。
若界面需要频繁刷新，主进程应节流并一次传递完整快照，Renderer 不要逐窗口调用
IPC。

## 4. 应用图标

```ts
import { apps } from '@zerob13/nativekit'

const icon = await apps.icon(applicationPath, { size: 'medium' })
```

返回值是精确 16×16（`small`）或 32×32（`medium`）PNG data URL；无法读取时是
`null`。macOS 可传 `.app` 或其可执行文件绝对路径，Windows 传 Shell 可识别的
文件绝对路径；Linux 可传 `.desktop`、可执行文件、AppImage 或其他文件的绝对
路径，并通过 GIO 与 freedesktop 图标主题解析。找不到应用条目时可能返回通用文件图标。

## 5. 打包 Electron 应用

发布包已包含 `.node` 预编译文件，但原生模块不能留在 `app.asar` 内执行。如果打包
工具没有自动识别它，显式解包：

```json
{
  "build": {
    "asarUnpack": [
      "node_modules/@zerob13/nativekit/prebuilds/**/*"
    ]
  }
}
```

若使用其他打包器，目标相同：最终应用中
`node_modules/@zerob13/nativekit/prebuilds/<platform>-<arch>/*.node` 必须位于
`app.asar.unpacked` 或等价的真实文件系统目录。不要只复制 `dist/`，否则
`node-gyp-build` 找不到原生文件。

构建产物至少在每个平台检查：

```text
macOS arm64 app → darwin-arm64 prebuild
macOS x64 app   → darwin-x64 prebuild
Windows x64 app → win32-x64 prebuild
```

Linux build CI 会生成 `linux-x64` 与 `linux-arm64` 检查产物，但 release workflow
暂不打包它们。Linux 应用在这一阶段应固定到源码构建产物，不应假设 npm tarball
已经包含 Linux prebuild。

## 6. 验证和故障排查

仓库内 demo 是可运行的参考实现：

```bash
pnpm demo
pnpm demo:smoke
```

集成完成后至少检查：

- Renderer 无法直接访问 Node、Electron 或 `nativekit`；
- 重复 show/hide、窗口移动与跨显示器后 Overlay 状态正确；
- Overlay 背景可拖动，控件仍可点击，relocate 能恢复锚点布局；
- Window 查询坐标与 Electron `screen` 坐标一致；
- 打包后的应用能从 `app.asar.unpacked` 加载匹配架构的 `.node` 文件。

常见错误：

| 错误 | 原因与处理 |
|---|---|
| `nativekit must run in the Electron main process` | Renderer 导入了包；移动到 main 并缩窄 preload API。 |
| `no native binary for ...` | 缺少对应 prebuild，或打包时未复制/解包 `prebuilds/`。 |
| `windowHandle has an unexpected size` | 修改了 Electron 返回的 Buffer；应原样传递。 |
| `Linux window APIs require an available X11 display` | 当前是无显示环境或原生 Wayland；提供 `DISPLAY`，并以 `--ozone-platform=x11` 启动 Electron。 |
| `overlays require Electron to use X11/XWayland` | Electron 没有提供可验证的 X11 窗口句柄；在创建窗口前切换 Electron Ozone backend。 |
| Overlay 回到锚点 | presentation 被删除重建、调用了 relocate，或重启了 Overlay。 |
