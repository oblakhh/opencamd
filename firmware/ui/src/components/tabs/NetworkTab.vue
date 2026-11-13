<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Settings > Network: wired (DHCP/static) and Wi-Fi configuration, scan,
  connection test and uplink status.
-->
<script setup lang="ts">
import { computed, nextTick, onUnmounted, reactive, ref, watch } from 'vue'
import type { Config, ConfigPatch, ScanEntry, Status, WifiSave, WiredConfig } from '../../types'
import { api, ApiError } from '../../api'
import { useAction } from '../../composables/useAction'
import { useDirty } from '../../composables/useDirty'
import { useRevertWindow } from '../../composables/useRevertWindow'
import {
  CARD_NOTE, RESCUE_NOTE, WIFI_LIVE_TEXT,
  addressPlan, addressingHint, channelLabel, describeCableLane, describeTestResult, describeWifiLane,
  groupScan, guessGateway, isValid, joinDns, passwordCopy, scanSummary, splitDns, validateHostname,
  validateWifi, validateWired, wifiSaveMessage,
  type AddressPlan, type GroupedScan, type TestCopy, type TestOutcome, type WiredForm,
} from '../../utils/network'
import ActionBar from '../ActionBar.vue'
import PasswordInput from '../PasswordInput.vue'
import RevertBanner from '../RevertBanner.vue'
import Icon from '../Icon.vue'

const props = defineProps<{
  cfg: Config
  status: Status | null
  save: (patch: ConfigPatch) => Promise<Config | undefined>
}>()
// `applied` hands the address plan to App's floating banner; `refresh` asks
// the modal to reload `cfg` (network changes don't go through `save`).
const emit = defineEmits<{ applied: [plan: AddressPlan]; refresh: [] }>()

const INVALID_MSG = 'Fix the highlighted fields first.'
const TEST_TIMEOUT_MS = 40_000

let alive = true
onUnmounted(() => { alive = false })

// --- live state (Connection card) ---
const net = computed(() => props.status?.net ?? null)
const uplink = computed(() => net.value?.uplink)
const cableLane = computed(() => describeCableLane(net.value))
const wifiLane = computed(() => describeWifiLane(net.value))
const lanes = computed(() => [
  { key: 'cable', icon: 'cable' as const, live: false, lane: cableLane.value },
  { key: 'wifi', icon: 'wifi' as const, live: true, lane: wifiLane.value },
])
const wrongPw = computed(() => net.value?.wifi.state === 'wrong_passphrase')

// --- Wi-Fi backup ---
const wifi = reactive({
  enabled: props.cfg.network.wifi.enabled,
  ssid: props.cfg.network.wifi.ssid,
  psk: '', // empty = keep the stored one
})
const saved = reactive({
  passphraseSet: props.cfg.network.wifi.passphrase_set,
  savedSsid: props.cfg.network.wifi.ssid,
})
const wifiDirty = useDirty(() => wifi)
const wifiAct = useAction()
const wifiTouched = reactive({ ssid: false, psk: false })
const wifiAttempted = ref(false)
const wifiErrors = computed(() => validateWifi(wifi, saved))
const wifiValid = computed(() => isValid(wifiErrors.value))
watch(wifiValid, (v) => { if (v && wifiAct.errMsg.value === INVALID_MSG) wifiAct.errMsg.value = '' })
const ssidErr = computed(() => (wifiTouched.ssid || wifiAttempted.value ? wifiErrors.value.ssid : undefined))
const pskErr = computed(() => (wifiTouched.psk || wifiAttempted.value ? wifiErrors.value.psk : undefined))

// Scan
const scanBusy = ref(false)
const scanFailed = ref(false)
const scan = ref<GroupedScan | null>(null)
const scanText = computed(() => (scan.value ? scanSummary(scan.value) : null))

// Test
const testBusy = ref(false)
const testSsid = ref('')
const test = ref<TestCopy | null>(null)
const testWrong = ref(false) // the last test said "wrong password"
let testSeq = 0

const busyAny = computed(() => testBusy.value || wifiAct.busy.value)
const canScan = computed(() => !scanBusy.value && !busyAny.value)
const canTest = computed(
  () => wifi.enabled && wifiValid.value && !busyAny.value && !scanBusy.value && uplink.value !== 'wifi',
)

