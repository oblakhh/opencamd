// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Unit tests for the multipart/MJPEG parser (utils/mjpeg.ts).

import { describe, it, expect } from 'vitest'
import { MultipartParser, parseBoundary, isMultipartType, indexOfBytes } from '../src/utils/mjpeg'

const enc = new TextEncoder()

// Build a camd-style part exactly as firmware/camd/http.c writes it.
function part(body: Uint8Array, opts: { length?: boolean; type?: string; boundary?: string } = {}): Uint8Array {
  const { length = true, type = 'image/jpeg', boundary = 'camdframe' } = opts
  const hdr =
    `--${boundary}\r\nContent-Type: ${type}\r\n` +
    (length ? `Content-Length: ${body.length}\r\n` : '') +
    `\r\n`
  return concat(enc.encode(hdr), body, enc.encode('\r\n'))
}

function concat(...arrs: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(arrs.reduce((n, a) => n + a.length, 0))
  let o = 0
  for (const a of arrs) { out.set(a, o); o += a.length }
  return out
}

// A fake "JPEG" with SOI/EOI markers and bytes that contain CRLFs and even a
// fake boundary-looking run, to prove Content-Length parsing is exact.
function fakeJpeg(seed: number, size = 300): Uint8Array {
  const b = new Uint8Array(size)
  b[0] = 0xff; b[1] = 0xd8
  for (let i = 2; i < size - 2; i++) b[i] = (i * 31 + seed) & 0xff
  b.set(enc.encode('\r\n--camd'), 40)
  b[size - 2] = 0xff; b[size - 1] = 0xd9
  return b
}

function feed(parser: MultipartParser, stream: Uint8Array, chunk: number) {
  const out = []
  for (let i = 0; i < stream.length; i += chunk) out.push(...parser.push(stream.subarray(i, i + chunk)))
  return out
}

describe('parseBoundary', () => {
  it('extracts the boundary parameter', () => {
    expect(parseBoundary('multipart/x-mixed-replace; boundary=camdframe')).toBe('camdframe')
    expect(parseBoundary('multipart/x-mixed-replace;boundary="abc-123"')).toBe('abc-123')
    expect(parseBoundary('multipart/x-mixed-replace; boundary=x; charset=utf-8')).toBe('x')
  })
  it('falls back to camdframe', () => {
    expect(parseBoundary(null)).toBe('camdframe')
    expect(parseBoundary('image/jpeg')).toBe('camdframe')
  })
  it('recognises the multipart media type', () => {
    expect(isMultipartType('multipart/x-mixed-replace; boundary=camdframe')).toBe(true)
    expect(isMultipartType('image/jpeg')).toBe(false)
    expect(isMultipartType(null)).toBe(false)
  })
})

describe('indexOfBytes', () => {
  it('finds needles and respects the start offset', () => {
    const hay = enc.encode('xx--b--b')
    const needle = enc.encode('--b')
    expect(indexOfBytes(hay, needle)).toBe(2)
    expect(indexOfBytes(hay, needle, 3)).toBe(5)
    expect(indexOfBytes(hay, enc.encode('zz'))).toBe(-1)
  })
})

describe('MultipartParser', () => {
  const frames = [fakeJpeg(1, 300), fakeJpeg(2, 1200), fakeJpeg(3, 77)]
  const stream = concat(...frames.map((f) => part(f)))

  it('parses a whole stream delivered in one chunk', () => {
    const parts = new MultipartParser('camdframe').push(stream)
    expect(parts).toHaveLength(3)
    parts.forEach((p, i) => {
      expect(p.type).toBe('image/jpeg')
      expect(p.data).toEqual(frames[i])
    })
  })

  it.each([1, 7, 64, 256, 1000, 5000])('is chunking-agnostic (chunk=%i)', (chunk) => {
    const parts = feed(new MultipartParser(), stream, chunk)
    expect(parts.map((p) => p.data)).toEqual(frames)
  })

  it('returns copies, not views into its internal buffer', () => {
    const parser = new MultipartParser()
    const [p] = parser.push(stream)
    const snapshot = p!.data.slice()
    parser.push(stream) // mutates the internal buffer
    expect(p!.data).toEqual(snapshot)
  })

  it('does not hold onto bytes once a part has been emitted', () => {
    const parser = new MultipartParser()
    parser.push(stream)
    expect(parser.pending).toBe(0)
  })

  it('ignores preamble junk before the first boundary', () => {
    const junk = enc.encode('\r\nsome preamble --camd that is not a boundary\r\n')
    const parts = feed(new MultipartParser(), concat(junk, stream), 13)
    expect(parts.map((p) => p.data)).toEqual(frames)
  })

  it('falls back to boundary scanning when Content-Length is absent', () => {
    const noLen = concat(...frames.map((f) => part(f, { length: false })))
    for (const chunk of [1, 50, 10000]) {
      const parts = feed(new MultipartParser(), noLen, chunk)
      // The last part can only be emitted once the next boundary arrives.
      expect(parts.map((p) => p.data)).toEqual(frames.slice(0, 2))
    }
  })

  it('accepts bare-LF header terminators', () => {
    const body = fakeJpeg(9, 100)
    const lf = concat(enc.encode(`--camdframe\nContent-Length: ${body.length}\n\n`), body, enc.encode('\n'))
    const parts = feed(new MultipartParser(), concat(lf, lf), 3)
    expect(parts.map((p) => p.data)).toEqual([body, body])
  })

  it('honours a custom boundary and per-part content type', () => {
    const svg = enc.encode('<svg/>')
    const p = part(svg, { boundary: 'zz', type: 'image/svg+xml' })
    const parts = new MultipartParser('zz').push(p)
    expect(parts).toHaveLength(1)
    expect(parts[0]!.type).toBe('image/svg+xml')
    expect(parts[0]!.data).toEqual(svg)
  })

  it('stops at the closing boundary marker', () => {
    const parser = new MultipartParser()
    const parts = parser.push(concat(stream, enc.encode('--camdframe--\r\n')))
    expect(parts).toHaveLength(3)
    expect(parser.done).toBe(true)
    expect(parser.push(stream)).toHaveLength(0)
  })

  it('waits for a partial frame rather than emitting it early', () => {
    const parser = new MultipartParser()
    const whole = part(frames[1]!)
    expect(parser.push(whole.subarray(0, whole.length - 10))).toHaveLength(0)
    const rest = parser.push(whole.subarray(whole.length - 10))
    expect(rest).toHaveLength(1)
    expect(rest[0]!.data).toEqual(frames[1])
  })
})
