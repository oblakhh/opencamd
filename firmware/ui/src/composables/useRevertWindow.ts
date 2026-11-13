// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Shared state of the wired "try, then keep or revert" window.

import { computed, reactive, readonly } from 'vue'
import { api, ApiError } from '../api'

// Shared state of the wired "try for 90 s, then keep or revert" window.
//
// There is one window on the device, but two RevertBanner instances show it
// (inline in the Network tab, floating at App level), so the countdown, the
// in-flight Keep and the outcome live here, once, as a module singleton. Every
// banner feeds fresh status polls into `observe()`; the calls are idempotent.

export type RevertResult = '' | 'kept' | 'notkept' | 'nothing' | 'failed'
export type RevertEvent = '' | 'confirmed' | 'settled'

const state = reactive({
  pending: false,   // a window is (believed to be) open
  seed: 0,          // revert_in at the last seed…
  seededAt: 0,      // …and when it was taken (Date.now())
  now: Date.now(),  // ticks every second while anyone is watching
  result: '' as RevertResult,
  keeping: false,   // confirm request in flight
  wantConfirm: false, // "Sign in to keep" pressed: confirm as soon as admin flips
  inline: 0,        // inline banners mounted (the floating one defers to them)
  event: '' as RevertEvent,
  seq: 0,           // bumped per event so every banner can emit it
})

let users = 0
let beganAt = 0 // local Apply landed
let endedAt = 0 // local Keep / 409 closed the window
let ticker: number | undefined
let resultTimer: number | undefined

const left = computed(() =>
  state.pending ? Math.max(0, state.seed - Math.floor((state.now - state.seededAt) / 1000)) : 0,
)

function setResult(r: RevertResult, hideAfterMs = 0) {
  if (resultTimer) clearTimeout(resultTimer)
  resultTimer = undefined
  state.result = r
  if (r && hideAfterMs) resultTimer = window.setTimeout(() => (state.result = ''), hideAfterMs)
}

function fire(e: RevertEvent) {
  state.event = e
  state.seq++
}

function seed(revertIn: number) {
  state.seed = revertIn
  state.seededAt = Date.now()
  state.now = state.seededAt
}

// Feed every *fresh* status poll here (a stale one, kept while the camera is
// unreachable, must not be re-fed — the local ticker carries on from the last seed).
let lastSource: object | null = null
function observe(revertIn: number, source?: object) {
  // Both banners see the same poll object; seeding twice from it would drift.
  if (source) {
    if (source === lastSource) return
    lastSource = source
  }
  if (revertIn > 0) {
    if (!state.pending) {
      // …and one that left before our Keep landed can still report a countdown.
      if (Date.now() - endedAt < 4000) return
      state.pending = true
      if (state.result !== 'failed') setResult('')
    }
    seed(revertIn)
  } else if (state.pending) {
    // A poll that left before our Apply landed can still report 0; ignore it.
    if (Date.now() - beganAt < 4000) return
    // Window over and nobody here kept it: the old settings are back.
    state.pending = false
    state.wantConfirm = false
    setResult('notkept', 6000)
    fire('settled')
  }
}

// The Apply response arrived: start showing the window before the next poll.
function begin(revertIn: number) {
  beganAt = Date.now()
  state.pending = true
  setResult('')
  seed(revertIn)
}

async function keep() {
  if (state.keeping) return
  state.keeping = true
  state.wantConfirm = false
  try {
    await api.confirmWired()
    endedAt = Date.now()
    state.pending = false
    setResult('kept', 4000)
    fire('confirmed')
  } catch (e) {
    if (e instanceof ApiError && e.status === 409) {
      endedAt = Date.now()
      state.pending = false
      setResult('nothing', 4000)
      fire('settled')
    } else {
      setResult('failed')
    }
  } finally {
    state.keeping = false
  }
}

function acquire() {
  if (users++ === 0) ticker = window.setInterval(() => (state.now = Date.now()), 1000)
}
function release() {
  if (--users === 0 && ticker) {
    clearInterval(ticker)
    ticker = undefined
  }
}

export function useRevertWindow() {
  return {
    state: readonly(state),
    left,
    observe,
    begin,
    keep,
    acquire,
    release,
    requestConfirm: () => { state.wantConfirm = true },
    cancelConfirm: () => { state.wantConfirm = false },
    registerInline: () => { state.inline++ },
    unregisterInline: () => { state.inline-- },
  }
}
