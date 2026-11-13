<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Password field with a show/hide toggle.
-->
<script setup lang="ts">
import { ref } from 'vue'
import Icon from './Icon.vue'

const model = defineModel<string>({ required: true })
defineProps<{
  id?: string
  placeholder?: string
  autocomplete?: string
  disabled?: boolean
  maxlength?: number
  invalid?: boolean
  describedby?: string
}>()
const shown = ref(false)
</script>

<template>
  <div class="input-wrap">
    <input
      :id="id"
      v-model="model"
      :type="shown ? 'text' : 'password'"
      :placeholder="placeholder"
      :autocomplete="autocomplete"
      :disabled="disabled"
      :maxlength="maxlength"
      :aria-invalid="invalid ? 'true' : undefined"
      :aria-describedby="describedby"
      spellcheck="false"
      autocapitalize="off"
    />
    <button
      class="btn btn-icon"
      type="button"
      :aria-label="shown ? 'Hide password' : 'Show password'"
      :aria-pressed="shown"
      tabindex="-1"
      @click="shown = !shown"
    >
      <Icon :name="shown ? 'eyeOff' : 'eye'" :size="17" />
    </button>
  </div>
</template>
