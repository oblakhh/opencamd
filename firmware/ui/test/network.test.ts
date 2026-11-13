// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Unit tests for the Network tab helpers (utils/network.ts).

import { describe, it, expect } from 'vitest'
import type { NetStatus } from '../src/types'
import {
  isIPv4, isNetmask, sameSubnet, validateWired, validateWifi, validateHostname, isValid,
  joinDns, splitDns, guessGateway, signal, groupScan, scanSummary, channelLabel,
  describeCableLane, describeWifiLane, describeTestResult, addressPlan, formatCountdown,
  countdownTone, countdownMilestone, passwordCopy, addressingHint, wifiSaveMessage, needsPassphrase,
  type WiredForm,
} from '../src/utils/network'

const stat: WiredForm = { mode: 'static', ip: '192.0.2.80', netmask: '255.255.255.0', gateway: '192.0.2.1', dns1: '192.0.2.1', dns2: '1.1.1.1' }

describe('address primitives', () => {
  it('accepts only canonical dotted quads', () => {
    expect(isIPv4('192.0.2.80')).toBe(true)
    expect(isIPv4('0.0.0.0')).toBe(true)
    expect(isIPv4('255.255.255.255')).toBe(true)
    for (const bad of ['', '192.0.2', '192.0.2.256', '010.1.1.1', '1.2.3.4.5', ' 1.2.3.4', '1.2.3.x', '1..2.3']) {
      expect(isIPv4(bad), bad).toBe(false)
    }
  })

  it('accepts contiguous masks /1–/30 only', () => {
    expect(isNetmask('255.255.255.0')).toBe(true)
    expect(isNetmask('128.0.0.0')).toBe(true)
    expect(isNetmask('255.255.255.252')).toBe(true)
    expect(isNetmask('255.255.255.254')).toBe(false) // /31
    expect(isNetmask('255.255.255.255')).toBe(false)
    expect(isNetmask('0.0.0.0')).toBe(false)
    expect(isNetmask('255.0.255.0')).toBe(false)
    expect(isNetmask('255.255.255.1')).toBe(false)
  })

  it('compares subnets', () => {
    expect(sameSubnet('192.0.2.80', '192.0.2.1', '255.255.255.0')).toBe(true)
    expect(sameSubnet('192.0.2.80', '192.0.3.1', '255.255.255.0')).toBe(false)
    expect(sameSubnet('192.0.2.80', '192.0.3.1', '255.255.0.0')).toBe(true)
  })
})

describe('validateWired', () => {
  it('ignores the fields entirely for DHCP', () => {
    expect(validateWired({ ...stat, mode: 'dhcp', ip: 'junk', netmask: '' })).toEqual({})
  })

  it('accepts a good static config, gateway and DNS optional', () => {
    expect(validateWired(stat)).toEqual({})
    expect(validateWired({ ...stat, gateway: '', dns1: '', dns2: '' })).toEqual({})
  })

  it('rejects malformed and unusable addresses', () => {
    expect(validateWired({ ...stat, ip: '192.0.2' }).ip).toMatch(/IPv4 address like/)
    expect(validateWired({ ...stat, ip: '' }).ip).toMatch(/IPv4 address like/)
    for (const ip of ['0.0.0.0', '255.255.255.255', '127.0.0.1', '224.0.0.5', '239.1.1.1']) {
      expect(validateWired({ ...stat, ip, gateway: '' }).ip, ip).toMatch(/can’t be used/)
    }
  })

  it('rejects bad netmasks and network/broadcast host parts', () => {
    expect(validateWired({ ...stat, netmask: '255.0.255.0' }).netmask).toMatch(/netmask like/)
    expect(validateWired({ ...stat, ip: '192.0.2.0', gateway: '' }).ip).toMatch(/network\/broadcast/)
    expect(validateWired({ ...stat, ip: '192.0.2.255', gateway: '' }).ip).toMatch(/network\/broadcast/)
    expect(validateWired({ ...stat, ip: '192.0.2.127', netmask: '255.255.255.128', gateway: '' }).ip).toMatch(/broadcast/)
  })

  it('checks the gateway like netcfg_validate', () => {
    expect(validateWired({ ...stat, gateway: '192.0.2' }).gateway).toMatch(/gateway address like/)
    expect(validateWired({ ...stat, gateway: '192.0.3.1' }).gateway).toMatch(/same network/)
    expect(validateWired({ ...stat, gateway: '192.0.2.80' }).gateway).toMatch(/own address/)
    expect(validateWired({ ...stat, gateway: '192.0.2.255' }).gateway).toMatch(/broadcast/)
  })

  it('checks each DNS server independently', () => {
    const e = validateWired({ ...stat, dns1: 'dns.google', dns2: '8.8.8.8' })
    expect(e.dns1).toMatch(/DNS server address/)
    expect(e.dns2).toBeUndefined()
    expect(validateWired({ ...stat, dns2: '1.1.1' }).dns2).toBeTruthy()
    expect(isValid(validateWired({ ...stat, dns2: '1.1.1' }))).toBe(false)
  })
})

