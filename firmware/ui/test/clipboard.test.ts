// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Unit tests for the copy-to-clipboard helper (utils/clipboard.ts).
// Vitest runs in plain node here, so the DOM bits are minimal fakes.

import { describe, it, expect, vi, afterEach } from 'vitest'
import { copyText, selectText } from '../src/utils/clipboard'

interface FakeTextarea {
  value: string
  style: { cssText: string }
  contentEditable: string
  attrs: Record<string, string>
  setAttribute(k: string, v: string): void
  focus(): void
  select(): void
  setSelectionRange(a: number, b: number): void
  remove(): void
  selected: [number, number] | null
}

function fakeDocument(execResult: boolean | 'throw') {
  const created: FakeTextarea[] = []
  const attached = new Set<FakeTextarea>()
  const selection = { ranges: [] as unknown[], get rangeCount() { return this.ranges.length },
    getRangeAt(i: number) { return this.ranges[i] }, removeAllRanges() { this.ranges = [] }, addRange(r: unknown) { this.ranges.push(r) } }
  const prevFocus = { focus: vi.fn() }
  const doc = {
    activeElement: prevFocus,
    body: { appendChild: (el: FakeTextarea) => attached.add(el) },
    createElement: () => {
      const ta: FakeTextarea = {
        value: '', style: { cssText: '' }, contentEditable: 'inherit', attrs: {}, selected: null,
        setAttribute(k, v) { this.attrs[k] = v },
        focus() {}, select() {},
        setSelectionRange(a, b) { this.selected = [a, b] },
        remove() { attached.delete(this) },
      }
      created.push(ta)
      return ta
    },
    createRange: () => ({ node: null as unknown, selectNodeContents(n: unknown) { this.node = n } }),
    getSelection: () => selection,
    execCommand: vi.fn((cmd: string) => {
      if (execResult === 'throw') throw new Error('nope')
      // Only "copies" if the textarea is in the DOM and fully selected.
      const ta = created.at(-1)!
      return cmd === 'copy' && execResult && attached.has(ta) && ta.selected?.[1] === ta.value.length
    }),
  }
  return { doc, created, attached, selection, prevFocus }
}

afterEach(() => vi.unstubAllGlobals())

describe('copyText', () => {
  it('uses navigator.clipboard in a secure context', async () => {
    const writeText = vi.fn().mockResolvedValue(undefined)
    const f = fakeDocument(true)
    vi.stubGlobal('isSecureContext', true)
    vi.stubGlobal('navigator', { clipboard: { writeText } })
    vi.stubGlobal('document', f.doc)
    expect(await copyText('http://cam/stream.mjpg')).toBe(true)
    expect(writeText).toHaveBeenCalledWith('http://cam/stream.mjpg')
    expect(f.doc.execCommand).not.toHaveBeenCalled()
  })

  it('falls back to execCommand on a plain-http origin (no navigator.clipboard)', async () => {
    const f = fakeDocument(true)
    const prior = { id: 'user-selection' }
    f.selection.addRange(prior)
    vi.stubGlobal('isSecureContext', false)
    vi.stubGlobal('navigator', {})
    vi.stubGlobal('document', f.doc)
    expect(await copyText('http://192.168.1.50/snapshot.jpg')).toBe(true)
    expect(f.doc.execCommand).toHaveBeenCalledWith('copy')
    const ta = f.created[0]
    expect(ta.value).toBe('http://192.168.1.50/snapshot.jpg')
    expect(ta.attrs.readonly).toBe('')
    expect(f.attached.size).toBe(0) // cleaned up
    expect(f.selection.ranges).toEqual([prior]) // user selection restored
    expect(f.prevFocus.focus).toHaveBeenCalled()
  })

  it('falls back when writeText rejects (e.g. permission denied)', async () => {
    const f = fakeDocument(true)
    vi.stubGlobal('isSecureContext', true)
    vi.stubGlobal('navigator', { clipboard: { writeText: vi.fn().mockRejectedValue(new Error('denied')) } })
    vi.stubGlobal('document', f.doc)
    expect(await copyText('x')).toBe(true)
    expect(f.doc.execCommand).toHaveBeenCalled()
  })

  it('reports failure when no method works', async () => {
    for (const r of [false, 'throw'] as const) {
      const f = fakeDocument(r)
      vi.stubGlobal('isSecureContext', false)
      vi.stubGlobal('navigator', {})
      vi.stubGlobal('document', f.doc)
      expect(await copyText('x')).toBe(false)
      expect(f.attached.size).toBe(0)
    }
  })
})

describe('selectText', () => {
  it('selects the element contents for a manual copy', () => {
    const f = fakeDocument(false)
    vi.stubGlobal('document', f.doc)
    const el = { tag: 'a' }
    selectText(el as unknown as Element)
    expect(f.selection.ranges).toHaveLength(1)
    expect((f.selection.ranges[0] as { node: unknown }).node).toBe(el)
  })
})
