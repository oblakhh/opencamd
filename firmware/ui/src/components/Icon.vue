<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Renders one of the inline SVG icons from icons.ts.
-->
<script setup lang="ts">
import { ICONS, type IconName } from '../icons'

withDefaults(defineProps<{ name: IconName; size?: number | string }>(), { size: 18 })
</script>

<template>
  <svg
    class="icon"
    :width="size"
    :height="size"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="1.75"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    focusable="false"
    v-html="ICONS[name]"
  />
</template>

<style scoped>
.icon { flex: none; display: inline-block; vertical-align: -0.2em; }
</style>
