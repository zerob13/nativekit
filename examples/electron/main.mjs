import { writeFile } from 'node:fs/promises'
import { basename } from 'node:path'
import { fileURLToPath } from 'node:url'

import {
  app,
  BrowserWindow,
  dialog,
  ipcMain,
  nativeImage,
} from 'electron'
import {
  apps,
  overlay,
  windows,
} from '@zerob13/nativekit'

const hostId = 'nativekit-demo'
const presentationId = 'nativekit-sample'
const sessionId = 'nativekit-demo-session'
const overlayRotationIntervalMs = 5_000
const maximumOverlaySourceSize = 1_600
const smokeMode = process.argv.includes('--smoke')
const channels = []

let mainWindow = null
let selectedOverlayImages = []
let overlayImageIndex = 0
let overlayRotationTimer = null
let overlayStarted = false
let boundsTimer = null

function sendEvent(topic, data) {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.webContents.send('nativekit:event', { topic, data })
  }
}

function errorMessage(error) {
  return error instanceof Error ? error.message : String(error)
}

function handle(channel, handler) {
  channels.push(channel)
  ipcMain.handle(channel, async (event, ...arguments_) => {
    try {
      if (!mainWindow || event.sender !== mainWindow.webContents) {
        throw new Error('Unexpected IPC sender')
      }
      return { ok: true, data: await handler(...arguments_) }
    } catch (error) {
      return { ok: false, error: errorMessage(error) }
    }
  })
}

function attachOverlayHost() {
  if (!mainWindow || mainWindow.isDestroyed()) return
  const windowHandle = mainWindow.getNativeWindowHandle()
  if (!overlayStarted) {
    overlay.start({
      tooltip: { hide: 'Hide', relocate: 'Move to next anchor' },
    })
    overlayStarted = true
  }
  const bounds = mainWindow.getContentBounds()
  overlay.attachHost({
    id: hostId,
    title: 'nativekit demo',
    bounds,
    windowHandle,
    anchor: { edge: 'trailing', offset: 24 },
  })
}

function scheduleOverlayHostUpdate() {
  if (boundsTimer !== null) clearTimeout(boundsTimer)
  boundsTimer = setTimeout(() => {
    boundsTimer = null
    attachOverlayHost()
  }, 80)
}

function sampleImageDataUrl() {
  const width = 360
  const height = 220
  const bitmap = Buffer.alloc(width * height * 4)
  const fill = (x, y, rectangleWidth, rectangleHeight, color) => {
    for (let row = y; row < y + rectangleHeight; row += 1) {
      for (let column = x; column < x + rectangleWidth; column += 1) {
        const offset = (row * width + column) * 4
        bitmap[offset] = color[2]
        bitmap[offset + 1] = color[1]
        bitmap[offset + 2] = color[0]
        bitmap[offset + 3] = color[3] ?? 255
      }
    }
  }
  fill(0, 0, width, height, [28, 28, 30])
  fill(24, 24, 48, 48, [10, 132, 255])
  fill(38, 46, 20, 4, [255, 255, 255])
  fill(46, 38, 4, 20, [255, 255, 255])
  fill(88, 34, 150, 12, [242, 242, 247])
  fill(88, 54, 96, 8, [142, 142, 147])
  fill(24, 104, 292, 10, [209, 209, 214])
  fill(24, 128, 238, 8, [142, 142, 147])
  fill(24, 178, 312, 1, [58, 58, 60])
  fill(24, 198, 190, 7, [100, 210, 255])
  const image = nativeImage.createFromBitmap(bitmap, {
    width,
    height,
    scaleFactor: 1,
  })
  if (image.isEmpty()) throw new Error('Could not create the sample image')
  return `data:image/png;base64,${image.toPNG().toString('base64')}`
}

function selectedImageDataUrl(path) {
  const image = nativeImage.createFromPath(path)
  if (image.isEmpty()) throw new Error(`Could not load ${basename(path)}`)
  const size = image.getSize()
  const longestEdge = Math.max(size.width, size.height)
  const renderedImage = longestEdge > maximumOverlaySourceSize
    ? image.resize({
        width: Math.max(
          1,
          Math.round(size.width * maximumOverlaySourceSize / longestEdge),
        ),
        height: Math.max(
          1,
          Math.round(size.height * maximumOverlaySourceSize / longestEdge),
        ),
        quality: 'good',
      })
    : image
  return `data:image/png;base64,${renderedImage.toPNG().toString('base64')}`
}

function overlayImageAt(imagePaths, imageIndex) {
  if (imagePaths.length === 0) {
    return { imageData: sampleImageDataUrl(), name: 'Built-in sample' }
  }
  const path = imagePaths[imageIndex]
  return { imageData: selectedImageDataUrl(path), name: basename(path) }
}

function presentOverlayImage(image, imageCount, imageIndex) {
  attachOverlayHost()
  overlay.pushImage({
    hostId,
    presentationId,
    sessionId,
    imageData: image.imageData,
    appIconPath: process.execPath,
  })
  overlay.setActiveSession(sessionId)
  overlay.setVisible(true)
  return {
    active: overlay.hasActive(),
    any: overlay.hasAny(),
    imageCount,
    imageIndex: imageCount === 0 ? null : imageIndex,
    imageName: image.name,
  }
}

function showOverlay() {
  return presentOverlayImage(
    overlayImageAt(selectedOverlayImages, overlayImageIndex),
    selectedOverlayImages.length,
    overlayImageIndex,
  )
}

function stopOverlayRotation() {
  if (overlayRotationTimer !== null) clearInterval(overlayRotationTimer)
  overlayRotationTimer = null
}

