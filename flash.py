#!/usr/bin/env python3
"""Install open-z1 on a Makera Z1, or put a backup back.

    python flash.py install [--port PORT]
    python flash.py restore BACKUP.bin [--port PORT]

First hold BOOT while switching the Z1 on (USB connected).
Needs esptool 5: pip install esptool
"""

import argparse
import time
from pathlib import Path

from esptool import cmds
from serial.tools import list_ports

HERE = Path(__file__).resolve().parent
BACKUPS = Path.home() / "open-z1-backups"


def connect(port):
    if not port:
        port = next(p.device for p in list_ports.comports() if p.vid == 0x303A)
    esp = cmds.detect_chip(port, connect_mode="no-reset")
    esp = cmds.run_stub(esp)
    cmds.attach_flash(esp)
    return esp


def write(esp, offset, data):
    cmds.write_flash(esp, [(offset, data)])


def firmware():
    """offset -> bytes, from flash_args next to this script (or build/z1)."""
    folder = HERE if (HERE / "flash_args").is_file() else HERE / "build" / "z1"
    images = {}
    for line in (folder / "flash_args").read_text().splitlines()[1:]:
        offset, name = line.split()
        path = folder / name
        if not path.is_file():
            path = folder / Path(name).name
        images[int(offset, 0)] = path.read_bytes()
    return images


def install(port):
    images = firmware()
    esp = connect(port)

    # 1. Back up the whole flash.
    print("Backing up the full flash ...")
    flash = cmds.read_flash(esp, 0, 16 << 20)
    BACKUPS.mkdir(exist_ok=True)
    mac = "".join(f"{b:02x}" for b in esp.read_mac())
    backup = BACKUPS / f"z1-backup-{mac}-{time.strftime('%Y%m%d-%H%M%S')}.bin"
    backup.write_bytes(flash)
    print(f"Backup: {backup}  (contains serial number and Wi-Fi password: keep it private)")

    # 2. Stock has its partition table at 0x8000 and its settings (NVS) at
    #    0x9000. open-z1 keeps them at 0x10000 and 0x11000, so on stock the
    #    settings are copied over first. On open-z1 they are left alone.
    if flash[0x8000:0x8002] == b"\xaa\x50":
        print("Stock firmware found: copying the settings to 0x11000")
        write(esp, 0x11000, flash[0x9000:0xD000] + b"\xff" * 0x2000)

    # 3. Firmware: partition table, otadata, app, then the bootloader.
    for offset in (0x10000, 0x17000, 0x20000, 0x0):
        write(esp, offset, images[offset])

    # 4. Remove stock's old partition table and settings.
    write(esp, 0x8000, b"\xff" * 0x8000)

    cmds.reset_chip(esp, "hard-reset")
    print("Done. Switch the machine off and on again.")


def restore(port, backup):
    data = Path(backup).read_bytes()
    esp = connect(port)
    write(esp, 0, data)
    cmds.reset_chip(esp, "hard-reset")
    print("Restored. Switch the machine off and on again.")


def main():
    p = argparse.ArgumentParser(description="Install open-z1, or restore a full flash backup.")
    p.add_argument("command", choices=["install", "restore"])
    p.add_argument("backup", nargs="?", help="backup file (restore only)")
    p.add_argument("--port", help="serial port (default: the Espressif USB device)")
    args = p.parse_args()
    if args.command == "install":
        install(args.port)
    elif args.backup:
        restore(args.port, args.backup)
    else:
        p.error("restore needs a backup file")


if __name__ == "__main__":
    main()
