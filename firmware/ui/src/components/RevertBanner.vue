<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Countdown banner for the 90 s "keep or revert" window after a wired
  network change.
-->
<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import type { Status } from '../types'
import type { AddressPlan } from '../utils/network'
import { countdownMilestone, countdownTone, formatCountdown } from '../utils/network'
import { useRevertWindow } from '../composables/useRevertWindow'
import type { IconName } from '../icons'
import Icon from './Icon.vue'

// The 90 s "keep or revert" window after a wired change. Rendered twice: inline
// in the Network tab's Cable section and floating under the status bar (App).
// The floating one is always mounted and decides its own visibility, so it
// also works on a freshly opened page at the new address, signed out.
const props = defineProps<{
  status: Status | null
  reachable: boolean
  admin: boolean
  plan: AddressPlan | null
  variant: 'inline' | 'floating'
}>()
const emit = defineEmits<{ login: []; confirmed: []; settled: [] }>()

const rw = useRevertWindow()
const st = rw.state
const left = rw.left

// Fresh polls only: `status` is a new object per successful poll and is left
// untouched while the camera is unreachable.
watch(() => props.status, (s) => { if (s) rw.observe(s.net.revert_in ?? 0, s) }, { immediate: true })
// "Sign in to keep": confirm as soon as the login lands.
watch(() => props.admin, (a) => { if (a && st.wantConfirm && st.pending) void rw.keep() })
// Re-emit shared outcomes from every instance.
watch(() => st.seq, () => {
  if (st.event === 'confirmed') emit('confirmed')
  else if (st.event === 'settled') emit('settled')
})

type Phase = 'hidden' | 'A' | 'B' | 'Bq' | 'Bend' | 'C' | 'D' | 'E' | 'F'
const phase = computed<Phase>(() => {
  switch (st.result) {
    case 'kept': return 'C'
    case 'notkept': return 'D'
    case 'nothing': return 'E'
    case 'failed': return 'F'
  }
  if (!st.pending) return 'hidden'
  const k = props.plan?.kind
  // Only the floating banner can be on a page that has lost the camera; the
  // inline one stays in A (stale) under it.
  if (props.variant === 'floating' && !props.reachable && k && k !== 'same') {
    if (left.value <= 0) return 'Bend'
    return k === 'moves' ? 'B' : 'Bq'
  }
  return 'A'
})

// The floating banner steps aside while the inline one (Network tab) is on
// screen showing the same thing; lost-contact states are floating-only.
const visible = computed(() => {
  if (phase.value === 'hidden') return false
  if (props.variant === 'inline') return true
  return st.inline === 0 || phase.value === 'B' || phase.value === 'Bq' || phase.value === 'Bend'
})

const tone = computed(() => {
  switch (phase.value) {
    case 'B': case 'Bq': case 'D': case 'E': return 'warn'
    case 'C': return 'ok'
    case 'F': return 'bad'
    case 'Bend': return 'quiet'
    default: return 'info'
  }
})
const icon = computed<IconName>(() => {
  switch (phase.value) {
    case 'C': return 'check'
    case 'D': case 'E': case 'F': return 'alert'
    default: return 'clock'
  }
})

const clock = computed(() => formatCountdown(left.value))
const pct = computed(() => Math.min(100, Math.max(0, (left.value / 90) * 100)))
const barTone = computed(() => countdownTone(left.value))
const hostname = computed(() => props.status?.name || 'the camera')
const url = computed(() => props.plan?.url ?? '')
const ip = computed(() => props.plan?.ip ?? '')
const counting = computed(() => phase.value === 'A' || phase.value === 'B' || phase.value === 'Bq')

// Announce a few milestones once each; never the ticks.
const milestone = ref('')
watch(left, (l) => {
  if (!counting.value) return
  const m = countdownMilestone(l)
  if (m && m !== milestone.value) milestone.value = m
})
watch(() => st.pending, (p) => { if (p) milestone.value = '' })

const keepBtn = ref<HTMLButtonElement | null>(null)
function focusKeep() {
  keepBtn.value?.focus()
}
defineExpose({ focusKeep })

function onKeep() {
  if (props.admin) {
    void rw.keep()
  } else {
    rw.requestConfirm()
    emit('login')
  }
}

onMounted(() => {
  rw.acquire()
  if (props.variant === 'inline') rw.registerInline()
})
onUnmounted(() => {
  rw.release()
  if (props.variant === 'inline') rw.unregisterInline()
})
</script>

