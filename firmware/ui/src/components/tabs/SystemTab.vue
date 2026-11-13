<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Settings > System: time and logging, admin password, firmware upgrade, reboot.
-->
<script setup lang="ts">
import { computed, reactive, ref } from 'vue'
import type { Config, ConfigPatch } from '../../types'
import { api } from '../../api'
import { useAction } from '../../composables/useAction'
import { useDirty } from '../../composables/useDirty'
import ActionBar from '../ActionBar.vue'
import PasswordInput from '../PasswordInput.vue'
import Icon from '../Icon.vue'

const props = defineProps<{
  cfg: Config
  save: (patch: ConfigPatch) => Promise<Config | undefined>
}>()
// `reboot` tells App to show the rebooting/reconnect overlay.
const emit = defineEmits<{ reboot: [label: string] }>()

// --- time & logging ---
const form = reactive({
  ntp: props.cfg.system.ntp,
  syslogRemote: props.cfg.system.syslog_remote,
  syslogLevel: props.cfg.system.syslog_level,
  telnet: props.cfg.system.telnet,
})
const { dirty, markClean } = useDirty(() => form)
const sys = useAction()
async function saveSystem() {
  const updated = await sys.run(() =>
    props.save({
      system: {
        ntp: form.ntp,
        syslog_remote: form.syslogRemote,
        syslog_level: form.syslogLevel,
        telnet: form.telnet,
      },
    }),
  )
  if (updated) markClean()
}

// --- password ---
const pw = reactive({ a: '', b: '' })
const pwAction = useAction()
const pwMismatch = computed(() => pw.b.length > 0 && pw.a !== pw.b)
const pwReady = computed(() => pw.a.length > 0 && pw.a === pw.b)
async function changePw() {
  if (pw.a.length < 1) { pwAction.errMsg.value = 'Enter a new password.'; return }
  if (pw.a !== pw.b) { pwAction.errMsg.value = 'Passwords don’t match.'; return }
  const ok = await pwAction.run(() => api.changePassword(pw.a), 'Password changed.')
  if (ok) { pw.a = ''; pw.b = '' }
}

// --- firmware ---
const fwFile = ref<File | null>(null)
const fwProgress = ref(0)
const fwBusy = ref(false)
const fwError = ref('')
const dragging = ref(false)
const fileInput = ref<HTMLInputElement | null>(null)

