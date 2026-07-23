/** Cross-platform native capabilities for the Electron main process. */

import { EventEmitter } from 'node:events'
import { createRequire } from 'node:module'
import { dirname, isAbsolute, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { isMainThread } from 'node:worker_threads'

export interface Point {
  x: number
  y: number
}

export interface Rect extends Point {
  width: number
  height: number
}

export interface AnchorConfig {
  edge: 'leading' | 'trailing' | 'top' | 'bottom'
  offset: number
}

export interface HostConfig {
  id: string
  title: string
  bounds: Rect
  windowHandle: Buffer
  anchor: AnchorConfig
}

export interface ImageFrame {
  /** Required when more than one host is attached. */
  hostId?: string
  presentationId: string
  sessionId: string
  imageData: string
  appIconPath?: string | null
}

export interface OverlayOptions {
  tooltip?: {
    hide?: string
    relocate?: string
  }
}

export interface SystemWindow {
  id: number
  name: string | null
  bounds: Rect
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

interface NativeBinding {
  overlayStart(options: OverlayOptions): boolean
  overlayStop(): boolean
  overlayAttachHost(config: HostConfig): boolean
  overlayDetachHost(hostId: string): boolean
  overlaySetVisible(visible: boolean): boolean
  overlaySetMaxSize(size: number): boolean
  overlayPushImage(frame: ImageFrame): boolean
  overlayRemoveImage(presentationId: string): boolean
  overlayCompleteSession(sessionId: string): boolean
  overlayInvalidateSession(sessionId: string, presentationId: string): boolean
  overlaySuppressSessions(sessionIds: string[]): boolean
  overlaySetActiveSession(sessionId: string): boolean
  overlayHasActive(): boolean
  overlayHasAny(): boolean
  overlayOnMaxSizeChanged?(callback: (size: number) => void): void
  overlayOnActivate?(callback: () => void): void
  overlayOnVisibilityRequest?(callback: (visible: boolean) => void): void

  windowsFrontmost(): FrontmostWindow | null
  windowsList(relativeTo: number): SystemWindow[]
  windowsFind(id: number): SystemWindow | null
  windowsAtPoint(point: Point, belowId: number): SystemWindow | null

  appsIcon(appPath: string, size: 'small' | 'medium'): string | null

}

interface ElectronScreen {
  dipToScreenPoint(point: Point): Point
  screenToDipRect(window: null, rect: Rect): Rect
}

const modulePath =
  typeof import.meta.url === 'string'
    ? fileURLToPath(import.meta.url)
    : __filename
const require = createRequire(modulePath)
const packageRoot = resolve(dirname(modulePath), '..')

function loadNative(): NativeBinding {
  try {
    const nodeGypBuild = require('node-gyp-build') as (
      directory: string,
    ) => NativeBinding
    return nodeGypBuild(packageRoot)
  } catch (cause) {
    throw new Error(
      `nativekit: no native binary for ${process.platform}-${process.arch}. ` +
        'Install a published prebuild or run `pnpm build:native`.',
      { cause },
    )
  }
}

function assertMainProcess(): void {
  const electronProcess = process as NodeJS.Process & { type?: string }
  if (!process.versions.electron) return
  const electron = require('electron') as { BrowserWindow?: unknown }
  if (
    electronProcess.type !== 'browser' ||
    !isMainThread ||
    typeof electron.BrowserWindow !== 'function'
  ) {
    throw new Error(
      'nativekit must run in the Electron main process; expose only the ' +
        'operations you need to renderers through a context-isolated preload.',
    )
  }
}

assertMainProcess()
const native = loadNative()

class Overlay extends EventEmitter {
  start(options: OverlayOptions = {}): boolean {
    assertMainProcess()
    validateOverlayOptions(options)
    return native.overlayStart(options)
  }

  stop(): boolean {
    assertMainProcess()
    return native.overlayStop()
  }

  attachHost(config: HostConfig): boolean {
    assertMainProcess()
    requireRecord(config, 'config')
    requireNonEmptyString(config.id, 'config.id')
    requireNonEmptyString(config.title, 'config.title')
    requireRect(config.bounds, 'config.bounds')
    requireBuffer(config.windowHandle, 'config.windowHandle')
    requireRecord(config.anchor, 'config.anchor')
    if (!['leading', 'trailing', 'top', 'bottom'].includes(config.anchor.edge)) {
      throw new TypeError(
        'config.anchor.edge must be leading, trailing, top, or bottom',
      )
    }
    requireNonNegative(config.anchor.offset, 'config.anchor.offset')
    return native.overlayAttachHost(config)
  }

  detachHost(hostId: string): boolean {
    assertMainProcess()
    requireNonEmptyString(hostId, 'hostId')
    return native.overlayDetachHost(hostId)
  }

  setVisible(visible: boolean): boolean {
    assertMainProcess()
    requireBoolean(visible, 'visible')
    return native.overlaySetVisible(visible)
  }

  setMaxSize(size: number): boolean {
    assertMainProcess()
    requirePositive(size, 'size')
    return native.overlaySetMaxSize(size)
  }

  pushImage(frame: ImageFrame): boolean {
    assertMainProcess()
    requireRecord(frame, 'frame')
    requireNonEmptyString(frame.presentationId, 'frame.presentationId')
    requireNonEmptyString(frame.sessionId, 'frame.sessionId')
    requireNonEmptyString(frame.imageData, 'frame.imageData')
    if (frame.imageData.length > 32 * 1024 * 1024) {
      throw new RangeError('frame.imageData exceeds the 32 MiB limit')
    }
    if (!/^data:image\/(?:png|jpe?g);base64,/i.test(frame.imageData)) {
      throw new TypeError('frame.imageData must be a PNG or JPEG data URL')
    }
    if (frame.appIconPath !== undefined && frame.appIconPath !== null) {
      requireAbsolutePath(frame.appIconPath, 'frame.appIconPath')
    }
    if (frame.hostId !== undefined) {
      requireNonEmptyString(frame.hostId, 'frame.hostId')
    }
    return native.overlayPushImage(frame)
  }

  removeImage(presentationId: string): boolean {
    assertMainProcess()
    requireNonEmptyString(presentationId, 'presentationId')
    return native.overlayRemoveImage(presentationId)
  }

  completeSession(sessionId: string): boolean {
    assertMainProcess()
    requireNonEmptyString(sessionId, 'sessionId')
    return native.overlayCompleteSession(sessionId)
  }

  invalidateSession(sessionId: string, presentationId: string): boolean {
    assertMainProcess()
    requireNonEmptyString(sessionId, 'sessionId')
    requireNonEmptyString(presentationId, 'presentationId')
    return native.overlayInvalidateSession(sessionId, presentationId)
  }

  suppressSessions(sessionIds: string[]): boolean {
    assertMainProcess()
    if (!Array.isArray(sessionIds)) {
      throw new TypeError('sessionIds must be an array')
    }
    sessionIds.forEach((id, index) =>
      requireNonEmptyString(id, `sessionIds[${index}]`),
    )
    return native.overlaySuppressSessions(sessionIds)
  }

  setActiveSession(sessionId: string): boolean {
    assertMainProcess()
    requireNonEmptyString(sessionId, 'sessionId')
    return native.overlaySetActiveSession(sessionId)
  }

  hasActive(): boolean {
    assertMainProcess()
    return native.overlayHasActive()
  }

  hasAny(): boolean {
    assertMainProcess()
    return native.overlayHasAny()
  }
}

class Windows {
  frontmost(): Promise<FrontmostWindow | null> {
    assertMainProcess()
    return Promise.resolve(native.windowsFrontmost())
  }

  list(options: { relativeTo?: number } = {}): Promise<SystemWindow[]> {
    assertMainProcess()
    requireRecord(options, 'options')
    if (options.relativeTo !== undefined) {
      requireWindowId(options.relativeTo, 'options.relativeTo')
    }
    return Promise.resolve(
      native
        .windowsList(options.relativeTo ?? 0)
        .map(windowFromNativeCoordinates),
    )
  }

  find(id: number): Promise<SystemWindow | null> {
    assertMainProcess()
    requireWindowId(id, 'id')
    return Promise.resolve(
      nullableWindowFromNativeCoordinates(native.windowsFind(id)),
    )
  }

  atPoint(
    point: Point,
    options: { belowId?: number } = {},
  ): Promise<SystemWindow | null> {
    assertMainProcess()
    requirePoint(point, 'point')
    requireRecord(options, 'options')
    if (options.belowId !== undefined) {
      requireWindowId(options.belowId, 'options.belowId')
    }
    return Promise.resolve(
      nullableWindowFromNativeCoordinates(
        native.windowsAtPoint(
          pointToNativeScreenCoordinates(point),
          options.belowId ?? 0,
        ),
      ),
    )
  }
}

class Apps {
  icon(
    appPath: string,
    options: { size?: 'small' | 'medium' } = {},
  ): Promise<string | null> {
    assertMainProcess()
    requireAbsolutePath(appPath, 'appPath')
    requireRecord(options, 'options')
    const size = options.size ?? 'small'
    if (size !== 'small' && size !== 'medium') {
      throw new TypeError('options.size must be small or medium')
    }
    return Promise.resolve(native.appsIcon(appPath, size))
  }
}

export const overlay = new Overlay()
export const windows = new Windows()
export const apps = new Apps()

native.overlayOnMaxSizeChanged?.((size) =>
  overlay.emit('maxSizeChanged', size),
)
native.overlayOnActivate?.(() => overlay.emit('activate'))
native.overlayOnVisibilityRequest?.((visible) =>
  overlay.emit('visibilityRequest', visible),
)

export default { overlay, windows, apps }

function electronScreen(): ElectronScreen | null {
  if (process.platform !== 'win32' || !process.versions.electron) return null
  const electron = require('electron') as { screen?: ElectronScreen }
  return electron.screen ?? null
}

function pointToNativeScreenCoordinates(point: Point): Point {
  return electronScreen()?.dipToScreenPoint(point) ?? point
}

function windowFromNativeCoordinates(window: SystemWindow): SystemWindow {
  const screen = electronScreen()
  return screen === null
    ? window
    : { ...window, bounds: screen.screenToDipRect(null, window.bounds) }
}

function nullableWindowFromNativeCoordinates(
  window: SystemWindow | null,
): SystemWindow | null {
  return window === null ? null : windowFromNativeCoordinates(window)
}

function validateOverlayOptions(options: OverlayOptions): void {
  requireRecord(options, 'options')
  if (options.tooltip === undefined) return
  requireRecord(options.tooltip, 'options.tooltip')
  if (options.tooltip.hide !== undefined) {
    requireNonEmptyString(options.tooltip.hide, 'options.tooltip.hide')
  }
  if (options.tooltip.relocate !== undefined) {
    requireNonEmptyString(
      options.tooltip.relocate,
      'options.tooltip.relocate',
    )
  }
}

function requireRecord(
  value: unknown,
  name: string,
): asserts value is Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new TypeError(`${name} must be an object`)
  }
}

