<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Settings > About: version information and integration URLs.
-->
<script setup lang="ts">
import { computed, ref } from 'vue'
import type { Status } from '../../types'
import Icon from '../Icon.vue'
import { copyText, selectText } from '../../utils/clipboard'

const props = defineProps<{ status: Status | null }>()


function fmtUptime(s: number): string {
  const d = Math.floor(s / 86400)
  const h = Math.floor((s % 86400) / 3600)
  const m = Math.floor((s % 3600) / 60)
  const parts = []
  if (d) parts.push(`${d}d`)
  if (h || d) parts.push(`${h}h`)
  parts.push(`${m}m`)
  return parts.join(' ')
}
const uptime = computed(() => (props.status ? fmtUptime(props.status.uptime) : '—'))
// camd reports "camd <version>"; the heading already names the project.
const version = computed(() => props.status?.version.replace(/^camd\s+/, '') ?? '…')

// Integration URLs: what you paste into Home Assistant / PrusaLink.
const origin = location.origin
const endpoints = [
  { label: 'MJPEG stream', path: '/stream.mjpg', note: 'Home Assistant “MJPEG IP Camera”' },
  { label: 'Snapshot', path: '/snapshot.jpg', note: 'Home Assistant “Generic Camera” · PrusaLink pull' },
]
const copied = ref('')
// Path whose copy failed: its URL is selected for a manual Ctrl/Cmd+C.
const copyFailed = ref('')
let copiedTimer: number | undefined
async function copy(path: string, ev: MouseEvent) {
  const row = (ev.currentTarget as HTMLElement | null)?.closest('li')
  if (copiedTimer) clearTimeout(copiedTimer)
  if (await copyText(origin + path)) {
    copyFailed.value = ''
    copied.value = path
    copiedTimer = window.setTimeout(() => (copied.value = ''), 1600)
    return
  }
  // No clipboard access at all: select the URL so the user can copy it.
  copied.value = ''
  copyFailed.value = path
  const el = row?.querySelector('.eurl')
  if (el) selectText(el)
}
const copyKey = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent) ? '⌘C' : 'Ctrl+C'
</script>

<template>
  <section class="group hero">
    <div class="mark"><Icon name="camera" :size="26" /></div>
    <div>
      <h3>opencamd <span class="ver mono">{{ version }}</span></h3>
      <p>A small, local-only firmware for the Edimax IC-3115W. No cloud, no accounts, no telemetry.</p>
    </div>
  </section>

  <section class="group">
    <div class="readouts">
      <span class="k">Model</span><span class="v">{{ status?.model ?? '—' }}</span>
      <span class="k">Hardware</span><span class="v">RTL8196C · 390 MHz MIPS · 32 MB</span>
      <span class="k">Uptime</span><span class="v">{{ uptime }}</span>
      <span class="k">MAC address</span><span class="v mono">{{ status?.net.mac ?? '—' }}</span>
    </div>
  </section>

  <section class="group">
    <header>
      <div>
        <h3>Integration URLs</h3>
        <p>Both are open (no password) so other tools on your network can use them.</p>
      </div>
    </header>
    <ul class="endpoints">
      <li v-for="e in endpoints" :key="e.path">
        <div class="etext">
          <span class="elabel">{{ e.label }}</span>
          <a class="mono eurl" :href="e.path" target="_blank" rel="noopener">{{ origin }}{{ e.path }}</a>
          <span v-if="copyFailed === e.path" class="hint err" role="status">Couldn’t copy automatically — URL selected, press {{ copyKey }} (or long-press it).</span>
          <span v-else class="hint">{{ e.note }}</span>
        </div>
        <button class="btn btn-sm" type="button" :aria-label="`Copy ${e.label} URL`" @click="copy(e.path, $event)">
          <Transition name="fade" mode="out-in">
            <span v-if="copied === e.path" key="ok" class="copied"><Icon name="check" :size="14" /> Copied</span>
            <span v-else key="c"><Icon name="copy" :size="14" /> Copy</span>
          </Transition>
        </button>
      </li>
    </ul>
  </section>

  <section class="group credits">
    <p class="hint">
      Built on civetweb and Vue. Free software under the GPLv2 or later — source and credits ship with the
      device. The fps number is honest: this sensor does ~10 fps and we don’t pretend otherwise.
    </p>
  </section>
</template>

<style scoped>
.hero { display: flex; align-items: center; gap: 16px; }
.mark {
  width: 56px; height: 56px; flex: none;
  display: grid; place-items: center;
  border-radius: 16px;
  background: linear-gradient(145deg, var(--accent-soft), rgb(255 138 76 / 0.04));
  color: var(--accent);
  box-shadow: inset 0 0 0 1px rgb(255 138 76 / 0.25);
}
.hero h3 { font-size: var(--fs-xl); font-weight: 650; letter-spacing: -0.3px; display: flex; align-items: baseline; gap: 10px; flex-wrap: wrap; }
.ver { font-size: var(--fs-sm); font-weight: 500; color: var(--text-2); }
.hero p { color: var(--text-2); font-size: var(--fs-sm); margin-top: 3px; line-height: 1.5; }

.endpoints { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 8px; }
.endpoints li {
  display: flex; align-items: center; gap: 12px;
  padding: 12px 14px;
  border-radius: var(--r-md);
  background: var(--surface-0);
  box-shadow: var(--hairline);
}
.etext { flex: 1 1 auto; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.elabel { font-weight: 600; font-size: var(--fs-md); }
.eurl { color: var(--accent); text-decoration: none; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: var(--fs-sm); }
.eurl:hover { text-decoration: underline; }
.endpoints .btn { flex: none; min-width: 88px; }
.copied { color: var(--ok); display: inline-flex; align-items: center; gap: 6px; }
.endpoints .btn > span { display: inline-flex; align-items: center; gap: 6px; }
.credits { padding-bottom: 4px; }
</style>
