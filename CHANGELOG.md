# Changelog

Notable changes per release. The release workflow uses the section of the tagged version as
the release notes ([`docs/building.md`](docs/building.md)).

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
