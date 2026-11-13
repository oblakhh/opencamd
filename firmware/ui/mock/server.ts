// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Dev-only mock of the camd HTTP API, served by the Vite dev server.

import type { Connect, Plugin, ViteDevServer } from 'vite'
import type { ServerResponse, IncomingMessage } from 'node:http'

// ---------------------------------------------------------------------------
// Dev-only mock of the camd C backend. Lets `npm run dev` run with no camera.
// Serves /api/*, /snapshot.jpg and a real multipart /stream.mjpg (SVG frames).
// None of this ships in the build.
// ---------------------------------------------------------------------------

const MOCK_PASSWORD = 'admin'
const SESSION_COOKIE = 'camd_session'
const SESSION_VALUE = 'mock-session-token'

const bootTime = Date.now()

const config = {
  video: { width: 1280, height: 960, quality: 80 },
  upload: {
    enabled: false,
    url: 'http://prusa.local/api/v1/cameras/snap',
    interval: 10,
    method: 'PUT',
    field: 'file',
    headers: 'Fingerprint: ic3115w\nToken: ',
  },
  system: {
    hostname: 'ic3115w', ntp: 'pool.ntp.org',
    syslog_remote: '', syslog_level: 6, telnet: true,
  },
}

function isAuthed(req: IncomingMessage): boolean {
  const cookie = req.headers.cookie ?? ''
  return cookie.split(';').some((c: string) => c.trim() === `${SESSION_COOKIE}=${SESSION_VALUE}`)
}

function readBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve) => {
    let data = ''
    req.on('data', (chunk: Buffer) => (data += chunk))
    req.on('end', () => resolve(data))
  })
}

function sendJson(res: ServerResponse, status: number, body: unknown, headers: Record<string, string> = {}) {
  const payload = JSON.stringify(body)
  res.writeHead(status, { 'Content-Type': 'application/json', ...headers })
  res.end(payload)
}

// Deep-ish merge used to emulate the server's PUT /api/config partial-merge.
function merge(target: any, patch: any): any {
  for (const key of Object.keys(patch)) {
    const val = patch[key]
    if (Array.isArray(val)) {
      target[key] = val
    } else if (val && typeof val === 'object') {
      if (!target[key] || typeof target[key] !== 'object') target[key] = {}
      merge(target[key], val)
    } else {
      target[key] = val
    }
  }
  return target
}

// Pretend uploader. Counts one successful upload per interval while enabled;
// a URL containing "fail" simulates a rejected target (401) so the error UI
// can be demoed. Re-based whenever the upload section is saved.
const uploader = { since: Date.now(), base: 0 }
function uploadStatus() {
  const u = config.upload
  const elapsed = (Date.now() - uploader.since) / 1000
  const ok = !/fail/i.test(u.url)
  const attempts = Math.floor(elapsed / Math.max(1, u.interval))
  if (!u.enabled) return { enabled: false, ok: true, last_code: 0, count: uploader.base }
  if (!ok) return { enabled: true, ok: false, last_code: 401, count: uploader.base }
  return { enabled: true, ok: true, last_code: attempts > 0 ? 200 : 0, count: uploader.base + attempts }
}
function rebaseUploader() {
  uploader.base = uploadStatus().count
  uploader.since = Date.now()
}

// ---------------------------------------------------------------------------
// Network: a small model of camd's net.c / netcfg.c, plus scenarios that pin
// /api/status.net to each state the Network tab has to render.
//
// Pick a scenario by opening the UI with `?net=<name>` (read from the Referer
// of the API calls, and sticky until another `?net=` is seen):
//   wired-standby (default) · wired-nowifi · failover · wifi · wrongpw · none
//   · pending · apply-move
// Scan variants (per request): `?scan=empty`, `?scan=fail`.
// Wi-Fi test results are picked by the passphrase (substring): "wrong" →
// wrong_passphrase, "busy" → busy, "nodhcp" → no_dhcp, "err" → error;
// SSID "Nope" → not_found; anything else → ok (192.0.2.151).
// Static IP 192.0.2.13 → 400 from the "backend" (to show `Failed: …`).
// ---------------------------------------------------------------------------

