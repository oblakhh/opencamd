<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Top status bar: clock, connection state, uplink, Wi-Fi and uploader indicators.
-->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import type { Status } from '../types'
import type { StreamMode } from '../composables/useMjpeg'
import { describeUploadStatus } from '../utils/upload'
import Icon from './Icon.vue'

const props = defineProps<{
  status: Status | null
  streamOk: boolean
  streamMode: StreamMode
  streamFps: number
  reachable: boolean
  admin: boolean
}>()
const emit = defineEmits<{ account: [] }>()

// Live wall clock, independent of the status poll.
const now = ref(new Date())
let timer: number | undefined
onMounted(() => { timer = window.setInterval(() => (now.value = new Date()), 1000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

const clock = computed(() =>
  now.value.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }),
)

const name = computed(() => props.status?.name ?? 'camera')
const connected = computed(() => props.streamOk && props.reachable)
const state = computed<'ok' | 'warn' | 'bad'>(() => {
  if (connected.value) return 'ok'
  if (props.reachable) return 'warn'
  return 'bad'
})
const stateLabel = computed(() => {
  if (connected.value) return 'Live'
  if (props.reachable) return 'Stream stalled'
  return 'Offline'
})
const res = computed(() => {
  const v = props.status?.video
  return v ? `${v.width}×${v.height}` : null
})
const fps = computed(() => {
  const f = props.status?.video.fps
  return f != null ? `~${f.toFixed(1)} fps` : null
})
// 'none' (rescue address / mid-failover) gets no icon.
const link = computed(() => {
  const u = props.status?.net.uplink
  return u === 'wired' || u === 'wifi' ? u : null
})
// The saved Wi-Fi password was rejected: worth seeing without opening settings.
const wifiBad = computed(() => props.status?.net.wifi?.state === 'wrong_passphrase')

// Fallback indicator: the live multipart stream is the norm, so only the
// snapshot-polling mode gets a label. It reads as information, not alarm.
const polling = computed(() => props.streamMode === 'polling' && props.streamOk)
const pollingTitle = computed(() =>
  `Showing snapshots (~${props.streamFps.toFixed(0)} fps) instead of the live stream — ` +
  'the stream will be retried automatically.',
)

// Uploader chip: shown whenever the uploader is on, from /api/status.upload.
// Ember while it's working, red once the last attempt failed; the tooltip
// carries the full sentence (count, last HTTP code).
const upload = computed(() => props.status?.upload)
const uploadSummary = computed(() => describeUploadStatus(upload.value))
const uploadShown = computed(() => upload.value?.enabled === true)
const uploadBad = computed(() => uploadSummary.value.tone === 'bad')
const uploadTitle = computed(() => uploadSummary.value.text)
const uploadLabel = computed(() =>
  uploadSummary.value.tone === 'bad' ? 'upload failed'
  : uploadSummary.value.tone === 'idle' ? 'starting…'
  : 'uploads on')
</script>

<template>
  <header class="bar" :data-state="state">
    <div class="scrim" aria-hidden="true"></div>
    <div class="row">
      <div class="cluster id">
        <span class="dot" role="img" :aria-label="stateLabel" :title="stateLabel">
          <span class="dot-core"></span>
        </span>
        <span class="name">{{ name }}</span>
        <Transition name="fade">
          <span v-if="state !== 'ok'" class="state">{{ stateLabel }}</span>
        </Transition>
        <Transition name="fade">
          <span v-if="polling" class="mode" :title="pollingTitle">snapshots</span>
        </Transition>
      </div>

      <div class="cluster meta tnum" aria-label="Video">
        <Icon v-if="link" :name="link === 'wifi' ? 'wifi' : 'cable'" :size="14" class="link" />
        <span v-if="res">{{ res }}</span>
        <span v-if="fps" class="fps">{{ fps }}</span>
      </div>

      <div class="cluster right">
        <Transition name="fade">
          <span
            v-if="wifiBad"
            class="chip bad"
            title="The Wi-Fi network rejected the saved password — fix it in Settings › Network."
            role="status"
          >
            <span class="chip-icon"><Icon name="alert" :size="12" /></span>
            <span class="chip-text">wi-fi password</span>
          </span>
        </Transition>
        <Transition name="fade">
          <span v-if="uploadShown" class="chip" :class="{ bad: uploadBad }" :title="uploadTitle" role="status">
            <span class="chip-icon"><Icon :name="uploadBad ? 'alert' : 'arrowUp'" :size="12" /></span>
            <span class="chip-text">{{ uploadLabel }}</span>
          </span>
        </Transition>
        <time class="clock tnum">{{ clock }}</time>
        <button
          class="ctl"
          :class="{ gear: admin }"
          type="button"
          :aria-label="admin ? 'Open settings' : 'Sign in'"
          @click="emit('account')"
        >
          <Icon v-if="admin" name="gear" :size="18" />
          <template v-else>
            <Icon name="lock" :size="15" />
            <span>Sign in</span>
          </template>
        </button>
      </div>
    </div>
  </header>
</template>

<style scoped>
.bar {
  position: fixed;
  top: 0; left: 0; right: 0;
  z-index: 20;
  pointer-events: none;
  color: var(--text);
  font-size: var(--fs-sm);
}
/* A soft scrim that fades into the picture: legible over bright frames without
   becoming a slab. The blur is masked so it dissolves too. */
.scrim {
  position: absolute;
  inset: 0 0 -70px 0;
  background: linear-gradient(to bottom, rgb(4 6 9 / 0.72) 0%, rgb(4 6 9 / 0.35) 45%, transparent 100%);
  backdrop-filter: blur(14px) saturate(130%);
  -webkit-backdrop-filter: blur(14px) saturate(130%);
  mask-image: linear-gradient(to bottom, #000 35%, transparent 100%);
  -webkit-mask-image: linear-gradient(to bottom, #000 35%, transparent 100%);
}
.row {
  position: relative;
  pointer-events: auto;
  display: flex;
  align-items: center;
  gap: 18px;
  min-height: 48px;
  padding: 6px 14px 6px 16px;
  padding-top: max(6px, env(safe-area-inset-top));
}
.cluster { display: flex; align-items: center; gap: 9px; min-width: 0; }
.id { flex: 1 1 0; }
.meta { flex: 1 1 0; justify-content: center; color: var(--text-2); gap: 8px; }
.right { flex: 1 1 0; justify-content: flex-end; gap: 12px; }

.name { font-weight: 600; letter-spacing: 0.1px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.state { color: var(--text-2); white-space: nowrap; }
.state::before { content: "·"; margin-right: 9px; color: var(--text-3); }

/* Snapshot-fallback tag: a hairline pill in the quiet text colour. */
.mode {
  flex: none;
  height: 20px; padding: 0 8px;
  display: inline-flex; align-items: center;
  border-radius: 999px;
  box-shadow: inset 0 0 0 1px var(--line-strong);
  color: var(--text-2);
  font-size: var(--fs-xs); font-weight: 600; letter-spacing: 0.3px;
  text-transform: uppercase;
}

/* Connection dot: a core plus an outer ring that breathes when live. */
.dot { position: relative; width: 10px; height: 10px; flex: none; display: grid; place-items: center; }
.dot-core { width: 8px; height: 8px; border-radius: 50%; background: var(--c); box-shadow: 0 0 10px var(--c); }
.dot::after {
  content: "";
  position: absolute; inset: -3px;
  border-radius: 50%;
  border: 1.5px solid var(--c);
  opacity: 0;
}
.bar[data-state="ok"] { --c: var(--ok); }
.bar[data-state="warn"] { --c: var(--warn); }
.bar[data-state="bad"] { --c: var(--bad); }
.bar[data-state="ok"] .dot::after { animation: breathe 2.6s var(--ease) infinite; }
.bar[data-state="warn"] .dot-core { animation: blink 1.1s steps(1) infinite; }
@keyframes breathe {
  0% { transform: scale(0.6); opacity: 0.8; }
  70%, 100% { transform: scale(1.7); opacity: 0; }
}
@keyframes blink { 50% { opacity: 0.35; } }

.link { color: var(--text-3); }
.fps::before { content: "·"; margin-right: 8px; color: var(--text-3); }

.clock { color: var(--text-2); }

/* Upload chip */
.chip {
  display: inline-flex; align-items: center; gap: 6px;
  height: 24px; padding: 0 9px 0 5px;
  border-radius: 999px;
  background: var(--accent-soft);
  color: var(--accent);
  font-size: var(--fs-xs); font-weight: 600; letter-spacing: 0.2px;
  box-shadow: inset 0 0 0 1px rgb(255 138 76 / 0.25);
}
.chip-icon { display: grid; place-items: center; width: 16px; height: 16px; border-radius: 50%; background: var(--accent); color: var(--accent-ink); overflow: hidden; }
.chip.bad { background: var(--bad-soft); color: var(--bad); box-shadow: inset 0 0 0 1px rgb(255 107 107 / 0.3); }
.chip.bad .chip-icon { background: var(--bad); color: #1a0707; }
.chip:not(.bad) .chip-icon :deep(svg) { animation: lift 2.4s var(--ease) infinite; }
@keyframes lift {
  0% { transform: translateY(8px); opacity: 0; }
  30%, 70% { transform: translateY(0); opacity: 1; }
  100% { transform: translateY(-8px); opacity: 0; }
}

/* Sign-in / gear control */
.ctl {
  display: inline-flex; align-items: center; justify-content: center; gap: 7px;
  height: 34px; padding: 0 13px 0 11px;
  border-radius: 999px;
  background: rgb(255 255 255 / 0.08);
  box-shadow: inset 0 0 0 1px rgb(255 255 255 / 0.12);
  color: var(--text);
  font-weight: 600; font-size: var(--fs-sm);
  transition: background var(--dur) var(--ease), transform var(--dur) var(--ease);
}
.ctl:hover { background: rgb(255 255 255 / 0.14); }
.ctl:active { transform: scale(0.96); }
.ctl.gear { width: 36px; height: 36px; padding: 0; }
.ctl.gear :deep(svg) { transition: transform 0.5s var(--ease-out); }
.ctl.gear:hover :deep(svg) { transform: rotate(45deg); }

@media (max-width: 640px) {
  .row { gap: 10px; padding-left: 14px; padding-right: 10px; }
  .meta { flex: 0 0 auto; }
  .fps, .clock, .link { display: none; }
  .chip-text { display: none; }
  .chip { padding: 0 4px; }
  .ctl:not(.gear) span { display: none; }
  .ctl:not(.gear) { width: 36px; padding: 0; }
}
</style>
