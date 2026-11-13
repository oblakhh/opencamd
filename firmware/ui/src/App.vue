<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Root component: live MJPEG view, status bar, login/settings dialogs and
  the wired-change revert banner.
-->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { api, setUnauthorizedHandler } from './api'
import { useStatus } from './composables/useStatus'
import { useMjpeg } from './composables/useMjpeg'
import StatusBar from './components/StatusBar.vue'
import LoginModal from './components/LoginModal.vue'
import SettingsModal from './components/SettingsModal.vue'
import RevertBanner from './components/RevertBanner.vue'
import { useRevertWindow } from './composables/useRevertWindow'
import type { AddressPlan } from './utils/network'
import Icon from './components/Icon.vue'

const { status, reachable, refresh } = useStatus(3000)

// --- session -------------------------------------------------------------
const admin = ref(false)
// Keep local admin in sync with the authoritative poll, unless we've just been
// kicked out by a 401 (handled below).
watch(status, (s) => { if (s) admin.value = s.admin })

const showLogin = ref(false)
const showSettings = ref(false)
// Why the login was opened: 'keep' (from the revert banner) signs in and lets
// the banner confirm, without opening settings.
const loginReason = ref<'settings' | 'keep'>('settings')

// Any 401 on an admin call flips the whole UI back to logged-out.
setUnauthorizedHandler(() => {
  admin.value = false
  showSettings.value = false
})

function onAccount() {
  if (admin.value) showSettings.value = true
  else {
    loginReason.value = 'settings'
    showLogin.value = true
  }
}
async function onLoggedIn() {
  admin.value = true
  showLogin.value = false
  if (loginReason.value !== 'keep') showSettings.value = true
  loginReason.value = 'settings'
  await refresh()
}
function onLoginClosed() {
  showLogin.value = false
  if (loginReason.value === 'keep') revertWindow.cancelConfirm()
  loginReason.value = 'settings'
}

// --- wired try-window ----------------------------------------------------
// The plan (where the camera will be) of the last Apply from this page; the
// floating banner needs it once the old address stops answering.
const revertWindow = useRevertWindow()
const plan = ref<AddressPlan | null>(null)
function onKeepLogin() {
  loginReason.value = 'keep'
  showLogin.value = true
}
// A finished window (kept, or back on the old settings and reachable) clears it.
function onWindowEnded() {
  plan.value = null
}
function onLogout() {
  admin.value = false
  showSettings.value = false
}

// --- stream connection ---------------------------------------------------
// One connection, ever: a fetch()-driven MJPEG reader (works on Safari, which
// won't render multipart in an <img>), with snapshot polling as the fallback
// and a polite wait when the camera reports "too many viewers".
const stream = useMjpeg()
const streamOk = stream.ok
// Top-level binding so the template's ref="canvas" lands on the composable's ref.
const canvas = stream.canvas
const fallbackText = computed(() => {
  if (stream.busy.value) {
    return stream.retryIn.value > 0
      ? `Too many viewers — retrying in ${stream.retryIn.value}s`
      : 'Too many viewers — retrying…'
  }
  if (stream.mode.value === 'offline') return 'Looking for the camera…'
  return reachable.value ? 'Reconnecting to the stream…' : 'Looking for the camera…'
})
// While a picture is on screen, the 503 notice sits quietly at the bottom.
const busyNotice = computed(() => stream.busy.value && streamOk.value)

// --- reboot / OTA flow ---------------------------------------------------
const rebooting = ref(false)
const rebootLabel = ref('Rebooting')
const rebootSeconds = ref(0)
let rebootPoll: number | undefined
let rebootTick: number | undefined

