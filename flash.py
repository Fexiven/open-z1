#!/usr/bin/env python3
"""Install open-z1 on a Makera Z1, or put a backup back.

    python flash.py install [--port P] [--backup-dir D] [--images DIR] [--yes] [--no-backup]
    python flash.py restore <backup.bin> [--port P] [--yes] [--other-board]

The images come from --images, else from this script's folder (an unpacked
release zip), else from build/z1/. The app must be signed: official releases
are; tools/build.sh builds unsigned.

Needs only esptool 5 (`pip install esptool`).

1. Read-only checks: ESP32-S3, flash size, no secure boot or flash encryption,
   the images match the board, and the current layout is either the old one
   (table 0x8000, NVS 0x9000 16 KB: stock) or the new one (table 0x10000,
   NVS 0x11000 24 KB). Anything else is refused.
2. A full flash backup, read twice and compared.
3. Writes, each verified: the old NVS + 8 KB of 0xFF at 0x11000 (old layout
   only); (a) table, otadata, app; (b) bootloader; (c) 0xFF over 0x8000-0xFFFF.
   On the new layout NVS is not touched. Nothing is erased.
4. Interrupted: run the same command again with the same --backup-dir. It
   never overwrites an NVS the new firmware may already have used.

NVS contents (serial number, Wi-Fi password) are never printed.
"""

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time
from pathlib import Path

MIN_ESPTOOL = (5, 0)
ESPRESSIF_VID = 0x303A
FLASH_SIZE = 16 << 20

OLD_TABLE, OLD_NVS, OLD_NVS_SIZE = 0x8000, 0x9000, 0x4000
NEW_TABLE, NEW_NVS, NEW_NVS_SIZE = 0x10000, 0x11000, 0x6000
OTADATA, APP = 0x17000, 0x20000
CLEAR_START, CLEAR_SIZE = 0x8000, 0x8000  # 0x8000-0xFFFF: old table, old NVS
IMAGES = {0x0: "bootloader", NEW_TABLE: "partition table", OTADATA: "otadata", APP: "app"}
# ESP image header byte 3, high nibble -> flash size.
SIZE_CODES = {0: 1, 1: 2, 2: 4, 3: 8, 4: 16, 5: 32, 6: 64, 7: 128}

ROOT = Path(__file__).resolve().parent


class Refusal(Exception):
    """An expected failure: printed as one line, no stack trace."""


def mb(n):
    return f"{n >> 20} MB"


def parse_table(data):
    """The partition entries of a binary table: (type, subtype, offset, size, name)."""
    parts = []
    for i in range(0, len(data) - 31, 32):
        magic, typ, sub, off, size = struct.unpack_from("<HBBII", data, i)
        if magic != 0x50AA:
            break
        name = data[i + 12:i + 28].split(b"\0")[0].decode("ascii", "replace")
        parts.append((typ, sub, off, size, name))
    return parts


def table_has_nvs_at(data, offset, size):
    """Exactly one nvs, at offset/size, and no nvs_keys (encrypted NVS)."""
    parts = parse_table(data)
    if any(p[0] == 1 and p[1] == 4 for p in parts):
        return False
    return [(p[2], p[3]) for p in parts if p[0] == 1 and p[1] == 2] == [(offset, size)]


def esptool_api():
    try:
        import esptool
        from esptool import cmds
        from esptool.logger import log
        from esptool.util import FatalError
    except ImportError:
        raise Refusal("esptool 5 is not installed: run `pip install esptool`")
    version = tuple(int(x) for x in re.findall(r"\d+", esptool.__version__)[:2])
    if version < MIN_ESPTOOL:
        raise Refusal(f"esptool {esptool.__version__} is too old: run `pip install -U esptool`")
    return cmds, log, FatalError


