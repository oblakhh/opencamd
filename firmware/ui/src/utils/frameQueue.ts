// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
//
// Serialises an async decode step and keeps only the newest pending input.
//
// Frames can arrive faster than they decode (createImageBitmap is async). We
// never want a backlog: while one frame is decoding, later arrivals replace
// each other and only the most recent is decoded next. `reset()` starts a new
// epoch so decodes already in flight are discarded rather than presented.

export class LatestFrameQueue<In, Out> {
  private busy = false
  private queued: In | null = null
  private epoch = 0

  constructor(
    private readonly decode: (item: In) => Promise<Out>,
    private readonly present: (out: Out) => void,
    private readonly discard: (out: Out) => void = () => {},
  ) {}

  /** Offer a new input. Returns once the queue has accepted it (not drawn it). */
  push(item: In): void {
    if (this.busy) {
      this.queued = item
      return
    }
    void this.run(item)
  }

  /** Drop anything queued and ignore results of decodes already in flight. */
  reset(): void {
    this.epoch++
    this.queued = null
  }

  get pending(): boolean {
    return this.busy || this.queued !== null
  }

  private async run(first: In): Promise<void> {
    this.busy = true
    let item: In | null = first
    try {
      while (item !== null) {
        const epoch = this.epoch
        let out: Out
        try {
          out = await this.decode(item)
        } catch {
          item = this.take()
          continue // undecodable frame: skip it
        }
        if (epoch !== this.epoch) this.discard(out)
        else this.present(out)
        item = this.take()
      }
    } finally {
      this.busy = false
    }
  }

  private take(): In | null {
    const next = this.queued
    this.queued = null
    return next
  }
}
