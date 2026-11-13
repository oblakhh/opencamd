// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Polls /api/status and tracks whether the camera is reachable.

import { ref, shallowRef, onMounted, onUnmounted } from 'vue'
import { api } from '../api'
import type { Status } from '../types'

// Polls /api/status on an interval. `reachable` tracks whether the control
// plane is answering (independent of the MJPEG img, which has its own signal).
export function useStatus(intervalMs = 3000) {
  const status = shallowRef<Status | null>(null)
  const reachable = ref(false)
  let timer: number | undefined
  let stopped = false

  async function poll() {
    try {
      status.value = await api.getStatus()
      reachable.value = true
    } catch {
      reachable.value = false
    }
  }

  function schedule() {
    timer = window.setTimeout(async () => {
      if (stopped) return
      await poll()
      schedule()
    }, intervalMs)
  }

  onMounted(async () => {
    await poll()
    schedule()
  })

  onUnmounted(() => {
    stopped = true
    if (timer) clearTimeout(timer)
  })

  return { status, reachable, refresh: poll }
}
