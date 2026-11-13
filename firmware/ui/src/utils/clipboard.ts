// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
//
// Copy-to-clipboard that works on the camera's plain-HTTP LAN origin.
//
// The Async Clipboard API (navigator.clipboard) is only exposed in secure
// contexts (https:// or localhost). The camera is reached as http://<lan-ip>,
// so there navigator.clipboard is undefined and writeText() can't be used.
// We then fall back to the legacy execCommand('copy') on a temporary
// off-screen <textarea>, which browsers still allow from a user gesture in
// non-secure contexts. If both fail the caller should let the user copy by
// hand (see selectText()).

/** Copy `text`; resolves true on success, false if no method worked. */
export async function copyText(text: string): Promise<boolean> {
  const nav = globalThis.navigator
  if (globalThis.isSecureContext && nav?.clipboard?.writeText) {
    try {
      await nav.clipboard.writeText(text)
      return true
    } catch { /* permission denied / not focused: try the legacy path */ }
  }
  return legacyCopy(text)
}

/** execCommand('copy') via a hidden textarea. Synchronous; must run in a user gesture. */
export function legacyCopy(text: string): boolean {
  if (typeof document === 'undefined' || !document.body) return false
  const active = document.activeElement as HTMLElement | null
  const sel = document.getSelection?.()
  const prevRange = sel && sel.rangeCount > 0 ? sel.getRangeAt(0) : null

  const ta = document.createElement('textarea')
  ta.value = text
  // readonly keeps the iOS keyboard from popping up; contentEditable is
  // still needed for iOS Safari to allow a selection on it.
  ta.setAttribute('readonly', '')
  ta.contentEditable = 'true'
  // >=16px avoids iOS zoom-on-focus; fixed + off-screen avoids scroll jumps.
  ta.style.cssText = 'position:fixed;top:0;left:-9999px;opacity:0;font-size:16px;border:0;padding:0;margin:0'
  document.body.appendChild(ta)

  let ok = false
  try {
    ta.focus()
    ta.select()
    ta.setSelectionRange(0, text.length) // iOS ignores select()
    ok = document.execCommand('copy')
  } catch {
    ok = false
  } finally {
    ta.remove()
    if (sel && prevRange) {
      sel.removeAllRanges()
      sel.addRange(prevRange)
    }
    active?.focus?.()
  }
  return ok
}

/** Select an element's text so the user can press Ctrl/Cmd+C themselves. */
export function selectText(el: Element): void {
  const sel = document.getSelection?.()
  if (!sel) return
  const range = document.createRange()
  range.selectNodeContents(el)
  sel.removeAllRanges()
  sel.addRange(range)
}
