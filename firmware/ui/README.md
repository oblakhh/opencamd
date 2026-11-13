# opencamd web UI

Frontend SPA of opencamd, the custom firmware for the Edimax IC-3115W (served by the `camd` daemon). Vue 3 + Vite +
TypeScript. Builds to a small static bundle that ships in the camera's rootfs at
`/www` and is served by the on-device civetweb server.

The API contract is [`docs/firmware.md`](../../docs/firmware.md#4-http-api); the product
ethos (local-only, no cloud, a hardened and pleasant printer cam) is in
[`docs/design.md`](../../docs/design.md).

## Layout

The live MJPEG stream fills the whole viewport, letterboxed on black. It is
not a plain `<img src="/stream.mjpg">` — Safari never renders multipart MJPEG
in an `<img>` yet still holds the connection open. Instead `useMjpeg()`
(`src/composables/useMjpeg.ts`) `fetch()`es the stream, splits the
`multipart/x-mixed-replace` body into frames (`src/utils/mjpeg.ts`) and feeds
each one through `createImageBitmap()` onto a `<canvas>` (atomic repaints: no
flicker, and no `<img>` load events to depend on). It keeps exactly one connection
open (closed on tab hide / unload / reboot), falls back to polling
`/snapshot.jpg` when the stream fails, and on a `503 too many viewers` shows
snapshots with a notice until `Retry-After` elapses. A slim translucent status bar floats over the top (name,
connection dot, resolution · ~fps, clock, upload chip) with a Sign in / gear
control in the corner. Logged in, the gear opens a settings dialog with a side
rail of sections (Video · Network · Uploads · System · About); on phones the
dialog becomes a bottom sheet with a horizontal icon strip.

## Design system

Everything visual lives in `src/style.css` (tokens + primitives) and scoped
component styles; there are no UI/icon/animation libraries.

- **Palette** — near-black blue-gray surfaces (`--bg #07090c` → `--surface-3
  #252b34`), hairline borders at 8–14% white, text at three levels, and one
  warm **ember** accent (`#ff8a4c`, dark ink on it for buttons). Semantic
  green/amber/red for the connection dot and messages.
- **Type** — system UI stack, 12/13/14/15/17/20 px scale, tabular numerals for
  readouts, `ui-monospace` for IPs, MACs, URLs and headers.
- **Shape & depth** — 6/9/12/16/22 px radii, soft long shadows, glass scrims
  with `backdrop-filter`. The blur always sits on its own layer
  (`::before`), never on an ancestor of a scroll container.
- **Icons** — a hand-drawn 24 px stroke set in `src/icons.ts`, rendered by
  `components/Icon.vue` (inline SVG, no font).
- **Primitives** — `.btn` (+ `-primary` / `-ghost` / `-danger` / `-icon`),
  `.switch`, `.segmented` radio groups, styled range, `.readouts` key/value
  lists, `.group` sections, `.msg`, `.badge`, `.progress`, `.spin`.
- **Motion** — 160–260 ms eased transitions; modal rise/fade, tab cross-fade,
  card enter/leave, breathing connection dot, lifting upload arrow. All
  collapse under `prefers-reduced-motion`.
- **Shared behaviour** — `ActionBar.vue` (save row with honest *No changes* /
  *Saving…* / *Saved* / error states, optionally sticky), `PasswordInput.vue`
  (reveal toggle), `useDirty()` (form dirty tracking), `BaseModal.vue` (focus
  trap, Esc/backdrop close, animated leave, bottom sheet on phones).

## Develop

```bash
npm install
npm run dev
```

`npm run dev` runs with **no camera attached**: a Vite dev-middleware mock
(`mock/server.ts`) serves fake `/api/*`, a `/snapshot.jpg`, and a genuine
`multipart/x-mixed-replace` `/stream.mjpg` whose frames are SVG rather than
JPEG (no encoder needed on the host; the parser does not care). It also caps
streams at 2 like the device, so a third tab exercises the 503 path. The mock
admin password is **`admin`**.

The mock also models the network backend (`POST /api/network`, the 90 s
confirm window, Wi-Fi scan/test) and can pin `/api/status.net` to every state
the Network tab renders. Open the UI with `?net=<scenario>` — `wired-standby`
(default), `wired-nowifi`, `failover`, `wifi`, `wrongpw`, `none`, `pending`,
`apply-move` — and optionally `?scan=empty|fail`. Wi-Fi test results follow
the passphrase (contains `wrong` / `busy` / `nodhcp` / `err`; SSID `Nope` →
not found). `MOCK_REVERT_S=20 npm run dev` shortens the revert window. Details
at the top of the network section in `mock/server.ts`.

To develop against a real camera instead, proxy the API/stream to it — add to
`vite.config.ts`:

```ts
server: { proxy: { '/api': 'http://camera.lan', '/stream.mjpg': 'http://camera.lan', '/snapshot.jpg': 'http://camera.lan' } }
```

(and drop the `mockApi()` plugin, or it will answer first).

## Build

```bash
npm run build      # vue-tsc typecheck + vite build -> dist/
npm run preview    # serve the built bundle locally
```

Output is fully static in `dist/` with `base: './'` (relative asset URLs), so it
works from any mount path. Copy `dist/` into the rootfs web root.

Current bundle size: **~70 KB gzipped** (JS 61 KB + CSS 9 KB + HTML 0.6 KB);
target is well under 150 KB.

## Test

```bash
npm test           # vitest: config-merge, upload + network validation/wording, MJPEG parser
```

## Network tab

`components/tabs/NetworkTab.vue`, `components/RevertBanner.vue`,
`composables/useRevertWindow.ts` and the pure helpers in `utils/network.ts`
(tested in `test/network.test.ts`). The goal: a non-expert sets up Wi-Fi
backup or moves the camera to a static address without docs and without
ever stranding it.

**Decisions**
- Three independent forms with three action bars (Wi-Fi, Cable, Name), none
  sticky: they hit three endpoints with three semantics (apply immediately,
  apply then confirm, plain save). One "Save network settings" button would
  make the 90 s confirm flow incoherent.
- The status card is a two-lane list, Cable above Wi-Fi, the active lane
  highlighted; the Wi-Fi lane's default caption says it *takes over by itself
  if the cable is unplugged*. The order of the lanes is the priority.
- "Test connection" is a first-class row with its own result line, separate
  from Save feedback: it is the only way to verify credentials while the
  cable is in (Wi-Fi is cold standby). A wrong password gets its own tone and
  marks the password field invalid.
- The Cable section is a disabled `<fieldset>` with an explanation while the
  uplink is not the cable (the backend answers 409; the UI pre-empts it and
  still handles the 409 in the same words).
- The scan list shows no signal bars and no lock icons, because the scan API
  returns neither; help text covers WPA3-only networks.
- A different SSID always requires a password, even when one is stored: the
  stored one belongs to the old network.
- No "Undo now" button: doing nothing *is* undo, and the copy says so.

**RevertBanner.** Rendered twice from one component: inline in the Cable
section (replacing its action bar while a trial is pending) and floating
under the status bar at App level, always mounted. After an address change
the user lands on a fresh page at the new address with no session, so the
floating instance offers **Sign in to keep**, which signs in and confirms in
one step. It seeds its countdown from `status.net.revert_in` on every poll
and keeps ticking locally if polls stop. Phases: *A* pending and reachable
("New cable settings are active — m:ss left to keep them", with a variant
when the address may have moved); *B* the old page lost contact after a
known move ("The camera moved to <url>", with an Open button) or an unknown
one (look the hostname up in the router); *C* kept; *D* not kept (the
previous settings are back); *E* nothing left to keep (409); *F* confirm
failed (retry).

**Copy rules.** Every state tells the user what happens next and what to do;
badges are ACTIVE / STANDBY / OFF / NOT SET UP / CONNECTING / CONNECTED /
UNPLUGGED / WRONG PASSWORD; test results are complete sentences that name the
network; the 90 s trial is explained in the section blurb before the user
presses anything; edits to SSID or password clear a stale test result; all
tones have a non-colour cue (badge text, icon, words next to signal bars).

## Notes / assumptions

- **API contract** is [`docs/firmware.md`](../../docs/firmware.md#4-http-api). The Uploads tab
  edits the single `upload` section (`enabled`, `url`, `interval`, `method`,
  `field`, `headers` — one `Key: Value` per line in a single string) and reads the read-only `upload` object on `/api/status`
  (`enabled`, `ok`, `last_code`, `count`) for its live status line and the
  status-bar chip. Firmware without the uploader simply omits that status
  object; the UI then says so instead of guessing. Both `http://` and
  `https://` targets are accepted; HTTPS is built into camd (mbedTLS,
  TLS 1.2, redirects followed), which the tab mentions in a hint when the
  URL is https.
- **Network** is read from `/api/config` (`network.wired`, `network.wifi`
  with `passphrase_set`) but changed only via `POST /api/network`: Wi-Fi
  applies immediately (an omitted passphrase keeps the stored one); a wired
  change is tried for 90 s and must be confirmed (`/api/network/confirm`) or
  it reverts. See the Network tab section above.
- **Session is a cookie** set by `POST /api/login`; the client sends no token.
  Any `401` on an admin call flips the UI back to logged-out.
- **Reboot / OTA**: firmware upload, reboot, and factory reset all expect the
  connection to drop; the UI shows a "rebooting…" overlay and polls
  `/api/status` until the device returns, then reloads.