function setFile(f: File | null | undefined) {
  fwFile.value = f ?? null
  fwError.value = ''
}
function onFile(e: Event) {
  setFile((e.target as HTMLInputElement).files?.[0])
}
function onDrop(e: DragEvent) {
  dragging.value = false
  if (fwBusy.value) return
  setFile(e.dataTransfer?.files?.[0])
}
function fmtSize(n: number): string {
  return n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.round(n / 1024)} KB`
}
const fwPct = computed(() => Math.round(fwProgress.value * 100))
const fwPhase = computed(() => (fwProgress.value >= 1 ? 'Verifying and flashing…' : `Uploading ${fwPct.value}%`))

function startFirmware() {
  if (!fwFile.value || fwBusy.value) return
  fwBusy.value = true
  fwError.value = ''
  fwProgress.value = 0
  const { promise } = api.uploadFirmware(fwFile.value, (f) => (fwProgress.value = f))
  promise
    .then(() => emit('reboot', 'Installing firmware'))
    .catch((err) => {
      // A dropped connection once bytes are uploaded is the expected OTA path:
      // the device started flashing and rebooted. Treat near-complete as success.
      if (fwProgress.value > 0.98) emit('reboot', 'Installing firmware')
      else fwError.value = `Upload failed: ${err?.message ?? 'error'}`
    })
    .finally(() => (fwBusy.value = false))
}

// --- power ---
const confirming = ref<'' | 'reboot' | 'reset'>('')
async function doReboot() {
  try { await api.reboot() } catch { /* connection may drop first */ }
  emit('reboot', 'Rebooting')
}
async function doFactoryReset() {
  try { await api.factoryReset() } catch { /* connection may drop first */ }
  emit('reboot', 'Factory reset')
}
</script>

<template>
  <section class="group">
    <header>
      <div>
        <h3>Time &amp; logging</h3>
        <p>The camera has no clock battery; it sets its time from the network on boot.</p>
      </div>
    </header>
    <div class="field">
      <label for="ntp">NTP server</label>
      <input id="ntp" v-model="form.ntp" class="mono" spellcheck="false" autocapitalize="off" placeholder="pool.ntp.org" />
    </div>
    <div class="grid">
      <div class="field">
        <label for="syslog">Remote syslog</label>
        <input id="syslog" v-model="form.syslogRemote" class="mono" placeholder="host:514 (optional)" spellcheck="false" autocapitalize="off" />
      </div>
      <div class="field lvl">
        <label for="lvl">Log level</label>
        <select id="lvl" v-model.number="form.syslogLevel">
          <option :value="3">Errors only</option>
          <option :value="4">Warnings</option>
          <option :value="6">Info</option>
          <option :value="7">Debug</option>
        </select>
      </div>
    </div>
    <label class="toggle-row">
      <span>
        <span class="t"><Icon name="terminal" :size="15" class="ti" /> Telnet shell</span>
        <span class="d">Admin-password protected. Handy for debugging; off is the safer default.</span>
      </span>
      <input v-model="form.telnet" class="switch" type="checkbox" role="switch" />
    </label>
    <ActionBar
      :busy="sys.busy.value"
      :ok="sys.okMsg.value"
      :err="sys.errMsg.value"
      :dirty="dirty"
      label="Save system settings"
      @click="saveSystem"
    />
  </section>

  <section class="group">
    <header>
      <div>
        <h3>Admin password</h3>
        <p>Protects settings, the API and the telnet shell.</p>
      </div>
    </header>
    <div class="grid">
      <div class="field">
        <label for="npw">New password</label>
        <PasswordInput id="npw" v-model="pw.a" autocomplete="new-password" />
      </div>
      <div class="field">
        <label for="cpw">Confirm</label>
        <PasswordInput id="cpw" v-model="pw.b" autocomplete="new-password" />
        <p v-if="pwMismatch" class="hint err">Doesn’t match yet.</p>
      </div>
    </div>
    <ActionBar
      :busy="pwAction.busy.value"
      :ok="pwAction.okMsg.value"
      :err="pwAction.errMsg.value"
      :dirty="pwReady"
      label="Change password"
      idle-label="Change password"
      busy-label="Changing…"
      @click="changePw"
    />
  </section>

  <section class="group">
    <header>
      <div>
        <h3>Firmware update</h3>
        <p>Upload a <span class="mono">*_upg.bin</span> image. The camera checks it, flashes it and reboots; your settings are kept.</p>
      </div>
    </header>

    <div
      class="drop"
      :class="{ over: dragging, has: !!fwFile, busy: fwBusy }"
      @dragover.prevent="dragging = !fwBusy"
      @dragleave="dragging = false"
      @drop.prevent="onDrop"
    >
      <input
        ref="fileInput"
        class="sr-only"
        type="file"
        accept=".bin"
        :disabled="fwBusy"
        aria-label="Choose firmware image"
        @change="onFile"
      />
      <template v-if="!fwBusy">
        <div class="drop-icon"><Icon name="file" :size="22" /></div>
        <div class="drop-text">
          <template v-if="fwFile">
            <strong class="mono">{{ fwFile.name }}</strong>
            <span class="hint">{{ fmtSize(fwFile.size) }} · ready to install</span>
          </template>
          <template v-else>
            <strong>Drop a firmware image here</strong>
            <span class="hint">or choose a file from your computer</span>
          </template>
        </div>
        <button class="btn btn-sm" type="button" @click="fileInput?.click()">{{ fwFile ? 'Change' : 'Choose file' }}</button>
      </template>
      <template v-else>
        <div class="drop-icon live"><span class="spin"></span></div>
        <div class="drop-text wide">
          <strong>{{ fwPhase }}</strong>
          <div class="progress" :class="{ indeterminate: fwProgress >= 1 }" role="progressbar" :aria-valuenow="fwPct" aria-valuemin="0" aria-valuemax="100">
            <div class="bar" :style="{ width: `${fwPct}%` }"></div>
          </div>
          <span class="hint">Keep the camera powered. The page will reconnect on its own.</span>
        </div>
      </template>
    </div>

    <Transition name="fade">
      <div v-if="fwError" class="msg msg-bad fw-err"><Icon name="alert" :size="15" />{{ fwError }}</div>
    </Transition>

    <div class="row-end">
      <button class="btn btn-primary" :class="{ busy: fwBusy }" type="button" :disabled="!fwFile || fwBusy" @click="startFirmware">
        <Icon v-if="!fwBusy" name="upload" :size="16" />
        <span v-else class="spin"></span>
        {{ fwBusy ? 'Installing…' : 'Upload & install' }}
      </button>
    </div>
  </section>

  <section class="group">
    <header>
      <div>
        <h3>Power</h3>
      </div>
    </header>
    <div class="power">
      <div class="prow">
        <div class="pico"><Icon name="rotate" :size="18" /></div>
        <div class="ptext">
          <strong>Restart</strong>
          <span class="hint">Takes about half a minute. Nothing is changed.</span>
        </div>
        <Transition name="fade" mode="out-in">
          <div v-if="confirming === 'reboot'" key="c" class="confirm">
            <button class="btn btn-sm" type="button" @click="confirming = ''">Cancel</button>
            <button class="btn btn-sm btn-primary" type="button" @click="doReboot">Restart now</button>
          </div>
          <button v-else key="b" class="btn btn-sm" type="button" @click="confirming = 'reboot'">Restart…</button>
        </Transition>
      </div>
      <div class="prow danger">
        <div class="pico bad"><Icon name="alert" :size="18" /></div>
        <div class="ptext">
          <strong>Factory reset</strong>
          <span class="hint">Erases every setting — network, password, upload targets — and restarts with defaults.</span>
        </div>
        <Transition name="fade" mode="out-in">
          <div v-if="confirming === 'reset'" key="c" class="confirm">
            <button class="btn btn-sm" type="button" @click="confirming = ''">Cancel</button>
            <button class="btn btn-sm btn-danger solid" type="button" @click="doFactoryReset">Erase &amp; reset</button>
          </div>
          <button v-else key="b" class="btn btn-sm btn-danger" type="button" @click="confirming = 'reset'">Reset…</button>
        </Transition>
      </div>
    </div>
  </section>
</template>

<style scoped>
.lvl { max-width: 180px; }
.toggle-row { margin-top: 14px; }
.ti { color: var(--text-3); margin-right: 2px; }
.group :deep(.actionbar) { margin-top: 18px; }

/* Firmware drop zone */
.drop {
  position: relative;
  display: flex; align-items: center; gap: 14px;
  padding: 14px 16px;
  border-radius: var(--r-md);
  background: var(--surface-0);
  box-shadow: inset 0 0 0 1px var(--line-strong);
  outline: 1px dashed transparent;
  transition: box-shadow var(--dur) var(--ease), background var(--dur) var(--ease);
}
.drop:not(.has):not(.busy) { box-shadow: none; outline: 1.5px dashed var(--line-strong); outline-offset: -1.5px; }
.drop.over { outline-color: var(--accent); background: var(--accent-soft); }
.drop.has { box-shadow: inset 0 0 0 1px rgb(255 138 76 / 0.4); }
.drop-icon {
  width: 42px; height: 42px; flex: none;
  display: grid; place-items: center;
  border-radius: 12px; background: var(--surface-2); color: var(--text-2);
}
.drop.has .drop-icon { color: var(--accent); background: var(--accent-soft); }
.drop-icon.live { color: var(--accent); }
.drop-text { flex: 1 1 auto; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.drop-text strong { font-size: var(--fs-md); font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.drop-text .progress { margin: 8px 0 6px; }
.fw-err { margin-top: 10px; }
.row-end { display: flex; justify-content: flex-end; margin-top: 14px; }

/* Power */
.power { display: flex; flex-direction: column; border-radius: var(--r-md); background: var(--surface-0); box-shadow: var(--hairline); }
.prow { display: flex; align-items: center; gap: 14px; padding: 14px 16px; }
.prow + .prow { border-top: 1px solid var(--line); }
.pico { width: 36px; height: 36px; flex: none; display: grid; place-items: center; border-radius: 10px; background: var(--surface-2); color: var(--text-2); }
.pico.bad { color: var(--bad); background: var(--bad-soft); }
.ptext { flex: 1 1 auto; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
.ptext strong { font-size: var(--fs-md); font-weight: 600; }
.confirm { display: flex; gap: 6px; flex: none; }
.prow .btn { flex: none; }

@media (max-width: 520px) {
  .drop { flex-wrap: wrap; }
  .drop-text { flex-basis: calc(100% - 56px); }
  .drop > .btn { margin-left: 56px; }
  .prow { flex-wrap: wrap; }
  .ptext { flex-basis: calc(100% - 50px); }
  .prow > .btn, .prow > .confirm { margin-left: 50px; }
}
</style>
