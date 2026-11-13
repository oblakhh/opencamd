// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
//
// Pure helpers behind the Network tab and the revert banner: validation that
// mirrors camd's netcfg_validate / wifi_validate, scan grouping, the address
// plan for a wired change, and every piece of state-dependent copy. Kept free
// of Vue so they can be unit-tested directly.

import type { NetMode, NetStatus, ScanEntry, Uplink, WifiTestResult } from '../types'

// --- addresses -------------------------------------------------------------

// Canonical dotted quad only (no leading zeros, no shorthand) — the backend
// compares against inet_ntoa()'s output, so "010.1.1.1" is refused there too.
const OCTET = '(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)'
const QUAD = new RegExp(`^${OCTET}\\.${OCTET}\\.${OCTET}\\.${OCTET}$`)

export function isIPv4(s: string): boolean {
  return QUAD.test(s)
}

// Unsigned 32-bit value of a dotted quad (caller checks isIPv4 first).
export function ipToInt(s: string): number {
  return s.split('.').reduce((acc, o) => ((acc << 8) | Number(o)) >>> 0, 0)
}

// Contiguous mask from /1 to /30 (/31 and /32 leave no usable host).
export function isNetmask(s: string): boolean {
  if (!isIPv4(s)) return false
  const m = ipToInt(s)
  if (m === 0) return false
  const inv = ~m >>> 0
  if ((inv & (inv + 1)) !== 0) return false // not contiguous
  return inv >= 3
}

export function sameSubnet(a: string, b: string, mask: string): boolean {
  const m = ipToInt(mask)
  return ((ipToInt(a) & m) >>> 0) === ((ipToInt(b) & m) >>> 0)
}

// --- wired form --------------------------------------------------------------

export interface WiredForm {
  mode: NetMode
  ip: string
  netmask: string
  gateway: string
  dns1: string
  dns2: string
}

export interface WiredErrors {
  ip?: string
  netmask?: string
  gateway?: string
  dns1?: string
  dns2?: string
}

export function validateWired(f: WiredForm): WiredErrors {
  const e: WiredErrors = {}
  if (f.mode !== 'static') return e
  const ip = f.ip.trim()
  const nm = f.netmask.trim()
  const gw = f.gateway.trim()

  const ipOk = isIPv4(ip)
  if (!ipOk) {
    e.ip = 'Enter an IPv4 address like 192.0.2.80.'
  } else {
    const first = Number(ip.split('.')[0])
    if (ip === '0.0.0.0' || ip === '255.255.255.255' || first === 127 || first >= 224 || first === 0) {
      e.ip = 'That address can’t be used for the camera.'
    }
  }

  const nmOk = isNetmask(nm)
  if (!nmOk) e.netmask = 'Enter a netmask like 255.255.255.0.'

  if (ipOk && nmOk && !e.ip) {
    const host = (ipToInt(ip) & ~ipToInt(nm)) >>> 0
    const hostMask = ~ipToInt(nm) >>> 0
    if (host === 0 || host === hostMask) {
      e.ip = 'That’s the network/broadcast address for this netmask — pick another.'
    }
  }

  if (gw) {
    if (!isIPv4(gw)) {
      e.gateway = 'Enter a gateway address like 192.0.2.1, or leave it blank.'
    } else if (ipOk && nmOk) {
      const hostMask = ~ipToInt(nm) >>> 0
      if (!sameSubnet(gw, ip, nm)) e.gateway = 'The gateway must be in the same network as the IP address.'
      else if (gw === ip) e.gateway = 'The gateway can’t be the camera’s own address.'
      else if (((ipToInt(gw) & hostMask) >>> 0) === hostMask) {
        // camd refuses this too ("gateway must be a host in the same subnet").
        e.gateway = 'The gateway can’t be the broadcast address for this netmask.'
      }
    }
  }

  const DNS_MSG = 'Enter a DNS server address, or leave it blank.'
  if (f.dns1.trim() && !isIPv4(f.dns1.trim())) e.dns1 = DNS_MSG
  if (f.dns2.trim() && !isIPv4(f.dns2.trim())) e.dns2 = DNS_MSG
  return e
}

export function isValid(errors: object): boolean {
  return Object.values(errors).every((v) => !v)
}

// Two inputs ⇄ the backend's single "a.b.c.d e.f.g.h" string.
export function joinDns(d1: string, d2: string): string {
  return [d1.trim(), d2.trim()].filter(Boolean).join(' ')
}
export function splitDns(dns: string): [string, string] {
  const parts = (dns ?? '').split(/[\s,]+/).filter(Boolean)
  return [parts[0] ?? '', parts[1] ?? '']
}

