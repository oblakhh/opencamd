<!--
  SPDX-License-Identifier: GPL-2.0-or-later
  Copyright (C) 2026 opencamd contributors
  Settings > Uploads: snapshot uploader target, interval, method and headers.
-->
<script setup lang="ts">
import { computed, reactive, watch } from 'vue'
import type { Config, ConfigPatch, Status, UploadMethod } from '../../types'
import { describeUploadStatus, formatCadence, isUploadValid, normalizeHeaders, validateUpload } from '../../utils/upload'
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
  enabled: props.cfg.upload.enabled,
  url: props.cfg.upload.url,
  interval: props.cfg.upload.interval,
  method: props.cfg.upload.method as UploadMethod,
  field: props.cfg.upload.field,
  headers: props.cfg.upload.headers,
})
const { dirty, markClean } = useDirty(() => form)
const { busy, okMsg, errMsg, run } = useAction()

// Errors are computed live; they only appear once something is actually
// wrong (a saved config is valid by construction), so no "touched" tracking.
const errors = computed(() => validateUpload(form))
const valid = computed(() => isUploadValid(errors.value))
// If a save was refused for validation, let the message go as soon as it's fixed.
watch(valid, (v) => { if (v && errMsg.value === INVALID_MSG) errMsg.value = '' })
const INVALID_MSG = 'Fix the highlighted fields first.'

const cadence = computed(() => (errors.value.interval ? '' : formatCadence(form.interval)))
const isPost = computed(() => form.method === 'POST')
const methodBlurb = computed(() =>
  isPost.value
    ? 'Multipart form upload — what OctoPrint-style servers expect.'
    : 'Raw JPEG as the request body — what PrusaLink expects.',
)

// Live uploader state from the status poll, worded with the *saved* interval.
const live = computed(() => describeUploadStatus(props.status?.upload, props.cfg.upload.interval))

async function onSave() {
  if (!valid.value) {
    errMsg.value = INVALID_MSG
    return
  }
  // Normalise what we send and keep the form in step with it.
  form.url = form.url.trim()
  form.field = form.field.trim()
  form.headers = normalizeHeaders(form.headers)
  form.interval = Math.round(form.interval)
  const updated = await run(() =>
    props.save({
      upload: {
        enabled: form.enabled,
        url: form.url,
        interval: form.interval,
        method: form.method,
        field: form.field,
        headers: form.headers,
      },
    }),
  )
  if (updated) markClean()
}
</script>