type Scenario = 'wired-standby' | 'wired-nowifi' | 'failover' | 'wifi' | 'wrongpw' | 'none' | 'pending' | 'apply-move'
const SCENARIOS: Scenario[] = ['wired-standby', 'wired-nowifi', 'failover', 'wifi', 'wrongpw', 'none', 'pending', 'apply-move']

interface Wired { mode: 'dhcp' | 'static'; ip: string; netmask: string; gateway: string; dns: string }

const DHCP_IP = '192.0.2.65'
const MAC = '02:00:00:00:00:02'
const REVERT_S = Number(process.env.MOCK_REVERT_S) || 90 // shorten for testing the revert path

const net = {
  scenario: 'wired-standby' as Scenario,
  since: Date.now(),
  wired: { mode: 'dhcp', ip: '', netmask: '', gateway: '', dns: '' } as Wired, // committed
  wifi: { enabled: true, ssid: 'Home-2G', passphrase: 'correct-horse' },
  pending: null as null | { tried: Wired; deadline: number },
  wifiWrong: false,
  silentUntil: 0, // apply-move: the camera "lost" this address
}

function setScenario(name: Scenario) {
  net.scenario = name
  net.since = Date.now()
  net.pending = null
  net.wifiWrong = false
  net.silentUntil = 0
  net.wired = { mode: 'dhcp', ip: '', netmask: '', gateway: '', dns: '' }
  net.wifi = name === 'wired-nowifi' || name === 'none'
    ? { enabled: false, ssid: '', passphrase: '' }
    : { enabled: true, ssid: 'Home-2G', passphrase: 'correct-horse' }
  if (name === 'wrongpw') net.wifiWrong = true
  if (name === 'pending') {
    net.pending = {
      tried: { mode: 'static', ip: DHCP_IP, netmask: '255.255.255.0', gateway: '192.0.2.1', dns: '192.0.2.1' },
      deadline: Date.now() + REVERT_S * 1000,
    }
  }
  console.log(`[mock] network scenario: ${name}`)
}

function query(req: IncomingMessage, key: string): string | null {
  const ref = req.headers.referer
  if (!ref) return null
  try { return new URL(ref).searchParams.get(key) } catch { return null }
}
function syncScenario(req: IncomingMessage) {
  const want = query(req, 'net') as Scenario | null
  if (want && SCENARIOS.includes(want) && want !== net.scenario) setScenario(want)
}

function tickPending() {
  if (net.pending && Date.now() >= net.pending.deadline) {
    net.pending = null // not confirmed: back to the committed settings
    console.log('[mock] wired try-window expired: reverted')
  }
}

function uplinkNow(): { uplink: 'wired' | 'wifi' | 'none'; wiredLink: boolean; wifiState: string; rssi: number } {
  const standby = net.wifiWrong ? 'wrong_passphrase' : net.wifi.enabled ? 'standby' : 'off'
  switch (net.scenario) {
    case 'wifi':
      return { uplink: 'wifi', wiredLink: false, wifiState: net.wifi.enabled ? 'connected' : 'off', rssi: 85 }
    case 'none':
      return { uplink: 'none', wiredLink: false, wifiState: net.wifi.enabled ? 'connecting' : 'off', rssi: -1 }
    case 'failover': {
      // Cable pulled → Wi-Fi takes over → cable back, 4 s per step.
      const step = Math.floor((Date.now() - net.since) / 4000) % 6
      return [
        { uplink: 'wired', wiredLink: false, wifiState: standby, rssi: -1 },
        { uplink: 'none', wiredLink: false, wifiState: 'connecting', rssi: -1 },
        { uplink: 'none', wiredLink: false, wifiState: 'associated', rssi: -1 },
        { uplink: 'wifi', wiredLink: false, wifiState: 'connected', rssi: 62 },
        { uplink: 'wifi', wiredLink: true, wifiState: 'connected', rssi: 62 },
        { uplink: 'wired', wiredLink: true, wifiState: standby, rssi: -1 },
      ][step] as ReturnType<typeof uplinkNow>
    }
    default:
      return { uplink: 'wired', wiredLink: true, wifiState: standby, rssi: -1 }
  }
}