// Static-mode convenience prefill: x.y.z.1 for a.b.c.d.
export function guessGateway(ip: string): string {
  return isIPv4(ip) ? `${ip.split('.').slice(0, 3).join('.')}.1` : ''
}

export function addressingHint(formMode: NetMode, committedMode: NetMode | undefined, currentIp: string): {
  text: string
  ip?: string
} {
  if (formMode === 'static') return { text: 'Fixed address. Fill in at least the IP address and netmask.' }
  if (committedMode === 'static') {
    return { text: 'Switching to automatic: the router will hand out an address, which may differ from the current one.' }
  }
  return { text: 'Your router hands out the address. Right now it’s ', ip: currentIp || '—' }
}

// --- Wi-Fi form --------------------------------------------------------------

export interface WifiForm {
  enabled: boolean
  ssid: string
  psk: string
}
export interface WifiSaved {
  passphraseSet: boolean
  savedSsid: string
}
export interface WifiErrors {
  ssid?: string
  psk?: string
}

function utf8Length(s: string): number {
  return new TextEncoder().encode(s).length
}

// A stored passphrase belongs to the saved network; a different SSID needs its own.
export function needsPassphrase(f: WifiForm, saved: WifiSaved): boolean {
  return !saved.passphraseSet || f.ssid !== saved.savedSsid
}

export function validateWifi(f: WifiForm, saved: WifiSaved): WifiErrors {
  const e: WifiErrors = {}
  if (!f.enabled) return e
  if (!f.ssid) e.ssid = 'Enter the network’s name, or pick one from Scan.'
  else if (utf8Length(f.ssid) > 32) e.ssid = 'Network names are at most 32 characters.'
  else if (/[\x00-\x1f\x7f]/.test(f.ssid)) e.ssid = 'Network names can’t contain control characters.'

  if (f.psk) {
    if (f.psk.length < 8 || f.psk.length > 63) e.psk = 'Wi-Fi passwords are 8 to 63 characters.'
    else if (!/^[\x20-\x7e]*$/.test(f.psk)) e.psk = 'Use letters, digits and ordinary punctuation only.'
  } else if (needsPassphrase(f, saved)) {
    e.psk = f.ssid ? `Enter the password for “${f.ssid}”.` : 'Enter the network’s password.'
  }
  return e
}

export const WPA_NOTE = 'WPA2 and mixed WPA2/WPA3 networks work; WPA3-only isn’t supported yet.'

export function passwordCopy(ssid: string, saved: WifiSaved): { placeholder: string; hint: string } {
  if (!saved.passphraseSet) {
    return {
      placeholder: '8–63 characters',
      hint: 'WPA2 and mixed WPA2/WPA3 networks work; WPA3-only networks aren’t supported yet.',
    }
  }
  if (ssid === saved.savedSsid) {
    return {
      placeholder: '•••••••• (saved — leave blank to keep)',
      hint: `A password is saved. Leave this blank to keep it, or type a new one (8–63 characters). ${WPA_NOTE}`,
    }
  }
  return {
    placeholder: `Password for “${ssid}”`,
    hint: `This is a different network, so enter its password (8–63 characters). ${WPA_NOTE}`,
  }
}

export function wifiSaveMessage(uplink: Uplink | undefined, enabled: boolean): string {
  if (!enabled) return 'Saved. Wi-Fi is off.'
  if (uplink === 'wifi') return 'Saved — reconnecting…'
  if (uplink === 'none') return 'Saved — connecting…'
  return 'Saved. Wi-Fi is on standby until the cable is unplugged.'
}

// --- scan --------------------------------------------------------------------

export interface ScanGroup {
  ssid: string
  channels: number[]
}
export interface GroupedScan {
  networks: ScanGroup[]
  hidden: number
}

// One row per SSID (the scan reports one entry per BSSID), hidden ones only
// counted. The network currently typed in goes first, then A→Z.
export function groupScan(list: ScanEntry[], current = ''): GroupedScan {
  const map = new Map<string, Set<number>>()
  let hidden = 0
  for (const n of list ?? []) {
    if (!n || !n.ssid) { hidden++; continue }
    let set = map.get(n.ssid)
    if (!set) map.set(n.ssid, (set = new Set()))
    if (Number.isFinite(n.channel) && n.channel > 0) set.add(n.channel)
  }
  const networks = [...map.entries()].map(([ssid, ch]) => ({ ssid, channels: [...ch].sort((a, b) => a - b) }))
  networks.sort((a, b) => {
    if (current) {
      if (a.ssid === current && b.ssid !== current) return -1
      if (b.ssid === current && a.ssid !== current) return 1
    }
    const x = a.ssid.toLowerCase()
    const y = b.ssid.toLowerCase()
    return x < y ? -1 : x > y ? 1 : a.ssid < b.ssid ? -1 : a.ssid > b.ssid ? 1 : 0
  })
  return { networks, hidden }
}

