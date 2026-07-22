const api = window.nativekitDemo
const element = (id) => document.getElementById(id)

const runtime = element('runtime')
const status = element('status')
const frontmost = element('frontmost')
const windowList = element('window-list')
const overlayState = element('overlay-state')
const workerState = element('worker-state')
const appIcon = element('app-icon')
const appName = element('app-name')
const dragHandle = element('drag-handle')
const dragFile = element('drag-file')
const events = element('events')

let dragActive = false

function unwrap(result) {
  if (!result?.ok) throw new Error(result?.error ?? 'Unknown main-process error')
  return result.data
}

function setStatus(message, failed = false) {
  status.textContent = message
  status.classList.toggle('error', failed)
}

function log(message) {
  const item = document.createElement('li')
  item.textContent = `${new Date().toLocaleTimeString()}  ${message}`
  events.prepend(item)
  while (events.children.length > 80) events.lastElementChild.remove()
}

async function run(button, action) {
  button.disabled = true
  button.setAttribute('aria-busy', 'true')
  setStatus('Working…')
  try {
    const data = unwrap(await action())
    setStatus('Ready.')
    return data
  } catch (error) {
    setStatus(error.message, true)
    log(`Error: ${error.message}`)
    return null
  } finally {
    button.disabled = false
    button.removeAttribute('aria-busy')
  }
}

function renderWindows(snapshot) {
  const current = snapshot.frontmost
  frontmost.textContent = current
    ? `Frontmost: ${current.name}${current.title ? ` — ${current.title}` : ''}`
    : 'No frontmost application was available.'
  windowList.replaceChildren()
  for (const windowInfo of snapshot.list) {
    const row = document.createElement('tr')
    const owner = document.createElement('td')
    const title = document.createElement('td')
    owner.textContent = windowInfo.ownerName ?? `PID ${windowInfo.ownerPid}`
    title.textContent = windowInfo.name ?? 'Untitled'
    row.append(owner, title)
    windowList.append(row)
  }
}

function renderIcon(icon, name) {
  appIcon.src = icon ?? ''
  appIcon.alt = icon ? `${name} icon` : ''
  appName.textContent = icon ? name : 'No icon was returned.'
}

const unsubscribe = api.onEvent(({ topic, data }) => {
  if (topic === 'worker') {
    if (data.type === 'data') {
      workerState.textContent = `Frame: ${data.payload}`
      log(`Worker frame: ${data.payload}`)
    } else {
      workerState.textContent = `Worker exited with code ${data.code}.`
      log(workerState.textContent)
    }
  } else if (topic === 'overlay') {
    log(`Overlay event: ${JSON.stringify(data)}`)
  } else if (topic === 'drag') {
    dragActive = false
    log(`Drag ended: ${data.dropped ? 'dropped' : 'cancelled'} at ${data.x}, ${data.y}`)
  }
})

window.addEventListener('beforeunload', unsubscribe, { once: true })

element('refresh-windows').addEventListener('click', async (event) => {
  const snapshot = await run(event.currentTarget, api.refreshWindows)
  if (snapshot) {
    renderWindows(snapshot)
    log(`Read ${snapshot.list.length} system windows.`)
  }
})

element('show-overlay').addEventListener('click', async (event) => {
  const state = await run(event.currentTarget, api.showOverlay)
  if (state) {
    overlayState.textContent = `Visible: ${state.active}; presentations: ${state.any}.`
    log('Overlay sample shown.')
  }
})

element('hide-overlay').addEventListener('click', async (event) => {
  const state = await run(event.currentTarget, api.hideOverlay)
  if (state) {
    overlayState.textContent = `Visible: ${state.active}; presentations retained: ${state.any}.`
    log('Overlay hidden.')
  }
})

element('start-worker').addEventListener('click', async (event) => {
  const worker = await run(event.currentTarget, api.startWorker)
  if (worker) {
    workerState.textContent = `Worker PID ${worker.pid}.`
    log(`Verified worker ${worker.pid}.`)
  }
})

element('stop-worker').addEventListener('click', async (event) => {
  const stopped = await run(event.currentTarget, api.stopWorker)
  if (stopped !== null) {
    workerState.textContent = stopped ? 'Worker terminated.' : 'No worker to stop.'
    log(workerState.textContent)
  }
})

element('pick-app').addEventListener('click', async (event) => {
  const selected = await run(event.currentTarget, api.pickApp)
  if (selected) {
    renderIcon(selected.icon, selected.name)
    log(`Extracted icon for ${selected.name}.`)
  }
})

element('pick-file').addEventListener('click', async (event) => {
  const selected = await run(event.currentTarget, api.pickDragFile)
  if (selected) {
    dragFile.textContent = selected.name
    dragHandle.disabled = false
    log(`Selected ${selected.name} for drag-out.`)
  }
})

dragHandle.addEventListener('pointerdown', (event) => {
  if (event.button !== 0 || !event.isPrimary || dragActive) return
  event.preventDefault()
  dragActive = true
  setStatus('Native drag session active…')
  api
    .startDrag({ x: event.clientX, y: event.clientY })
    .then(unwrap)
    .then(() => setStatus('Ready.'))
    .catch((error) => {
      dragActive = false
      setStatus(error.message, true)
      log(`Drag error: ${error.message}`)
    })
})

element('clear-events').addEventListener('click', () => events.replaceChildren())

async function initialize() {
  try {
    const info = unwrap(await api.status())
    runtime.textContent = `${info.platform}-${info.arch} · Electron ${info.electron}`
    const snapshot = unwrap(await api.refreshWindows())
    renderWindows(snapshot)

    if (new URLSearchParams(location.search).has('smoke')) {
      setStatus('Running native smoke validation…')
      const result = unwrap(await api.runSmoke())
      renderWindows(result.windows)
      renderIcon(result.icon, 'Electron')
      overlayState.textContent = `Visible: ${result.overlay.active}; presentations: ${result.overlay.any}.`
      workerState.textContent = `Frame: ${result.worker.payload}`
      dragFile.textContent = 'worker.mjs'
      dragHandle.disabled = false
      await new Promise((resolvePromise) => requestAnimationFrame(resolvePromise))
      const dragBounds = dragHandle.getBoundingClientRect()
      const dragResult = unwrap(
        await api.runSmokeDrag({
          x: dragBounds.left + dragBounds.width / 2,
          y: dragBounds.top + dragBounds.height / 2,
        }),
      )
      log('Native smoke validation passed.')
      setStatus('Smoke validation passed.')
      await new Promise((resolvePromise) =>
        requestAnimationFrame(() => requestAnimationFrame(resolvePromise)),
      )
      await api.completeSmoke({
        platform: result.platform,
        windows: result.windows.list.length,
        icon: Boolean(result.icon),
        overlay: result.overlay.active,
        worker: Boolean(result.worker.payload),
        drag: typeof dragResult.dropped === 'boolean',
      })
    }
  } catch (error) {
    setStatus(error.message, true)
    log(`Startup error: ${error.message}`)
    if (new URLSearchParams(location.search).has('smoke')) {
      await api.completeSmoke({ error: error.message })
    }
  }
}

initialize()
