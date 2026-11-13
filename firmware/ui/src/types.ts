// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
//
// Shapes that mirror the camd HTTP API contract exactly.

export type NetMode = 'dhcp' | 'static'
export type Uplink = 'wired' | 'wifi' | 'none'
export type WifiState = 'off' | 'standby' | 'connecting' | 'associated' | 'connected' | 'wrong_passphrase'
export type UploadMethod = 'POST' | 'PUT'

export interface VideoStatus {
  width: number
  height: number
  fps: number
  quality: number
}

// Live Wi-Fi client state. `rssi` is 0..100, or -1 when unknown.
export interface WifiLinkStatus {
  state: WifiState
  ssid: string
  bssid: string
  ip: string
  rssi: number
}

export interface NetStatus {
  uplink: Uplink
  iface: string
  ip: string
  mac: string
  wired_link: boolean
  mode: NetMode     // the COMMITTED wired mode
  revert_in: number // seconds left in a wired try-window; 0 = nothing pending
  wifi: WifiLinkStatus
}

export interface Status {
  name: string
  model: string
  version: string
  uptime: number
  time: string
  video: VideoStatus
  net: NetStatus
  admin: boolean
  // Read-only uploader state. Absent on firmware built without the uploader;
  // the UI then shows "no status" rather than guessing.
  upload?: UploadStatus
}

export interface UploadStatus {
  enabled: boolean
  ok: boolean        // was the last attempt successful
  last_code: number  // last HTTP status code (0 = never tried / couldn't connect)
  count: number      // successful uploads since boot
}

export interface VideoConfig {
  width: number
  height: number
  quality: number
}

export interface WiredConfig {
  mode: NetMode
  ip: string
  netmask: string
  gateway: string
  dns: string // "" | "a.b.c.d" | "a.b.c.d e.f.g.h"
}

// The passphrase itself is never returned, only whether one is stored.
export interface WifiConfig {
  enabled: boolean
  ssid: string
  passphrase_set: boolean
}

// Read-only via /api/config; changed only through POST /api/network.
export interface NetworkConfig {
  wired: WiredConfig
  wifi: WifiConfig
}

// One PrusaLink/OctoPrint-style snapshot target.
export interface UploadConfig {
  enabled: boolean
  url: string          // http(s)://host[:port]/path — both handled by camd (TLS via mbedTLS)
  interval: number     // seconds between uploads, integer >= 1
  method: UploadMethod // POST = multipart form; PUT = raw JPEG body
  field: string        // multipart form-field name (POST only), e.g. "file"
  headers: string      // extra request headers, one "Key: Value" per line (newline-separated)
}

export interface SystemConfig {
  hostname: string
  ntp: string
  syslog_remote: string
  syslog_level: number
  telnet: boolean
}

export interface Config {
  video: VideoConfig
  network: NetworkConfig
  upload: UploadConfig
  system: SystemConfig
}

// A PUT /api/config body — any subset of sections/fields.
export type ConfigPatch = {
  [K in keyof Config]?: Partial<Config[K]>
}

// POST /api/network {"wired":…} — `ip` is "" for DHCP.
export interface ApplyWiredResult {
  ok: boolean
  mode: NetMode
  ip: string
  revert_in: number
}

// POST /api/network {"wifi":…} — omit `passphrase` to keep the stored one.
export interface WifiSave {
  enabled: boolean
  ssid: string
  passphrase?: string
}

// One BSSID from POST /api/wifi/scan. Duplicated per BSSID; `ssid` may be "".
export interface ScanEntry {
  ssid: string
  bssid: string
  channel: number
}

export type WifiTestResult = 'ok' | 'wrong_passphrase' | 'not_found' | 'no_dhcp' | 'busy' | 'error'

export interface WifiTestResponse {
  result: WifiTestResult
  ip?: string
  // seconds until the router accepts a new attempt (busy after a rejected password)
  retry_in?: number
}

export interface ResolutionMode {
  label: string
  width: number
  height: number
}

export const RESOLUTIONS: ResolutionMode[] = [
  { label: '1280 × 960 (4:3)', width: 1280, height: 960 },
  { label: '1280 × 720 (16:9)', width: 1280, height: 720 },
  { label: '640 × 480', width: 640, height: 480 },
  { label: '320 × 240', width: 320, height: 240 },
]

// Hardware fps ceiling (see docs/hardware.md). Shown honestly in the UI.
export const FPS_CEILING = 10
