/**
 * nativekit — cross-platform native capabilities for Electron.
 * @module nativekit
 *
 * Main-process only. All methods throw if called from a renderer process.
 */

import { EventEmitter } from 'node:events'
import { createRequire } from 'node:module'
import * as path from 'node:path'
import * as fs from 'node:fs'

// ---------------------------------------------------------------------------
// Native addon loading
// ---------------------------------------------------------------------------

const require_ = createRequire(import.meta.url)

function loadNative(): any {
  // 1. prebuilds via node-gyp-build
  try {
    return require_('node-gyp-build')(path.resolve(__dirname, '..'))
  } catch {
    // fall through
  }

  // 2. dev build (cmake-js / node-gyp output)
  const devPaths = [
    path.resolve(__dirname, '..', 'build', 'Release', 'nativekit.node'),
    path.resolve(__dirname, '..', 'build', 'Debug', 'nativekit.node'),
  ]
  for (const p of devPaths) {
    if (fs.existsSync(p)) return require_(p)
  }

  // 3. Electron asar-unpacked fallback
  const asarPath = path.resolve(
    __dirname,
    '..',
    `prebuilds/${process.platform}-${process.arch}/nativekit.node`,
  )
  if (fs.existsSync(asarPath)) return require_(asarPath)

  throw new Error(
    `nativekit: no prebuilt binary for ${process.platform}-${process.arch}. ` +
      'Run `pnpm build` to compile from source.',
  )
}

const native: any = loadNative()

// ---------------------------------------------------------------------------
// Process guard
// ---------------------------------------------------------------------------

function assertMainThread(): void {
  // Electron sets process.type === 'browser' in the main process.
  // In plain Node, process.type is undefined — allow it for testing/dev.
  const proc: any = process as any
  if (proc.type && proc.type !== 'browser') {
    throw new Error(
      'nativekit must be used in the Electron main process, ' +
        `but process.type is "${proc.type}".`,
    )
  }
}

// ---------------------------------------------------------------------------
// Type definitions
// ---------------------------------------------------------------------------

export interface AnchorConfig {
  edge: 'leading' | 'trailing' | 'top' | 'bottom'
  offset: number
}

export interface HostConfig {
  id: string
  title: string
  bounds: { x: number; y: number; width: number; height: number }
  windowHandle: Buffer
  anchor: AnchorConfig
  animated?: boolean
}

export interface ImageFrame {
  presentationId: string
  sessionId: string
  imageData: string
  appIconPath?: string | null
}

export interface OverlayOptions {
  tooltip?: { hide?: string; relocate?: string }
}

export interface SystemWindow {
  id: number
  name: string | null
  bounds: { x: number; y: number; width: number; height: number }
  level: number
  ownerPid: number
  ownerName: string | null
  isOnscreen: boolean
}

export interface FrontmostWindow {
  bundleId: string
  icon: string | null
  name: string
  title: string | null
}

export interface DragConfig {
  files: string[]
  windowHandle: Buffer
  position: { x: number; y: number }
}

// ---------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------

class Overlay extends EventEmitter {
  start(options?: OverlayOptions): boolean {
    assertMainThread()
    return native.overlayStart(options ?? {})
  }

  stop(): boolean {
    assertMainThread()
    return native.overlayStop()
  }

  attachHost(config: HostConfig): boolean {
    assertMainThread()
    requireNonEmptyString(config.id, 'config.id')
    requireBuffer(config.windowHandle, 'config.windowHandle')
    return native.overlayAttachHost(config)
  }

  detachHost(hostId: string): boolean {
    assertMainThread()
    requireNonEmptyString(hostId, 'hostId')
    return native.overlayDetachHost(hostId)
  }

  setVisible(visible: boolean): boolean {
    assertMainThread()
    return native.overlaySetVisible(visible)
  }

  setMaxSize(size: number): boolean {
    assertMainThread()
    requireNonNegative(size, 'size')
    return native.overlaySetMaxSize(size)
  }

  pushImage(frame: ImageFrame): boolean {
    assertMainThread()
    requireNonEmptyString(frame.presentationId, 'frame.presentationId')
    requireNonEmptyString(frame.sessionId, 'frame.sessionId')
    requireNonEmptyString(frame.imageData, 'frame.imageData')
    return native.overlayPushImage(frame)
  }

  removeImage(presentationId: string): boolean {
    assertMainThread()
    requireNonEmptyString(presentationId, 'presentationId')
    return native.overlayRemoveImage(presentationId)
  }

  completeSession(sessionId: string): boolean {
    assertMainThread()
    requireNonEmptyString(sessionId, 'sessionId')
    return native.overlayCompleteSession(sessionId)
  }

  invalidateSession(sessionId: string, presentationId: string): boolean {
    assertMainThread()
    requireNonEmptyString(sessionId, 'sessionId')
    requireNonEmptyString(presentationId, 'presentationId')
    return native.overlayInvalidateSession(sessionId, presentationId)
  }

  suppressSessions(sessionIds: string[]): boolean {
    assertMainThread()
    if (!Array.isArray(sessionIds)) {
      throw new TypeError('sessionIds must be an array')
    }
    return native.overlaySuppressSessions(sessionIds)
  }