class Images:
    """bootloader, table, otadata and signed app, as flash_args names them."""

    def __init__(self, folder):
        self.folder = folder
        args = folder / "flash_args"
        if not args.is_file():
            raise Refusal(f"no flash_args in {folder}")
        lines = [ln.strip() for ln in args.read_text().splitlines()]
        opts = lines[0].split()
        self.flash_opts = {}
        for key in ("--flash-mode", "--flash-freq", "--flash-size"):
            if key in opts and opts.index(key) + 1 < len(opts):
                self.flash_opts[key[2:].replace("-", "_")] = opts[opts.index(key) + 1]
        self.data = {}
        for line in lines[1:]:
            fields = line.split()
            if not fields or fields[0].startswith("--"):
                continue
            if len(fields) != 2:
                raise Refusal(f"flash_args: cannot read line '{line}'")
            offset = int(fields[0], 0)
            if offset not in IMAGES:
                raise Refusal(f"flash_args writes {fields[1]} at unexpected offset {hex(offset)}")
            path = folder / fields[1]
            if not path.is_file():
                path = folder / Path(fields[1]).name  # a flat copy of the build folder
            if not path.is_file():
                raise Refusal(f"missing {folder / fields[1]}")
            self.data[offset] = path.read_bytes()
        missing = [IMAGES[o] for o in IMAGES if o not in self.data]
        if missing:
            raise Refusal(f"flash_args has no {', '.join(missing)}")
        self.check()

    def check(self):
        want = FLASH_SIZE
        sdkconfig = self.folder / "sdkconfig"
        if sdkconfig.is_file():
            text = [ln.strip() for ln in sdkconfig.read_text().splitlines()]
            if f'CONFIG_ESPTOOLPY_FLASHSIZE="{want >> 20}MB"' not in text:
                raise Refusal(f"{self.folder} is not a {mb(want)} build")
            if "CONFIG_PARTITION_TABLE_OFFSET=0x10000" not in text:
                raise Refusal(f"{self.folder} is not the new layout")
        for offset in (0x0, APP):
            img = self.data[offset]
            if len(img) < 24 or img[0] != 0xE9:
                raise Refusal(f"the {IMAGES[offset]} is not an ESP image")
            size = SIZE_CODES.get(img[3] >> 4, 0) << 20
            if size != want:
                raise Refusal(f"the images are built for {mb(size)} flash, the Z1 has {mb(want)}")
        if len(self.data[0x0]) > 0x8000:
            raise Refusal("bootloader over 32 KB: it would cover the old table being cleared")
        table = self.data[NEW_TABLE]
        if len(table) > 0x1000:
            raise Refusal("the partition table image is larger than 4 KB")
        parts = parse_table(table)
        if not table_has_nvs_at(table, NEW_NVS, NEW_NVS_SIZE):
            raise Refusal("the partition table does not have NVS at 0x11000 (24 KB)")
        by_off = {p[2]: p for p in parts}
        if OTADATA not in by_off or by_off[OTADATA][:2] != (1, 0) or APP not in by_off or by_off[APP][0] != 0:
            raise Refusal("the partition table does not have otadata at 0x17000 and an app at 0x20000")
        if any(p[2] + p[3] > want for p in parts):
            raise Refusal(f"the partition table is larger than {mb(want)}")
        if len(self.data[APP]) > by_off[APP][3]:
            raise Refusal("the app is larger than its partition")
        if len(self.data[OTADATA]) > min(0x2000, by_off[OTADATA][3]):
            raise Refusal("otadata is larger than its partition")
        app = self.data[APP]
        if len(app) % 4096 or app[-4096:-4092] != b"\xe7\x02\x00\x00":
            raise Refusal("the app is not signed: images must be signed (use a release, or sign "
                          "your own build); an unsigned first flash would refuse all updates")


