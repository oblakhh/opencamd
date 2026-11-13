/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 opencamd contributors
 *
 * Firmware version (CAMD_VER) and build suffix (CAMD_BUILD).
 */

#ifndef CAMD_VERSION_H
#define CAMD_VERSION_H

/* Single source of truth for the firmware version. A release tag vX.Y.Z must match CAMD_VER.
 * CAMD_BUILD marks development builds (e.g. "-dev.20261111183000.gabc1234", set by CI);
 * empty for releases. */
#define CAMD_VER     "0.5.0"
#ifndef CAMD_BUILD
#define CAMD_BUILD   ""
#endif
#define CAMD_VERSION "camd " CAMD_VER CAMD_BUILD   /* banner / /api/status */
#define CAMD_UA      "Camd/" CAMD_VER   /* HTTP User-Agent for uploads */

#endif
