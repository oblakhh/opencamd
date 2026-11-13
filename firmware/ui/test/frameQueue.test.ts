// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Unit tests for LatestFrameQueue (utils/frameQueue.ts).

import { describe, it, expect } from 'vitest'
import { LatestFrameQueue } from '../src/utils/frameQueue'

// A decode step we can release by hand, to control interleaving.
function gate() {
  const waiting: Array<{ item: number; resolve: (v: string) => void; reject: (e: Error) => void }> = []
  const decode = (item: number) =>
    new Promise<string>((resolve, reject) => waiting.push({ item, resolve, reject }))
  const release = () => {
    const w = waiting.shift()!
    w.resolve(`f${w.item}`)
  }
  const fail = () => waiting.shift()!.reject(new Error('bad frame'))
  return { decode, release, fail, waiting }
}
const tick = () => new Promise((r) => setTimeout(r, 0))

describe('LatestFrameQueue', () => {
  it('presents frames in order when decode keeps up', async () => {
    const shown: string[] = []
    const g = gate()
    const q = new LatestFrameQueue(g.decode, (s) => shown.push(s))
    q.push(1); g.release(); await tick()
    q.push(2); g.release(); await tick()
    expect(shown).toEqual(['f1', 'f2'])
    expect(q.pending).toBe(false)
  })

  it('drops intermediate frames and decodes only the newest while busy', async () => {
    const shown: string[] = []
    const g = gate()
    const q = new LatestFrameQueue(g.decode, (s) => shown.push(s))
    q.push(1)
    q.push(2)
    q.push(3)
    q.push(4)
    expect(g.waiting.map((w) => w.item)).toEqual([1]) // only one decode in flight
    g.release(); await tick()
    expect(g.waiting.map((w) => w.item)).toEqual([4]) // 2 and 3 were skipped
    g.release(); await tick()
    expect(shown).toEqual(['f1', 'f4'])
  })

  it('discards results that finish after a reset', async () => {
    const shown: string[] = []
    const dropped: string[] = []
    const g = gate()
    const q = new LatestFrameQueue(g.decode, (s) => shown.push(s), (s) => dropped.push(s))
    q.push(1)
    q.push(2)
    q.reset()
    g.release(); await tick()
    expect(shown).toEqual([])
    expect(dropped).toEqual(['f1'])
    expect(g.waiting).toHaveLength(0) // the queued frame was dropped too
    expect(q.pending).toBe(false)
  })

  it('skips an undecodable frame and carries on', async () => {
    const shown: string[] = []
    const g = gate()
    const q = new LatestFrameQueue(g.decode, (s) => shown.push(s))
    q.push(1)
    q.push(2)
    g.fail(); await tick()
    g.release(); await tick()
    expect(shown).toEqual(['f2'])
    q.push(3); g.release(); await tick()
    expect(shown).toEqual(['f2', 'f3'])
  })
})
