// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Typed client for the camd HTTP API (fetch wrappers, ApiError, 401 handling).

import type {
  ApplyWiredResult, Config, ConfigPatch, ScanEntry, Status, WifiSave, WifiTestResponse, WiredConfig,
} from './types'

// Thrown for any non-2xx response. `status` lets callers special-case 401.
export class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.name = 'ApiError'
    this.status = status
  }
}

// Callers (e.g. the session store) register a hook so that ANY 401 from an
// admin call flips the whole UI back to logged-out, covering session expiry.
type UnauthorizedHandler = () => void
let onUnauthorized: UnauthorizedHandler | null = null
export function setUnauthorizedHandler(fn: UnauthorizedHandler): void {
  onUnauthorized = fn
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const init: RequestInit = {
    method,
    credentials: 'same-origin',
    headers: {},
  }
  if (body !== undefined) {
    ;(init.headers as Record<string, string>)['Content-Type'] = 'application/json'
    init.body = JSON.stringify(body)
  }

  const res = await fetch(path, init)

  if (res.status === 401) {
    onUnauthorized?.()
    throw new ApiError(401, 'unauthorized')
  }
  if (!res.ok) {
    let msg = `HTTP ${res.status}`
    try {
      const data = await res.json()
      if (data && typeof data.error === 'string') msg = data.error
    } catch {
      /* no JSON body */
    }
    throw new ApiError(res.status, msg)
  }

  if (res.status === 204) return undefined as T
  const text = await res.text()
  return (text ? JSON.parse(text) : undefined) as T
}

export const api = {
  getStatus: () => request<Status>('GET', '/api/status'),

  login: (password: string) =>
    request<{ ok: boolean }>('POST', '/api/login', { password }),

  logout: () => request<{ ok: boolean }>('POST', '/api/logout'),

  getConfig: () => request<Config>('GET', '/api/config'),

  putConfig: (patch: ConfigPatch) => request<Config>('PUT', '/api/config', patch),

  changePassword: (password: string) =>
    request<{ ok: boolean }>('POST', '/api/password', { password }),

  // Applies live and reverts in `revert_in` s unless confirmWired() follows.
  // 409 while the cable isn't the uplink.
  applyWired: (wired: WiredConfig | { mode: 'dhcp' }) =>
    request<ApplyWiredResult>('POST', '/api/network', { wired }),

  // 409 when nothing is pending.
  confirmWired: () => request<{ ok: boolean; confirmed: boolean }>('POST', '/api/network/confirm'),

  saveWifi: (wifi: WifiSave) => request<{ ok: boolean; wifi: string }>('POST', '/api/network', { wifi }),

  wifiScan: () => request<ScanEntry[]>('POST', '/api/wifi/scan'),

  // Synchronous on the device (up to ~25 s). Omitted passphrase = the stored one.
  // 409 while Wi-Fi is the live uplink.
  wifiTest: (ssid: string, passphrase?: string) =>
    request<WifiTestResponse>('POST', '/api/wifi/test', passphrase ? { ssid, passphrase } : { ssid }),

  reboot: () => request<{ ok: boolean }>('POST', '/api/reboot'),

  factoryReset: () => request<{ ok: boolean }>('POST', '/api/factory-reset'),

  // Raw upload of the image to camd's /api/ota (it accepts *_upg.bin, root.bin or a
  // bare rootfs) with progress; resolves on success. Uses XHR for upload progress
  // events, which fetch() still can't report.
  uploadFirmware(
    file: File,
    onProgress: (fraction: number) => void,
  ): { promise: Promise<void>; abort: () => void } {
    const xhr = new XMLHttpRequest()
    const promise = new Promise<void>((resolve, reject) => {
      xhr.open('POST', '/api/ota')
      xhr.setRequestHeader('Content-Type', 'application/octet-stream')
      xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) onProgress(e.loaded / e.total)
      })
      xhr.addEventListener('load', () => {
        if (xhr.status >= 200 && xhr.status < 300) resolve()
        else if (xhr.status === 401) {
          onUnauthorized?.()
          reject(new ApiError(401, 'unauthorized'))
        } else {
          // camd explains rejected images, e.g. {"ok":false,"error":"rootfs checksum mismatch"}
          let msg = `HTTP ${xhr.status}`
          try {
            const body = JSON.parse(xhr.responseText) as { error?: string }
            if (body.error) msg = body.error
          } catch { /* not JSON */ }
          reject(new ApiError(xhr.status, msg))
        }
      })
      // Connection drop mid-flash is the expected success path for OTA; the
      // caller handles it as "device rebooting".
      xhr.addEventListener('error', () => reject(new ApiError(0, 'network')))
      xhr.addEventListener('abort', () => reject(new ApiError(0, 'aborted')))
      xhr.send(file)
    })
    return { promise, abort: () => xhr.abort() }
  },
}
