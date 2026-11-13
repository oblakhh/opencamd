# Changelog

Notable changes per release. The release workflow uses the section of the tagged version as
the release notes ([`docs/building.md`](docs/building.md)).

## 0.5.1

- Remote syslog works: the camera now sends its log to the configured host (`host[:port]`, UDP
  514 by default), and the log level setting takes effect. Both were stored but not applied.
- Settings → About: the copy buttons for the stream and snapshot URLs now work on the camera's
  plain-HTTP address (browsers only allow the clipboard API on HTTPS; there is now a fallback, and
  if copying still fails the URL is selected for copying by hand).

## 0.5.0

First public release.

- MJPEG live stream and snapshots from the camera module (up to 1280×1024, about 10 fps), with the
  legacy stock URLs kept for existing integrations.
- Web UI (Vue 3) with live view and settings for video, network, uploads and system.
- Wired networking with DHCP or a static address and a 90-second safe-apply.
- Wi-Fi client for WPA2-PSK and WPA2/WPA3 transition mode with camd's own supplicant; automatic
  wired-over-Wi-Fi failover with a shared MAC address; connection test with wrong-password detection.
- Snapshot uploads over HTTP or HTTPS (mbedTLS, TLS 1.2), e.g. to Prusa Connect.
- Over-the-air updates with the release's upgrade image, validated before anything is written.
- Admin password shared with the optional telnet shell (off by default); CSRF protection.
- Factory reset from the UI or by holding the reset button for 10 s.
