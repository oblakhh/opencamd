// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 opencamd contributors
// Vite/Vitest config for the opencamd web UI: relative base and the dev-server
// mock API.

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { mockApi } from './mock/server'

// The camera serves the bundle from its web root; base './' keeps asset URLs
// relative so the bundle works regardless of mount path.
export default defineConfig({
  base: './',
  plugins: [vue(), mockApi()],
  build: {
    target: 'es2022',
    // Inline nothing large; keep a single small chunk. Assets are tiny.
    assetsInlineLimit: 4096,
    reportCompressedSize: true,
  },
  server: {
    host: true,
  },
})
