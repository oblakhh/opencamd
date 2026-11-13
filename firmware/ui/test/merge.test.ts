// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Unit tests for the partial-merge helpers (utils/merge.ts).

import { describe, it, expect } from 'vitest'
import { mergePatch, deepClone } from '../src/utils/merge'

describe('mergePatch', () => {
  it('merges nested objects recursively', () => {
    const base = { video: { width: 1280, height: 960, quality: 80 } }
    const out = mergePatch(base, { video: { quality: 50 } })
    expect(out).toEqual({ video: { width: 1280, height: 960, quality: 50 } })
  })

  it('replaces arrays wholesale instead of merging them', () => {
    const base = { uploads: [{ name: 'a' }, { name: 'b' }] }
    const out = mergePatch(base, { uploads: [{ name: 'c' }] })
    expect(out.uploads).toEqual([{ name: 'c' }])
  })

  it('does not mutate the base object', () => {
    const base = { net: { mode: 'dhcp', ip: '' } }
    const out = mergePatch(base, { net: { mode: 'static' } })
    expect(base.net.mode).toBe('dhcp')
    expect(out.net.mode).toBe('static')
  })

  it('adds new keys from the patch', () => {
    const out = mergePatch({ a: 1 } as Record<string, unknown>, { b: 2 })
    expect(out).toEqual({ a: 1, b: 2 })
  })

  it('deepClone produces an independent copy', () => {
    const src = { x: { y: [1, 2] } }
    const copy = deepClone(src)
    copy.x.y.push(3)
    expect(src.x.y).toEqual([1, 2])
  })
})
