<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Settings dialog: tab navigation and the shared config load/save path.
-->
<script setup lang="ts">
import { ref, shallowRef, onMounted, computed, watch } from 'vue'
import type { Config, ConfigPatch, Status } from '../types'
import { api } from '../api'
import { mergePatch } from '../utils/merge'
import type { AddressPlan } from '../utils/network'
import type { IconName } from '../icons'
import BaseModal from './BaseModal.vue'
import Icon from './Icon.vue'
import VideoTab from './tabs/VideoTab.vue'
import NetworkTab from './tabs/NetworkTab.vue'
import UploadsTab from './tabs/UploadsTab.vue'
import SystemTab from './tabs/SystemTab.vue'
import AboutTab from './tabs/AboutTab.vue'

defineProps<{ status: Status | null }>()
const emit = defineEmits<{ close: []; reboot: [label: string]; logout: []; applied: [plan: AddressPlan] }>()

interface TabDef { id: Tab; icon: IconName; blurb: string }
type Tab = 'Video' | 'Network' | 'Uploads' | 'System' | 'About'
const tabs: TabDef[] = [
  { id: 'Video', icon: 'video', blurb: 'Resolution and picture quality for the stream and snapshots.' },
  { id: 'Network', icon: 'network', blurb: 'Cable, Wi-Fi backup and the camera’s name.' },
  { id: 'Uploads', icon: 'upload', blurb: 'Push snapshots to your printer, or anything with an HTTP endpoint.' },
  { id: 'System', icon: 'chip', blurb: 'Time, logging, password, firmware and power.' },
  { id: 'About', icon: 'info', blurb: 'What this camera is running.' },
]
const active = ref<Tab>('Video')
const current = computed(() => tabs.find((t) => t.id === active.value)!)
const tabEls = ref<HTMLButtonElement[]>([])
const body = ref<HTMLElement | null>(null)
// The scroll pane is shared by all tabs; start each one at the top.
watch(active, () => body.value?.scrollTo({ top: 0 }))

// Roving focus for the tablist: arrows move, Home/End jump.
function onTabKey(e: KeyboardEvent, i: number) {
  const n = tabs.length
  let next: number | null = null
  if (e.key === 'ArrowDown' || e.key === 'ArrowRight') next = (i + 1) % n
  else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') next = (i - 1 + n) % n
  else if (e.key === 'Home') next = 0
  else if (e.key === 'End') next = n - 1
  if (next === null) return
  e.preventDefault()
  active.value = tabs[next].id
  tabEls.value[next]?.focus()
}

const cfg = shallowRef<Config | null>(null)
const loadError = ref('')
const loading = ref(false)

async function load() {
  loading.value = true
  loadError.value = ''
  try {
    cfg.value = await api.getConfig()
  } catch {
    loadError.value = 'Couldn’t load the camera’s settings.'
  } finally {
    loading.value = false
  }
}
onMounted(load)

// Quiet reload after a change made outside `save` (Network tab): keeps the
// current cfg on screen meanwhile, and a failure just leaves it as it was.
async function reload() {
  try {
    cfg.value = await api.getConfig()
  } catch {
    /* keep what we have */
  }
}

// Single save path for every tab. Applies the server's returned config as the
// new source of truth (falling back to an optimistic local merge).
async function save(patch: ConfigPatch): Promise<Config | undefined> {
  const updated = await api.putConfig(patch)
  cfg.value = updated ?? (cfg.value ? mergePatch(cfg.value as never, patch as never) : cfg.value)
  return cfg.value ?? undefined
}

async function logout() {
  try { await api.logout() } catch { /* ignore */ }
  emit('logout')
}
</script>