describe('DNS join/split and prefill', () => {
  it('joins with one space, dropping blanks', () => {
    expect(joinDns('192.0.2.1', '1.1.1.1')).toBe('192.0.2.1 1.1.1.1')
    expect(joinDns('', ' 1.1.1.1 ')).toBe('1.1.1.1')
    expect(joinDns('', '')).toBe('')
  })
  it('splits on spaces or commas', () => {
    expect(splitDns('192.0.2.1 1.1.1.1')).toEqual(['192.0.2.1', '1.1.1.1'])
    expect(splitDns('192.0.2.1,1.1.1.1')).toEqual(['192.0.2.1', '1.1.1.1'])
    expect(splitDns('1.1.1.1')).toEqual(['1.1.1.1', ''])
    expect(splitDns('')).toEqual(['', ''])
  })
  it('guesses x.y.z.1 as the gateway', () => {
    expect(guessGateway('192.0.2.65')).toBe('192.0.2.1')
    expect(guessGateway('nope')).toBe('')
  })
})

describe('validateWifi', () => {
  const savedSame = { passphraseSet: true, savedSsid: 'Home-2G' }
  const none = { passphraseSet: false, savedSsid: '' }

  it('validates nothing while disabled', () => {
    expect(validateWifi({ enabled: false, ssid: '', psk: '' }, none)).toEqual({})
  })

  it('requires an SSID of at most 32 bytes', () => {
    expect(validateWifi({ enabled: true, ssid: '', psk: '' }, savedSame).ssid).toMatch(/pick one from Scan/)
    expect(validateWifi({ enabled: true, ssid: 'x'.repeat(33), psk: 'password1' }, none).ssid).toMatch(/at most 32/)
    // 11 × 3-byte characters = 33 bytes
    expect(validateWifi({ enabled: true, ssid: '€'.repeat(11), psk: 'password1' }, none).ssid).toMatch(/at most 32/)
    expect(validateWifi({ enabled: true, ssid: 'x'.repeat(32), psk: 'password1' }, none).ssid).toBeUndefined()
  })

  it('checks the passphrase length and character set', () => {
    expect(validateWifi({ enabled: true, ssid: 'Home-2G', psk: 'short' }, savedSame).psk).toMatch(/8 to 63/)
    expect(validateWifi({ enabled: true, ssid: 'Home-2G', psk: 'x'.repeat(64) }, savedSame).psk).toMatch(/8 to 63/)
    expect(validateWifi({ enabled: true, ssid: 'Home-2G', psk: 'pässwörter' }, savedSame).psk).toMatch(/ordinary punctuation/)
    expect(validateWifi({ enabled: true, ssid: 'Home-2G', psk: 'ok pass ~!' }, savedSame).psk).toBeUndefined()
  })

  it('lets a blank password keep the stored one only for the same SSID', () => {
    expect(validateWifi({ enabled: true, ssid: 'Home-2G', psk: '' }, savedSame)).toEqual({})
    expect(validateWifi({ enabled: true, ssid: 'Other', psk: '' }, savedSame).psk).toBe('Enter the password for “Other”.')
    expect(validateWifi({ enabled: true, ssid: 'Home-2G', psk: '' }, none).psk).toBe('Enter the password for “Home-2G”.')
    expect(needsPassphrase({ enabled: true, ssid: 'Home-2G', psk: '' }, savedSame)).toBe(false)
  })
})

