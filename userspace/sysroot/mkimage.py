#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

"""Builds a LuOS root volume with unmodified Ubuntu 24.04 userspace: glibc, Mesa 25 (llvmpipe, lavapipe,
radeonsi, iris, ...), glmark2, vulkan-tools. FAT32 has no symlinks, so links are resolved into copies.

usage: mkimage.py OUT.img [SIZE_MB] [EXTRA_DIR]
  downloads the packages with apt-get (Ubuntu 24.04 host, deb-src not needed), extracts them, writes OUT.img;
  EXTRA_DIR (optional) is copied on top (e.g. LuOS test programs).
"""
import os, shutil, subprocess, sys, tempfile

PACKAGES = ["glmark2-es2-drm", "vulkan-tools", "mesa-utils-bin", "libgl1-mesa-dri",
            "mesa-vulkan-drivers", "libegl-mesa0", "libgbm1", "libgles2", "libegl1", "coreutils"]
DROP = ["usr/share/doc", "usr/share/man", "usr/share/info", "usr/share/locale", "usr/share/lintian",
        "usr/share/bug", "usr/share/iso-codes", "usr/share/gstreamer-1.0"]

def run(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)

def download(work):
    arch = os.path.join(work, "archives")
    os.makedirs(os.path.join(arch, "partial"), exist_ok=True)
    status = os.path.join(work, "status")
    open(status, "w").close()
    run("apt-get", "-q", "-o", "Dir::State::status=" + status, "-o", "Dir::Cache::archives=" + arch,
        "-o", "APT::Install-Recommends=false", "--download-only", "-y", "install", *PACKAGES)
    return sorted(os.path.join(arch, f) for f in os.listdir(arch) if f.endswith(".deb"))

def needed_names(root):
    names = set()
    for dirpath, _, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            with open(p, "rb") as fh:
                if fh.read(4) != b"\x7fELF":
                    continue
            out = subprocess.run(["readelf", "-d", p], capture_output=True, text=True).stdout
            for line in out.splitlines():
                if "(NEEDED)" in line:
                    names.add(line.split("[", 1)[1].split("]", 1)[0])
    return names

def flatten(src, dst):
    needed = needed_names(src)
    for dirpath, dirs, files in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files + [d for d in dirs if os.path.islink(os.path.join(dirpath, d))]:
            s = os.path.join(dirpath, f)
            d = os.path.join(dst, rel, f)
            if os.path.islink(s):
                real = os.path.realpath(s)
                if not os.path.exists(real) or os.path.isdir(real):
                    continue
                in_libdir = rel == "usr/lib/x86_64-linux-gnu"
                if in_libdir and f.endswith(".so") and f not in needed:
                    continue
                shutil.copy2(real, d)
            else:
                shutil.copy2(s, d)

def main():
    out = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
    extra = sys.argv[3] if len(sys.argv) > 3 else None
    work = tempfile.mkdtemp(prefix="luos-sysroot-")
    raw = os.path.join(work, "raw")
    tree = os.path.join(work, "tree")
    for deb in download(work):
        run("dpkg-deb", "-x", deb, raw)
    for d in DROP:
        shutil.rmtree(os.path.join(raw, d), ignore_errors=True)
    flatten(raw, tree)
    os.makedirs(os.path.join(tree, "lib64"), exist_ok=True)
    shutil.copy2(os.path.join(raw, "usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"),
                 os.path.join(tree, "lib64/ld-linux-x86-64.so.2"))
    shutil.rmtree(os.path.join(tree, "usr/lib64"), ignore_errors=True)
    os.makedirs(os.path.join(tree, "etc"), exist_ok=True)
    with open(os.path.join(tree, "etc/ld.so.conf"), "w") as f:
        f.write("/usr/lib/x86_64-linux-gnu\n")
    run("ldconfig", "-r", tree, "-X")
    for d in ("tmp", "dev", "sys", "proc", "root"):
        os.makedirs(os.path.join(tree, d), exist_ok=True)
    if extra:
        run("cp", "-rL", extra + "/.", tree)
    if os.path.exists(out):
        os.remove(out)
    run("truncate", "-s", "%dM" % size, out)
    run("mkfs.vfat", "-F", "32", "-n", "LUOSROOT", out, stdout=subprocess.DEVNULL)
    for entry in sorted(os.listdir(tree)):
        run("mcopy", "-s", "-i", out, os.path.join(tree, entry), "::/")
    shutil.rmtree(work)
    print("wrote", out)

if __name__ == "__main__":
    main()