function beginReboot(label: string) {
  rebootLabel.value = label
  rebooting.value = true
  rebootSeconds.value = 0
  showSettings.value = false
  // Let go of the stream so the device can go down without a client holding
  // a worker thread, and so we don't hammer it while it's away.
  stream.stop()
  rebootTick = window.setInterval(() => rebootSeconds.value++, 1000)
  // Give the device a moment to actually go down before polling for its return.
  rebootPoll = window.setTimeout(pollForReturn, 6000)
}
async function pollForReturn() {
  try {
    await api.getStatus()
    // It's back — reload for a clean slate (new firmware, fresh session).
    location.reload()
  } catch {
    rebootPoll = window.setTimeout(pollForReturn, 3000)
  }
}
const rebootElapsed = computed(() => {
  const s = rebootSeconds.value
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`
})
const rebootHint = computed(() => {
  if (rebootSeconds.value < 8) return 'The camera is shutting down.'
  if (rebootSeconds.value < 40) return 'Waiting for it to come back online.'
  return 'Still waiting — a firmware flash can take a minute or two.'
})

onMounted(() => {
  document.title = status.value?.name ?? 'IC-3115W'
})
onUnmounted(() => {
  if (rebootPoll) clearTimeout(rebootPoll)
  if (rebootTick) clearInterval(rebootTick)
})
watch(status, (s) => { if (s?.name) document.title = s.name })
</script>

<template>
  <main class="stage" :class="{ live: streamOk }">
    <!-- Frames are painted here by useMjpeg (canvas + createImageBitmap:
         atomic repaints, no flicker, no <img> load events to rely on). It
         sits blank under the overlay until the first frame lands. -->
    <canvas ref="canvas" class="stream" role="img" aria-label="Live camera view"></canvas>

    <Transition name="fade">
      <div v-if="!streamOk" class="fallback" aria-live="polite">
        <div class="fallback-card">
          <div class="pulse" aria-hidden="true">
            <Icon name="camera" :size="22" />
          </div>
          <span>{{ fallbackText }}</span>
        </div>
      </div>
    </Transition>

    <Transition name="fade">
      <div v-if="busyNotice" class="notice" role="status" aria-live="polite">
        <span class="notice-dot" aria-hidden="true"></span>
        <span>
          Too many viewers — showing snapshots,
          <span class="tnum">{{ stream.retryIn.value > 0 ? `retrying in ${stream.retryIn.value}s` : 'retrying…' }}</span>
        </span>
      </div>
    </Transition>

    <StatusBar
      :status="status"
      :stream-ok="streamOk"
      :stream-mode="stream.mode.value"
      :stream-fps="stream.fps.value"
      :reachable="reachable"
      :admin="admin"
      @account="onAccount"
    />

    <!-- First after the status bar in DOM order, so Tab reaches it quickly. -->
    <RevertBanner
      variant="floating"
      :status="status"
      :reachable="reachable"
      :admin="admin"
      :plan="plan"
      @login="onKeepLogin"
      @confirmed="onWindowEnded"
      @settled="onWindowEnded"
    />

    <LoginModal v-if="showLogin" @close="onLoginClosed" @logged-in="onLoggedIn" />
    <SettingsModal
      v-if="showSettings"
      :status="status"
      @close="showSettings = false"
      @logout="onLogout"
      @reboot="beginReboot"
      @applied="(p) => (plan = p)"
    />

    <Transition name="fade">
      <div v-if="rebooting" class="reboot" role="alert">
        <div class="reboot-box">
          <div class="ring" aria-hidden="true">
            <Icon name="power" :size="22" />
          </div>
          <h2>{{ rebootLabel }}…</h2>
          <p>{{ rebootHint }}</p>
          <p class="elapsed tnum">{{ rebootElapsed }} · the page reloads by itself</p>
        </div>
      </div>
    </Transition>
  </main>
</template>

<style scoped>
.stage {
  position: fixed;
  inset: 0;
  background: #000;
  overflow: hidden;
}
.stream {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
  background: #000;
}
/* "Too many viewers" while snapshots keep the picture alive: a quiet pill
   low on the stage, out of the way of the status bar. */
.notice {
  position: absolute;
  left: 50%;
  bottom: max(18px, env(safe-area-inset-bottom));
  transform: translateX(-50%);
  display: flex;
  align-items: center;
  gap: 9px;
  padding: 8px 14px 8px 12px;
  border-radius: 999px;
  background: var(--surface-1);
  box-shadow: var(--shadow-lg), var(--hairline);
  color: var(--text-2);
  font-size: var(--fs-sm);
  white-space: nowrap;
  pointer-events: none;
  z-index: 10;
}
.notice-dot {
  width: 7px; height: 7px; flex: none;
  border-radius: 50%;
  background: var(--warn);
  box-shadow: 0 0 8px var(--warn);
  animation: blink 1.1s steps(1) infinite;
}
@keyframes blink { 50% { opacity: 0.35; } }

/* Stream not yet up: a calm card on a very dim texture */
.fallback {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  pointer-events: none;
  background: radial-gradient(ellipse at 50% 40%, rgb(255 138 76 / 0.05), transparent 60%);
}
.fallback-card {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 18px 10px 10px;
  border-radius: 999px;
  background: var(--surface-1);
  box-shadow: var(--shadow-lg), var(--hairline);
  color: var(--text-2);
  font-size: var(--fs-md);
}
.pulse {
  position: relative;
  width: 34px; height: 34px;
  display: grid; place-items: center;
  border-radius: 50%;
  background: var(--surface-2);
  color: var(--text-2);
}
.pulse::after {
  content: "";
  position: absolute; inset: 0;
  border-radius: 50%;
  border: 1.5px solid var(--text-3);
  animation: ripple 1.8s var(--ease-out) infinite;
}
@keyframes ripple {
  from { transform: scale(0.8); opacity: 0.9; }
  to { transform: scale(1.7); opacity: 0; }
}

/* Reboot / OTA overlay */
.reboot {
  position: fixed;
  inset: 0;
  z-index: 200;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
}
.reboot::before {
  content: "";
  position: absolute;
  inset: 0;
  background: rgb(4 6 9 / 0.88);
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
}
.reboot-box { position: relative; text-align: center; max-width: 380px; }
.reboot-box h2 { margin: 18px 0 6px; font-size: var(--fs-xl); font-weight: 650; letter-spacing: -0.3px; }
.reboot-box p { color: var(--text-2); line-height: 1.5; font-size: var(--fs-md); }
.elapsed { margin-top: 14px; color: var(--text-3); font-size: var(--fs-sm); }
.ring {
  position: relative;
  width: 64px; height: 64px;
  margin: 0 auto;
  display: grid; place-items: center;
  border-radius: 50%;
  background: var(--surface-1);
  color: var(--accent);
  box-shadow: var(--hairline);
}
.ring::before {
  content: "";
  position: absolute; inset: -4px;
  border-radius: 50%;
  border: 2px solid transparent;
  border-top-color: var(--accent);
  border-right-color: rgb(255 138 76 / 0.3);
  animation: spin 1.1s linear infinite;
}
</style>