<template>
  <BaseModal title="Settings" size="lg" bare flush @close="emit('close')">
    <template #default="{ close }">
      <div class="settings">
        <aside class="side">
          <div class="side-head">
            <h2>Settings</h2>
          </div>
          <nav class="tabs" role="tablist" aria-label="Settings sections" aria-orientation="vertical">
            <button
              v-for="(t, i) in tabs"
              :key="t.id"
              :ref="(el) => { if (el) tabEls[i] = el as HTMLButtonElement }"
              role="tab"
              type="button"
              class="tab"
              :class="{ on: active === t.id }"
              :id="`tab-${t.id}`"
              :aria-selected="active === t.id"
              :aria-controls="`panel-${t.id}`"
              :tabindex="active === t.id ? 0 : -1"
              @click="active = t.id"
              @keydown="onTabKey($event, i)"
            >
              <Icon :name="t.icon" :size="17" />
              <span>{{ t.id }}</span>
            </button>
          </nav>
          <div class="side-foot">
            <button class="btn btn-ghost btn-sm signout" type="button" @click="logout">
              <Icon name="logout" :size="15" />
              <span>Sign out</span>
            </button>
          </div>
        </aside>

        <section class="content">
          <header class="content-head">
            <div>
              <h3>{{ current.id }}</h3>
              <p>{{ current.blurb }}</p>
            </div>
            <button class="btn btn-icon" type="button" aria-label="Close settings" @click="close">
              <Icon name="x" :size="18" />
            </button>
          </header>

          <div ref="body" class="content-body" :id="`panel-${active}`" role="tabpanel" :aria-labelledby="`tab-${active}`">
            <div v-if="loadError" class="state">
              <div class="state-icon bad"><Icon name="alert" :size="22" /></div>
              <p>{{ loadError }}</p>
              <button class="btn btn-sm" type="button" :disabled="loading" @click="load">
                <span v-if="loading" class="spin"></span><Icon v-else name="refresh" :size="15" /> Try again
              </button>
            </div>

            <div v-else-if="!cfg" class="skeleton" aria-busy="true" aria-label="Loading settings">
              <div class="sk w40"></div>
              <div class="sk tall"></div>
              <div class="sk w30"></div>
              <div class="sk tall"></div>
              <div class="sk w60"></div>
            </div>

            <Transition v-else name="tabswap" mode="out-in">
              <div :key="active" class="tab-inner">
                <VideoTab v-if="active === 'Video'" :cfg="cfg" :status="status" :save="save" />
                <NetworkTab
                  v-else-if="active === 'Network'"
                  :cfg="cfg"
                  :status="status"
                  :save="save"
                  @applied="(p) => emit('applied', p)"
                  @refresh="reload"
                />
                <UploadsTab v-else-if="active === 'Uploads'" :cfg="cfg" :status="status" :save="save" />
                <SystemTab
                  v-else-if="active === 'System'"
                  :cfg="cfg"
                  :save="save"
                  @reboot="(l) => emit('reboot', l)"
                />
                <AboutTab v-else :status="status" />
              </div>
            </Transition>
          </div>
        </section>
      </div>
    </template>
  </BaseModal>
</template>

<style scoped>
.settings {
  display: grid;
  grid-template-columns: 196px minmax(0, 1fr);
  flex: 1 1 auto;
  min-height: 0;
  height: min(640px, calc(100dvh - 10vh - 24px));
}

/* Side rail */
.side {
  display: flex;
  flex-direction: column;
  background: var(--surface-0);
  border-right: 1px solid var(--line);
  padding: 18px 12px 12px;
}
.side-head { padding: 2px 10px 14px; }
.side-head h2 { font-size: var(--fs-lg); font-weight: 650; letter-spacing: -0.2px; }
.tabs { display: flex; flex-direction: column; gap: 2px; }
.tab {
  display: flex;
  align-items: center;
  gap: 11px;
  padding: 9px 10px;
  border-radius: var(--r-sm);
  color: var(--text-2);
  font-size: var(--fs-md);
  font-weight: 500;
  text-align: left;
  transition: background var(--dur) var(--ease), color var(--dur) var(--ease);
}
.tab :deep(svg) { color: var(--text-3); transition: color var(--dur) var(--ease); }
.tab:hover { color: var(--text); background: var(--surface-2); }
.tab:hover :deep(svg) { color: var(--text-2); }
.tab.on { color: var(--text); background: var(--surface-2); box-shadow: var(--hairline); }
.tab.on :deep(svg) { color: var(--accent); }
.side-foot { margin-top: auto; padding-top: 12px; }
.signout { width: 100%; justify-content: flex-start; padding-left: 10px; }

