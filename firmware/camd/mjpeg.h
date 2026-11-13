/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * mjpeg — make a UVC MJPEG frame a self-contained, universally-decodable JPEG.
 * Many webcam sensors omit the Huffman table (DHT) segment, relying on the
 * decoder's built-in defaults. Lenient decoders (Chrome/Firefox, libjpeg) cope;
 * strict ones (Safari's createImageBitmap and <img>) fail to decode. We insert
 * the standard DHT before the first SOS so every frame decodes everywhere.
 */
#ifndef CAMD_MJPEG_H
#define CAMD_MJPEG_H

#include <stddef.h>

/* If `in` (len bytes) is a JPEG with no DHT, write `in` with the standard DHT
 * inserted before its SOS into `out` (cap bytes) and return the new length.
 * Returns 0 if a DHT is already present, the frame can't be parsed, or it
 * won't fit — in which case the caller should use `in` unchanged. */
size_t mjpeg_ensure_dht(const unsigned char *in, size_t len, unsigned char *out, size_t cap);

#endif
