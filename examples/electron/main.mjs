import { existsSync } from 'node:fs'
import { writeFile } from 'node:fs/promises'
import { basename, isAbsolute } from 'node:path'
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
  drag,
  overlay,
  secureChannel,
  windows,
} from '@zerob13/nativekit'

const hostId = 'nativekit-demo'
const presentationId = 'nativekit-sample'
const sessionId = 'nativekit-demo-session'
const workerPath = fileURLToPath(new URL('./worker.mjs', import.meta.url))
const smokeMode = process.argv.includes('--smoke')
const channels = []

let mainWindow = null
let selectedDragFile = null
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
  if (!overlayStarted) {
    overlay.start({ tooltip: { hide: 'Hide', relocate: 'Move' } })
    overlayStarted = true
  }
  overlay.attachHost({
    id: hostId,
    title: 'nativekit demo',
    bounds: mainWindow.getContentBounds(),
    windowHandle: mainWindow.getNativeWindowHandle(),
    anchor: { edge: 'trailing', offset: 24 },
    animated: true,
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

function showOverlay() {
  attachOverlayHost()
  overlay.setVisible(true)
  overlay.pushImage({
    hostId,
    presentationId,
    sessionId,
    imageData: sampleImageDataUrl(),
    appIconPath: process.execPath,
  })
  overlay.setActiveSession(sessionId)
  return { active: overlay.hasActive(), any: overlay.hasAny() }
}

function nodeExecutable() {
  for (const candidate of [
    process.env.npm_node_execpath,
    process.env.NODE,
  ]) {
    if (candidate && isAbsolute(candidate) && existsSync(candidate)) {
      return candidate
    }
  }
  throw new Error('Node executable not found; launch the demo through pnpm')
}

async function startWorker() {
  const executable = nodeExecutable()
  const pid = await secureChannel.spawn(executable, [workerPath])
  if (pid === null) throw new Error('A worker is already active or failed to start')
  return { pid, executable }
}

function waitForWorkerFrame(timeoutMs = 3_000) {
  let onData = null
  let timer = null
  const promise = new Promise((resolvePromise, rejectPromise) => {
    onData = (payload) => {
      clearTimeout(timer)
      secureChannel.off('data', onData)
      resolvePromise(payload.toString('utf8'))
    }
    timer = setTimeout(() => {
      secureChannel.off('data', onData)
      rejectPromise(new Error('Timed out waiting for a worker frame'))
    }, timeoutMs)
    secureChannel.on('data', onData)
  })
  return {
    promise,
    cancel: () => {
      if (timer !== null) clearTimeout(timer)
      if (onData !== null) secureChannel.off('data', onData)
    },
  }
}

async function windowSnapshot() {
  const [frontmost, list] = await Promise.all([
    windows.frontmost(),
    windows.list(),
  ])
  return { frontmost, list: list.slice(0, 8) }
}

async function runSmoke() {
  const frame = waitForWorkerFrame()
  let worker
  let payload
  try {
    worker = await startWorker()
    payload = await frame.promise
  } catch (error) {
    frame.cancel()
    secureChannel.terminate()
    throw error
  }
  const stopped = secureChannel.terminate()
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
    worker: { ...worker, payload, stopped },
    drag: 'prepared for synthetic pointer validation',
  }
}

async function runSmokeDrag(position) {
  if (!smokeMode || !mainWindow) throw new Error('Smoke mode is not active')
  if (!position || !Number.isFinite(position.x) || !Number.isFinite(position.y)) {
    throw new TypeError('A finite drag position is required')
  }
  selectedDragFile = workerPath

  let timeout = null
  const ended = new Promise((resolvePromise, rejectPromise) => {
    const onEnded = (result) => {
      clearTimeout(timeout)
      drag.off('ended', onEnded)
      resolvePromise(result)
    }
    timeout = setTimeout(() => {
      drag.off('ended', onEnded)
      rejectPromise(new Error('Timed out waiting for the native drag session'))
    }, 3_000)
    drag.on('ended', onEnded)
  })

  const x = Math.round(position.x)
  const y = Math.round(position.y)
  const inputs = [
    setTimeout(() => {
      mainWindow.webContents.sendInputEvent({
        type: 'mouseDown',
        x,
        y,
        button: 'left',
        clickCount: 1,
      })
    }, 20),
    setTimeout(() => {
      mainWindow.webContents.sendInputEvent({
        type: 'mouseMove',
        x: x + 12,
        y: y + 8,
        movementX: 12,
        movementY: 8,
      })
    }, 140),
    setTimeout(() => {
      mainWindow.webContents.sendInputEvent({
        type: 'mouseUp',
        x: x + 12,
        y: y + 8,
        button: 'left',
        clickCount: 1,
      })
    }, 280),
    setTimeout(() => {
      mainWindow.webContents.sendInputEvent({
        type: 'keyDown',
        keyCode: 'Escape',
      })
      mainWindow.webContents.sendInputEvent({
        type: 'keyUp',
        keyCode: 'Escape',
      })
    }, 1_000),
  ]
  try {
    return await ended
  } finally {
    for (const input of inputs) clearTimeout(input)
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
    nodeWorker: nodeExecutable(),
  }))
  handle('nativekit:windows:refresh', windowSnapshot)
  handle('nativekit:overlay:show', async () => showOverlay())
  handle('nativekit:overlay:hide', async () => {
    overlay.setVisible(false)
    return { active: overlay.hasActive(), any: overlay.hasAny() }
  })
  handle('nativekit:worker:start', startWorker)
  handle('nativekit:worker:stop', async () => secureChannel.terminate())
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
  handle('nativekit:drag:pick', async () => {
    const result = await dialog.showOpenDialog(mainWindow, {
      title: 'Choose a file to drag',
      properties: ['openFile'],
    })
    selectedDragFile = result.canceled ? null : result.filePaths[0] ?? null
    return selectedDragFile ? { name: basename(selectedDragFile) } : null
  })
  handle('nativekit:drag:start', async (position) => {
    if (!selectedDragFile) throw new Error('Choose a file first')
    if (
      !position ||
      !Number.isFinite(position.x) ||
      !Number.isFinite(position.y)
    ) {
      throw new TypeError('A finite drag position is required')
    }
    await drag.start({
      files: [selectedDragFile],
      windowHandle: mainWindow.getNativeWindowHandle(),
      position,
    })
    return true
  })
  handle('nativekit:smoke:run', runSmoke)
  handle('nativekit:smoke:drag', runSmokeDrag)
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
    sendEvent('overlay', { type: 'visibility', visible })
  })
  secureChannel.on('data', (payload) => {
    sendEvent('worker', { type: 'data', payload: payload.toString('utf8') })
  })
  secureChannel.on('exit', (code) => {
    sendEvent('worker', { type: 'exit', code })
  })
  drag.on('ended', (result) => sendEvent('drag', result))
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
    attachOverlayHost()
    mainWindow.show()
  })
  mainWindow.on('closed', () => {
    if (boundsTimer !== null) clearTimeout(boundsTimer)
    boundsTimer = null
    secureChannel.terminate()
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
  secureChannel.terminate()
  overlay.stop()
  for (const channel of channels) ipcMain.removeHandler(channel)
})