function requireNonEmptyString(
  value: unknown,
  name: string,
): asserts value is string {
  if (typeof value !== 'string' || value.length === 0) {
    throw new TypeError(`${name} must be a non-empty string`)
  }
}

function requireAbsolutePath(
  value: unknown,
  name: string,
): asserts value is string {
  requireNonEmptyString(value, name)
  if (!isAbsolute(value)) {
    throw new TypeError(`${name} must be an absolute path`)
  }
}

function requireBuffer(value: unknown, name: string): asserts value is Buffer {
  const isBuffer = Buffer.isBuffer(value)
  const byteLength = isBuffer ? value.byteLength : 0
  const expectedByteLength = process.platform === 'linux' ? 4 : 8
  if (!isBuffer || byteLength !== expectedByteLength) {
    throw new TypeError(
      `${name} must be the Buffer returned by getNativeWindowHandle()`,
    )
  }
}

function requireBoolean(
  value: unknown,
  name: string,
): asserts value is boolean {
  if (typeof value !== 'boolean') {
    throw new TypeError(`${name} must be a boolean`)
  }
}

function requireFiniteNumber(
  value: unknown,
  name: string,
): asserts value is number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`${name} must be a finite number`)
  }
}

function requireNonNegative(value: unknown, name: string): void {
  requireFiniteNumber(value, name)
  if (value < 0) throw new TypeError(`${name} must be non-negative`)
}

function requirePositive(value: unknown, name: string): void {
  requireFiniteNumber(value, name)
  if (value <= 0) throw new TypeError(`${name} must be positive`)
}

function requirePositiveInteger(value: unknown, name: string): void {
  requireFiniteNumber(value, name)
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new TypeError(`${name} must be a positive safe integer`)
  }
}

function requireWindowId(value: unknown, name: string): void {
  requirePositiveInteger(value, name)
}

function requirePoint(value: unknown, name: string): asserts value is Point {
  requireRecord(value, name)
  requireFiniteNumber(value.x, `${name}.x`)
  requireFiniteNumber(value.y, `${name}.y`)
}

function requireRect(value: unknown, name: string): asserts value is Rect {
  requireRecord(value, name)
  requireFiniteNumber(value.x, `${name}.x`)
  requireFiniteNumber(value.y, `${name}.y`)
  requirePositive(value.width, `${name}.width`)
  requirePositive(value.height, `${name}.height`)
}
