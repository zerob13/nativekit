import { execFileSync } from 'node:child_process'
import { resolve } from 'node:path'
import { pathToFileURL } from 'node:url'

import { describe, expect, it } from 'vitest'

const root = resolve(import.meta.dirname, '..')

describe('published entry points', () => {
  it('loads the ESM bundle', () => {
    const entry = pathToFileURL(resolve(root, 'dist/index.js')).href
    const output = execFileSync(
      process.execPath,
      [
        '--input-type=module',
        '--eval',
        `const value = await import(${JSON.stringify(entry)}); process.stdout.write(Object.keys(value.default).sort().join(','))`,
      ],
      { encoding: 'utf8' },
    )
    expect(output).toBe('apps,overlay,windows')
  })

  it('loads the CommonJS bundle', () => {
    const entry = resolve(root, 'dist/index.cjs')
    const output = execFileSync(
      process.execPath,
      [
        '--eval',
        `const value = require(${JSON.stringify(entry)}); process.stdout.write(Object.keys(value.default).sort().join(','))`,
      ],
      { encoding: 'utf8' },
    )
    expect(output).toBe('apps,overlay,windows')
  })

  it('rejects an Electron runtime without BrowserWindow', () => {
    const entry = resolve(root, 'dist/index.cjs')
    expect(() =>
      execFileSync(
        process.execPath,
        [
          '--eval',
          `Object.defineProperty(process.versions, 'electron', { value: '28.0.0' }); process.type = 'browser'; require(${JSON.stringify(entry)})`,
        ],
        {
          encoding: 'utf8',
          stdio: 'pipe',
          env: { ...process.env, ELECTRON_OVERRIDE_DIST_PATH: root },
        },
      ),
    ).toThrow('nativekit must run in the Electron main process')
  })
})
