<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Settings > Video: resolution and JPEG quality, with the live frame rate.
-->
<script setup lang="ts">
import { computed, reactive } from 'vue'
import type { Config, ConfigPatch, Status } from '../../types'
import { RESOLUTIONS, FPS_CEILING } from '../../types'
import { useAction } from '../../composables/useAction'
import { useDirty } from '../../composables/useDirty'
import ActionBar from '../ActionBar.vue'

const props = defineProps<{
  cfg: Config
  status: Status | null
  save: (patch: ConfigPatch) => Promise<Config | undefined>
}>()

// Local editing copy; applied on Save.
const form = reactive({
  resKey: `${props.cfg.video.width}x${props.cfg.video.height}`,
  quality: props.cfg.video.quality,
})
const { dirty, markClean } = useDirty(() => form)
const { busy, okMsg, errMsg, run } = useAction()

const qualityWord = computed(() => {
  const q = form.quality
  if (q < 40) return 'Light — small frames, soft detail'
  if (q < 70) return 'Balanced'
  if (q < 90) return 'Sharp — a good default'
  return 'Maximum — big frames, lower fps'
})
const pct = computed(() => `${((form.quality - 10) / 85) * 100}%`)

const resLabel = (r: { label: string }) => r.label.split(' (')[0]
const resAspect = (r: { label: string }) => r.label.match(/\((.+)\)/)?.[1] ?? ''

async function onSave() {
  const [width, height] = form.resKey.split('x').map(Number)
  const updated = await run(() => props.save({ video: { width, height, quality: form.quality } }))
  if (updated) markClean()
}
</script>

<template>
  <section class="group">
    <header>
      <div>
        <h3>Resolution</h3>
        <p>Applies to the live stream and snapshots alike.</p>
      </div>
    </header>
    <div class="segmented res" role="radiogroup" aria-label="Resolution">
      <label v-for="r in RESOLUTIONS" :key="r.width + 'x' + r.height">
        <input v-model="form.resKey" type="radio" name="res" :value="`${r.width}x${r.height}`" />
        <span class="tnum">{{ resLabel(r) }}<small v-if="resAspect(r)">{{ resAspect(r) }}</small></span>
      </label>
    </div>
  </section>

  <section class="group">
    <header>
      <div>
        <h3>JPEG quality</h3>
        <p>{{ qualityWord }}</p>
      </div>
      <output class="q tnum" for="q">{{ form.quality }}</output>
    </header>
    <input
      id="q"
      v-model.number="form.quality"
      type="range"
      min="10"
      max="95"
      step="1"
      aria-label="JPEG quality"
      :style="{ '--pct': pct }"
    />
    <div class="scale hint"><span>Smaller files</span><span>More detail</span></div>
  </section>

  <section class="group">
    <header>
      <div>
        <h3>Live</h3>
        <p>What the sensor is actually delivering right now.</p>
      </div>
    </header>
    <div class="readouts">
      <span class="k">Frame rate</span>
      <span class="v">{{ status ? `~${status.video.fps.toFixed(1)} fps` : '—' }}</span>
      <span class="k">Streaming</span>
      <span class="v">{{ status ? `${status.video.width}×${status.video.height} · q${status.video.quality}` : '—' }}</span>
    </div>
    <p class="hint note">
      This sensor tops out around {{ FPS_CEILING }} fps (≈7–8 fps at 1280×960, high quality). Lower
      resolution or quality won’t push it much past that — it’s a hardware ceiling, not a setting.
    </p>
  </section>

  <ActionBar
    :busy="busy"
    :ok="okMsg"
    :err="errMsg"
    :dirty="dirty"
    sticky
    label="Save video settings"
    hint="Unsaved changes"
    @click="onSave"
  />
</template>

<style scoped>
.q {
  font-size: var(--fs-xl);
  font-weight: 650;
  color: var(--accent);
  letter-spacing: -0.5px;
  line-height: 1;
  padding-top: 2px;
}
.scale { display: flex; justify-content: space-between; margin-top: 2px; }
.note { margin-top: 12px; }
@media (max-width: 440px) {
  .res { grid-auto-flow: row; grid-template-columns: 1fr 1fr; }
}
</style>
