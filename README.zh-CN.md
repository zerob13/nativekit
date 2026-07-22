# nativekit

[English](README.md) · [Electron 集成指南](docs/integration-guide.zh-CN.md) · [API reference](docs/api-reference.md)

`nativekit` 是一个只在 Electron 主进程运行的跨平台原生能力库。它由
TypeScript API 与 Node-API v8 插件组成，JavaScript 部分使用 Vite library
mode 打包，原生部分继续使用 CMake/cmake-js。

当前支持 macOS arm64/x64 与 Windows x64，主要提供：

- 可拖动、不会抢占应用焦点的原生图片 Overlay；
- 前台应用、系统窗口枚举、查找与坐标命中；
- 系统应用图标提取。

## 安装

发布到 npm 后直接安装：

```bash
pnpm add @zerob13/nativekit
```

npm 包会携带以下 Node-API 预编译文件，正常安装不需要本机 C++ 编译环境：

```text
darwin-arm64
darwin-x64
win32-x64
```

库只能在 Electron 主进程导入。Renderer 应通过启用
`contextIsolation` 的 preload 暴露最小业务接口，不能直接暴露整个模块。

## 最小示例

```ts
import { app, BrowserWindow } from 'electron'
import { overlay, windows } from '@zerob13/nativekit'

await app.whenReady()

const win = new BrowserWindow({ width: 800, height: 600 })

overlay.start({
  tooltip: { hide: 'Hide', relocate: 'Move to next anchor' },
})
overlay.attachHost({
  id: 'main',
  title: 'Assistant',
  bounds: win.getContentBounds(),
  windowHandle: win.getNativeWindowHandle(),
  anchor: { edge: 'trailing', offset: 16 },
})

overlay.pushImage({
  hostId: 'main',
  presentationId: 'capture-1',
  sessionId: 'task-42',
  imageData: 'data:image/png;base64,iVBORw0KGgo...',
})

overlay.on('activate', () => win.show())
console.log(await windows.frontmost())

app.on('will-quit', () => overlay.stop())
```

用户可以直接按住 Overlay 图片的可见区域拖动。手动位置会在 host 和图片更新时保留，
并被限制在当前显示器工作区内；点击 Overlay 自带的 relocate 控件会清除手动位置，
让该面板回到下一个锚点。位置只保留到 presentation 被删除或 `overlay.stop()`，
不会跨应用启动持久化。

## Electron demo

```bash
pnpm install
pnpm demo
pnpm demo:smoke
```

交互 demo 覆盖主要桌面交互模块。点击 `Choose images` 可一次选择多张图片，Overlay
会立即显示第一张，并每 5 秒切换一张；也可直接拖动主窗口外侧的原生 Overlay。
Smoke 模式会集中验证窗口枚举、图标与 Overlay 渲染。

```text
┌ nativekit demo ───────────────────────────────────────┐
│ Window awareness  │ Overlay: show / hide / drag      │
│                   │ multi-image rotation every 5s    │
│ Application icon                                     │
│ Ordered event log                                    │
└───────────────────────────────────────────────────────┘
```

## 本地开发

要求 Node.js 20.19+、pnpm 10、CMake 3.22+，以及 macOS 的 Xcode Command
Line Tools 或 Windows 的 Visual Studio C++ Build Tools。

```bash
pnpm build:js       # Vite ESM/CJS + TypeScript declarations
pnpm build:native   # CMake + cmake-js
pnpm check
```

完整的主进程、preload、Renderer、打包和故障排查示例见
[Electron 集成指南](docs/integration-guide.zh-CN.md)。全部方法与事件定义见
[API reference](docs/api-reference.md)。

## 发布

版本号与 `vX.Y.Z` tag 必须一致。推送匹配 `v*` 的 tag 会触发发布，普通分支
push（包括 `main`）不会；也可以在 GitHub Actions 手动运行 `release` workflow
并输入已有 tag。workflow 会分别构建并测试 macOS arm64、macOS x64 和 Windows
x64，组装唯一 npm tarball，再发布同一份归档到 npm 与 GitHub Release。发布身份
由 npm Trusted Publishing 提供。

许可证：MIT。