describe('validateHostname', () => {
  it('follows the RFC 1123 label rule', () => {
    expect(validateHostname('ic3115w')).toBeUndefined()
    expect(validateHostname('printer-cam-2')).toBeUndefined()
    expect(validateHostname('a')).toBeUndefined()
    expect(validateHostname('x'.repeat(63))).toBeUndefined()
    for (const bad of ['', '-cam', 'cam-', 'my cam', 'cam.local', 'x'.repeat(64), 'cäm']) {
      expect(validateHostname(bad), bad).toMatch(/letters, digits and hyphens/)
    }
  })
})

describe('signal', () => {
  it('maps rssi 0..100 to bars and a word', () => {
    expect(signal(-1)).toEqual({ bars: 0, word: 'signal unknown' })
    expect(signal(0)).toEqual({ bars: 1, word: 'Weak' })
    expect(signal(24).bars).toBe(1)
    expect(signal(25)).toEqual({ bars: 2, word: 'Fair' })
    expect(signal(62)).toEqual({ bars: 3, word: 'Good' })
    expect(signal(75)).toEqual({ bars: 4, word: 'Excellent' })
    expect(signal(100).bars).toBe(4)
  })
})

describe('groupScan', () => {
  const list = [
    { ssid: 'Home-2G', bssid: 'a', channel: 6 },
    { ssid: '', bssid: 'b', channel: 11 },
    { ssid: 'neighbour', bssid: 'c', channel: 11 },
    { ssid: 'Home-2G', bssid: 'd', channel: 1 },
    { ssid: '', bssid: 'e', channel: 1 },
    { ssid: 'PrinterRoom', bssid: 'f', channel: 11 },
    { ssid: 'Home-2G', bssid: 'g', channel: 6 },
  ]

  it('drops hidden SSIDs, dedups per SSID and collects sorted channels', () => {
    const g = groupScan(list)
    expect(g.hidden).toBe(2)
    expect(g.networks).toEqual([
      { ssid: 'Home-2G', channels: [1, 6] },
      { ssid: 'neighbour', channels: [11] },
      { ssid: 'PrinterRoom', channels: [11] },
    ])
  })

  it('puts the typed SSID first, the rest A→Z case-insensitively', () => {
    expect(groupScan(list, 'PrinterRoom').networks.map((n) => n.ssid)).toEqual(['PrinterRoom', 'Home-2G', 'neighbour'])
    expect(groupScan(list, 'not-there').networks.map((n) => n.ssid)).toEqual(['Home-2G', 'neighbour', 'PrinterRoom'])
  })

  it('labels channels and summarises every outcome', () => {
    expect(channelLabel([6])).toBe('ch 6')
    expect(channelLabel([1, 11])).toBe('ch 1, 11')
    expect(scanSummary(groupScan([]))).toEqual({
      hint: 'No networks found. Move the camera closer to the router and scan again.',
      announce: 'No networks found. Move the camera closer to the router and scan again.',
    })
    expect(scanSummary(groupScan([{ ssid: '', bssid: 'x', channel: 1 }])).hint).toBe('Only hidden networks nearby. Type your network’s name above.')
    const s = scanSummary(groupScan(list))
    expect(s.announce).toBe('3 networks found.')
    expect(s.hint).toBe('2 hidden networks nearby aren’t listed — type the name above if yours is one of them.')
    const one = scanSummary(groupScan([{ ssid: 'A', bssid: '1', channel: 1 }, { ssid: '', bssid: '2', channel: 1 }]))
    expect(one.announce).toBe('1 network found.')
    expect(one.hint).toMatch(/^1 hidden network nearby isn’t listed/)
    expect(scanSummary(groupScan([{ ssid: 'A', bssid: '1', channel: 1 }])).hint).toBe('')
  })
})

function mkNet(over: Partial<NetStatus> = {}, wifi: Partial<NetStatus['wifi']> = {}): NetStatus {
  return {
    uplink: 'wired', iface: 'eth0', ip: '192.0.2.65', mac: '02:00:00:00:00:02', wired_link: true,
    mode: 'dhcp', revert_in: 0,
    ...over,
    wifi: { state: 'standby', ssid: 'Home-2G', bssid: '', ip: '', rssi: -1, ...wifi },
  }
}

