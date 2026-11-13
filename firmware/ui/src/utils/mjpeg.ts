// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
//
// Incremental parser for `multipart/x-mixed-replace` (MJPEG) bodies.
//
// Pure logic, no DOM: feed it the chunks a fetch() body reader hands you and
// it returns completed parts. camd emits each frame as
//
//   --camdframe\r\n
//   Content-Type: image/jpeg\r\n
//   Content-Length: <n>\r\n
//   \r\n
//   <n bytes of JPEG>\r\n
//
// The parser honours Content-Length when present (fast path, exact) and
// otherwise scans for the next boundary (slow path, for servers that omit it).
// It tolerates arbitrary chunking, preamble junk before the first boundary,
// `\n\n` as well as `\r\n\r\n` header terminators, and the closing
// `--boundary--` marker.

export interface MultipartPart {
  /** Lower-cased media type from the part's Content-Type (default image/jpeg). */
  type: string
  /** The part body. A fresh copy — safe to hand to a Blob or keep around. */
  data: Uint8Array
}

const DEFAULT_BOUNDARY = 'camdframe'
// Give up on a part body longer than this: it means we lost sync. Real frames
// from this sensor are well under 1 MB even at 1280×960 q95.
const MAX_PART = 8 * 1024 * 1024

/** Pull the `boundary=` parameter out of a Content-Type header value. */
export function parseBoundary(contentType: string | null | undefined): string {
  if (!contentType) return DEFAULT_BOUNDARY
  const m = /boundary\s*=\s*"?([^";,\s]+)"?/i.exec(contentType)
  return m?.[1] ?? DEFAULT_BOUNDARY
}

export function isMultipartType(contentType: string | null | undefined): boolean {
  return /^multipart\/x-mixed-replace/i.test(contentType ?? '')
}

function encodeAscii(s: string): Uint8Array {
  const out = new Uint8Array(s.length)
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff
  return out
}

function decodeLatin1(bytes: Uint8Array): string {
  let s = ''
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]!)
  return s
}

/** indexOf for byte arrays. */
export function indexOfBytes(hay: Uint8Array, needle: Uint8Array, from = 0): number {
  const n = needle.length
  if (n === 0) return from
  const first = needle[0]!
  const last = hay.length - n
  outer: for (let i = Math.max(0, from); i <= last; i++) {
    if (hay[i] !== first) continue
    for (let j = 1; j < n; j++) if (hay[i + j] !== needle[j]) continue outer
    return i
  }
  return -1
}

const CRLFCRLF = encodeAscii('\r\n\r\n')
const LFLF = encodeAscii('\n\n')

export class MultipartParser {
  private buf = new Uint8Array(0)
  private readonly delim: Uint8Array
  private closed = false

  constructor(boundary = DEFAULT_BOUNDARY) {
    this.delim = encodeAscii(`--${boundary}`)
  }

  /** True once the closing `--boundary--` marker has been seen. */
  get done(): boolean {
    return this.closed
  }

  /** Bytes currently buffered and not yet turned into a part. */
  get pending(): number {
    return this.buf.length
  }

  /** Append a chunk and return every part completed by it (possibly none). */
  push(chunk: Uint8Array): MultipartPart[] {
    if (this.closed) return []
    this.append(chunk)
    const parts: MultipartPart[] = []
    for (;;) {
      const part = this.next()
      if (!part) break
      parts.push(part)
    }
    return parts
  }

  private append(chunk: Uint8Array) {
    if (chunk.length === 0) return
    if (this.buf.length === 0) {
      this.buf = chunk.slice()
      return
    }
    const merged = new Uint8Array(this.buf.length + chunk.length)
    merged.set(this.buf, 0)
    merged.set(chunk, this.buf.length)
    this.buf = merged
  }

  private next(): MultipartPart | null {
    const buf = this.buf
    const b = indexOfBytes(buf, this.delim)
    if (b < 0) {
      // No boundary in sight. Keep only a delimiter-length tail so a boundary
      // split across chunks still matches, and drop any preamble junk.
      const keep = Math.max(0, buf.length - this.delim.length + 1)
      if (keep > 0) this.buf = buf.slice(keep)
      return null
    }
    const afterDelim = b + this.delim.length
    // Closing marker: "--boundary--"
    if (buf[afterDelim] === 0x2d && buf[afterDelim + 1] === 0x2d) {
      this.closed = true
      this.buf = new Uint8Array(0)
      return null
    }

    // Header block ends at the first blank line.
    let hdrEnd = indexOfBytes(buf, CRLFCRLF, afterDelim)
    let dataStart: number
    if (hdrEnd >= 0) {
      dataStart = hdrEnd + 4
    } else {
      hdrEnd = indexOfBytes(buf, LFLF, afterDelim)
      if (hdrEnd < 0) return null // headers incomplete
      dataStart = hdrEnd + 2
    }
    const headers = decodeLatin1(buf.subarray(afterDelim, hdrEnd))
    const lenMatch = /content-length\s*:\s*(\d+)/i.exec(headers)
    const typeMatch = /content-type\s*:\s*([^\r\n;]+)/i.exec(headers)
    const type = (typeMatch?.[1] ?? 'image/jpeg').trim().toLowerCase()

    let dataEnd: number
    let consumed: number
    if (lenMatch) {
      const len = Number(lenMatch[1])
      if (len > MAX_PART) {
        // Lost sync; skip this boundary and look for the next one.
        this.buf = buf.slice(afterDelim)
        return this.next()
      }
      dataEnd = dataStart + len
      if (buf.length < dataEnd) return null // body incomplete
      consumed = dataEnd
      // Swallow the CRLF that precedes the next delimiter if it has arrived;
      // harmless if it hasn't, the delimiter search skips it anyway.
      if (buf[consumed] === 0x0d && buf[consumed + 1] === 0x0a) consumed += 2
    } else {
      const nextB = indexOfBytes(buf, this.delim, dataStart)
      if (nextB < 0) {
        if (buf.length - dataStart > MAX_PART) {
          this.buf = buf.slice(afterDelim) // lost sync; resync on a later boundary
          return this.next()
        }
        return null // wait for the next boundary
      }
      dataEnd = nextB
      // Strip the line break that precedes the delimiter.
      if (dataEnd >= 2 && buf[dataEnd - 2] === 0x0d && buf[dataEnd - 1] === 0x0a) dataEnd -= 2
      else if (dataEnd >= 1 && buf[dataEnd - 1] === 0x0a) dataEnd -= 1
      consumed = nextB
    }

    const data = buf.slice(dataStart, dataEnd)
    this.buf = buf.slice(consumed)
    if (data.length === 0) return this.next() // empty part: skip
    return { type, data }
  }
}
