// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Busy/feedback state wrapper for async actions in the settings tabs.

import { ref } from 'vue'
import { ApiError } from '../api'

// Wraps an async action with busy + transient feedback state, so every
// save/scan/reboot button in the settings tabs behaves the same way.
export function useAction() {
  const busy = ref(false)
  const okMsg = ref('')
  const errMsg = ref('')
  let clearTimer: number | undefined

  async function run<T>(fn: () => Promise<T>, successMsg = 'Saved.'): Promise<T | undefined> {
    if (busy.value) return
    busy.value = true
    okMsg.value = ''
    errMsg.value = ''
    if (clearTimer) clearTimeout(clearTimer)
    try {
      const result = await fn()
      okMsg.value = successMsg
      clearTimer = window.setTimeout(() => (okMsg.value = ''), 3000)
      return result
    } catch (e) {
      errMsg.value =
        e instanceof ApiError
          ? e.status === 401
            ? 'Session expired — please sign in again.'
            : `Failed: ${e.message}`
          : 'Something went wrong.'
      return undefined
    } finally {
      busy.value = false
    }
  }

  return { busy, okMsg, errMsg, run }
}
