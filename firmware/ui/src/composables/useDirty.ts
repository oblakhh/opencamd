// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Dirty-tracking for forms against their last saved snapshot.

import { computed, ref, type Ref } from 'vue'

// Tracks whether a reactive form differs from its last "clean" snapshot, so a
// Save button can honestly say "No changes" until there is something to save.
// Pass a getter that returns the serialisable form state.
export function useDirty(get: () => unknown) {
  const snapshot: Ref<string> = ref(JSON.stringify(get()))
  const dirty = computed(() => JSON.stringify(get()) !== snapshot.value)
  function markClean() {
    snapshot.value = JSON.stringify(get())
  }
  return { dirty, markClean }
}
