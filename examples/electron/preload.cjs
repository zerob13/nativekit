const { contextBridge, ipcRenderer } = require('electron')

const invoke = (channel, payload) => ipcRenderer.invoke(channel, payload)

contextBridge.exposeInMainWorld(
  'nativekitDemo',
  Object.freeze({
    status: () => invoke('nativekit:status'),
    refreshWindows: () => invoke('nativekit:windows:refresh'),
    showOverlay: () => invoke('nativekit:overlay:show'),
    hideOverlay: () => invoke('nativekit:overlay:hide'),
    startWorker: () => invoke('nativekit:worker:start'),
    stopWorker: () => invoke('nativekit:worker:stop'),
    pickApp: () => invoke('nativekit:apps:pick'),
    pickDragFile: () => invoke('nativekit:drag:pick'),
    startDrag: (position) => invoke('nativekit:drag:start', position),
    runSmoke: () => invoke('nativekit:smoke:run'),
    runSmokeDrag: (position) =>
      invoke('nativekit:smoke:drag', position),
    completeSmoke: (result) =>
      invoke('nativekit:smoke:complete', result),
    onEvent: (callback) => {
      const listener = (_event, message) => callback(message)
      ipcRenderer.on('nativekit:event', listener)
      return () => ipcRenderer.removeListener('nativekit:event', listener)
    },
  }),
)
