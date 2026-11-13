<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Admin sign-in dialog.
-->
<script setup lang="ts">
import { ref } from 'vue'
import { api, ApiError } from '../api'
import BaseModal from './BaseModal.vue'
import PasswordInput from './PasswordInput.vue'
import Icon from './Icon.vue'

const emit = defineEmits<{ close: []; 'logged-in': [] }>()

const password = ref('')
const busy = ref(false)
const error = ref('')
const shaking = ref(false)

async function submit() {
  if (busy.value) return
  if (!password.value) {
    error.value = 'Enter the admin password.'
    shaking.value = true
    return
  }
  error.value = ''
  busy.value = true
  try {
    await api.login(password.value)
    emit('logged-in')
  } catch (e) {
    error.value = e instanceof ApiError && e.status === 401
      ? 'That password isn’t right.'
      : 'Couldn’t reach the camera.'
    shaking.value = true
  } finally {
    busy.value = false
    password.value = ''
  }
}
</script>

<template>
  <BaseModal title="Sign in" bare @close="emit('close')">
    <template #header="{ close }">
      <button class="btn btn-icon corner" type="button" aria-label="Close" @click="close">
        <Icon name="x" :size="18" />
      </button>
    </template>
    <form class="login" @submit.prevent="submit">
      <div class="mark" aria-hidden="true"><Icon name="lock" :size="22" /></div>
      <h2>Sign in</h2>
      <p class="sub">The live view is open to everyone on your network. Settings need the admin password.</p>

      <div class="field" :class="{ shake: shaking }" @animationend="shaking = false">
        <label for="pw" class="sr-only">Admin password</label>
        <PasswordInput id="pw" v-model="password" placeholder="Admin password" autocomplete="current-password" />
      </div>

      <div class="feedback" aria-live="assertive">
        <Transition name="fade">
          <div v-if="error" class="msg msg-bad"><Icon name="alert" :size="15" />{{ error }}</div>
        </Transition>
      </div>

      <button class="btn btn-primary btn-block" :class="{ busy }" type="submit" :disabled="busy">
        <span v-if="busy" class="spin"></span>
        <span>{{ busy ? 'Signing in…' : 'Sign in' }}</span>
      </button>
    </form>
  </BaseModal>
</template>

<style scoped>
.corner { position: absolute; top: 12px; right: 12px; z-index: 1; }
.login { display: flex; flex-direction: column; text-align: center; padding: 10px 4px 2px; }
.mark {
  width: 48px; height: 48px;
  margin: 0 auto 14px;
  display: grid; place-items: center;
  border-radius: 14px;
  background: var(--accent-soft);
  color: var(--accent);
  box-shadow: inset 0 0 0 1px rgb(255 138 76 / 0.25);
}
h2 { font-size: var(--fs-xl); font-weight: 650; letter-spacing: -0.3px; }
.sub { color: var(--text-2); font-size: var(--fs-sm); margin: 6px 0 22px; line-height: 1.5; }
.field { text-align: left; }
.feedback { min-height: 0; margin: 10px 0 0; }
.feedback .msg { margin-bottom: 10px; text-align: left; }
.btn-block { margin-top: 4px; min-height: 42px; }
.shake { animation: shake 0.4s var(--ease); }
@keyframes shake {
  20% { transform: translateX(-5px); }
  45% { transform: translateX(4px); }
  70% { transform: translateX(-3px); }
  100% { transform: translateX(0); }
}
</style>