describe('describeCableLane', () => {
  it('covers unknown, active, unplugged and switching back', () => {
    expect(describeCableLane(null)).toMatchObject({ badge: '…', detail: '—' })
    expect(describeCableLane(mkNet())).toMatchObject({ badge: 'ACTIVE', badgeKind: 'ok', tone: 'active', ip: '192.0.2.65', mac: '02:00:00:00:00:02' })
    expect(describeCableLane(mkNet({ uplink: 'none', wired_link: false }))).toMatchObject({ badge: 'UNPLUGGED', badgeKind: 'plain' })
    expect(describeCableLane(mkNet({ uplink: 'wifi', wired_link: false })).detail).toMatch(/within about 10 seconds/)
    expect(describeCableLane(mkNet({ uplink: 'wifi', wired_link: true }))).toMatchObject({
      badge: 'CONNECTING', badgeKind: 'warn', detail: 'Cable detected — switching over…',
    })
  })
})

describe('describeWifiLane', () => {
  it('covers every wifi.state', () => {
    expect(describeWifiLane(null)).toMatchObject({ badge: '…' })
    expect(describeWifiLane(mkNet({}, { state: 'off', ssid: '' }))).toMatchObject({ badge: 'OFF', ssid: undefined })
    expect(describeWifiLane(mkNet())).toMatchObject({ badge: 'STANDBY', ssid: 'Home-2G', detail: 'Takes over by itself if the cable is unplugged.' })
    expect(describeWifiLane(mkNet({}, { ssid: '' }))).toMatchObject({ badge: 'NOT SET UP', detail: 'No network chosen yet — pick one below.' })
    expect(describeWifiLane(mkNet({}, { state: 'connecting' }))).toMatchObject({ badge: 'CONNECTING', badgeKind: 'warn', spin: true, detail: 'Connecting…' })
    expect(describeWifiLane(mkNet({}, { state: 'associated' })).detail).toMatch(/waiting for an address/)
    expect(describeWifiLane(mkNet({ uplink: 'wifi' }, { state: 'connected', ip: '192.0.2.65', rssi: 62 }))).toMatchObject({
      badge: 'ACTIVE', tone: 'active', ip: '192.0.2.65', mac: '02:00:00:00:00:02', signal: { bars: 3, word: 'Good' },
    })
    const handBack = describeWifiLane(mkNet({ uplink: 'wired' }, { state: 'connected', ip: '192.0.2.65', rssi: 90 }))
    expect(handBack).toMatchObject({ badge: 'CONNECTED', badgeKind: 'plain', signal: { bars: 4 } })
    expect(handBack.mac).toBeUndefined()
    expect(describeWifiLane(mkNet({}, { state: 'wrong_passphrase' }))).toMatchObject({ badge: 'WRONG PASSWORD', badgeKind: 'bad', tone: 'bad' })
  })
})

describe('describeTestResult', () => {
  it('words every outcome', () => {
    expect(describeTestResult('ok', 'Home-2G', '192.0.2.151', true)).toEqual({
      tone: 'ok', text: 'Connected to “Home-2G” and got the address 192.0.2.151. Press Save to keep these settings.',
    })
    expect(describeTestResult('ok', 'Home-2G', '192.0.2.151', false).text).toBe('Connected to “Home-2G” and got 192.0.2.151. Wi-Fi is ready to take over.')
    const wrong = describeTestResult('wrong_passphrase', 'Home-2G')
    expect(wrong).toEqual({ tone: 'bad', lead: 'Wrong password', text: 'Wrong password for “Home-2G”. Check it and try again.' })
    expect(wrong.text.startsWith(wrong.lead!)).toBe(true)
    expect(describeTestResult('not_found', 'Nope').text).toMatch(/^Couldn’t find “Nope”/)
    expect(describeTestResult('no_dhcp', 'X').text).toMatch(/^Joined “X” but got no address/)
    expect(describeTestResult('busy', 'X', '', true, 25).text).toMatch(/try again in 25 seconds/)
    expect(describeTestResult('busy', 'X').text).toMatch(/a few seconds/)
    expect(describeTestResult('busy', 'X').tone).toBe('warn')
    expect(describeTestResult('error', 'X').text).toMatch(/log on the System tab/)
    expect(describeTestResult('conflict', 'X')).toEqual({ tone: 'warn', text: 'You’re on Wi-Fi right now — the Connection card above is the live result.' })
    expect(describeTestResult('unreachable', 'X').text).toMatch(/^Couldn’t reach the camera to run the test/)
  })
})