function wiredIp(w: Wired): string {
  return w.mode === 'static' ? w.ip : DHCP_IP
}

function netStatus() {
  tickPending()
  const u = uplinkNow()
  const live = net.pending ? net.pending.tried : net.wired
  const ip = u.uplink === 'none' ? '' : u.uplink === 'wifi' ? DHCP_IP : wiredIp(live)
  const connected = u.wifiState === 'connected'
  return {
    uplink: u.uplink,
    iface: u.uplink === 'wifi' ? 'wlan0' : 'eth0',
    ip,
    mac: MAC,
    link: u.uplink === 'wifi' ? 'wifi' : 'wired', // legacy field camd still sends
    wired_link: u.wiredLink,
    mode: net.wired.mode,
    revert_in: net.pending ? Math.max(0, Math.ceil((net.pending.deadline - Date.now()) / 1000)) : 0,
    wifi: {
      state: u.wifiState,
      ssid: net.wifi.ssid,
      bssid: connected ? '02:00:00:00:01:01' : '',
      ip: connected ? DHCP_IP : '',
      rssi: connected ? u.rssi : -1,
    },
  }
}

// Ports of camd's netcfg_validate / wifi_validate, with its error strings.
function ipParse(s: string): number | null {
  if (!/^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/.test(s)) return null
  return s.split('.').reduce((a, o) => ((a << 8) | Number(o)) >>> 0, 0)
}
function validateWiredBackend(w: Wired): string | null {
  if (w.mode === 'dhcp') return null
  if (w.mode !== 'static') return 'mode must be dhcp or static'
  const ip = ipParse(w.ip)
  if (ip === null) return 'invalid IP address'
  const m = ipParse(w.netmask)
  const inv = m === null ? 0 : ~m >>> 0
  if (m === null || m === 0 || m === 0xffffffff || (inv & (inv + 1)) !== 0) return 'invalid netmask'
  if ((ip & inv) >>> 0 === 0 || (ip & inv) >>> 0 === inv) return 'IP is the network or broadcast address'
  if (w.gateway) {
    const gw = ipParse(w.gateway)
    if (gw === null || ((gw & m) >>> 0) !== ((ip & m) >>> 0) || gw === ip || ((gw & inv) >>> 0) === inv) {
      return 'gateway must be a host in the same subnet'
    }
  }
  const dns = (w.dns || '').split(/[ ,]+/).filter(Boolean)
  if (dns.length > 2 || dns.some((d) => ipParse(d) === null)) return 'DNS must be up to two IPv4 addresses'
  if (w.ip === '192.0.2.13') return 'address 192.0.2.13 is already in use (mock)'
  return null
}
function validateWifiBackend(w: { enabled: boolean; ssid: string; passphrase: string }): string | null {
  if (/[\x00-\x1f\x7f]/.test(w.ssid)) return 'SSID contains control characters'
  if (!/^[\x20-\x7e]*$/.test(w.passphrase)) return 'passphrase must be printable ASCII'
  if (w.enabled && (w.ssid.length < 1 || Buffer.byteLength(w.ssid) > 32)) return 'SSID must be 1-32 characters'
  if (w.enabled && (w.passphrase.length < 8 || w.passphrase.length > 63)) return 'passphrase must be 8-63 characters'
  return null
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

function status(req: IncomingMessage) {
  // A little life in the fps number so the status bar feels real.
  const fps = 7.5 + Math.random() * 0.8
  return {
    name: config.system.hostname,
    model: 'IC-3115W',
    version: 'camd 0.1.0 (mock)',
    uptime: Math.floor((Date.now() - bootTime) / 1000),
    time: new Date().toISOString(),
    video: {
      width: config.video.width,
      height: config.video.height,
      fps: Math.round(fps * 10) / 10,
      quality: config.video.quality,
    },
    net: netStatus(),
    admin: isAuthed(req),
    upload: uploadStatus(),
  }
}

// SVG frames stand in for JPEGs in dev (no encoder needed). The mock stream
// is a genuine multipart/x-mixed-replace body in exactly camd's framing, so
// the fetch()-based reader in src/composables/useMjpeg.ts is exercised for
// real; the per-part Content-Type just says image/svg+xml instead of
// image/jpeg. 4:3 so the object-fit letterboxing matches a real frame.
const BOUNDARY = 'camdframe'
const MOCK_FPS = 8
const MAX_STREAMS = 2 // mirrors camd: the third viewer gets a 503
let streams = 0

function frameSvg(t: number): string {
  // A ball on a Lissajous path; position is a pure function of time so
  // snapshots and stream frames line up.
  const cx = 640 + 440 * Math.sin((t / 6000) * 2 * Math.PI)
  const cy = 480 + 180 * Math.sin((t / 4500) * 2 * Math.PI + 1)
  const stamp = new Date(t).toISOString().slice(11, 23)
  return `<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="960" viewBox="0 0 1280 960">
  <rect width="1280" height="960" fill="#0d1014"/>
  <g fill="none" stroke="#1b2733" stroke-width="2">
    ${Array.from({ length: 13 }, (_, i) => `<line x1="${i * 107}" y1="0" x2="${i * 107}" y2="960"/>`).join('')}
    ${Array.from({ length: 10 }, (_, i) => `<line x1="0" y1="${i * 107}" x2="1280" y2="${i * 107}"/>`).join('')}
  </g>
  <circle cx="${cx.toFixed(1)}" cy="${cy.toFixed(1)}" r="60" fill="#ff8a4c"/>
  <text x="640" y="150" fill="#5b6977" font-family="monospace" font-size="54" text-anchor="middle">MOCK STREAM</text>
  <text x="640" y="860" fill="#3a4a5a" font-family="monospace" font-size="34" text-anchor="middle">IC-3115W · 1280x960 · ${stamp}</text>
</svg>`
}

function serveSnapshot(res: ServerResponse) {
  res.writeHead(200, { 'Content-Type': 'image/svg+xml', 'Cache-Control': 'no-store' })
  res.end(frameSvg(Date.now()))
}

function serveStream(req: IncomingMessage, res: ServerResponse) {
  if (streams >= MAX_STREAMS) {
    res.writeHead(503, { 'Content-Type': 'text/plain', 'Retry-After': '5', Connection: 'close' })
    res.end('too many viewers\n')
    return
  }
  streams++
  res.writeHead(200, {
    'Content-Type': `multipart/x-mixed-replace; boundary=${BOUNDARY}`,
    'Cache-Control': 'no-store',
    Connection: 'close',
  })
  const writeFrame = () => {
    const body = Buffer.from(frameSvg(Date.now()), 'utf8')
    res.write(`--${BOUNDARY}\r\nContent-Type: image/svg+xml\r\nContent-Length: ${body.length}\r\n\r\n`)
    res.write(body)
    res.write('\r\n')
  }
  writeFrame()
  const timer = setInterval(writeFrame, 1000 / MOCK_FPS)
  let released = false
  const release = () => {
    if (released) return
    released = true
    clearInterval(timer)
    streams--
  }
  req.on('close', release)
  res.on('close', release)
  res.on('error', release)
}

export function mockApi(): Plugin {
  return {
    name: 'camd-mock-api',
    configureServer(server: ViteDevServer) {
      const handle = async (req: Connect.IncomingMessage, res: ServerResponse, next: Connect.NextFunction) => {
        const url = (req.url ?? '').split('?')[0]
        const method = req.method ?? 'GET'

        // --- media placeholders ---
        if (url === '/stream.mjpg' || url === '/mjpg/video.mjpg') return serveStream(req, res)
        if (url === '/snapshot.jpg' || url === '/snapshot.cgi') return serveSnapshot(res)

        if (!url.startsWith('/api/')) return next()

        syncScenario(req)
        // apply-move: the camera has left this address; nothing answers.
        if (Date.now() < net.silentUntil) {
          req.socket.destroy()
          return
        }

        const needAuth = url !== '/api/status' && url !== '/api/login'
        if (needAuth && !isAuthed(req)) {
          return sendJson(res, 401, { ok: false, error: 'unauthorized' })
        }

        try {
          if (url === '/api/status' && method === 'GET') {
            return sendJson(res, 200, status(req))
          }

          if (url === '/api/login' && method === 'POST') {
            const { password } = JSON.parse((await readBody(req)) || '{}')
            if (password === MOCK_PASSWORD) {
              return sendJson(res, 200, { ok: true }, {
                'Set-Cookie': `${SESSION_COOKIE}=${SESSION_VALUE}; Path=/; HttpOnly; SameSite=Strict`,
              })
            }
            return sendJson(res, 401, { ok: false })
          }

          if (url === '/api/logout' && method === 'POST') {
            return sendJson(res, 200, { ok: true }, {
              'Set-Cookie': `${SESSION_COOKIE}=; Path=/; Max-Age=0`,
            })
          }

          if (url === '/api/config' && method === 'GET') {
            return sendJson(res, 200, redactConfig(config))
          }

          if (url === '/api/config' && method === 'PUT') {
            const patch = JSON.parse((await readBody(req)) || '{}')
            // Like camd: network only changes through POST /api/network.
            delete patch.network
            delete patch.wifi
            if (patch.upload) rebaseUploader()
            merge(config, patch)
            return sendJson(res, 200, redactConfig(config))
          }

          if (url === '/api/password' && method === 'POST') {
            await readBody(req)
            return sendJson(res, 200, { ok: true })
          }

          if (url === '/api/network/confirm' && method === 'POST') {
            tickPending()
            if (!net.pending) return sendJson(res, 409, { ok: false, error: 'nothing to confirm' })
            net.wired = net.pending.tried
            net.pending = null
            console.log('[mock] wired change confirmed')
            return sendJson(res, 200, { ok: true, confirmed: true })
          }

          if (url === '/api/network' && method === 'POST') {
            const body = JSON.parse((await readBody(req)) || '{}')
            if (body.wifi && !body.wired) {
              const nw = { ...net.wifi }
              if (typeof body.wifi.enabled === 'boolean') nw.enabled = body.wifi.enabled
              if (typeof body.wifi.ssid === 'string') nw.ssid = body.wifi.ssid
              if (typeof body.wifi.passphrase === 'string' && body.wifi.passphrase) nw.passphrase = body.wifi.passphrase
              const err = validateWifiBackend(nw)
              if (err) return sendJson(res, 400, { ok: false, error: err })
              const changedCreds = nw.ssid !== net.wifi.ssid || nw.passphrase !== net.wifi.passphrase
              net.wifi = nw
              if (changedCreds || !nw.enabled) net.wifiWrong = false
              await sleep(400)
              return sendJson(res, 200, { ok: true, wifi: 'applying' })
            }
            if (body.wired && typeof body.wired === 'object') {
              tickPending()
              if (uplinkNow().uplink !== 'wired') {
                return sendJson(res, 409, { ok: false, error: 'wired settings can only be changed while the cable is the uplink' })
              }
              const w: Wired = { ...net.wired }
              for (const k of ['mode', 'ip', 'netmask', 'gateway', 'dns'] as const) {
                if (typeof body.wired[k] === 'string') (w as unknown as Record<string, string>)[k] = body.wired[k]
              }
              const err = validateWiredBackend(w)
              if (err) return sendJson(res, 400, { ok: false, error: err })
              const before = wiredIp(net.pending ? net.pending.tried : net.wired)
              net.pending = { tried: w, deadline: Date.now() + REVERT_S * 1000 } // keeps the committed fallback
              if (net.scenario === 'apply-move' && w.mode === 'static' && w.ip !== before) {
                net.silentUntil = Date.now() + 1000 + 60_000
                console.log('[mock] apply-move: going silent for 60 s')
              }
              await sleep(300)
              return sendJson(res, 200, { ok: true, mode: w.mode, ip: w.mode === 'static' ? w.ip : '', revert_in: REVERT_S })
            }
            return sendJson(res, 400, { ok: false, error: 'expected {"wired":{...}}' })
          }

          if (url === '/api/wifi/scan' && method === 'POST') {
            const variant = query(req, 'scan')
            await sleep(5000)
            if (variant === 'fail') return sendJson(res, 500, { ok: false, error: 'scan failed' })
            if (variant === 'empty') return sendJson(res, 200, [])
            return sendJson(res, 200, [
              { ssid: 'Home-2G', bssid: '02:00:00:00:01:01', channel: 6 },
              { ssid: '', bssid: '0a:11:22:33:44:01', channel: 11 },
              { ssid: 'Neighbour-Net', bssid: '02:00:00:00:02:01', channel: 11 },
              { ssid: 'Home-2G', bssid: '02:00:00:00:01:02', channel: 1 },
              { ssid: '', bssid: '0a:11:22:33:44:02', channel: 1 },
              { ssid: 'PrinterRoom', bssid: '02:00:00:00:03:01', channel: 11 },
            ])
          }

          if (url === '/api/wifi/test' && method === 'POST') {
            const body = JSON.parse((await readBody(req)) || '{}')
            if (uplinkNow().uplink === 'wifi') {
              return sendJson(res, 409, { ok: false, error: 'a Wi-Fi test is only possible while the cable is the uplink' })
            }
            const ssid = typeof body.ssid === 'string' ? body.ssid : ''
            const pass = typeof body.passphrase === 'string' && body.passphrase ? body.passphrase : net.wifi.passphrase
            if (!pass) return sendJson(res, 400, { ok: false, error: 'no passphrase given and none stored' })
            const err = validateWifiBackend({ enabled: true, ssid, passphrase: pass })
            if (err) return sendJson(res, 400, { ok: false, error: err })
            await sleep(8000)
            if (/wrong/i.test(pass)) return sendJson(res, 200, { result: 'wrong_passphrase' })
            if (/busy/i.test(pass)) return sendJson(res, 200, { result: 'busy' })
            if (/nodhcp/i.test(pass)) return sendJson(res, 200, { result: 'no_dhcp' })
            if (/err/i.test(pass)) return sendJson(res, 200, { result: 'error' })
            if (ssid === 'Nope') return sendJson(res, 200, { result: 'not_found' })
            return sendJson(res, 200, { result: 'ok', ip: '192.0.2.151' })
          }

          if ((url === '/api/reboot' || url === '/api/factory-reset') && method === 'POST') {
            return sendJson(res, 200, { ok: true })
          }

          if (url === '/api/ota' && method === 'POST') {
            await readBody(req) // drain the upload
            return sendJson(res, 200, { ok: true })
          }

          return sendJson(res, 404, { ok: false, error: 'not found' })
        } catch (err) {
          return sendJson(res, 500, { ok: false, error: String(err) })
        }
      }

      server.middlewares.use((req, res, next) => {
        handle(req, res, next).catch(next)
      })
    },
  }
}

// Mirror the real server: never return secrets in cleartext.
function redactConfig(c: typeof config) {
  tickPending()
  return {
    ...structuredClone(c),
    network: {
      wired: { ...net.wired },
      wifi: { enabled: net.wifi.enabled, ssid: net.wifi.ssid, passphrase_set: net.wifi.passphrase !== '' },
    },
  }
}