<template>
  <Transition name="rise">
    <div v-if="visible" class="revert" :class="variant" :data-tone="tone">
      <span class="ricon" aria-hidden="true"><Icon :name="icon" :size="17" /></span>
      <div class="rbody">
        <div class="rtext" role="status" aria-live="polite">
          <template v-if="phase === 'A'">
            <p class="lead">New cable settings are active — <span class="tnum" aria-hidden="true">{{ clock }}</span> left to keep them.</p>
            <p>Not what you wanted? Do nothing — they’re undone when the time runs out.</p>
            <p v-if="plan?.kind === 'maybe' && url">If this page stops responding, open <a :href="url" target="_self" class="mono">{{ url }}</a> and press Keep there.</p>
            <p v-else-if="plan?.kind === 'dhcp'">The router now hands out the address. If this page stops responding, find “{{ hostname }}” in your router’s device list, open it, and press Keep there.</p>
          </template>
          <template v-else-if="phase === 'B'">
            <p class="lead">The camera moved to <span class="mono">{{ url }}</span>.</p>
            <p>Open it and sign in to keep the new settings — about <span class="tnum" aria-hidden="true">{{ clock }}</span> left. If nobody does, it comes back to this address by itself.</p>
          </template>
          <template v-else-if="phase === 'Bq'">
            <p class="lead">Lost contact with the camera — its address probably changed.</p>
            <p>Find “{{ hostname }}” in your router’s device list, open it and sign in to keep the new settings — about <span class="tnum" aria-hidden="true">{{ clock }}</span> left. If nobody does, it comes back to this address by itself.</p>
            <p v-if="plan?.kind === 'maybe' && url">It may also still answer at <a :href="url" target="_self" class="mono">{{ url }}</a>.</p>
          </template>
          <template v-else-if="phase === 'Bend'">
            <p class="lead">Time’s up.</p>
            <p v-if="url">If the settings weren’t kept, the camera is back on this address and this page reconnects by itself. If they were, it’s now at <a :href="url" target="_self" class="mono">{{ url }}</a>.</p>
            <p v-else>If the settings weren’t kept, the camera is back on this address and this page reconnects by itself. If they were, find “{{ hostname }}” in your router’s device list.</p>
          </template>
          <p v-else-if="phase === 'C'" class="lead">Kept — the new cable settings are saved.</p>
          <p v-else-if="phase === 'D'" class="lead">Not kept — the previous cable settings are back.</p>
          <p v-else-if="phase === 'E'" class="lead">Nothing left to keep — the window had already ended.</p>
          <p v-else-if="phase === 'F'" class="lead">Couldn’t reach the camera to keep the settings. Try again.</p>
          <span v-if="counting && milestone" class="sr-only">{{ milestone }}</span>
        </div>

        <div v-if="counting" class="progress" aria-hidden="true">
          <div class="bar" :data-tone="barTone" :style="{ width: `${pct}%` }"></div>
        </div>

        <div v-if="phase === 'A' || phase === 'F' || ((phase === 'B' || phase === 'Bend') && url)" class="ractions">
          <a v-if="phase === 'B' || phase === 'Bend'" class="btn btn-primary btn-sm" :href="url" target="_self">Open {{ ip }}</a>
          <button
            v-else
            ref="keepBtn"
            class="btn btn-primary btn-sm"
            :class="{ busy: st.keeping }"
            type="button"
            :disabled="st.keeping"
            @click="onKeep"
          >
            <span v-if="st.keeping" class="spin"></span>
            <span>{{ st.keeping ? 'Keeping…' : admin ? 'Keep these settings' : 'Sign in to keep' }}</span>
          </button>
        </div>
      </div>
    </div>
  </Transition>
</template>

<style scoped>
.revert {
  --c: var(--accent);
  --soft: var(--accent-soft);
  display: flex; align-items: flex-start; gap: 12px;
  padding: 12px;
  border-radius: var(--r-md);
  background: var(--surface-0);
  box-shadow: var(--hairline), inset 3px 0 0 var(--c);
  font-size: var(--fs-sm);
  color: var(--text-2);
  line-height: 1.45;
}
.revert[data-tone="warn"] { --c: var(--warn); --soft: rgb(245 196 81 / 0.14); }
.revert[data-tone="ok"] { --c: var(--ok); --soft: var(--ok-soft); }
.revert[data-tone="bad"] { --c: var(--bad); --soft: var(--bad-soft); }
.revert[data-tone="quiet"] { --c: var(--text-3); --soft: var(--surface-2); }

.inline { margin-top: 20px; }
.floating {
  position: fixed;
  top: 56px;
  left: 50%;
  transform: translateX(-50%);
  width: calc(100% - 32px);
  max-width: 560px;
  z-index: 150;
  background: var(--surface-1);
  box-shadow: var(--shadow-lg), var(--hairline), inset 3px 0 0 var(--c);
}
/* The shared `rise` transition animates transform; keep the centring. */
.floating.rise-enter-from { transform: translate(-50%, 6px); }
.floating.rise-leave-to { transform: translate(-50%, -4px); }

.ricon {
  width: 30px; height: 30px; flex: none;
  display: grid; place-items: center;
  border-radius: 9px;
  background: var(--soft);
  color: var(--c);
}
.rbody { flex: 1 1 auto; min-width: 0; display: flex; flex-direction: column; gap: 8px; }
.rtext { display: flex; flex-direction: column; gap: 3px; overflow-wrap: anywhere; }
.lead { color: var(--text); font-weight: 600; font-size: var(--fs-md); }
.rtext a { color: var(--accent); }
.progress { height: 4px; }
.progress > .bar[data-tone="warn"] { background: var(--warn); }
.progress > .bar[data-tone="bad"] { background: var(--bad); }
.progress > .bar { transition: width 1s linear, background var(--dur) var(--ease); }
.ractions { display: flex; justify-content: flex-end; }
.ractions .btn { text-decoration: none; }

@media (max-width: 640px) {
  .floating { top: 52px; }
  .ractions .btn { width: 100%; }
}
</style>
