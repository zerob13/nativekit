const { contextBridge, ipcRenderer } = require('electron')

const invoke = (channel, payload) => ipcRenderer.invoke(channel, payload)

contextBridge.exposeInMainWorld(
  'nativekitDemo',
  Object.freeze({
    status: () => invoke('nativekit:status'),
    refreshWindows: () => invoke('nativekit:windows:refresh'),
    showOverlay: () => invoke('nativekit:overlay:show'),
    pickOverlayImages: () => invoke('nativekit:overlay:pick'),
    hideOverlay: () => invoke('nativekit:overlay:hide'),
    pickApp: () => invoke('nativekit:apps:pick'),
    runSmoke: () => invoke('nativekit:smoke:run'),
    completeSmoke: (result) =>
      invoke('nativekit:smoke:complete', result),
    onEvent: (callback) => {
      const listener = (_event, message) => callback(message)
      ipcRenderer.on('nativekit:event', listener)
      return () => ipcRenderer.removeListener('nativekit:event', listener)
    },
  }),
)