  setActiveSession(sessionId: string): boolean {
    assertMainThread()
    requireNonEmptyString(sessionId, 'sessionId')
    return native.overlaySetActiveSession(sessionId)
  }

  hasActive(): boolean {
    assertMainThread()
    return native.overlayHasActive()
  }

  hasAny(): boolean {
    assertMainThread()
    return native.overlayHasAny()
  }
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

class Windows extends EventEmitter {
  frontmost(): Promise<FrontmostWindow | null> {
    assertMainThread()
    return Promise.resolve(native.windowsFrontmost())
  }

  list(options?: { relativeTo?: number }): Promise<SystemWindow[]> {
    assertMainThread()
    return Promise.resolve(native.windowsList(options?.relativeTo ?? 0) ?? [])
  }

  find(id: number): Promise<SystemWindow | null> {
    assertMainThread()
    return Promise.resolve(native.windowsFind(id))
  }

  atPoint(
    point: { x: number; y: number },
    options?: { belowId?: number },
  ): Promise<SystemWindow | null> {
    assertMainThread()
    if (typeof point?.x !== 'number' || typeof point?.y !== 'number') {
      throw new TypeError('point.x and point.y must be numbers')
    }
    return Promise.resolve(native.windowsAtPoint(point, options?.belowId ?? 0))
  }
}

// ---------------------------------------------------------------------------
// SecureChannel
// ---------------------------------------------------------------------------

class SecureChannel extends EventEmitter {
  spawn(executablePath: string): Promise<number | null> {
    assertMainThread()
    requireNonEmptyString(executablePath, 'executablePath')
    return Promise.resolve(native.secureChannelSpawn(executablePath))
  }

  verify(pid: number, executablePath: string): Promise<boolean> {
    assertMainThread()
    requireNonNegative(pid, 'pid')
    requireNonEmptyString(executablePath, 'executablePath')
    return Promise.resolve(native.secureChannelVerify(pid, executablePath))
  }

  wasTerminatedByPrivacy(): boolean {
    assertMainThread()
    return native.secureChannelWasTerminatedByPrivacy()
  }
}

// ---------------------------------------------------------------------------
// Apps
// ---------------------------------------------------------------------------

class Apps {
  icon(
    appPath: string,
    options?: { size?: 'small' | 'medium' },
  ): Promise<string | null> {
    assertMainThread()
    requireNonEmptyString(appPath, 'appPath')
    return Promise.resolve(
      native.appsIcon(appPath, options?.size ?? 'small'),
    )
  }
}

// ---------------------------------------------------------------------------
// Drag
// ---------------------------------------------------------------------------

class Drag extends EventEmitter {
  start(config: DragConfig): Promise<void> {
    assertMainThread()
    if (!Array.isArray(config.files) || config.files.length === 0) {
      throw new TypeError('config.files must be a non-empty array')
    }
    requireBuffer(config.windowHandle, 'config.windowHandle')
    return Promise.resolve(native.dragStart(config))
  }
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

function requireNonEmptyString(value: unknown, name: string): void {
  if (typeof value !== 'string' || value.length === 0) {
    throw new TypeError(`${name} must be a non-empty string`)
  }
}

function requireBuffer(value: unknown, name: string): void {
  if (!Buffer.isBuffer(value)) {
    throw new TypeError(`${name} must be a Buffer (use getNativeWindowHandle())`)
  }
}

function requireNonNegative(value: unknown, name: string): void {
  if (typeof value !== 'number' || value < 0 || !Number.isFinite(value)) {
    throw new TypeError(`${name} must be a non-negative finite number`)
  }
}

// ---------------------------------------------------------------------------
// Wire native events → EventEmitter
// ---------------------------------------------------------------------------

// The native addon calls these registration functions to install callbacks.
// We bridge them into the EventEmitter pattern.
;[
  ['overlayOnMaxSizeChanged', 'overlay', 'maxSizeChanged'] as const,
  ['overlayOnActivate', 'overlay', 'activate'] as const,
  ['overlayOnVisibilityRequest', 'overlay', 'visibilityRequest'] as const,
  ['overlayOnCursor', 'overlay', 'cursor'] as const,
].forEach(([registerFn, _module, event]) => {
  if (typeof native[registerFn] === 'function') {
    native[registerFn]((...args: unknown[]) => {
      overlay.emit(event as string, ...args)
    })
  }
})

;[
  ['secureChannelOnData', 'secureChannel', 'data'] as const,
  ['secureChannelOnExit', 'secureChannel', 'event:exit'] as const,
].forEach(([registerFn, _module, event]) => {
  if (typeof native[registerFn] === 'function') {
    native[registerFn]((...args: unknown[]) => {
      secureChannel.emit(event as string, ...args)
    })
  }
})

;[
  ['dragOnEnded', 'drag', 'ended'] as const,
].forEach(([registerFn, _module, event]) => {
  if (native[registerFn] !== undefined) {
    native[registerFn]((...args: unknown[]) => {
      drag.emit(event as string, ...args)
    })
  }
})

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

export const overlay = new Overlay()
export const windows = new Windows()
export const secureChannel = new SecureChannel()
export const apps = new Apps()
export const drag = new Drag()

export default {
  overlay,
  windows,
  secureChannel,
  apps,
  drag,
}
