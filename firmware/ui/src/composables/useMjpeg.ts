// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Live-view composable: fetch-based MJPEG streaming with snapshot-polling fallback.

import { computed, onMounted, onUnmounted, ref, shallowRef, watch } from 'vue'
import { MultipartParser, isMultipartType, parseBoundary } from '../utils/mjpeg'
import { LatestFrameQueue } from '../utils/frameQueue'

// Live-view renderer that works on Safari as well as Chrome/Firefox, and that
// holds at most ONE connection to the camera at any time.
//
// Why not <img src="/stream.mjpg">? Safari doesn't render multipart MJPEG in
// an <img> at all, yet it still opens (and never drains) the connection. The
// camera's embedded server has only a handful of worker threads and caps
// concurrent streams at 2, so a dangling connection is expensive.
//
// Why a <canvas> and not <img src=objectURL>? Swapping an <img>'s src per
// frame makes Chrome clear-then-repaint (visible flicker), and Safari does not
// reliably fire `load` for rapid blob-URL swaps, so nothing ever showed.
// Decoding each frame with createImageBitmap() and drawImage()-ing it onto a
// canvas is atomic: no flicker, no load events to depend on.
//
// Primary path: fetch('/stream.mjpg') and read the body incrementally, split
// it into JPEG parts, decode and paint each one. Fallback path: poll
// /snapshot.jpg (~8 fps, works everywhere, not counted against the stream
// cap). A 503 "too many viewers" from the stream shows a notice, polls
// snapshots meanwhile, and retries after Retry-After.
//
// Invariants:
//  - `gen` is bumped by every stop/restart. Every async continuation checks
//    it and bails if stale, so a superseded fetch can never touch state.
//  - the stream AbortController and the snapshot AbortController are never
//    both active: polling is halted before a stream attempt begins.
//  - at most one frame is decoding at a time; newer arrivals replace older
//    ones that haven't started decoding (no backlog, no growing latency).

export type StreamMode =
  | 'connecting' // nothing on screen yet, first attempt in progress
  | 'live' // multipart stream flowing
  | 'polling' // snapshot fallback
  | 'paused' // tab hidden: connection closed on purpose
  | 'offline' // neither the stream nor snapshots are answering

export interface UseMjpegOptions {
  streamUrl?: string
  snapshotUrl?: string
  /** Delay between snapshot polls (measured from the end of the last one). */
  pollIntervalMs?: number
  /** No frame for this long on the live stream → reconnect. */
  stallMs?: number
  /** Time allowed for the stream response headers to arrive. */
  connectTimeoutMs?: number
  /** Start immediately on mount (default true). */
  autoStart?: boolean
}

const BACKOFF_MS = [3000, 6000, 12000, 24000, 30000]

// A decoded frame ready for drawImage(), plus how to release it.
interface Frame {
  source: CanvasImageSource
  width: number
  height: number
  close: () => void
}

// Decode a JPEG (or any image) blob. createImageBitmap is the fast, atomic
// path (Safari 15+, Chrome, Firefox). Older engines fall back to an
// off-screen <img> fed an object URL; the result is still painted onto the
// canvas, so the on-screen behaviour is identical.
// Safari *has* createImageBitmap but throws InvalidStateError on the sensor's
// MJPEG JPEGs (AVI1-marked), while its <img> decoder handles them fine. Once
// createImageBitmap fails we latch off and decode via <img> for the session.
let bitmapBroken = false

async function decodeFrame(blob: Blob): Promise<Frame> {
  if (!bitmapBroken && typeof createImageBitmap === 'function') {
    try {
      const bmp = await createImageBitmap(blob)
      return { source: bmp, width: bmp.width, height: bmp.height, close: () => bmp.close() }
    } catch {
      bitmapBroken = true // fall through to the <img> path
    }
  }
  const url = URL.createObjectURL(blob)
  try {
    const img = new Image()
    await new Promise<void>((resolve, reject) => {
      img.onload = () => resolve()
      img.onerror = () => reject(new Error('image decode failed'))
      img.src = url
    })
    return { source: img, width: img.naturalWidth, height: img.naturalHeight, close: () => {} }
  } finally {
    URL.revokeObjectURL(url)
  }
}