export function channelLabel(ch: number[]): string {
  return ch.length ? `ch ${ch.join(', ')}` : ''
}

export function scanSummary(s: GroupedScan): { hint: string; announce: string } {
  const n = s.networks.length
  if (n === 0 && s.hidden === 0) {
    const t = 'No networks found. Move the camera closer to the router and scan again.'
    return { hint: t, announce: t }
  }
  if (n === 0) {
    const t = 'Only hidden networks nearby. Type your network’s name above.'
    return { hint: t, announce: t }
  }
  const hint = s.hidden > 0
    ? `${s.hidden} hidden network${s.hidden === 1 ? '' : 's'} nearby ${s.hidden === 1 ? 'isn’t' : 'aren’t'} listed — type the name above if yours is one of them.`
    : ''
  return { hint, announce: `${n} network${n === 1 ? '' : 's'} found.` }
}

// --- signal ------------------------------------------------------------------

export interface Signal {
  bars: 0 | 1 | 2 | 3 | 4
  word: string
}
export function signal(rssi: number): Signal {
  if (!Number.isFinite(rssi) || rssi < 0) return { bars: 0, word: 'signal unknown' }
  if (rssi < 25) return { bars: 1, word: 'Weak' }
  if (rssi < 50) return { bars: 2, word: 'Fair' }
  if (rssi < 75) return { bars: 3, word: 'Good' }
  return { bars: 4, word: 'Excellent' }
}

// --- connection card lanes ---------------------------------------------------

export type BadgeKind = 'ok' | 'plain' | 'warn' | 'bad'
export type LaneTone = 'active' | 'quiet' | 'warn' | 'bad'

export interface Lane {
  title: string          // "Cable" | "Wi-Fi" — the SSID is separate so it can be quoted/styled
  ssid?: string
  badge: string
  badgeKind: BadgeKind
  tone: LaneTone
  detail?: string        // a sentence, when there's no address to show
  ip?: string
  mac?: string
  signal?: Signal
  spin?: boolean
}

const UNKNOWN_LANE = { badge: '…', badgeKind: 'plain' as const, tone: 'quiet' as const, detail: '—' }

export function describeCableLane(net: NetStatus | null | undefined): Lane {
  if (!net) return { title: 'Cable', ...UNKNOWN_LANE }
  if (net.uplink === 'wired') {
    return { title: 'Cable', badge: 'ACTIVE', badgeKind: 'ok', tone: 'active', ip: net.ip || '—', mac: net.mac }
  }
  if (!net.wired_link) {
    return {
      title: 'Cable', badge: 'UNPLUGGED', badgeKind: 'plain', tone: 'quiet',
      detail: 'Plug the cable in and the camera switches back to it within about 10 seconds.',
    }
  }
  return { title: 'Cable', badge: 'CONNECTING', badgeKind: 'warn', tone: 'warn', detail: 'Cable detected — switching over…' }
}

export function describeWifiLane(net: NetStatus | null | undefined): Lane {
  if (!net) return { title: 'Wi-Fi', ...UNKNOWN_LANE }
  const w = net.wifi
  const base = { title: 'Wi-Fi', ssid: w.ssid || undefined }
  switch (w.state) {
    case 'off':
      return { ...base, badge: 'OFF', badgeKind: 'plain', tone: 'quiet', detail: 'Off. Turn it on below so the camera has a backup if the cable goes.' }
    case 'standby':
      return w.ssid
        ? { ...base, badge: 'STANDBY', badgeKind: 'plain', tone: 'quiet', detail: 'Takes over by itself if the cable is unplugged.' }
        : { ...base, badge: 'NOT SET UP', badgeKind: 'plain', tone: 'quiet', detail: 'No network chosen yet — pick one below.' }
    case 'connecting':
      return { ...base, badge: 'CONNECTING', badgeKind: 'warn', tone: 'warn', detail: 'Connecting…', spin: true }
    case 'associated':
      return { ...base, badge: 'CONNECTING', badgeKind: 'warn', tone: 'warn', detail: 'Joined the network, waiting for an address from the router…' }
    case 'connected':
      return net.uplink === 'wifi'
        ? { ...base, badge: 'ACTIVE', badgeKind: 'ok', tone: 'active', ip: w.ip || net.ip, mac: net.mac, signal: signal(w.rssi) }
        : { ...base, badge: 'CONNECTED', badgeKind: 'plain', tone: 'quiet', ip: w.ip || '—', signal: signal(w.rssi) }
    case 'wrong_passphrase':
      return {
        ...base, badge: 'WRONG PASSWORD', badgeKind: 'bad', tone: 'bad',
        detail: 'The network rejected the saved password. Enter the correct one below and save.',
      }
    default:
      return { ...base, badge: String(w.state ?? '…').toUpperCase(), badgeKind: 'plain', tone: 'quiet', detail: '—' }
  }
}

