# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [1.0.0] – 2026-11-19

First public release. Summary of what the firmware does:

### Added

- Official Makera app support, unchanged app: UDP 3333 discovery, the framed
  TCP 2222 protocol (4 equal clients, one file transfer at a time), status
  relay with stock's appended fields, job playback from the SD card (play,
  pause, abort, goto, the machine's button restart), file transfer and
  listing, `config-get/-set/-restore`, `wlan` Wi-Fi commands, `sn-get`,
  `sys-time`, the camera stream on port 82.
- Serial number and usage counters from the stock NVS, as stock.
- Signed OTA (RSA-3072, app-level signature check, no eFuses written) with
  rollback; images are checked for board, flash layout and version.
- Web UI on port 80: status, statistics and graphs, job progress, camera
  snapshot, log, diagnostics, update upload, Stop and Pause; Host and Origin
  checks against cross-site requests and DNS rebinding.
- WPA2 setup hotspot for first-time Wi-Fi, serial number as password, only
  in the first 10 minutes after power-on, setup page only.
- SD card log in stock's format and place.
- Motion-controller (LPC1768) firmware update from Makera bundles
  (`/sd/firmware.bin`, `upgrade`, `upgrade cancel`); tested with the 1.1.2
  and 1.1.4 bundles.
- Public documentation: README, CHANGELOG, SECURITY; CI build.

### Changed

- The LPC firmware update follows stock's FIRM handshake: no automatic LPC
  resets, no retries, stock's frame size, every FIRM frame logged.
- FIRM frames use the LPC bootloader's CRC variant, which it requires.

### Removed

- Personal identifiers from the documentation.

## [0.3.0]

### Added

- Motion-controller firmware update from the app's Firmware Update page.
- Web UI v1: status, diagnostics, update upload, Stop and Pause; later
  system and machine statistics, job progress bar, history graphs, and a
  redesign.
- First-time setup hotspot and camera snapshots for the web UI.
- SD card log as stock; stock's alarm when the LPC goes silent.

## [0.2.0]

### Added

- Recovery of a file replaced during a power cut (`config.txt` never goes
  missing).
- Task watchdog on the LPC link; card-detect debounce; realtime bytes (`!`,
  `~`, halt) handled before any other work.
- Stock details: 0x91 refusal frames, the long-line limit, local `time`, the
  Z1 status LED, separate camera flip and mirror.

### Changed

- Job state is cleared when the LPC resets mid-job; no button restart during
  an OTA.
- The LPC config load runs in its own task with a right-sized buffer.
- Code health: the app server split up, fixed protocol values as
  `#define`s, an eventfd wake-up, one housekeeping task.

## [0.1.0]

### Added

- Initial firmware: LPC link and keepalive, app protocol, discovery, files,
  jobs, config, Wi-Fi, camera, signed OTA with rollback.
- Z1 flash layout 3 with 4 MB app slots, and the one-time layout migration
  that keeps the stock NVS.
- Stock multi-client model; Z1 camera orientation; the LPC's config load
  served early after boot; firmware and `factory.ini` requests declined.
