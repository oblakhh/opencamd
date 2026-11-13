// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Unit tests for the Uploads tab helpers (utils/upload.ts).

import { describe, it, expect } from 'vitest'
import { validateUpload, isUploadValid, formatCadence, describeUploadStatus, httpLabel, normalizeHeaders, headerLines, isHttpsUrl } from '../src/utils/upload'

const good = { enabled: true, url: 'http://printer.local/api/v1/cameras/snap', interval: 10, method: 'PUT' as const, headers: '' }

describe('validateUpload', () => {
  it('accepts a plain http target', () => {
    expect(validateUpload(good)).toEqual({})
    expect(isUploadValid(validateUpload(good))).toBe(true)
  })

  it('accepts an https target', () => {
    expect(validateUpload({ ...good, url: 'https://prusa.local/api/v1/cameras/snap' })).toEqual({})
    expect(isHttpsUrl('https://x')).toBe(true)
    expect(isHttpsUrl('http://x')).toBe(false)
  })

  it('rejects anything that is not a full http(s):// URL', () => {
    expect(validateUpload({ ...good, url: 'printer.local/snap' }).url).toMatch(/http:\/\/ or https:\/\//)
    expect(validateUpload({ ...good, url: 'https://' }).url).toBeTruthy()
    expect(validateUpload({ ...good, url: 'ftp://x' }).url).toBeTruthy()
    expect(validateUpload({ ...good, url: 'http://' }).url).toBeTruthy()
  })

  it('requires a URL only while enabled', () => {
    expect(validateUpload({ ...good, url: '' }).url).toMatch(/needed/)
    expect(validateUpload({ ...good, url: '  ', enabled: false }).url).toBeUndefined()
  })

  it('requires an integer interval of at least 1', () => {
    expect(validateUpload({ ...good, interval: 0 }).interval).toBeTruthy()
    expect(validateUpload({ ...good, interval: 2.5 }).interval).toBeTruthy()
    expect(validateUpload({ ...good, interval: '' }).interval).toBeTruthy() // empty number box
    expect(validateUpload({ ...good, interval: 1 }).interval).toBeUndefined()
  })

  it('only allows POST or PUT', () => {
    expect(validateUpload({ ...good, method: 'GET' as never }).method).toBeTruthy()
    expect(validateUpload({ ...good, method: 'POST' }).method).toBeUndefined()
  })

  it('accepts one Name: value header per line, blank lines included', () => {
    const headers = 'Fingerprint: cam-01\n\nToken: abc123\n'
    expect(validateUpload({ ...good, headers }).headers).toBeUndefined()
    expect(validateUpload({ ...good, headers: 'X-Api-Key:' }).headers).toBeUndefined() // empty value is legal
  })

  it('rejects a line without a colon or without a name', () => {
    expect(validateUpload({ ...good, headers: 'Token abc123' }).headers).toMatch(/Token abc123/)
    expect(validateUpload({ ...good, headers: 'Good: 1\n: nameless' }).headers).toMatch(/nameless/)
    expect(validateUpload({ ...good, headers: 'Bad Name: x' }).headers).toBeTruthy()
  })
})

describe('headers string', () => {
  it('splits on newlines, trims and drops blanks', () => {
    expect(headerLines(' A: 1 \r\n\n  \nB: 2')).toEqual(['A: 1', 'B: 2'])
  })
  it('normalises to the newline-joined form the backend splits', () => {
    expect(normalizeHeaders('A: 1\n\n B: 2 \n')).toBe('A: 1\nB: 2')
    expect(normalizeHeaders('')).toBe('')
  })
})

describe('formatCadence', () => {
  it('reads as every N s ≈ M/min', () => {
    expect(formatCadence(10)).toBe('every 10 s ≈ 6/min')
    expect(formatCadence(7)).toBe('every 7 s ≈ 8.6/min')
    expect(formatCadence(60)).toBe('every 60 s ≈ 1/min')
  })
  it('switches to per-hour for slow cadences', () => {
    expect(formatCadence(90)).toBe('every 90 s ≈ 40/h')
    expect(formatCadence(3600)).toBe('every 3600 s ≈ 1/h')
  })
})

describe('describeUploadStatus', () => {
  it('handles firmware without the status object', () => {
    expect(describeUploadStatus(undefined).tone).toBe('unknown')
  })
  it('says off when disabled', () => {
    const d = describeUploadStatus({ enabled: false, ok: true, last_code: 0, count: 3 })
    expect(d.tone).toBe('off')
    expect(d.text).toMatch(/^Off/)
  })
  it('reports a healthy uploader with count, interval and last code', () => {
    const d = describeUploadStatus({ enabled: true, ok: true, last_code: 200, count: 42 }, 10)
    expect(d.tone).toBe('ok')
    expect(d.text).toBe('Uploading every 10 s — 42 sent, last HTTP 200 OK.')
  })
  it('omits the interval when it is not known (status bar)', () => {
    const d = describeUploadStatus({ enabled: true, ok: true, last_code: 200, count: 1 })
    expect(d.text).toBe('Uploading — 1 sent, last HTTP 200 OK.')
  })
  it('is idle before the first attempt', () => {
    expect(describeUploadStatus({ enabled: true, ok: false, last_code: 0, count: 0 }).tone).toBe('idle')
  })
  it('reports the last HTTP error', () => {
    const d = describeUploadStatus({ enabled: true, ok: false, last_code: 401, count: 5 }, 10)
    expect(d.tone).toBe('bad')
    expect(d.text).toBe('Last error: HTTP 401 Unauthorized — 5 sent so far.')
  })
  it('treats code 0 as a connection failure once running', () => {
    expect(describeUploadStatus({ enabled: true, ok: false, last_code: 0, count: 3 }).text).toMatch(/couldn’t connect/)
  })
  it('labels unknown codes plainly', () => {
    expect(httpLabel(418)).toBe('HTTP 418')
  })
})