async function runScan() {
  if (!canScan.value) return
  scanBusy.value = true
  scanFailed.value = false
  try {
    const list = await api.wifiScan()
    if (!alive) return
    // Grouped (and ordered) once, so the list doesn't reshuffle while typing.
    scan.value = groupScan(Array.isArray(list) ? (list as ScanEntry[]) : [], wifi.ssid)
  } catch {
    if (!alive) return
    scan.value = null
    scanFailed.value = true
  } finally {
    scanBusy.value = false
  }
}

function focusPsk() {
  void nextTick(() => document.getElementById('psk')?.focus())
}
function pick(ssid: string) {
  wifi.ssid = ssid
  wifiTouched.ssid = true
  focusPsk()
}

function withTimeout<T>(p: Promise<T>, ms: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const t = window.setTimeout(() => reject(new Error('timeout')), ms)
    p.then((v) => { clearTimeout(t); resolve(v) }, (e) => { clearTimeout(t); reject(e) })
  })
}

async function runTest() {
  if (!canTest.value) return
  const seq = ++testSeq
  const ssid = wifi.ssid
  testBusy.value = true
  testSsid.value = ssid
  test.value = null
  testWrong.value = false
  let outcome: TestOutcome = 'error'
  let ip = ''
  let retryIn = 0
  let refused = ''
  try {
    const res = await withTimeout(api.wifiTest(ssid, wifi.psk || undefined), TEST_TIMEOUT_MS)
    outcome = res?.result ?? 'error'
    ip = res?.ip ?? ''
    retryIn = res?.retry_in ?? 0
  } catch (e) {
    if (e instanceof ApiError && e.status === 409) outcome = 'conflict'
    else if (e instanceof ApiError && e.status === 400) refused = e.message
    else if (e instanceof ApiError && e.status !== 0) outcome = 'error'
    else outcome = 'unreachable'
  }
  // The test keeps running on the device after we leave; ignore late answers.
  if (!alive || seq !== testSeq) return
  testBusy.value = false
  test.value = refused
    ? { tone: 'bad', text: `The camera refused the test: ${refused}.` }
    : describeTestResult(outcome, ssid, ip, wifiDirty.dirty.value, retryIn)
  if (outcome === 'wrong_passphrase' && !refused) {
    testWrong.value = true
    focusPsk()
  }
}

// Any edit makes a previous result stale.
watch(() => [wifi.ssid, wifi.psk], () => {
  if (testBusy.value) return
  test.value = null
  testWrong.value = false
})

const pwCopy = computed(() => passwordCopy(wifi.ssid, saved))
const pskHint = computed<{ text: string; err: boolean }>(() => {
  if (pskErr.value) return { text: pskErr.value, err: true }
  if (testWrong.value && test.value) return { text: test.value.text, err: true }
  if (wrongPw.value && !wifi.psk) {
    return { text: 'The network rejected the saved password. Enter the correct one and save.', err: true }
  }
  return { text: pwCopy.value.hint, err: false }
})

const testText = computed(() => {
  if (testBusy.value) {
    return { tone: 'busy', text: `Testing “${testSsid.value}”… up to 30 seconds. Your cable connection isn’t affected.` }
  }
  if (test.value) return test.value
  if (uplink.value === 'wifi') return { tone: 'idle', text: WIFI_LIVE_TEXT }
  return null
})

// Save
const confirmOff = ref(false)
watch(() => wifi.enabled, (on) => { if (on) confirmOff.value = false })
const wifiHint = computed(() =>
  uplink.value === 'wifi' ? 'Saving will drop your Wi-Fi connection until the camera reconnects.' : 'Unsaved changes',
)

async function saveWifi(force = false) {
  if (!wifiValid.value) {
    wifiAttempted.value = true
    wifiAct.errMsg.value = INVALID_MSG
    return
  }
  // Turning off the link this page is using gets one explicit confirmation.
  if (!wifi.enabled && uplink.value === 'wifi' && !force) {
    confirmOff.value = true
    return
  }
  confirmOff.value = false
  const body: WifiSave = { enabled: wifi.enabled, ssid: wifi.ssid }
  const sentPsk = !!wifi.psk
  if (sentPsk) body.passphrase = wifi.psk
  const ok = await wifiAct.run(() => api.saveWifi(body), wifiSaveMessage(uplink.value, wifi.enabled))
  if (!ok || !alive) return
  wifi.psk = ''
  if (sentPsk) saved.passphraseSet = true
  saved.savedSsid = wifi.ssid
  wifiDirty.markClean()
  wifiTouched.ssid = wifiTouched.psk = false
  wifiAttempted.value = false
  test.value = null
  testWrong.value = false
  emit('refresh')
}

