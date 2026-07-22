const api = window.nativekitDemo
const element = (id) => document.getElementById(id)

const runtime = element('runtime')
const status = element('status')
const frontmost = element('frontmost')
const windowList = element('window-list')
const overlayState = element('overlay-state')
const overlayImages = element('overlay-images')
const appIcon = element('app-icon')
const appName = element('app-name')
const events = element('events')

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

function renderOverlay(state) {
  overlayState.textContent = `Visible: ${state.active}; presentations: ${state.any}.`
  overlayImages.textContent = state.imageCount > 1
    ? `${state.imageCount} images selected · showing ${state.imageIndex + 1}/${state.imageCount}: ${state.imageName} · changes every 5s.`
    : state.imageCount === 1
      ? `1 image selected: ${state.imageName}.`
      : 'Using the built-in sample.'
}

const unsubscribe = api.onEvent(({ topic, data }) => {
  if (topic === 'overlay') {
    if (data.type === 'image') renderOverlay(data)
    if (data.type === 'visibility') {
      overlayState.textContent = `Visible: ${data.active}; presentations retained: ${data.any}.`
    }
    if (data.type === 'rotationError') {
      setStatus(`Image rotation stopped: ${data.message}`, true)
    }
    log(`Overlay event: ${JSON.stringify(data)}`)
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
    renderOverlay(state)
    log('Overlay shown.')
  }
})

element('pick-overlay-images').addEventListener('click', async (event) => {
  const state = await run(event.currentTarget, api.pickOverlayImages)
  if (state) {
    renderOverlay(state)
    log(`Selected ${state.imageCount} overlay images.`)
  }
})

element('hide-overlay').addEventListener('click', async (event) => {
  const state = await run(event.currentTarget, api.hideOverlay)
  if (state) {
    overlayState.textContent = `Visible: ${state.active}; presentations retained: ${state.any}.`
    log('Overlay hidden.')
  }
})

element('pick-app').addEventListener('click', async (event) => {
  const selected = await run(event.currentTarget, api.pickApp)
  if (selected) {
    renderIcon(selected.icon, selected.name)
    log(`Extracted icon for ${selected.name}.`)
  }
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
      renderOverlay(result.overlay)
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