class Board:
    """One esptool connection to the chip, kept for the whole run."""

    def __init__(self, port):
        # The Z1 must already be in download mode (BOOT held at power-on), so
        # a running machine is never reset.
        self.cmds, self.log, self.FatalError = esptool_api()
        self.port = port or self.find_port()
        self.esp = None

    @staticmethod
    def find_port():
        from serial.tools import list_ports
        ports = [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID]
        if len(ports) != 1:
            found = ", ".join(ports) if ports else "none"
            raise Refusal(f"expected exactly one Espressif USB device, found {found}; "
                          "connect only the machine (in download mode) or pass --port")
        return ports[0]

    def quiet(self, fn, *args, **kwargs):
        self.log.set_verbosity("silent")
        try:
            return fn(*args, **kwargs)
        finally:
            self.log.set_verbosity("auto")

    def connect(self):
        print(f"Connecting on {self.port} ...")
        try:
            esp = self.quiet(self.cmds.detect_chip, self.port, connect_mode="no-reset")
        except (self.FatalError, OSError) as e:
            raise Refusal(f"could not talk to the chip on {self.port}: {e}. "
                          "Hold BOOT while switching the machine on, then try again")
        self.esp = esp
        if esp.CHIP_NAME != "ESP32-S3":
            raise Refusal(f"the chip is an {esp.CHIP_NAME}, not an ESP32-S3")
        if esp.secure_download_mode:
            raise Refusal("the chip is in secure download mode: no read-back, so no backup")
        self.mac = ":".join(f"{b:02x}" for b in esp.read_mac())
        if esp.get_secure_boot_enabled():
            raise Refusal("secure boot is enabled in eFuse: this firmware would not boot")
        if esp.get_flash_encryption_enabled():
            raise Refusal("flash encryption is enabled in eFuse: this firmware would not boot")
        self.esp = self.quiet(self.cmds.run_stub, esp)
        self.quiet(self.cmds.attach_flash, self.esp)
        size = self.quiet(self.cmds.detect_flash_size, self.esp)
        m = re.fullmatch(r"(\d+)MB", size or "")
        if not m:
            raise Refusal(f"could not detect the flash size (got {size})")
        self.flash_size = int(m.group(1)) << 20
        print(f"Chip ESP32-S3, MAC {self.mac}, {mb(self.flash_size)} flash")

    def read(self, offset, size, progress=False):
        if progress:
            return self.cmds.read_flash(self.esp, offset, size, None)
        return self.quiet(self.cmds.read_flash, self.esp, offset, size, None, no_progress=True)

    def put(self, parts, opts):
        """Writes the (offset, bytes) pairs, then verifies them."""
        self.cmds.write_flash(self.esp, parts, **opts)
        self.cmds.verify_flash(self.esp, parts, **opts)

    def matches(self, parts, opts):
        try:
            self.quiet(self.cmds.verify_flash, self.esp, parts, **opts)
            return True
        except self.FatalError:
            return False

    def reset(self):
        """Back to normal run mode (as esptool's --after hard-reset)."""
        if self.esp is not None:
            try:
                self.quiet(self.cmds.reset_chip, self.esp, "hard-reset")
            except Exception:
                pass
            try:
                self.esp._port.close()
            except Exception:
                pass
            self.esp = None


def confirm(question, yes):
    if yes:
        return
    try:
        answer = input(f"{question} Type yes: ")
    except EOFError:
        answer = ""
    if answer.strip() != "yes":
        raise Refusal("aborted")


def full_backup(board, folder, name):
    """Reads the whole flash twice, compares, saves <name>.bin and .sha256."""
    print(f"Backing up the full {mb(board.flash_size)} flash (read twice) ...")
    first = board.read(0, board.flash_size, progress=True)
    second = board.read(0, board.flash_size, progress=True)
    if first != second or len(first) != board.flash_size:
        raise Refusal("the two reads of the flash differ: check the USB cable, nothing was written")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = folder / f"{name}-backup-{board.mac.replace(':', '')}-{stamp}.bin"
    digest = hashlib.sha256(first).hexdigest()
    write_private(path, first)
    write_private(path.with_suffix(".sha256"), f"{digest}  {path.name}\n".encode())
    info = {"board": name, "mac": board.mac, "size": len(first), "sha256": digest}
    write_private(path.with_suffix(".json"), (json.dumps(info, indent=1) + "\n").encode())
    sync_dir(folder)
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise Refusal(f"{path} does not read back as written; nothing was written to the board")
    print(f"Backup: {path}")
    print("  It contains the serial number and Wi-Fi password - keep it private.")
    return first