export const CARD_NOTE =
  'Cable first, Wi-Fi as backup. The camera uses one hardware address on both, so it normally keeps the same IP either way.'
export const RESCUE_NOTE =
  'Right now neither is up — you’re probably reaching the camera on its rescue address, 192.168.2.3.'

// --- Wi-Fi test --------------------------------------------------------------

// Backend results plus the two client-side outcomes.
export type TestOutcome = WifiTestResult | 'conflict' | 'unreachable'
export type Tone = 'ok' | 'warn' | 'bad'

export interface TestCopy {
  tone: Tone
  text: string
  lead?: string // bold prefix of `text`
}

export const WIFI_LIVE_TEXT = 'You’re on Wi-Fi right now — the Connection card above is the live result.'

export function describeTestResult(result: TestOutcome, ssid: string, ip = '', dirty = true, retryIn = 0): TestCopy {
  const q = `“${ssid}”`
  switch (result) {
    case 'ok':
      return dirty
        ? { tone: 'ok', text: `Connected to ${q} and got the address ${ip || 'an address'}. Press Save to keep these settings.` }
        : { tone: 'ok', text: `Connected to ${q} and got ${ip || 'an address'}. Wi-Fi is ready to take over.` }
    case 'wrong_passphrase':
      return { tone: 'bad', lead: 'Wrong password', text: `Wrong password for ${q}. Check it and try again.` }
    case 'not_found':
      return { tone: 'bad', text: `Couldn’t find ${q}. Check the name (capital letters matter), move the camera closer to the router, or scan again.` }
    case 'no_dhcp':
      return { tone: 'bad', text: `Joined ${q} but got no address from the router. Check the router’s DHCP settings and try again.` }
    case 'busy':
      return retryIn > 0
        ? { tone: 'warn', text: `The router briefly blocks new attempts after a wrong password — try again in ${retryIn} seconds.` }
        : { tone: 'warn', text: 'The camera’s Wi-Fi is busy right now — try again in a few seconds.' }
    case 'conflict':
      return { tone: 'warn', text: WIFI_LIVE_TEXT }
    case 'unreachable':
      return { tone: 'bad', text: 'Couldn’t reach the camera to run the test. Check your own connection and try again.' }
    case 'error':
    default:
      return { tone: 'bad', text: 'The test failed unexpectedly. Try again; if it keeps failing, check the camera’s log on the System tab.' }
  }
}

// --- hostname ----------------------------------------------------------------

const HOSTNAME = /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/
export function validateHostname(s: string): string | undefined {
  return HOSTNAME.test(s)
    ? undefined
    : 'Use letters, digits and hyphens only (1–63 characters, not starting or ending with a hyphen).'
}

// --- address plan & countdown -------------------------------------------------

export type PlanKind = 'same' | 'moves' | 'maybe' | 'dhcp'
export interface AddressPlan {
  kind: PlanKind
  url?: string
  ip?: string
}
export interface PageLocation {
  protocol: string
  hostname: string
  port: string
}

// Where this page will find the camera after a wired change. `target` is the
// form (before sending) or the response's {mode, ip} (which wins if it differs).
export function addressPlan(
  target: { mode: NetMode; ip: string },
  committedMode: NetMode | undefined,
  loc: PageLocation,
): AddressPlan {
  if (target.mode === 'dhcp') return committedMode === 'static' ? { kind: 'dhcp' } : { kind: 'same' }
  const ip = target.ip.trim()
  if (ip === loc.hostname) return { kind: 'same' }
  const url = `${loc.protocol}//${ip}${loc.port ? `:${loc.port}` : ''}/`
  return { kind: isIPv4(loc.hostname) ? 'moves' : 'maybe', url, ip }
}

export function formatCountdown(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds))
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`
}

export type CountdownTone = 'accent' | 'warn' | 'bad'
export function countdownTone(left: number): CountdownTone {
  return left <= 10 ? 'bad' : left <= 30 ? 'warn' : 'accent'
}

// Screen-reader milestones, announced once each as the window runs down.
export function countdownMilestone(left: number): string {
  if (left <= 0) return ''
  if (left <= 10) return '10 seconds left'
  if (left <= 30) return '30 seconds left'
  if (left <= 60) return 'About one minute left'
  return ''
}