describe('copy selection', () => {
  it('picks the password placeholder and hint', () => {
    expect(passwordCopy('Home-2G', { passphraseSet: false, savedSsid: '' }).placeholder).toBe('8–63 characters')
    expect(passwordCopy('Home-2G', { passphraseSet: true, savedSsid: 'Home-2G' })).toMatchObject({
      placeholder: '•••••••• (saved — leave blank to keep)',
    })
    expect(passwordCopy('Home-2G', { passphraseSet: true, savedSsid: 'Home-2G' }).hint).toMatch(/^A password is saved/)
    const other = passwordCopy('Other', { passphraseSet: true, savedSsid: 'Home-2G' })
    expect(other.placeholder).toBe('Password for “Other”')
    expect(other.hint).toMatch(/^This is a different network/)
  })

  it('picks the addressing hint', () => {
    expect(addressingHint('dhcp', 'dhcp', '192.0.2.65')).toEqual({ text: 'Your router hands out the address. Right now it’s ', ip: '192.0.2.65' })
    expect(addressingHint('dhcp', 'static', '192.0.2.80').text).toMatch(/^Switching to automatic/)
    expect(addressingHint('static', 'dhcp', '').text).toBe('Fixed address. Fill in at least the IP address and netmask.')
  })

  it('picks the Wi-Fi save message by uplink', () => {
    expect(wifiSaveMessage('wired', true)).toBe('Saved. Wi-Fi is on standby until the cable is unplugged.')
    expect(wifiSaveMessage('wifi', true)).toBe('Saved — reconnecting…')
    expect(wifiSaveMessage('none', true)).toBe('Saved — connecting…')
    expect(wifiSaveMessage('wifi', false)).toBe('Saved. Wi-Fi is off.')
  })
})

describe('addressPlan', () => {
  const byIp = { protocol: 'http:', hostname: '192.0.2.65', port: '' }
  const byName = { protocol: 'http:', hostname: 'camera.local', port: '8080' }

  it('classifies static targets by how this page was opened', () => {
    expect(addressPlan({ mode: 'static', ip: '192.0.2.65' }, 'dhcp', byIp)).toEqual({ kind: 'same' })
    expect(addressPlan({ mode: 'static', ip: '192.0.2.80' }, 'dhcp', byIp)).toEqual({ kind: 'moves', url: 'http://192.0.2.80/', ip: '192.0.2.80' })
    expect(addressPlan({ mode: 'static', ip: '192.0.2.80' }, 'static', byName)).toEqual({ kind: 'maybe', url: 'http://192.0.2.80:8080/', ip: '192.0.2.80' })
  })

  it('classifies DHCP targets by the committed mode', () => {
    expect(addressPlan({ mode: 'dhcp', ip: '' }, 'static', byIp)).toEqual({ kind: 'dhcp' })
    expect(addressPlan({ mode: 'dhcp', ip: '' }, 'dhcp', byIp)).toEqual({ kind: 'same' })
  })
})

describe('countdown', () => {
  it('formats m:ss', () => {
    expect(formatCountdown(90)).toBe('1:30')
    expect(formatCountdown(72)).toBe('1:12')
    expect(formatCountdown(7)).toBe('0:07')
    expect(formatCountdown(0)).toBe('0:00')
    expect(formatCountdown(-3)).toBe('0:00')
  })
  it('changes tone at 30 s and 10 s', () => {
    expect(countdownTone(31)).toBe('accent')
    expect(countdownTone(30)).toBe('warn')
    expect(countdownTone(10)).toBe('bad')
  })
  it('names the screen-reader milestones', () => {
    expect(countdownMilestone(75)).toBe('')
    expect(countdownMilestone(60)).toBe('About one minute left')
    expect(countdownMilestone(30)).toBe('30 seconds left')
    expect(countdownMilestone(9)).toBe('10 seconds left')
    expect(countdownMilestone(0)).toBe('')
  })
})
