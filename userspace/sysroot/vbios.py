#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

"""AMD VBIOS images for the firmware archive: lib/firmware/vbios/1002_DDDD[_SSVID_SSID].rom

usage: vbios.py dump OUT_DIR      save the VBIOS images of this machine (ACPI VFCT, amdgpu debugfs; run as root)
       vbios.py list DIR...       show what the firmware build would take from DIR
"""
import glob
import os
import struct
import sys

AMD = 0x1002
HOST_VFCT = "/sys/firmware/acpi/tables/VFCT"
HOST_DEBUGFS = "/sys/kernel/debug/dri/*/amdgpu_vbios"
SUFFIXES = (".rom", ".bin", ".vbios")


def parse_rom(data):
    if len(data) < 0x200 or data[0] != 0x55 or data[1] != 0xAA:
        return None
    pcir = struct.unpack_from("<H", data, 0x18)[0]
    if pcir < 0x1A or pcir + 0x18 > len(data) or data[pcir:pcir + 4] != b"PCIR":
        return None
    vendor, device, list_off = struct.unpack_from("<HHH", data, pcir + 4)
    if vendor != AMD:
        return None
    length = struct.unpack_from("<H", data, pcir + 0x10)[0] * 512
    if length < 0x4000 or length > len(data):
        return None
    hdr = struct.unpack_from("<H", data, 0x48)[0]
    if hdr < 0x4A or hdr + 8 > length or data[hdr + 4:hdr + 8] not in (b"ATOM", b"MOTA"):
        return None
    ids = [device]
    if data[pcir + 0x0C] >= 3 and list_off:
        at = pcir + list_off
        while at + 2 <= length and len(ids) < 64:
            dev = struct.unpack_from("<H", data, at)[0]
            if not dev:
                break
            if dev not in ids:
                ids.append(dev)
            at += 2
    end = len(data)
    while end > length and data[end - 1] in (0x00, 0xFF):
        end -= 1
    return {"device": device, "ids": ids, "data": bytes(data[:max(end, length)])}


def from_vfct(table):
    out = []
    if len(table) < 36 + 24 or table[:4] != b"VFCT":
        return out
    off = struct.unpack_from("<I", table, 36 + 16)[0]
    while off and off + 28 <= len(table):
        _, _, _, vendor, device, ssvid, ssid, _, size = struct.unpack_from("<IIIHHHHII", table, off)
        image = table[off + 28:off + 28 + size]
        if not size or len(image) != size:
            break
        rom = parse_rom(image)
        if rom and vendor == AMD:
            rom["device"] = device
            if device not in rom["ids"]:
                rom["ids"].insert(0, device)
            rom["subsys"] = (ssvid, ssid)
            out.append(rom)
        off += 28 + size
    return out


def read_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def from_host():
    found = []
    table = read_file(HOST_VFCT)
    if table:
        found += [(HOST_VFCT, r) for r in from_vfct(table)]
    for path in sorted(glob.glob(HOST_DEBUGFS)):
        data = read_file(path)
        rom = parse_rom(data) if data else None
        if rom:
            found.append((path, rom))
    return found


def subsys_of_name(name):
    parts = os.path.splitext(name)[0].lower().split("_")
    if len(parts) == 4 and all(len(p) == 4 for p in parts):
        try:
            return int(parts[2], 16), int(parts[3], 16)
        except ValueError:
            return None
    return None


def from_dirs(dirs):
    found = []
    for d in dirs:
        if not d or not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if not os.path.isfile(path) or not name.lower().endswith(SUFFIXES):
                continue
            data = read_file(path)
            if data and data[:4] == b"VFCT":
                found += [(path, r) for r in from_vfct(data)]
                continue
            rom = parse_rom(data) if data else None
            if not rom:
                print("vbios: %s is not an AMD ATOM VBIOS image, skipped" % path)
                continue
            sub = subsys_of_name(name)
            if sub:
                rom["subsys"] = sub
            found.append((path, rom))
    return found


def collect(dirs, host=False):
    files = {}
    origin = {}
    roms = from_dirs(dirs) + (from_host() if host else [])
    for src, rom in roms:
        sub = rom.get("subsys")
        if sub and sub[0]:
            name = "vbios/%04x_%04x_%04x_%04x.rom" % (AMD, rom["device"], sub[0], sub[1])
            files.setdefault(name, rom["data"])
            origin.setdefault(name, src)
    for src, rom in roms:
        for dev in rom["ids"]:
            name = "vbios/%04x_%04x.rom" % (AMD, dev)
            if name not in files:
                files[name] = rom["data"]
                origin[name] = src
    return files, origin


def dump(out_dir):
    roms = from_host()
    if not roms:
        sys.exit("vbios: no AMD VBIOS on this machine (%s, %s; run as root)" % (HOST_VFCT, HOST_DEBUGFS))
    os.makedirs(out_dir, exist_ok=True)
    for src, rom in roms:
        sub = rom.get("subsys")
        if sub and sub[0]:
            name = "%04x_%04x_%04x_%04x.rom" % (AMD, rom["device"], sub[0], sub[1])
        else:
            name = "%04x_%04x.rom" % (AMD, rom["device"])
        path = os.path.join(out_dir, name)
        with open(path, "wb") as f:
            f.write(rom["data"])
        os.chmod(path, 0o644)
        print("vbios: %s -> %s (%d KiB)" % (src, path, len(rom["data"]) // 1024))


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "dump":
        dump(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "list":
        files, origin = collect(sys.argv[2:])
        for name in sorted(files):
            print("%-40s %7d bytes  %s" % (name, len(files[name]), origin[name]))
        if not files:
            print("vbios: nothing found in " + ", ".join(sys.argv[2:]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