/* Content pane */
.content { display: flex; flex-direction: column; min-width: 0; min-height: 0; }
.content-head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
  padding: 20px 20px 14px 28px;
  border-bottom: 1px solid var(--line);
}
.content-head h3 { font-size: var(--fs-xl); font-weight: 650; letter-spacing: -0.3px; }
.content-head p { color: var(--text-2); font-size: var(--fs-sm); margin-top: 3px; }
.content-body:has(.actionbar.sticky) { padding-bottom: 0; }
.content-body {
  --pad-x: 28px;
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  overscroll-behavior: contain;
  padding: 22px var(--pad-x) 26px;
}

/* Tab swap */
.tabswap-enter-active { transition: opacity 180ms var(--ease-out), transform 180ms var(--ease-out); }
.tabswap-leave-active { transition: opacity 90ms var(--ease); }
.tabswap-enter-from { opacity: 0; transform: translateY(6px); }
.tabswap-leave-to { opacity: 0; }

/* Empty / error / loading */
.state {
  display: flex; flex-direction: column; align-items: center; gap: 12px;
  padding: 48px 16px; text-align: center; color: var(--text-2);
}
.state-icon { width: 44px; height: 44px; border-radius: 12px; display: grid; place-items: center; background: var(--surface-2); }
.state-icon.bad { color: var(--bad); background: var(--bad-soft); }
.skeleton { display: flex; flex-direction: column; gap: 12px; }
.sk {
  height: 14px; border-radius: 6px;
  background: linear-gradient(90deg, var(--surface-2) 25%, var(--surface-3) 50%, var(--surface-2) 75%);
  background-size: 200% 100%;
  animation: shimmer 1.4s linear infinite;
}
.sk.tall { height: 38px; border-radius: var(--r-sm); }
.sk.w40 { width: 40%; } .sk.w30 { width: 30%; } .sk.w60 { width: 60%; }
@keyframes shimmer { to { background-position: -200% 0; } }

/* Phone: header on top, horizontal icon strip, content below */
@media (max-width: 640px) {
  .settings {
    grid-template-columns: 1fr;
    grid-template-rows: auto minmax(0, 1fr);
    height: auto;
    min-height: 55dvh;
  }
  .side {
    flex-direction: row;
    align-items: center;
    gap: 8px;
    padding: 6px 10px;
    border-right: 0;
    border-bottom: 1px solid var(--line);
    overflow-x: auto;
    scrollbar-width: none;
    -webkit-overflow-scrolling: touch;
    background: transparent;
    /* Fade the strip's right edge so it's obvious there's more to scroll. */
    mask-image: linear-gradient(to right, #000 calc(100% - 36px), transparent);
    -webkit-mask-image: linear-gradient(to right, #000 calc(100% - 36px), transparent);
  }
  .side::-webkit-scrollbar { display: none; }
  .side-head { display: none; }
  .tabs { flex-direction: row; gap: 4px; }
  .tab { padding: 7px 11px; border-radius: 999px; font-size: var(--fs-sm); gap: 7px; white-space: nowrap; }
  .side-foot { margin: 0 0 0 auto; padding: 0; }
  .signout { width: auto; }
  .signout span { display: none; }
  .content-head { padding: 14px 14px 12px 18px; }
  .content-head h3 { font-size: var(--fs-lg); }
  .content-body { --pad-x: 18px; padding: 18px var(--pad-x) 24px; }
}
</style>