// --- Cable ---
function wiredFromCfg(w: WiredConfig): WiredForm {
  const [dns1, dns2] = splitDns(w.dns)
  return { mode: w.mode, ip: w.ip, netmask: w.netmask, gateway: w.gateway, dns1, dns2 }
}
const wired = reactive<WiredForm>(wiredFromCfg(props.cfg.network.wired))
const wiredDirty = useDirty(() => wired)
const wiredAct = useAction()
const wiredTouched = reactive<Record<string, boolean>>({})
const wiredAttempted = ref(false)
const wiredErrors = computed(() => validateWired(wired))
const wiredValid = computed(() => isValid(wiredErrors.value))
watch(wiredValid, (v) => { if (v && wiredAct.errMsg.value === INVALID_MSG) wiredAct.errMsg.value = '' })
function wErr(k: keyof WiredForm): string | undefined {
  return wiredTouched[k] || wiredAttempted.value ? (wiredErrors.value as Record<string, string | undefined>)[k] : undefined
}

const rw = useRevertWindow()
const plan = ref<AddressPlan | null>(null)
const banner = ref<InstanceType<typeof RevertBanner> | null>(null)
// Don't lock on missing data: only a known non-cable uplink locks the form.
const locked = computed(() => !!net.value && net.value.uplink !== 'wired')
// Seeded from status.net.revert_in by the banners (and by Apply), minus the
// stale polls around a Keep.
const pending = computed(() => rw.state.pending)
const bannerShown = computed(() => pending.value || !!rw.state.result)
const committedMode = computed(() => net.value?.mode ?? props.cfg.network.wired.mode)
const modeHint = computed(() => addressingHint(wired.mode, committedMode.value, net.value?.ip ?? ''))

// Convenience prefill when switching to Static with nothing filled in.
watch(() => wired.mode, (m) => {
  if (m !== 'static') return
  const cur = net.value?.ip ?? ''
  if (!wired.ip && cur) wired.ip = cur
  if (!wired.netmask) wired.netmask = '255.255.255.0'
  if (!wired.gateway) wired.gateway = guessGateway(wired.ip)
})

const LOCKED_409 = 'the cable isn’t the active connection right now — plug it in and try again.'

async function applyWired() {
  if (!wiredValid.value) {
    wiredAttempted.value = true
    wiredAct.errMsg.value = INVALID_MSG
    return
  }
  const before = committedMode.value
  const body: WiredConfig | { mode: 'dhcp' } = wired.mode === 'static'
    ? {
        mode: 'static',
        ip: wired.ip.trim(),
        netmask: wired.netmask.trim(),
        gateway: wired.gateway.trim(),
        dns: joinDns(wired.dns1, wired.dns2),
      }
    : { mode: 'dhcp' }
  const res = await wiredAct.run(async () => {
    try {
      return await api.applyWired(body)
    } catch (e) {
      if (e instanceof ApiError && e.status === 409) throw new ApiError(409, LOCKED_409)
      throw e
    }
  }, '')
  if (!res || !alive) return
  // The response is what the camera actually tries; it wins over the form.
  const p = addressPlan(
    { mode: res.mode ?? body.mode, ip: res.ip ?? ('ip' in body ? body.ip : '') },
    before,
    window.location,
  )
  plan.value = p
  rw.begin(res.revert_in > 0 ? res.revert_in : 90)
  emit('applied', p)
  await nextTick()
  banner.value?.focusKeep()
}

function onKept() {
  wiredDirty.markClean()
  wiredAttempted.value = false
  emit('refresh')
  void nextTick(() => document.querySelector<HTMLInputElement>('input[name="wired-mode"]:checked')?.focus())
}
function onSettled() {
  Object.assign(wired, wiredFromCfg(props.cfg.network.wired))
  wiredDirty.markClean()
  wiredAttempted.value = false
  emit('refresh')
}

// --- Name ---
const host = reactive({ hostname: props.cfg.system.hostname })
const hostDirty = useDirty(() => host)
const hostAct = useAction()
const hostTouched = ref(false)
const hostErr = computed(() => validateHostname(host.hostname))
watch(hostErr, (e) => { if (!e && hostAct.errMsg.value === INVALID_MSG) hostAct.errMsg.value = '' })
async function saveHost() {
  if (hostErr.value) {
    hostTouched.value = true
    hostAct.errMsg.value = INVALID_MSG
    return
  }
  const updated = await hostAct.run(() => props.save({ system: { hostname: host.hostname } }))
  if (updated) hostDirty.markClean()
}