export function useMjpeg(opts: UseMjpegOptions = {}) {
  // Request the raw (application/octet-stream) variant: it carries the same
  // multipart framing but Safari refuses to stream a multipart/x-mixed-replace
  // body via fetch(), whereas it streams octet-stream fine. The boundary comes
  // from the X-Stream-Boundary header.
  const streamUrl = opts.streamUrl ?? '/stream.mjpg?raw=1'
  const snapshotUrl = opts.snapshotUrl ?? '/snapshot.jpg'
  const pollIntervalMs = opts.pollIntervalMs ?? 120
  const stallMs = opts.stallMs ?? 8000
  const connectTimeoutMs = opts.connectTimeoutMs ?? 6000

  // --- reactive surface ----------------------------------------------------
  /** Bind to the <canvas> with `ref="canvas"`. Frames are painted onto it. */
  const canvas = shallowRef<HTMLCanvasElement | null>(null)
  const mode = ref<StreamMode>('connecting')
  /** The camera refused the stream with 503 "too many viewers". */
  const busy = ref(false)
  /** Seconds until the next stream attempt while `busy` (0 when n/a). */
  const retryIn = ref(0)
  /** True once at least one frame has been painted. */
  const hasFrame = ref(false)
  /** Frames per second actually painted, client-measured. */
  const fps = ref(0)
  /** Native size of the last painted frame (0×0 before the first). */
  const frameWidth = ref(0)
  const frameHeight = ref(0)
  /** Picture is flowing (live or snapshots) and not stale. */
  const ok = computed(() => hasFrame.value && (mode.value === 'live' || mode.value === 'polling'))

  // --- internals -------------------------------------------------------------
  let gen = 0
  let streamCtrl: AbortController | null = null
  let pollCtrl: AbortController | null = null
  let polling = false
  let pollTimer: number | undefined
  let retryTimer: number | undefined
  let countdownTimer: number | undefined
  let stallTimer: number | undefined
  let fpsTimer: number | undefined
  let streamFailures = 0
  let pollFailures = 0
  let frameStamps: number[] = []
  let started = false
  let unmounted = false
  // The most recent decoded frame is kept (not closed) so it can be repainted
  // if the canvas mounts late or is re-created.
  let lastFrame: Frame | null = null
  let ctx: CanvasRenderingContext2D | null = null

  function clearTimer(id: number | undefined) {
    if (id !== undefined) clearTimeout(id)
  }

  // --- painting --------------------------------------------------------------
  function paint(frame: Frame): boolean {
    const el = canvas.value
    if (!el) return false
    if (!ctx || ctx.canvas !== el) ctx = el.getContext('2d', { alpha: false })
    if (!ctx) return false
    if (el.width !== frame.width || el.height !== frame.height) {
      el.width = frame.width
      el.height = frame.height
      frameWidth.value = frame.width
      frameHeight.value = frame.height
    }
    ctx.drawImage(frame.source, 0, 0, el.width, el.height)
    return true
  }

  function present(frame: Frame) {
    if (unmounted) {
      frame.close()
      return
    }
    lastFrame?.close()
    lastFrame = frame
    if (paint(frame)) {
      hasFrame.value = true
      frameStamps.push(performance.now())
    }
    // If the canvas isn't mounted yet the frame stays in `lastFrame` and the
    // watcher below paints it as soon as the element appears.
  }

  const queue = new LatestFrameQueue<Blob, Frame>(decodeFrame, present, (f) => f.close())

  // Hand a frame to the renderer. Decoding is async and coalesced: only the
  // newest undecoded frame is kept, so a slow decoder never builds a backlog.
  function show(blob: Blob) {
    queue.push(blob)
  }

  watch(canvas, (el) => {
    ctx = null
    if (el && lastFrame && paint(lastFrame)) hasFrame.value = true
  })

  function tickFps() {
    const now = performance.now()
    frameStamps = frameStamps.filter((t) => now - t <= 2000)
    fps.value = frameStamps.length / 2
  }

  // --- stream path -----------------------------------------------------------
  function armStall(g: number, ms: number) {
    clearTimer(stallTimer)
    stallTimer = window.setTimeout(() => {
      if (g !== gen) return
      onStreamFailure(g, false)
    }, ms)
  }

  function parseRetryAfter(res: Response): number {
    const raw = res.headers.get('Retry-After')
    const n = raw ? Number(raw) : NaN
    return Number.isFinite(n) && n > 0 ? Math.min(n, 60) : 5
  }

  async function connectStream(g: number) {
    if (g !== gen) return
    // Exactly one connection: halt snapshot polling before opening the stream.
    stopPolling()
    streamCtrl = new AbortController()
    const { signal } = streamCtrl
    let gotFrame = false
    armStall(g, connectTimeoutMs)
    try {
      const res = await fetch(streamUrl, {
        signal,
        cache: 'no-store',
        credentials: 'same-origin',
        headers: { Accept: 'multipart/x-mixed-replace, image/jpeg' },
      })
      if (g !== gen) return

      if (res.status === 503) {
        const wait = parseRetryAfter(res)
        res.body?.cancel().catch(() => {})
        onBusy(g, wait)
        return
      }
      if (!res.ok || !res.body) {
        res.body?.cancel().catch(() => {})
        throw new Error(`stream HTTP ${res.status}`)
      }
      const ctype = res.headers.get('Content-Type')
      // Accept either a real multipart/x-mixed-replace response (native) or the
      // raw octet-stream variant that declares its boundary in a header.
      const hdrBoundary = res.headers.get('X-Stream-Boundary')
      if (!isMultipartType(ctype) && !hdrBoundary) {
        res.body.cancel().catch(() => {})
        throw new Error(`unexpected stream type ${ctype}`)
      }

      const parser = new MultipartParser(hdrBoundary || parseBoundary(ctype))
      const reader = res.body.getReader()
      armStall(g, stallMs)
      for (;;) {
        const { done, value } = await reader.read()
        if (g !== gen) {
          reader.cancel().catch(() => {})
          return
        }
        if (done) break
        const parts = parser.push(value)
        if (parts.length === 0) continue
        // Only the newest frame matters; older ones in the same chunk would
        // just be decoded and discarded.
        const last = parts[parts.length - 1]!
        show(new Blob([last.data as BlobPart], { type: last.type }))
        if (!gotFrame) {
          gotFrame = true
          streamFailures = 0
          busy.value = false
          retryIn.value = 0
          mode.value = 'live'
        }
        armStall(g, stallMs)
        if (parser.done) break
      }
      // Server closed the stream (camera lost for ~15 s, or reconfigure).
      if (g === gen) onStreamFailure(g, gotFrame)
    } catch {
      if (g !== gen) return // aborted by us
      onStreamFailure(g, gotFrame)
    }
  }

  // The stream refused/failed/stalled. Fall back to snapshots immediately so
  // the viewer sees something, and retry the stream with a growing backoff.
  function onStreamFailure(g: number, hadFrames: boolean) {
    if (g !== gen) return
    gen++ // invalidate the failed attempt's continuations
    const ng = gen
    clearTimer(stallTimer)
    streamCtrl?.abort()
    streamCtrl = null
    busy.value = false
    retryIn.value = 0
    // A drop after a healthy run gets one quick retry before backing off.
    streamFailures = hadFrames ? 0 : streamFailures + 1
    const delay = hadFrames ? 1000 : BACKOFF_MS[Math.min(streamFailures - 1, BACKOFF_MS.length - 1)]!
    startPolling(ng)
    scheduleStream(ng, delay, false)
  }

  function onBusy(g: number, waitSec: number) {
    if (g !== gen) return
    gen++
    const ng = gen
    clearTimer(stallTimer)
    streamCtrl?.abort()
    streamCtrl = null
    busy.value = true
    streamFailures = 0
    startPolling(ng)
    scheduleStream(ng, waitSec * 1000, true)
  }

  function scheduleStream(g: number, delay: number, countdown: boolean) {
    clearTimer(retryTimer)
    clearInterval(countdownTimer)
    retryIn.value = countdown ? Math.ceil(delay / 1000) : 0
    if (countdown) {
      countdownTimer = window.setInterval(() => {
        if (g !== gen) { clearInterval(countdownTimer); return }
        retryIn.value = Math.max(0, retryIn.value - 1)
      }, 1000)
    }
    retryTimer = window.setTimeout(() => {
      clearInterval(countdownTimer)
      if (g !== gen) return
      void connectStream(g)
    }, delay)
  }

  // --- snapshot path ---------------------------------------------------------
  function startPolling(g: number) {
    if (polling || g !== gen) return
    polling = true
    pollFailures = 0
    pollCtrl = new AbortController()
    if (mode.value !== 'offline') mode.value = 'polling'
    void pollOnce(g)
  }

  async function pollOnce(g: number) {
    if (!polling || g !== gen) return
    const signal = pollCtrl!.signal
    try {
      const res = await fetch(`${snapshotUrl}?t=${Date.now()}`, {
        signal,
        cache: 'no-store',
        credentials: 'same-origin',
      })
      if (!res.ok) {
        res.body?.cancel().catch(() => {})
        throw new Error(`snapshot HTTP ${res.status}`)
      }
      const blob = await res.blob()
      if (!polling || g !== gen) return
      show(blob)
      pollFailures = 0
      if (mode.value !== 'polling') mode.value = 'polling'
    } catch {
      if (!polling || g !== gen) return // aborted by us
      pollFailures++
      if (pollFailures >= 3) mode.value = 'offline'
    }
    const delay = pollFailures ? Math.min(1000 * pollFailures, 5000) : pollIntervalMs
    pollTimer = window.setTimeout(() => void pollOnce(g), delay)
  }

  function stopPolling() {
    polling = false
    clearTimer(pollTimer)
    pollTimer = undefined
    pollCtrl?.abort()
    pollCtrl = null
  }

  // --- lifecycle -------------------------------------------------------------
  /** Close whatever is open. Idempotent. The last frame stays on screen. */
  function stop() {
    gen++
    clearTimer(stallTimer)
    clearTimer(retryTimer)
    clearInterval(countdownTimer)
    streamCtrl?.abort()
    streamCtrl = null
    stopPolling()
    queue.reset()
    retryIn.value = 0
  }

  /** (Re)start from scratch: closes anything open first, resets backoff. */
  function start() {
    stop()
    started = true
    streamFailures = 0
    if (!hasFrame.value) mode.value = 'connecting'
    void connectStream(gen)
  }

  /** Close the connection and show nothing new; `start()` resumes. */
  function pause() {
    stop()
    mode.value = 'paused'
  }

  function onVisibility() {
    if (document.hidden) pause()
    else if (started) start()
  }
  function onPageHide() {
    stop()
  }
  function onPageShow(e: PageTransitionEvent) {
    if (e.persisted && started && !document.hidden) start()
  }

  onMounted(() => {
    document.addEventListener('visibilitychange', onVisibility)
    window.addEventListener('pagehide', onPageHide)
    window.addEventListener('pageshow', onPageShow)
    fpsTimer = window.setInterval(tickFps, 1000)
    if (opts.autoStart !== false) start()
  })
  onUnmounted(() => {
    unmounted = true
    document.removeEventListener('visibilitychange', onVisibility)
    window.removeEventListener('pagehide', onPageHide)
    window.removeEventListener('pageshow', onPageShow)
    clearInterval(fpsTimer)
    stop()
    lastFrame?.close()
    lastFrame = null
    ctx = null
  })

  return { canvas, mode, busy, retryIn, hasFrame, fps, frameWidth, frameHeight, ok, start, stop, pause }
}
