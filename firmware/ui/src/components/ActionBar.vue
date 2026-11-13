<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Form footer: transient feedback plus the primary (save) action.
-->
<script setup lang="ts">
import Icon from './Icon.vue'

// The row at the bottom of every form: transient feedback on the left, the
// primary action on the right. `dirty=false` turns the button into an honest
// "No changes" state instead of a dead-looking disabled Save.
withDefaults(
  defineProps<{
    busy: boolean
    ok?: string
    err?: string
    label?: string
    busyLabel?: string
    dirty?: boolean
    hint?: string
    idleLabel?: string
    // Stick to the bottom of the scrolling pane so Save is always in reach.
    sticky?: boolean
  }>(),
  { label: 'Save changes', busyLabel: 'Saving…', dirty: true, ok: '', err: '', hint: '', idleLabel: 'No changes' },
)
const emit = defineEmits<{ click: [] }>()
</script>

<template>
  <div class="actionbar" :class="{ sticky }">
    <div class="status" aria-live="polite">
      <Transition name="fade" mode="out-in">
        <span v-if="err" key="err" class="st bad"><Icon name="alert" :size="15" />{{ err }}</span>
        <span v-else-if="ok" key="ok" class="st ok"><Icon name="check" :size="15" />{{ ok }}</span>
        <span v-else-if="dirty && hint" key="hint" class="st dim">{{ hint }}</span>
      </Transition>
    </div>
    <button class="btn btn-primary" :class="{ busy }" type="button" :disabled="busy || !dirty" @click="emit('click')">
      <span v-if="busy" class="spin"></span>
      <span>{{ busy ? busyLabel : dirty ? label : idleLabel }}</span>
    </button>
  </div>
</template>

<style scoped>
.actionbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 14px;
  margin-top: 20px;
  padding-top: 16px;
  border-top: 1px solid var(--line);
}
.status { flex: 1 1 auto; min-width: 0; font-size: var(--fs-sm); }
.st { display: inline-flex; align-items: center; gap: 7px; line-height: 1.35; }
.st.ok { color: var(--ok); }
.st.bad { color: var(--bad); }
.st.dim { color: var(--text-3); }
.btn { flex: none; min-width: 128px; }
.sticky {
  position: sticky;
  bottom: 0;
  z-index: 2;
  margin-inline: calc(-1 * var(--pad-x, 0px));
  padding: 12px var(--pad-x, 0px);
  background: var(--surface-1);
}
.sticky::before {
  content: "";
  position: absolute;
  left: 0; right: 0; top: -18px; height: 18px;
  background: linear-gradient(to bottom, transparent, var(--surface-1));
  pointer-events: none;
}
@media (max-width: 420px) {
  .actionbar { flex-direction: column-reverse; align-items: stretch; }
  .status:empty { display: none; }
}
</style>
