// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
//
// Pure helpers behind the Uploads tab: client-side validation of the upload
// section and the wording of the live uploader status. Kept free of Vue so
// they can be unit-tested directly.

import type { UploadConfig, UploadMethod, UploadStatus } from '../types'

export const UPLOAD_METHODS: readonly UploadMethod[] = ['POST', 'PUT']

export interface UploadErrors {
  url?: string
  interval?: string
  method?: string
  headers?: string
}

// `interval` is typed as a number but arrives from a <input type=number> via
// v-model.number, which yields '' while the box is empty — hence `unknown`.
export type UploadForm = Pick<UploadConfig, 'enabled' | 'url' | 'method' | 'headers'> & { interval: unknown }

// Header lines: trimmed, blanks dropped, joined with '\n' — the exact string
// the backend splits again. Used both for validation and for what gets saved.
export function headerLines(headers: string): string[] {
  return headers.split(/\r?\n/).map((l) => l.trim()).filter(Boolean)
}
export function normalizeHeaders(headers: string): string {
  return headerLines(headers).join('\n')
}

// "Name: value" — a token name (no spaces), a colon, then anything.
const HEADER_LINE = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+\s*:/

export function validateHeaders(headers: string): string | undefined {
  const lines = headerLines(headers)
  for (const line of lines) {
    if (!HEADER_LINE.test(line)) {
      const shown = line.length > 32 ? `${line.slice(0, 32)}…` : line
      return `“${shown}” isn’t a header — use one Name: value per line.`
    }
  }
  return undefined
}

export function validateUpload(u: UploadForm): UploadErrors {
  const errors: UploadErrors = {}
  const url = u.url.trim()

  // http:// and https:// are both handled by camd's built-in client (TLS 1.2 via mbedTLS).
  if (url && !/^https?:\/\/[^\s/?#]+/i.test(url)) {
    errors.url = 'Enter a full address starting with http:// or https://, e.g. http://printer.local/snap'
  } else if (!url && u.enabled) {
    errors.url = 'A URL is needed while uploading is on.'
  }

  if (typeof u.interval !== 'number' || !Number.isInteger(u.interval) || u.interval < 1) {
    errors.interval = 'Use a whole number of seconds, 1 or more.'
  }

  if (!UPLOAD_METHODS.includes(u.method)) {
    errors.method = 'Choose POST or PUT.'
  }

  const headers = validateHeaders(u.headers)
  if (headers) errors.headers = headers

  return errors
}

export function isHttpsUrl(url: string): boolean {
  return /^https:\/\//i.test(url.trim())
}

export function isUploadValid(errors: UploadErrors): boolean {
  return Object.keys(errors).length === 0
}

// "every 10 s ≈ 6/min" — the rate hint under the interval field.
export function formatCadence(interval: number): string {
  const trim = (n: number) => (Math.round(n * 10) / 10).toString()
  const perMin = 60 / interval
  const rate = perMin >= 1 ? `${trim(perMin)}/min` : `${trim(3600 / interval)}/h`
  return `every ${interval} s ≈ ${rate}`
}

const HTTP_REASONS: Record<number, string> = {
  200: 'OK', 201: 'Created', 202: 'Accepted', 204: 'No content',
  400: 'Bad request', 401: 'Unauthorized', 403: 'Forbidden', 404: 'Not found',
  405: 'Method not allowed', 408: 'Timeout', 413: 'Too large', 415: 'Unsupported type',
  429: 'Too many requests', 500: 'Server error', 502: 'Bad gateway',
  503: 'Unavailable', 504: 'Gateway timeout',
}

export function httpLabel(code: number): string {
  const reason = HTTP_REASONS[code]
  return reason ? `HTTP ${code} ${reason}` : `HTTP ${code}`
}

export type UploadTone = 'unknown' | 'off' | 'idle' | 'ok' | 'bad'

export interface UploadSummary {
  tone: UploadTone
  text: string
}

// One line for the tab's status row and the status-bar chip tooltip.
// `interval` (from config) is optional: the status bar doesn't know it.
export function describeUploadStatus(
  s: UploadStatus | null | undefined,
  interval?: number,
): UploadSummary {
  if (!s) return { tone: 'unknown', text: 'No uploader status from the camera yet.' }
  if (!s.enabled) return { tone: 'off', text: 'Off — nothing is being sent.' }

  const sent = `${s.count} sent`
  const every = interval ? ` every ${interval} s` : ''

  if (s.count === 0 && s.last_code === 0) {
    return { tone: 'idle', text: `Starting${every} — waiting for the first upload.` }
  }
  if (s.ok) {
    return { tone: 'ok', text: `Uploading${every} — ${sent}, last ${httpLabel(s.last_code)}.` }
  }
  const err = s.last_code <= 0 ? 'couldn’t connect' : httpLabel(s.last_code)
  return { tone: 'bad', text: `Last error: ${err} — ${sent} so far.` }
}
