import { resolve } from 'node:path'

import { describe, expect, it } from 'vitest'

import {
  apps,
  overlay,
  windows,
  type SystemWindow,
} from '../js/index.js'

function expectWindowShape(window: SystemWindow): void {
  expect(Number.isSafeInteger(window.id)).toBe(true)
  expect(window.id).toBeGreaterThan(0)
  expect(Number.isFinite(window.bounds.x)).toBe(true)
  expect(Number.isFinite(window.bounds.y)).toBe(true)
  expect(window.bounds.width).toBeGreaterThan(0)
  expect(window.bounds.height).toBeGreaterThan(0)
  expect(Number.isSafeInteger(window.ownerPid)).toBe(true)
  expect(typeof window.isOnscreen).toBe('boolean')
}

describe('windows native integration', () => {
  it('enumerates and resolves a window snapshot', async () => {
    const snapshot = await windows.list()
    expect(Array.isArray(snapshot)).toBe(true)
    expect(snapshot.length).toBeGreaterThan(0)
    expectWindowShape(snapshot[0])

    let resolved = null
    for (const candidate of snapshot.slice(0, 20)) {
      resolved = await windows.find(candidate.id)
      if (resolved !== null) break
    }
    expect(resolved).not.toBeNull()
  })

  it('returns a well-formed frontmost application when available', async () => {
    const frontmost = await windows.frontmost()
    if (frontmost === null) return
    expect(frontmost.name.length).toBeGreaterThan(0)
    expect(typeof frontmost.bundleId).toBe('string')
    expect(frontmost.icon === null || frontmost.icon.startsWith('data:image/png;base64,'))
      .toBe(true)
  })
})

describe('JavaScript boundary validation', () => {
  it('rejects malformed overlay input before crossing N-API', () => {
    expect(() =>
      overlay.pushImage({
        presentationId: 'presentation',
        sessionId: 'session',
        imageData: 'not-an-image',
      }),
    ).toThrow('PNG or JPEG data URL')
  })

  it('rejects relative application paths', () => {
    expect(() => apps.icon('Safari.app')).toThrow('absolute path')
  })

  it('rejects malformed window query options', () => {
    expect(() => windows.list(null as never)).toThrow('options must be an object')
    expect(() =>
      windows.atPoint({ x: 0, y: 0 }, null as never),
    ).toThrow('options must be an object')
  })
})

describe('overlay lifecycle integration', () => {
  it('starts, reports empty state, emits size changes, and stops idempotently', async () => {
    expect(overlay.start()).toBe(true)
    expect(overlay.hasAny()).toBe(false)
    expect(overlay.hasActive()).toBe(false)

    const changed = new Promise<number>((resolvePromise) => {
      overlay.once('maxSizeChanged', resolvePromise)
    })
    expect(overlay.setMaxSize(360)).toBe(true)
    await expect(changed).resolves.toBe(360)

    expect(overlay.stop()).toBe(true)
    expect(overlay.stop()).toBe(true)
  })
})

describe('application icon integration', () => {
  it('extracts small and medium PNG data URLs', async () => {
    const small = await apps.icon(process.execPath, { size: 'small' })
    const medium = await apps.icon(process.execPath, { size: 'medium' })

    expect(pngSize(small)).toEqual([16, 16])
    expect(pngSize(medium)).toEqual([32, 32])
  })

  it('returns null for a missing application', async () => {
    await expect(
      apps.icon(resolve(import.meta.dirname, 'fixtures/missing.app')),
    ).resolves.toBeNull()
  })
})

function pngSize(dataUrl: string | null): [number, number] | null {
  if (dataUrl === null) return null
  const png = Buffer.from(dataUrl.split(',', 2)[1], 'base64')
  expect(png.subarray(0, 8)).toEqual(
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
  )
  return [png.readUInt32BE(16), png.readUInt32BE(20)]
}