// A reloaded cfg (after Save/Keep/revert) refreshes every form that isn't mid-edit.
watch(() => props.cfg, (c) => {
  saved.passphraseSet = c.network.wifi.passphrase_set
  saved.savedSsid = c.network.wifi.ssid
  if (!wifiDirty.dirty.value) {
    wifi.enabled = c.network.wifi.enabled
    wifi.ssid = c.network.wifi.ssid
    wifiDirty.markClean()
  }
  if (!wiredDirty.dirty.value) {
    Object.assign(wired, wiredFromCfg(c.network.wired))
    wiredDirty.markClean()
  }
  if (!hostDirty.dirty.value) {
    host.hostname = c.system.hostname
    hostDirty.markClean()
  }
})
</script>

<template>
  <!-- Connection -->
  <section class="group">
    <header>
      <div>
        <h3>Connection</h3>
      </div>
    </header>
    <ul class="lanes" role="list">
      <li v-for="l in lanes" :key="l.key" class="lane" :data-tone="l.lane.tone">
        <span class="lane-icon" aria-hidden="true"><Icon :name="l.icon" :size="18" /></span>
        <span class="lane-text">
          <strong class="lane-title">{{ l.lane.title }}<span v-if="l.lane.ssid" class="lane-ssid"> “{{ l.lane.ssid }}”</span></strong>
          <span class="lane-detail" :role="l.live ? 'status' : undefined" :aria-live="l.live ? 'polite' : undefined">
            <template v-if="l.lane.ip">
              <span class="part mono">{{ l.lane.ip }}</span>
              <template v-if="l.lane.mac"><span class="sep" aria-hidden="true"> · </span><span class="part mono">{{ l.lane.mac }}</span></template>
              <template v-if="l.lane.signal">
                <span class="sep" aria-hidden="true"> · </span>
                <span class="part sigw">
                  <span class="sig" :data-bars="l.lane.signal.bars" aria-hidden="true"><i></i><i></i><i></i><i></i></span>
                  {{ l.lane.signal.word }}
                </span>
              </template>
            </template>
            <template v-else>
              <span v-if="l.lane.spin" class="spin sm" aria-hidden="true"></span>{{ l.lane.detail }}
            </template>
          </span>
        </span>
        <span class="badge" :class="l.lane.badgeKind">
          <span v-if="l.lane.badgeKind === 'ok' || l.lane.badgeKind === 'warn'" class="bdot" aria-hidden="true"></span>
          {{ l.lane.badge }}
        </span>
      </li>
    </ul>
    <p class="hint note">
      {{ CARD_NOTE }}
      <span v-if="uplink === 'none'" class="rescue">{{ RESCUE_NOTE }}</span>
    </p>
  </section>

  <!-- Wi-Fi backup -->
  <section class="group">
    <header>
      <div>
        <h3>Wi-Fi backup</h3>
        <p>Only used when the cable is unplugged.</p>
      </div>
      <div class="hdr-end">
        <span v-if="wrongPw" class="badge bad"><Icon name="alert" :size="12" />Wrong password</span>
        <input
          v-model="wifi.enabled"
          class="switch"
          type="checkbox"
          role="switch"
          aria-label="Enable Wi-Fi backup"
          :disabled="testBusy"
        />
      </div>
    </header>

    <Transition name="rise">
      <div v-if="wifi.enabled">
        <div v-if="uplink === 'wired'" class="msg msg-info explain">
          <Icon name="info" :size="15" />
          <span>
            <strong>While the cable is in, Wi-Fi stays off.</strong> The camera switches to it by itself if the
            cable is unplugged. Use <strong>Test connection</strong> below to check the name and password now — the
            cable connection isn’t disturbed.
          </span>
        </div>
        <div v-else-if="uplink === 'wifi'" class="msg msg-info explain">
          <Icon name="info" :size="15" />
          <span>You’re connected over Wi-Fi right now. Saving a different network or password will drop this connection until the camera reconnects.</span>
        </div>

        <div class="field">
          <label for="ssid">Network name</label>
          <div class="ssid-row">
            <input
              id="ssid"
              v-model="wifi.ssid"
              autocomplete="off"
              spellcheck="false"
              autocapitalize="off"
              maxlength="32"
              :disabled="testBusy"
              :aria-invalid="ssidErr ? 'true' : undefined"
              :aria-describedby="ssidErr ? 'ssid-err' : undefined"
              @blur="wifiTouched.ssid = true"
            />
            <button class="btn" type="button" :disabled="!canScan" :aria-busy="scanBusy ? 'true' : undefined" @click="runScan">
              <span v-if="scanBusy" class="spin"></span>
              <Icon v-else name="refresh" :size="16" />
              <span>{{ scanBusy ? 'Scanning…' : 'Scan' }}</span>
            </button>
          </div>
          <p v-if="ssidErr" id="ssid-err" class="hint err">{{ ssidErr }}</p>
        </div>

        <div class="scan-area" aria-live="polite">
          <div v-if="scanBusy" class="scanning">
            <div class="progress indeterminate" aria-hidden="true"><div class="bar"></div></div>
            <p class="hint">Looking for networks nearby — about 5 seconds.</p>
          </div>
          <p v-else-if="scanFailed" class="hint err">Scan failed — try again in a moment.</p>
          <template v-else-if="scan && scanText">
            <span v-if="scan.networks.length" class="sr-only">{{ scanText.announce }}</span>
            <Transition name="rise" appear>
              <ul v-if="scan.networks.length" class="scan" aria-label="Networks found">
                <li v-for="n in scan.networks" :key="n.ssid">
                  <button
                    type="button"
                    class="scan-row"
                    :class="{ sel: n.ssid === wifi.ssid }"
                    :aria-current="n.ssid === wifi.ssid ? 'true' : undefined"
                    :disabled="testBusy"
                    @click="pick(n.ssid)"
                  >
                    <span class="ssid">{{ n.ssid }}</span>
                    <span class="chan mono">{{ channelLabel(n.channels) }}</span>
                    <span class="tick-slot"><Icon v-if="n.ssid === wifi.ssid" name="check" :size="15" class="tick" /></span>
                  </button>
                </li>
              </ul>
            </Transition>
            <p v-if="scanText.hint" class="hint scan-hint">{{ scanText.hint }}</p>
          </template>
        </div>

        <div class="field">
          <label for="psk">Password</label>
          <PasswordInput
            id="psk"
            v-model="wifi.psk"
            :placeholder="pwCopy.placeholder"
            autocomplete="new-password"
            :maxlength="63"
            :disabled="testBusy"
            :invalid="!!pskErr || testWrong"
            describedby="psk-hint"
            @focusout="wifiTouched.psk = wifi.psk !== '' || wifiTouched.psk"
          />
          <p id="psk-hint" class="hint" :class="{ err: pskHint.err }">{{ pskHint.text }}</p>
        </div>

        <div class="test" :class="{ busy: testBusy }">
          <div class="test-row">
            <button class="btn" type="button" :disabled="!canTest" :aria-busy="testBusy ? 'true' : undefined" @click="runTest">
              <span v-if="testBusy" class="spin"></span>
              <Icon v-else name="wifi" :size="16" />
              <span>{{ testBusy ? 'Testing…' : 'Test connection' }}</span>
            </button>
            <span class="test-text" :data-tone="testText?.tone" role="status" aria-live="polite">
              <template v-if="testText">
                <Icon v-if="testText.tone === 'ok'" name="check" :size="15" />
                <Icon v-else-if="testText.tone === 'bad' || testText.tone === 'warn'" name="alert" :size="15" />
                <span>
                  <template v-if="'lead' in testText && testText.lead"><strong>{{ testText.lead }}</strong>{{ testText.text.slice(testText.lead.length) }}</template>
                  <template v-else>{{ testText.text }}</template>
                </span>
              </template>
            </span>
          </div>
          <div v-if="testBusy" class="progress indeterminate" aria-hidden="true"><div class="bar"></div></div>
        </div>
      </div>
    </Transition>

    <Transition name="fade">
      <div v-if="confirmOff" class="msg msg-bad off-confirm" role="alert">
        <Icon name="alert" :size="15" />
        <div class="off-body">
          <span><strong>Turning Wi-Fi off will disconnect you.</strong> The camera will be unreachable until the cable is plugged in (or on 192.168.2.3).</span>
          <div class="confirm">
            <button class="btn btn-sm" type="button" @click="confirmOff = false">Cancel</button>
            <button class="btn btn-sm btn-danger solid" type="button" @click="saveWifi(true)">Turn Wi-Fi off anyway</button>
          </div>
        </div>
      </div>
    </Transition>

    <ActionBar
      :busy="wifiAct.busy.value"
      :ok="wifiAct.okMsg.value"
      :err="wifiAct.errMsg.value"
      :dirty="testBusy ? false : wifiDirty.dirty.value"
      :idle-label="testBusy ? 'Testing…' : 'No changes'"
      label="Save Wi-Fi settings"
      busy-label="Saving…"
      :hint="wifiHint"
      @click="saveWifi()"
    />
  </section>

  <!-- Cable -->
  <section class="group">
    <header>
      <div>
        <h3>Cable</h3>
        <p>Your main connection. Changes are tried for 90 seconds and undone unless you keep them, so a mistake can’t lock you out.</p>
      </div>
    </header>

    <div v-if="locked" id="cable-lock" class="msg msg-info lock">
      <Icon name="cable" :size="15" />
      <span v-if="uplink === 'wifi'">Cable settings can be changed only while the cable is the active connection. Plug it in — the camera switches back within about 10 seconds — and this section unlocks.</span>
      <span v-else>Cable settings can be changed only while the cable is connected and has an address.</span>
    </div>

    <fieldset class="plain" :disabled="locked || pending" :aria-describedby="locked ? 'cable-lock' : undefined">
      <div class="field">
        <span id="wired-mode" class="label">Addressing</span>
        <div class="segmented" role="radiogroup" aria-labelledby="wired-mode" aria-describedby="wired-mode-hint">
          <label><input v-model="wired.mode" type="radio" name="wired-mode" value="dhcp" /><span>Automatic (DHCP)</span></label>
          <label><input v-model="wired.mode" type="radio" name="wired-mode" value="static" /><span>Static address</span></label>
        </div>
        <p id="wired-mode-hint" class="hint">{{ modeHint.text }}<template v-if="modeHint.ip"><strong class="mono">{{ modeHint.ip }}</strong>.</template></p>
      </div>

      <Transition name="rise">
        <div v-if="wired.mode === 'static'" class="grid static mono">
          <div class="field">
            <label for="ip">IP address</label>
            <input
              id="ip" v-model="wired.ip" placeholder="192.0.2.80" inputmode="decimal" autocomplete="off" spellcheck="false"
              :aria-invalid="wErr('ip') ? 'true' : undefined" :aria-describedby="wErr('ip') ? 'ip-err' : undefined"
              @blur="wiredTouched.ip = true"
            />
            <p v-if="wErr('ip')" id="ip-err" class="hint err">{{ wErr('ip') }}</p>
          </div>
          <div class="field">
            <label for="nm">Netmask</label>
            <input
              id="nm" v-model="wired.netmask" placeholder="255.255.255.0" inputmode="decimal" autocomplete="off" spellcheck="false"
              :aria-invalid="wErr('netmask') ? 'true' : undefined" :aria-describedby="wErr('netmask') ? 'nm-err' : undefined"
              @blur="wiredTouched.netmask = true"
            />
            <p v-if="wErr('netmask')" id="nm-err" class="hint err">{{ wErr('netmask') }}</p>
          </div>
          <div class="field">
            <label for="gw">Gateway <span class="opt">(optional)</span></label>
            <input
              id="gw" v-model="wired.gateway" placeholder="192.0.2.1" inputmode="decimal" autocomplete="off" spellcheck="false"
              :aria-invalid="wErr('gateway') ? 'true' : undefined" :aria-describedby="wErr('gateway') ? 'gw-err' : undefined"
              @blur="wiredTouched.gateway = true"
            />
            <p v-if="wErr('gateway')" id="gw-err" class="hint err">{{ wErr('gateway') }}</p>
          </div>
          <div class="field">
            <label for="dns1">DNS <span class="opt">(optional)</span></label>
            <div class="dns-pair">
              <input
                id="dns1" v-model="wired.dns1" placeholder="192.0.2.1" inputmode="decimal" autocomplete="off" spellcheck="false"
                :aria-invalid="wErr('dns1') ? 'true' : undefined" :aria-describedby="wErr('dns1') || wErr('dns2') ? 'dns-err' : undefined"
                @blur="wiredTouched.dns1 = true"
              />
              <input
                id="dns2" v-model="wired.dns2" placeholder="1.1.1.1" inputmode="decimal" autocomplete="off" spellcheck="false"
                aria-label="Second DNS server"
                :aria-invalid="wErr('dns2') ? 'true' : undefined" :aria-describedby="wErr('dns1') || wErr('dns2') ? 'dns-err' : undefined"
                @blur="wiredTouched.dns2 = true"
              />
            </div>
            <p v-if="wErr('dns1') || wErr('dns2')" id="dns-err" class="hint err">{{ wErr('dns1') || wErr('dns2') }}</p>
          </div>
        </div>
      </Transition>

      <ActionBar
        v-if="!bannerShown"
        :busy="wiredAct.busy.value"
        :ok="wiredAct.okMsg.value"
        :err="wiredAct.errMsg.value"
        :dirty="wiredDirty.dirty.value"
        label="Apply for 90 s"
        busy-label="Applying…"
        hint="Tried for 90 seconds — you’ll need to press Keep."
        @click="applyWired"
      />
    </fieldset>

    <RevertBanner
      ref="banner"
      variant="inline"
      :status="status"
      :reachable="true"
      :admin="true"
      :plan="plan"
      @confirmed="onKept"
      @settled="onSettled"
    />
  </section>

  <!-- Name -->
  <section class="group">
    <header>
      <div>
        <h3>Name</h3>
      </div>
    </header>
    <div class="field">
      <label for="host">Hostname</label>
      <input
        id="host"
        v-model="host.hostname"
        class="mono"
        maxlength="63"
        autocomplete="off"
        spellcheck="false"
        autocapitalize="off"
        :aria-invalid="hostTouched && hostErr ? 'true' : undefined"
        aria-describedby="host-hint"
        @blur="hostTouched = true"
      />
      <p v-if="hostTouched && hostErr" id="host-hint" class="hint err">{{ hostErr }}</p>
      <p v-else id="host-hint" class="hint">Shown in the status bar and sent to your router, so the camera can show up in the router’s device list.</p>
    </div>
    <ActionBar
      :busy="hostAct.busy.value"
      :ok="hostAct.okMsg.value"
      :err="hostAct.errMsg.value"
      :dirty="hostDirty.dirty.value"
      label="Save name"
      @click="saveHost"
    />
  </section>
