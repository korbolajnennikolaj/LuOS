#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

"""Builds the complete GPU firmware directory that goes into the LuOS ISO: everything Ubuntu's
unified linux-firmware package ships for the three GPU drivers the kernel builds — the full
amdgpu/ (GC/PSP/SDMA/DMCUB/VCN blobs for every AMD ASIC), i915/ (DMC/GuC/HuC for Intel) and
nvidia/ (GSP-RM, ACR, gr, sec2, nvdec falcon blobs for every NVIDIA chip) trees, with the
.zst-compressed blobs unpacked and all symlinks resolved to plain files (the kernel's firmware
loader and the ISO9660 driver do not follow links). This is what request_firmware() reads from
/lib/firmware on the mounted ISO at boot; nothing is linked into the kernel image or loaded
through an initrd.

radeon/ and xe/ are left out: those drivers are not part of the build.

usage: mkfirmware.py OUT_DIR [--cache DIR]
Works on any Linux host with python3 and zstd, like mkinitfs.py (the package comes from
archive.ubuntu.com; questing is preferred because it still ships the unified package that
contains every chip's GSP blob — the split noble graphics packages lack some).
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkinitfs import fetch

MIRROR = "https://mirrors.aliyun.com/ubuntu"
SUITES = ["questing", "plucky"]     # suites shipping the unified linux-firmware package
GPU_DIRS = ("amdgpu", "i915", "nvidia")
MAX_DEPTH = 12                       # symlink-loop safety net
SKIP_NAMES = {"WHENCE"}              # notes/licences are not firmware


def package_index(suite, cache):
    """Packages index of suite/main, cached like mkinitfs.py does"""
    path = os.path.join(cache, "Packages-%s-main" % suite)
    if not os.path.exists(path):
        import lzma
        data = lzma.decompress(fetch("%s/dists/%s/main/binary-amd64/Packages.xz" % (MIRROR, suite)))
        with open(path, "wb") as f:
            f.write(data)
    fields = {}
    for para in open(path, encoding="utf-8", errors="replace").read().split("\n\n"):
        name = None
        for line in para.splitlines():
            if line.startswith("Package:"):
                name = line.split(":", 1)[1].strip()
            elif name and line.startswith(("Version:", "Filename:", "Size:")):
                key = line.split(":", 1)[0]
                fields.setdefault(name, {})[key] = line.split(":", 1)[1].strip()
    return fields


def find_package(cache):
    """the unified linux-firmware package: first suite that ships a real one (the noble
    'linux-firmware' is a 2 KB metapackage of split packages that miss some GSP blobs)"""
    for suite in SUITES:
        index = package_index(suite, cache)
        entry = index.get("linux-firmware")
        if entry and entry.get("Size") and int(entry["Size"]) > 100 * 1048576:
            return entry
    sys.exit("no unified linux-firmware package in " + ", ".join(SUITES))


def extract_deb(path, dest):
    import io
    import tarfile
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"!<arch>\n":
        raise ValueError("not a .deb")
    off = 8
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[:16].decode().strip().rstrip("/")
        size = int(hdr[48:58].decode().strip())
        if name.startswith("data.tar"):
            body = data[off + 60:off + 60 + size]
            if name.endswith(".zst"):
                body = subprocess.run(["zstd", "-dc", "-q"], input=body, capture_output=True, check=True).stdout
                mode = "r:"
            elif name.endswith(".xz"):
                mode = "r:xz"
            elif name.endswith(".gz"):
                mode = "r:gz"
            else:
                mode = "r:"
            with tarfile.open(fileobj=io.BytesIO(body), mode=mode) as t:
                t.extractall(dest, filter="data")
            return
        off += 60 + size + (size & 1)
    raise ValueError("no data member in " + path)


def scan(root, prefix, depth, out):
    """collect (display path, real path) of every file below root, following symlinks"""
    if depth > MAX_DEPTH:
        sys.exit("symlink loop below %s" % root)
    with os.scandir(root) as it:
        for entry in sorted(it, key=lambda e: e.name):
            rel = prefix + entry.name
            if entry.is_symlink():
                real = os.path.realpath(entry.path)
                if os.path.isdir(real):
                    scan(real, rel + "/", depth + 1, out)
                elif os.path.isfile(real):
                    out.append((rel, real))
            elif entry.is_dir(follow_symlinks=False):
                scan(entry.path, rel + "/", depth + 1, out)
            elif entry.is_file(follow_symlinks=False):
                out.append((rel, entry.path))


def unpack_zst(src, dst):
    with open(src, "rb") as f:
        data = subprocess.run(["zstd", "-dc", "-q"], input=f.read(), capture_output=True, check=True).stdout
    with open(dst, "wb") as f:
        f.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--cache", default=None)
    args = ap.parse_args()

    out_root = os.path.join(args.out, "lib", "firmware")
    cache = args.cache or os.path.join(os.path.dirname(os.path.abspath(args.out)), "debs")
    os.makedirs(cache, exist_ok=True)
    os.makedirs(out_root, exist_ok=True)

    entry = find_package(cache)
    deb = os.path.join(cache, os.path.basename(entry["Filename"]))
    if not os.path.exists(deb):
        print("fetch %s (%s)" % (os.path.basename(deb), entry["Version"]))
        with open(deb + ".part", "wb") as f:
            f.write(fetch(MIRROR + "/" + entry["Filename"]))
        os.rename(deb + ".part", deb)

    raw = os.path.join(os.path.dirname(os.path.abspath(args.out)), "fw-unpack")
    if os.path.exists(raw):
        shutil.rmtree(raw)
    os.makedirs(raw)
    print("extracting %s" % os.path.basename(deb))
    extract_deb(deb, raw)
    src_root = None
    for cand in ("usr/lib/firmware", "lib/firmware"):
        if os.path.isdir(os.path.join(raw, cand)):
            src_root = os.path.join(raw, cand)
            break
    if not src_root:
        sys.exit("the package ships no lib/firmware")

    count = 0
    total = 0
    for d in GPU_DIRS:
        top = os.path.join(src_root, d)
        if not os.path.isdir(top):
            sys.exit("no %s/ below %s" % (d, src_root))
        files = []
        scan(top, "", 0, files)
        for rel, real in files:
            if rel in SKIP_NAMES or rel.endswith(".txt") or rel.endswith(".txt.zst"):
                continue
            dst = os.path.join(out_root, d, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if rel.endswith(".zst"):
                dst = dst[:-len(".zst")]
                if os.path.exists(dst):
                    os.remove(dst)
                unpack_zst(real, dst)
            else:
                shutil.copyfile(real, dst)
            os.chmod(dst, 0o644)
            count += 1
            total += os.path.getsize(dst)
        print("firmware: %s/ -> %s/%s (%d files)" % (d, out_root, d, len([r for r, _ in files if not r.endswith('.txt')])))

    with open(os.path.join(cache, "firmware-package.txt"), "w") as f:
        f.write("linux-firmware %s (%s)\n" % (entry["Version"], os.path.basename(entry["Filename"])))
    shutil.rmtree(raw, ignore_errors=True)
    print("firmware: %d files, %.1f MiB for amdgpu, i915, nouveau -> %s" % (count, total / 1048576, out_root))


if __name__ == "__main__":
    main()
