// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
//
// Client-side mirror of the server's partial-merge semantics for PUT /api/config.
// Used for optimistic UI updates after a save. Objects merge recursively;
// arrays and scalars replace wholesale (matching the mock/C backend).

type Plain = Record<string, unknown>

function isPlainObject(v: unknown): v is Plain {
  return typeof v === 'object' && v !== null && !Array.isArray(v)
}

export function deepClone<T>(value: T): T {
  return structuredClone(value)
}

export function mergePatch<T extends Plain>(base: T, patch: Plain): T {
  const out = deepClone(base) as Plain
  for (const key of Object.keys(patch)) {
    const val = patch[key]
    if (isPlainObject(val) && isPlainObject(out[key])) {
      out[key] = mergePatch(out[key] as Plain, val)
    } else {
      out[key] = isPlainObject(val) || Array.isArray(val) ? deepClone(val) : val
    }
  }
  return out as T
}