</template>

<style scoped>
.group :deep(.actionbar) { margin-top: 18px; }
.note { margin-top: 10px; }
.rescue { display: block; margin-top: 4px; color: var(--warn); }

/* Connection lanes: order is priority (cable above Wi-Fi). */
.lanes {
  list-style: none; margin: 0; padding: 0;
  border-radius: var(--r-md);
  background: var(--surface-0);
  box-shadow: var(--hairline);
  overflow: hidden;
}
.lane {
  --c: var(--text-3);
  position: relative;
  display: flex; align-items: center; gap: 12px;
  padding: 12px 14px 12px 16px;
}
.lane + .lane { border-top: 1px solid var(--line); }
.lane::before {
  content: ""; position: absolute; left: 0; top: 0; bottom: 0; width: 3px;
  background: transparent;
}
.lane[data-tone="active"]::before { background: var(--accent); }
.lane[data-tone="bad"]::before { background: var(--bad); }
.lane-icon {
  width: 36px; height: 36px; flex: none;
  display: grid; place-items: center;
  border-radius: 10px;
  background: var(--surface-2);
  color: var(--text-2);
}
.lane[data-tone="active"] .lane-icon { background: var(--accent-soft); color: var(--accent); }
.lane[data-tone="bad"] .lane-icon { background: var(--bad-soft); color: var(--bad); }
.lane-text { flex: 1 1 auto; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
.lane-title { font-size: var(--fs-md); font-weight: 600; overflow-wrap: anywhere; }
.lane-ssid { font-weight: 500; color: var(--text-2); }
.lane-detail { color: var(--text-2); font-size: var(--fs-sm); line-height: 1.45; overflow-wrap: anywhere; }
.lane[data-tone="quiet"] .lane-detail { color: var(--text-3); }
.lane-detail .spin.sm { width: 14px; height: 14px; vertical-align: -2px; margin-right: 6px; }
.lane-detail .sep { color: var(--text-3); }
.sigw { display: inline-flex; align-items: center; gap: 6px; white-space: nowrap; }
.badge { flex: none; }
.badge .bdot { width: 6px; height: 6px; border-radius: 50%; background: currentColor; }
.badge.ok .bdot { box-shadow: 0 0 8px currentColor; }
.badge.warn .bdot { animation: blink 1.1s steps(1) infinite; }
@keyframes blink { 50% { opacity: 0.35; } }

/* Signal bars */
.sig { display: inline-flex; align-items: flex-end; gap: 2px; height: 14px; flex: none; }
.sig i { width: 3px; border-radius: 1px; background: var(--text-3); opacity: 0.35; }
.sig i:nth-child(1) { height: 5px; } .sig i:nth-child(2) { height: 8px; }
.sig i:nth-child(3) { height: 11px; } .sig i:nth-child(4) { height: 14px; }
.sig[data-bars="1"] i:nth-child(-n+1), .sig[data-bars="2"] i:nth-child(-n+2),
.sig[data-bars="3"] i:nth-child(-n+3), .sig[data-bars="4"] i:nth-child(-n+4) { background: var(--text); opacity: 1; }

/* Wi-Fi */
.hdr-end { display: flex; align-items: center; gap: 10px; flex: none; }
.explain { margin-bottom: 16px; }
.explain strong, .lock strong { color: var(--text); font-weight: 600; }
.ssid-row { display: flex; gap: 8px; }
.ssid-row input { min-width: 0; }
.ssid-row .btn { flex: none; }
.scan-area { margin-top: 8px; }
.scan-area:empty { display: none; }
.scanning { display: flex; flex-direction: column; gap: 6px; }
.scanning .progress { height: 4px; }
.scan {
  list-style: none; margin: 0; padding: 4px;
  border-radius: var(--r-sm);
  background: var(--surface-0);
  box-shadow: var(--hairline);
}
.scan-row {
  display: flex; align-items: center; gap: 10px;
  width: 100%; min-height: 40px; padding: 0 10px;
  border-radius: var(--r-xs);
  text-align: left;
  font-size: var(--fs-md);
  transition: background var(--dur) var(--ease);
}
.scan-row:hover:not(:disabled) { background: var(--surface-2); }
.scan-row:disabled { opacity: 0.5; cursor: not-allowed; }
.scan-row.sel { background: var(--accent-soft); }
.ssid { flex: 1 1 auto; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.chan { flex: none; color: var(--text-3); font-size: var(--fs-xs); }
.tick-slot { width: 15px; flex: none; display: grid; place-items: center; }
.tick { color: var(--accent); }
.scan-hint { margin-top: 6px; }
.scan-area + .field { margin-top: 16px; }

.test {
  margin-top: 16px;
  padding: 10px;
  border-radius: var(--r-sm);
  background: var(--surface-0);
  box-shadow: var(--hairline);
  display: flex; flex-direction: column; gap: 10px;
}
.test-row { display: flex; align-items: center; gap: 12px; }
.test-row .btn { flex: none; }
.test-text {
  flex: 1 1 auto; min-width: 0;
  display: flex; align-items: flex-start; gap: 7px;
  font-size: var(--fs-sm); line-height: 1.4; color: var(--text-2);
}
.test-text :deep(svg) { margin-top: 1px; }
.test-text[data-tone="ok"] { color: var(--ok); }
.test-text[data-tone="bad"] { color: var(--bad); }
.test-text[data-tone="warn"] { color: var(--warn); }
.test-text[data-tone="idle"] { color: var(--text-3); }
.test .progress { height: 4px; }

.off-confirm { margin-top: 16px; }
.off-body { display: flex; flex-direction: column; gap: 10px; flex: 1 1 auto; min-width: 0; }
.off-body strong { font-weight: 600; }
.confirm { display: flex; gap: 6px; flex-wrap: wrap; justify-content: flex-end; }

/* Cable */
.lock { margin-bottom: 16px; }
fieldset.plain { border: 0; padding: 0; margin: 0; min-width: 0; }
fieldset.plain:disabled .segmented { opacity: 0.55; }
fieldset.plain:disabled .segmented > label { cursor: not-allowed; }
.static {
  margin-top: 14px;
  grid-template-columns: repeat(auto-fit, minmax(max(160px, calc((100% - 14px) / 2)), 1fr));
}
.opt { color: var(--text-3); font-weight: 400; }
.dns-pair { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.dns-pair input { min-width: 0; }
.field .hint strong.mono { color: var(--text-2); font-weight: 600; }

@media (max-width: 640px) {
  .lane-detail .sep { display: none; }
  .lane-detail .part { display: block; }
  .lane-detail .sigw { display: flex; }
  .test-row { flex-direction: column; align-items: stretch; }
  .test-row .btn { width: 100%; }
}
</style>
