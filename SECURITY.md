# Security

## Reporting

Please report security problems privately, not in a public issue. Use the
repository host's private vulnerability reporting if it is enabled;
otherwise open an issue asking for a private contact, without details.
Include the firmware version (`GET /ota` or the console `version`) and
steps to reproduce. There is no bounty.

## Threat model

The firmware assumes an attacker on the local network or within Wi-Fi range,
and a malicious web page open in a browser on the same LAN. It does not
defend against someone with physical or USB access to the machine.

The official app's protocol (TCP 2222, UDP 3333, camera on port 82) has no
authentication, and compatibility with the unchanged app keeps it that way:
anyone on the LAN can control the machine through it, exactly as with stock.
Put the machine on a network you trust.

## Signing model

Official releases are signed (RSA-3072, Secure Boot V2 signature format) by
the maintainer with a key that is never in the repository or on GitHub; CI
builds unsigned. The running firmware installs a new image only if it is
signed with the key of the image it runs (`CONFIG_SECURE_SIGNED_ON_UPDATE`,
checked by the app; no secure boot, no eFuse is ever written), built for the
same board and flash layout, and not older unless forced; a new image is kept
only once healthy, otherwise the bootloader rolls back.

## What the hotspot and web UI protect against

- **Setup hotspot**: WPA2 with the machine's serial number (printed on its
  label) as password, instead of stock's open SoftAP. It runs only in the
  first 10 minutes after power-on and only while no Wi-Fi is configured or
  the saved one is unreachable. Its clients get only the Wi-Fi setup page;
  the app port and every other endpoint refuse them. Without a serial number
  in NVS it uses a random password shown on the USB console.
- **Web UI (port 80)**: unauthenticated, LAN only, and limited to the safe
  direction: Stop (halt), Pause (feed hold) and the signed update. No start,
  resume, jog or spindle control. Requests need a `Host` naming the board
  (against DNS rebinding); POSTs need our own `Origin` if one is sent, and
  POSTs to `/api/` an `X-Requested-With` header, so another web page cannot
  trigger them. No SSID, password, serial number or client address is
  served.
- **Update endpoint**: only signed images, never during a job, an upload or
  an LPC update. Stock's unauthenticated `/update` and `/updateffs` do not
  exist.

## Known limits

- **No secure boot and no flash encryption.** The signature check is done
  by the running app only. Anyone with USB access can read the flash
  (including the NVS with Wi-Fi password and serial number) and write any
  image in download mode.
- No anti-rollback: an older image signed with the same key can be installed
  with `--force`.
- The app protocol and camera stream are unauthenticated and unencrypted, as
  on stock.
- The web UI is plain HTTP; anyone on the LAN can press Stop or Pause.
- The hotspot password is printed on the device label.
- A lost or replaced signing key means the next update only over USB
  (`flash.py`).