<template>
  <section class="group">
    <header>
      <div>
        <h3>Snapshot uploader</h3>
        <p>Pushes the current JPEG to your printer, or any HTTP endpoint, on a timer.</p>
      </div>
      <input v-model="form.enabled" class="switch" type="checkbox" role="switch" aria-label="Enable snapshot uploads" />
    </header>
    <div class="live tnum" :data-tone="live.tone" role="status" aria-live="polite">
      <span class="live-dot" aria-hidden="true"></span>
      <span class="live-text">{{ live.text }}</span>
    </div>
  </section>

  <section class="group">
    <header>
      <div>
        <h3>Target</h3>
        <p>Where each snapshot goes, and how often.</p>
      </div>
    </header>

    <div class="field">
      <label for="up-url">URL</label>
      <input
        id="up-url"
        v-model="form.url"
        class="mono"
        placeholder="http://printer.local/api/v1/cameras/snap"
        spellcheck="false"
        autocapitalize="off"
        autocomplete="off"
        inputmode="url"
        :aria-invalid="errors.url ? 'true' : undefined"
        :aria-describedby="errors.url ? 'up-url-err' : undefined"
      />
      <p v-if="errors.url" id="up-url-err" class="hint err">{{ errors.url }}</p>
      <p v-else class="hint">Both <span class="mono">http://</span> and <span class="mono">https://</span> work; HTTPS uses the camera’s built-in TLS 1.2 client and follows redirects.</p>
    </div>

    <div class="grid">
      <div class="field">
        <label for="up-int">Interval</label>
        <div class="input-wrap unit">
          <input
            id="up-int"
            v-model.number="form.interval"
            type="number"
            min="1"
            step="1"
            inputmode="numeric"
            :aria-invalid="errors.interval ? 'true' : undefined"
          />
          <span class="suffix">sec</span>
        </div>
        <p v-if="errors.interval" class="hint err">{{ errors.interval }}</p>
        <p v-else class="hint tnum">{{ cadence }}</p>
      </div>
      <div class="field">
        <span class="label" id="up-method">Method</span>
        <div class="segmented" role="radiogroup" aria-labelledby="up-method">
          <label><input v-model="form.method" type="radio" name="up-method" value="POST" /><span>POST</span></label>
          <label><input v-model="form.method" type="radio" name="up-method" value="PUT" /><span>PUT</span></label>
        </div>
        <p class="hint" :class="{ err: errors.method }">{{ errors.method ?? methodBlurb }}</p>
      </div>
    </div>

    <div class="field">
      <label for="up-field">Form field</label>
      <input
        id="up-field"
        v-model="form.field"
        class="mono"
        placeholder="file"
        spellcheck="false"
        autocapitalize="off"
        autocomplete="off"
        :disabled="!isPost"
      />
      <p class="hint">
        <template v-if="isPost">Name of the multipart field that carries the JPEG. OctoPrint-style servers usually expect <span class="mono">file</span>.</template>
        <template v-else>Not used with PUT — the JPEG is the whole request body.</template>
      </p>
    </div>

    <div class="field">
      <label for="up-hdr">Extra headers</label>
      <textarea
        id="up-hdr"
        v-model="form.headers"
        class="mono headers"
        rows="3"
        placeholder="Fingerprint: <any stable id>&#10;Token: <your PrusaLink API token>"
        spellcheck="false"
        autocapitalize="off"
        autocomplete="off"
        wrap="off"
        :aria-invalid="errors.headers ? 'true' : undefined"
        :aria-describedby="errors.headers ? 'up-hdr-err' : undefined"
      ></textarea>
      <p v-if="errors.headers" id="up-hdr-err" class="hint err">{{ errors.headers }}</p>
      <p v-else class="hint">
        Optional. One <span class="mono">Name: value</span> per line, each sent verbatim with every upload.
        PrusaLink wants two — <span class="mono">Fingerprint</span> and <span class="mono">Token</span>;
        OctoPrint wants <span class="mono">X-Api-Key</span>. Stored and shown in the clear.
      </p>
    </div>
  </section>

  <ActionBar
    :busy="busy"
    :ok="okMsg"
    :err="errMsg"
    :dirty="dirty"
    sticky
    label="Save upload settings"
    hint="Unsaved changes"
    @click="onSave"
  />
</template>

<style scoped>
/* Live status row: a readout-style strip with a semantic dot. */
.live {
  --c: var(--text-3);
  display: flex; align-items: center; gap: 10px;
  padding: 10px 14px;
  border-radius: var(--r-sm);
  background: var(--surface-0);
  box-shadow: var(--hairline);
  font-size: var(--fs-md);
  color: var(--text-2);
}
.live-dot { width: 8px; height: 8px; flex: none; border-radius: 50%; background: var(--c); }
.live-text { min-width: 0; overflow-wrap: anywhere; }
.live[data-tone="ok"] { --c: var(--ok); color: var(--text); }
.live[data-tone="ok"] .live-dot { box-shadow: 0 0 8px var(--c); }
.live[data-tone="bad"] { --c: var(--bad); color: var(--text); }
.live[data-tone="bad"] .live-dot { box-shadow: 0 0 8px var(--c); }
.live[data-tone="idle"] { --c: var(--warn); color: var(--text); }
.live[data-tone="idle"] .live-dot { animation: blink 1.1s steps(1) infinite; }
@keyframes blink { 50% { opacity: 0.35; } }

.headers { resize: vertical; min-height: 86px; line-height: 1.5; white-space: pre; overflow-x: auto; }

.unit > input { padding-right: 44px; }
.suffix { position: absolute; right: 12px; color: var(--text-3); font-size: var(--fs-xs); pointer-events: none; }
</style>