def write_private(path, data):
    fd = os.open(str(path), os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def sync_dir(folder):
    """Makes new directory entries durable (POSIX; Windows has no directory fsync)."""
    if os.name != "posix":
        return
    fd = os.open(str(folder), os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def nvs_in_use(data):
    """True if any 4 KB page of this NVS image is active or full."""
    states = [struct.unpack_from("<I", data, p)[0] for p in range(0, len(data), 4096)]
    return any(s in (0xFFFFFFFE, 0xFFFFFFFC) for s in states)


def copy_records(folder, mac):
    """SHA-256 of each NVS copy written to 0x11000, newest last."""
    record = folder / f"z1-nvs-copy-{mac.replace(':', '')}.sha256"
    hashes = record.read_text().split() if record.is_file() else []
    return record, hashes


def stop_after(step, board):
    # Test hook: stop there, as an interruption would.
    if os.environ.get("Z1_MIGRATE_STOP_AFTER") == step:
        board.reset()
        print(f"stopped after step {step} (Z1_MIGRATE_STOP_AFTER); rerun to finish")
        sys.exit(0)


def default_images():
    """An unpacked release (images next to this script), else build/z1."""
    return ROOT if (ROOT / "flash_args").is_file() else ROOT / "build" / "z1"


def install(args):
    images = Images(Path(args.images).resolve() if args.images else default_images())
    opts = images.flash_opts
    backup_dir = Path(args.backup_dir).expanduser().resolve()
    if backup_dir == ROOT or ROOT in backup_dir.parents:
        raise Refusal("the backup must be outside the firmware folder (git clean would delete it); "
                      "pass another --backup-dir")
    backup_dir.mkdir(mode=0o700, parents=True, exist_ok=True)

    board = Board(args.port)
    try:
        board.connect()
        if board.flash_size != FLASH_SIZE:
            raise Refusal(f"the flash is {mb(board.flash_size)}; the Z1 has {mb(FLASH_SIZE)}")

        # Where the board is (each check reads only).
        old_table = table_has_nvs_at(board.read(OLD_TABLE, 0x1000), OLD_NVS, OLD_NVS_SIZE)
        new_table = table_has_nvs_at(board.read(NEW_TABLE, 0x1000), NEW_NVS, NEW_NVS_SIZE)
        new_boot = board.matches([(0x0, images.data[0x0])], opts)
        record, hashes = copy_records(backup_dir, board.mac)

        copy_nvs = False
        if new_table and old_table and not new_boot:
            # Interrupted after (a). The NVS copy at 0x11000 is unused only if
            # it still equals the last copy and our bootloader never ran; then
            # 0x9000 is the newer one (the old firmware may have run).
            if not hashes:
                raise Refusal(f"the migration was started but {backup_dir} has no record of it; "
                              "pass the --backup-dir of the first run")
            now = hashlib.sha256(board.read(NEW_NVS, NEW_NVS_SIZE)).hexdigest()
            copy_nvs = now == hashes[-1]
            plan = ("resume: NVS at 0x11000 is unused since the last copy; copying it again" if copy_nvs
                    else "resume: NVS at 0x11000 is in use; finishing without touching it")
        elif new_table and old_table:
            # Our bootloader is in, but (c) has not cleared 0x9000 yet. Only
            # finish if 0x11000 provably got the NVS copy.
            if not hashes and not nvs_in_use(board.read(NEW_NVS, NEW_NVS_SIZE)):
                raise Refusal("the new firmware is on the board but its NVS at 0x11000 was never copied "
                              "(not installed by flash.py?); clearing 0x9000 would lose the old NVS. "
                              "Restore the full backup (python flash.py restore ...), then run install")
            plan = "resume: finishing the migration without touching NVS"
        elif new_table:
            plan = "update: the board has the new layout; NVS is not touched"
        elif old_table:
            for typ, sub, off, size, pname in parse_table(board.read(OLD_TABLE, 0x1000)):
                # Stock's otadata (0xf000-0x10fff) and an app are expected
                # there and replaced; any other data is not.
                if off < APP and off + size > NEW_TABLE and not (typ == 0 or (typ, sub) == (1, 0)):
                    raise Refusal(f"the old table has partition '{pname}' in 0x10000-0x1FFFF, "
                                  "where the new table and NVS go; unknown layout")
            copy_nvs = True
            plan = "migrate: NVS moves from 0x9000 to 0x11000, then the new firmware is written"
        else:
            raise Refusal("unknown flash layout: neither the old table (0x8000, NVS 0x9000) "
                          "nor the new one (0x10000, NVS 0x11000)")
        print(plan)
        if args.no_backup and (old_table or not new_table):
            raise Refusal("--no-backup is only allowed on a board that already has the new layout")

        confirm(f"Install open-z1 on the board with MAC {board.mac}?", args.yes)
        flash = None
        if not args.no_backup:
            flash = full_backup(board, backup_dir, "z1")

        if copy_nvs:
            old_nvs = flash[OLD_NVS:OLD_NVS + OLD_NVS_SIZE]
            states = [struct.unpack_from("<I", old_nvs, p * 4096)[0] for p in range(4)]
            used = sum(s in (0xFFFFFFFE, 0xFFFFFFFC) for s in states)  # active, full
            if used == 0:
                raise Refusal("the old NVS has no active or full page; nothing was written")
            print(f"Old NVS: {used} of 4 pages in use (contents not shown)")
            nvs = old_nvs + b"\xff" * (NEW_NVS_SIZE - OLD_NVS_SIZE)
            with os.fdopen(os.open(str(record), os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600), "a") as f:
                f.write(hashlib.sha256(nvs).hexdigest() + "\n")
            # First, so NVS is on the chip at every moment: on the old layout
            # 0x11000 is unused.
            print("Writing NVS copy at 0x11000 ...")
            board.put([(NEW_NVS, nvs)], opts)
            stop_after("nvs", board)

        # (a) Table, otadata and app; the old bootloader still boots its old table.
        print("Writing partition table, otadata and app ...")
        board.put([(o, images.data[o]) for o in (NEW_TABLE, OTADATA, APP)], opts)
        stop_after("a", board)
        # (b) The bootloader alone: from here the new layout boots.
        print("Writing bootloader ...")
        board.put([(0x0, images.data[0x0])], opts)
        stop_after("b", board)
        # (c) Last, 0xFF over 0x8000-0xFFFF: the old table and old NVS copy go.
        print("Clearing 0x8000-0xFFFF (old table and old NVS) ...")
        board.put([(CLEAR_START, b"\xff" * CLEAR_SIZE)], opts)
    except BaseException:
        board.reset()
        raise
    board.reset()
    print("Done. Switch the machine off and on again.")


def restore(args):
    path = Path(args.backup)
    if not path.is_file():
        raise Refusal(f"no such file: {path}")
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    info, mac = path.with_suffix(".json"), None
    if info.is_file():
        try:
            meta = json.loads(info.read_text())
            want, mac = meta["sha256"].lower(), meta["mac"].lower()
        except (ValueError, KeyError, AttributeError):
            raise Refusal(f"cannot read {info.name}")
        if digest != want:
            raise Refusal(f"{path.name} does not match {info.name}: the backup is damaged")
        print(f"Backup checksum OK ({info.name})")
    else:
        sidecar = path.with_suffix(".sha256")
        if sidecar.is_file():
            if digest != sidecar.read_text().split()[0].lower():
                raise Refusal(f"{path.name} does not match {sidecar.name}: the backup is damaged")
            print(f"Backup checksum OK ({sidecar.name})")
        else:
            print(f"note: no {sidecar.name}, so the backup's checksum cannot be checked")
        m = re.search(r"-backup-([0-9a-f]{12})-", path.name)
        if m:
            mac = ":".join(m.group(1)[i:i + 2] for i in range(0, 12, 2))

    board = Board(args.port)
    try:
        board.connect()
        if len(data) != board.flash_size:
            raise Refusal(f"the backup is {len(data)} bytes, the flash is {mb(board.flash_size)}")
        if mac != board.mac:
            what = f"the backup of {mac}" if mac else "of an unknown board (no .json, no MAC in its name)"
            if not args.other_board:
                raise Refusal(f"{path.name} is {what}, not of this board ({board.mac}); it holds that "
                              "machine's serial number and Wi-Fi password. Pass --other-board if you are sure")
            print(f"WARNING: {path.name} is {what}; writing it to {board.mac} (--other-board)")
        confirm(f"Write {path.name} over the whole flash of the board with MAC {board.mac}?", args.yes)
        print("Writing the backup ...")
        board.put([(0x0, data)], {"flash_mode": "keep", "flash_freq": "keep", "flash_size": "keep"})
    except BaseException:
        board.reset()
        raise
    board.reset()
    print("Restored. Switch the machine off and on again.")


def main():
    p = argparse.ArgumentParser(description="Install open-z1, or restore a full flash backup.")
    sub = p.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("install", help="back up the flash, then install (or update) open-z1")
    i.add_argument("--port", help="serial port (default: the one Espressif USB device)")
    i.add_argument("--backup-dir", default=str(Path.home() / "open-z1-backups"),
                   help="where the backup goes (default: ~/open-z1-backups)")
    i.add_argument("--images", help="folder with flash_args and its images (default: the release images next to "
                   "flash.py, else build/z1)")
    i.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    i.add_argument("--no-backup", action="store_true", help="skip the backup (new layout only)")
    r = sub.add_parser("restore", help="write a full backup back")
    r.add_argument("backup")
    r.add_argument("--port", help="serial port (default: the one Espressif USB device)")
    r.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    r.add_argument("--other-board", action="store_true",
                   help="allow a backup made on another board (or of unknown origin)")
    args = p.parse_args()
    try:
        install(args) if args.cmd == "install" else restore(args)
    except Refusal as e:
        sys.exit(f"error: {e}")
    except KeyboardInterrupt:
        sys.exit("\ninterrupted: run the same command again, or restore the backup")
    except Exception as e:  # esptool and serial errors
        name = type(e).__name__
        if name in ("FatalError", "SerialException", "NotImplementedInROMError", "UnsupportedCommandError") \
                or isinstance(e, OSError):
            sys.exit(f"error: {e}\nNothing more was written. Run the same command again, or restore the backup.")
        raise


if __name__ == "__main__":
    main()
