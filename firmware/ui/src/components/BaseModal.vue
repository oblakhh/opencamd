<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Dialog shell: backdrop, panel, focus trap, Esc/backdrop to close.
-->
<script setup lang="ts">
import { onMounted, onBeforeUnmount, ref, nextTick } from 'vue'
import Icon from './Icon.vue'

// A dialog shell: backdrop, panel, focus trap, Esc/backdrop to close, focus
// restore. Closing is animated: the panel leaves, then `close` is emitted so
// the parent can unmount it. On narrow screens it becomes a bottom sheet.
const props = defineProps<{
  title: string
  size?: 'sm' | 'lg'
  // Hide the built-in header (the parent draws its own).
  bare?: boolean
  // No body padding/scroll: the slot content manages its own layout.
  flush?: boolean
}>()
const emit = defineEmits<{ close: [] }>()

const visible = ref(false)
const panel = ref<HTMLElement | null>(null)
let previouslyFocused: HTMLElement | null = null
let closing = false

function requestClose() {
  if (closing) return
  closing = true
  visible.value = false
}

function focusables(): HTMLElement[] {
  if (!panel.value) return []
  return Array.from(
    panel.value.querySelectorAll<HTMLElement>(
      'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]',
    ),
  ).filter(
    (el) => el.getAttribute('tabindex') !== '-1' && (el.offsetParent !== null || el === document.activeElement),
  )
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') {
    e.stopPropagation()
    requestClose()
    return
  }
  if (e.key !== 'Tab') return
  const items = focusables()
  if (items.length === 0) return
  const first = items[0]
  const last = items[items.length - 1]
  const active = document.activeElement as HTMLElement
  if (e.shiftKey && (active === first || !panel.value?.contains(active))) {
    e.preventDefault()
    last.focus()
  } else if (!e.shiftKey && (active === last || !panel.value?.contains(active))) {
    e.preventDefault()
    first.focus()
  }
}

onMounted(async () => {
  previouslyFocused = document.activeElement as HTMLElement
  document.addEventListener('keydown', onKeydown, true)
  visible.value = true
  await nextTick()
  // Prefer the first text input; fall back to the first focusable.
  const items = focusables()
  const firstInput = items.find((el) => el.matches('input:not([type="checkbox"]):not([type="radio"]), select, textarea'))
  ;(firstInput ?? items[0])?.focus({ preventScroll: true })
})

onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKeydown, true)
  previouslyFocused?.focus?.({ preventScroll: true })
})

defineExpose({ requestClose })
</script>

<template>
  <Transition name="modal" appear @after-leave="emit('close')">
    <div v-if="visible" class="overlay" @mousedown.self="requestClose">
      <div
        ref="panel"
        class="panel"
        :class="[props.size ?? 'sm']"
        role="dialog"
        aria-modal="true"
        :aria-label="props.title"
      >
        <div class="grabber" aria-hidden="true"></div>
        <header v-if="!props.bare" class="head">
          <h2>{{ props.title }}</h2>
          <button class="btn btn-icon" type="button" aria-label="Close" @click="requestClose">
            <Icon name="x" :size="18" />
          </button>
        </header>
        <slot name="header" :close="requestClose" />
        <slot name="nav" />
        <div class="body" :class="{ flush: props.flush }">
          <slot :close="requestClose" />
        </div>
        <slot name="footer" />
      </div>
    </div>
  </Transition>
</template>

<style scoped>
.overlay {
  position: fixed;
  inset: 0;
  z-index: 100;
  display: flex;
  align-items: flex-start;
  justify-content: center;
  padding: max(5vh, env(safe-area-inset-top)) 16px 24px;
}
/* The blurred scrim lives on its own layer: a backdrop-filter on an ancestor
   of a nested scroller makes Chrome drop paints of newly inserted children. */
.overlay::before {
  content: "";
  position: absolute;
  inset: 0;
  background: rgb(4 6 9 / 0.62);
  backdrop-filter: blur(10px) saturate(120%);
  -webkit-backdrop-filter: blur(10px) saturate(120%);
}
.panel {
  position: relative;
  display: flex;
  flex-direction: column;
  width: 100%;
  max-height: calc(100dvh - max(5vh, env(safe-area-inset-top)) - 24px);
  background: var(--surface-1);
  border-radius: var(--r-xl);
  box-shadow: var(--shadow-lg), var(--hairline);
  overflow: hidden;
}
.panel.sm { max-width: 400px; }
.panel.lg { max-width: 760px; }
.grabber { display: none; }

.head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 18px 20px 0;
}
.head h2 { font-size: var(--fs-lg); font-weight: 600; letter-spacing: -0.2px; }
.body {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  padding: 18px 20px 22px;
  overscroll-behavior: contain;
}
.body.flush { padding: 0; overflow: hidden; display: flex; flex-direction: column; }

/* Open / close */
.modal-enter-active, .modal-enter-active::before { transition: opacity var(--dur-slow) var(--ease-out); }
.modal-leave-active, .modal-leave-active::before { transition: opacity 140ms var(--ease); }
.modal-enter-active .panel { transition: transform var(--dur-slow) var(--ease-out), opacity var(--dur-slow) var(--ease-out); }
.modal-leave-active .panel { transition: transform 140ms var(--ease), opacity 140ms var(--ease); }
.modal-enter-from, .modal-leave-to { opacity: 0; }
.modal-enter-from .panel { transform: translateY(10px) scale(0.975); opacity: 0; }
.modal-leave-to .panel { transform: translateY(6px) scale(0.985); opacity: 0; }

/* Phone: bottom sheet */
@media (max-width: 600px) {
  .overlay { align-items: flex-end; padding: 0; }
  .panel {
    max-width: none;
    max-height: 94dvh;
    border-radius: var(--r-xl) var(--r-xl) 0 0;
    padding-bottom: env(safe-area-inset-bottom);
  }
  .grabber {
    display: block;
    width: 36px; height: 4px;
    margin: 8px auto 0;
    border-radius: 2px;
    background: var(--line-strong);
  }
  .head { padding-top: 12px; }
  .modal-enter-from .panel { transform: translateY(40px); opacity: 1; }
  .modal-leave-to .panel { transform: translateY(40px); opacity: 1; }
}
</style>
