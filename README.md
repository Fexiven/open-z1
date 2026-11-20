# open-z1

Open replacement firmware for the ESP32-S3 "Mainboard" of the Makera Z1.

It replaces only the Mainboard (Wi-Fi, app connection, SD card, camera,
updates). The motion controller keeps Makera's firmware, and the official
Makera app works unchanged.

## Features

- Works with the official Makera app: connect, jobs, files, config, camera
- Signed updates over Wi-Fi, with automatic rollback
- Web UI: machine state, job progress, statistics, camera, live log, Stop and Pause
- Wi-Fi setup hotspot, protected with the machine's serial number
- Motion-controller firmware updates from Makera's update packages

## Safety

- The firmware never moves the machine on its own.
- The web UI's Stop is not an emergency stop.
- **Keep the full flash backup that `flash.py install` makes.** It is the only
  way back to stock, and it contains your serial number and Wi-Fi password:
  keep it private.

## Install

Download `open-z1-<version>-z1.zip` from Releases and unzip it. Then, in that
folder:

1. `pip install esptool` (Python 3.10+)
2. Hold the BOOT button while switching the machine on, with the Z1 connected
   over USB.
3. `python flash.py install`. It backs up the full flash to
   `~/open-z1-backups`, then installs. Interrupted: run it again.
4. Switch the machine off and on again.

## Updates

Download `open-z1-<version>-z1-ota.bin` from Releases and upload it in the web
UI. Check it against `SHA256SUMS` from the same release. Official releases are
signed; a board accepts an update only if it is signed with the key of the
image it runs.

## Back to stock

Hold BOOT while switching on, then:

```
python flash.py restore ~/open-z1-backups/z1-backup-<mac>-<time>.bin
```

## Building and contributing

`tools/build.sh` builds the firmware into `build/z1/` (ESP-IDF v6.1 in
podman, or directly where `idf.py` is on PATH; extra args go to `idf.py`).
Settings belong in `sdkconfig.defaults` or a Kconfig default: `menuconfig`
edits are lost.

CI builds unsigned. To test on your own Z1, sign the build with your own key
(`espsecure generate-signing-key --version 2 --scheme rsa3072 <key>` once;
espsecure comes with esptool), in place:
`espsecure sign-data --version 2 --keyfile <key> build/z1/z1fw-z1-l3.bin`,
and install it once with `python flash.py install`. From then on that board
accepts only images signed with your key. Releases are signed by the
maintainer.

Pull requests: one logical change per commit, saying why and how it was
tested; changes to motion, playback or the LPC link need a test on a real Z1.
Describe the stock firmware's behaviour only as observed; never copy,
translate or paraphrase its code or submit disassembly. Never commit keys,
flash dumps, captures, logs, MAC or IP addresses, SSIDs, passwords or serial
numbers.

## Licence

TBD — licence not decided yet. Until a licence is published, no rights to
use, copy or distribute this code are granted beyond those required by
law.

## Disclaimer

Not affiliated with Makera. Use at your own risk; installing it replaces the
manufacturer's firmware. "Makera" and "Z1" are trademarks of their owners.