function startOverlayRotation() {
  stopOverlayRotation()
  if (selectedOverlayImages.length < 2) return
  overlayRotationTimer = setInterval(() => {
    const nextImageIndex =
      (overlayImageIndex + 1) % selectedOverlayImages.length
    try {
      const image = overlayImageAt(selectedOverlayImages, nextImageIndex)
      const state = presentOverlayImage(
        image,
        selectedOverlayImages.length,
        nextImageIndex,
      )
      overlayImageIndex = nextImageIndex
      sendEvent('overlay', { type: 'image', ...state })
    } catch (error) {
      stopOverlayRotation()
      sendEvent('overlay', { type: 'rotationError', message: errorMessage(error) })
    }
  }, overlayRotationIntervalMs)
  overlayRotationTimer.unref?.()
}

async function pickOverlayImages() {
  const result = await dialog.showOpenDialog(mainWindow, {
    title: 'Choose overlay images',
    properties: ['openFile', 'multiSelections'],
    filters: [{ name: 'Images', extensions: ['png', 'jpg', 'jpeg', 'webp'] }],
  })
  if (result.canceled || result.filePaths.length === 0) return null
  const image = overlayImageAt(result.filePaths, 0)
  const state = presentOverlayImage(image, result.filePaths.length, 0)
  stopOverlayRotation()
  selectedOverlayImages = result.filePaths
  overlayImageIndex = 0
  startOverlayRotation()
  return state
}

async function windowSnapshot() {
  const [frontmost, list] = await Promise.all([
    windows.frontmost(),
    windows.list(),
  ])
  return { frontmost, list: list.slice(0, 8) }
}

async function runSmoke() {
  const snapshot = await windowSnapshot()
  const icon = await apps.icon(process.execPath, { size: 'medium' })
  const overlayState = showOverlay()
  if (!icon || snapshot.list.length === 0 || !overlayState.active) {
    throw new Error('Smoke validation returned an incomplete native result')
  }
  return {
    platform: `${process.platform}-${process.arch}`,
    windows: snapshot,
    icon,
    overlay: overlayState,
  }
}

async function completeSmoke(result) {
  if (!smokeMode || !mainWindow) return false
  const image = await mainWindow.webContents.capturePage()
  const screenshotPath = process.env.NATIVEKIT_DEMO_SCREENSHOT
  if (screenshotPath) await writeFile(screenshotPath, image.toPNG())
  process.stdout.write(`NATIVEKIT_DEMO_SMOKE ${JSON.stringify(result)}\n`)
  setTimeout(() => app.exit(result?.error ? 1 : 0), 200)
  return true
}

function registerIpc() {
  handle('nativekit:status', async () => ({
    platform: process.platform,
    arch: process.arch,
    electron: process.versions.electron,
  }))
  handle('nativekit:windows:refresh', windowSnapshot)
  handle('nativekit:overlay:show', async () => {
    const state = showOverlay()
    startOverlayRotation()
    return state
  })
  handle('nativekit:overlay:pick', pickOverlayImages)
  handle('nativekit:overlay:hide', async () => {
    stopOverlayRotation()
    overlay.setVisible(false)
    return { active: overlay.hasActive(), any: overlay.hasAny() }
  })
  handle('nativekit:apps:pick', async () => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Choose an application',
      properties: ['openFile'],
    })
    if (result.canceled || result.filePaths.length === 0) return null
    const path = result.filePaths[0]
    return {
      name: basename(path),
      path,
      icon: await apps.icon(path, { size: 'medium' }),
    }
  })
  handle('nativekit:smoke:run', runSmoke)
  handle('nativekit:smoke:complete', completeSmoke)
}

function wireNativeEvents() {
  overlay.on('activate', () => {
    if (mainWindow) {
      mainWindow.show()
      mainWindow.focus()
    }
    sendEvent('overlay', { type: 'activate' })
  })
  overlay.on('visibilityRequest', (visible) => {
    if (visible) startOverlayRotation()
    else stopOverlayRotation()
    sendEvent('overlay', {
      type: 'visibility',
      visible,
      active: overlay.hasActive(),
      any: overlay.hasAny(),
    })
  })
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1040,
    height: 760,
    minWidth: 760,
    minHeight: 600,
    show: false,
    backgroundColor: '#f2f2f7',
    webPreferences: {
      preload: fileURLToPath(new URL('./preload.cjs', import.meta.url)),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  })
  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }))
  mainWindow.webContents.on('will-navigate', (event) => event.preventDefault())
  mainWindow.on('resize', scheduleOverlayHostUpdate)
  mainWindow.on('move', scheduleOverlayHostUpdate)
  mainWindow.once('ready-to-show', () => {
    mainWindow.show()
    setImmediate(attachOverlayHost)
  })
  mainWindow.on('closed', () => {
    if (boundsTimer !== null) clearTimeout(boundsTimer)
    boundsTimer = null
    stopOverlayRotation()
    selectedOverlayImages = []
    overlayImageIndex = 0
    overlay.stop()
    overlayStarted = false
    mainWindow = null
  })
  mainWindow.loadFile(fileURLToPath(new URL('./index.html', import.meta.url)), {
    query: smokeMode ? { smoke: '1' } : {},
  })
}

app.whenReady().then(() => {
  registerIpc()
  wireNativeEvents()
  createWindow()
})

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow()
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin' || smokeMode) app.quit()
})

app.on('will-quit', () => {
  stopOverlayRotation()
  overlay.stop()
  for (const channel of channels) ipcMain.removeHandler(channel)
})
