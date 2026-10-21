#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

"""Builds the LuOS built-in filesystem: unmodified Ubuntu 24.04 binaries of the Mesa stack (GL/GLES/EGL/GBM,
Vulkan loader and drivers, LLVM) and the glibc they run on, packed into one image that is linked into the kernel
(section .initfs) and served from RAM at boot. Nothing is read from a disk.

usage: mkinitfs.py OUT.img [--profile mesa|tests] [--firmware amd,intel,nvidia|none] [--extra DIR]...
                   [--extra-bin DIR]... [--cache DIR] [--level N]
  mesa   the libraries only (default)
  tests  also glmark2-es2-drm, vulkaninfo, eglinfo, peglgears and the LuOS test programs
  --firmware  GPU firmware groups from firmware.list, stored in the image as ONE compressed
              archive /usr/lib/firmware.zip (0.10.27): the kernel indexes its central directory
              once and reads every blob entry by entry, on the fly — never unpacked
Works on any Linux host with python3 and zstd: packages come straight from archive.ubuntu.com (no apt/dpkg).

Image format (little endian), produced here and read by src/kernel/proc/initfs.c:
  header  "LUOSIFS1", u32 entries, u32 0, u64 names offset, u64 names length, u64 image size
  entries (DFS order, root first), 40 bytes each:
          u32 name offset, u16 name length, u16 type, u32 mode, u32 index after the subtree,
          u64 size, u64 data offset, u64 data length
          type 1 directory, 2 file, 3 zstd-compressed file (one frame), 4 symbolic link (data = target)
  names, then file data (16-byte aligned)
"""
import argparse, io, lzma, os, shutil, stat, struct, subprocess, sys, tarfile, tempfile, time, urllib.request, zipfile

MIRROR = "http://archive.ubuntu.com/ubuntu"
SUITES = ["noble-updates", "noble-security", "noble"]
COMPONENTS = ["main", "universe"]
ARCH = "amd64"

MESA = ["libegl1", "libegl-mesa0", "libgles2", "libopengl0", "libgbm1", "libgl1-mesa-dri", "mesa-libgallium",
        "mesa-vulkan-drivers", "libvulkan1", "libdrm2", "libdrm-amdgpu1", "libdrm-intel1", "libdrm-nouveau2",
        "libdrm-common", "libglvnd0", "libglx0", "libgl1", "libglx-mesa0", "libllvm18"]
HOST_BUILD = ["libc6-dev", "linux-libc-dev"]
TESTS = ["glmark2-es2-drm", "vulkan-tools", "mesa-utils-bin"]

# packages a program never loads (maintainer scripts, tools, data for other programs)
SKIP = {"debconf", "dpkg", "perl-base", "libc-bin", "init-system-helpers", "sensible-utils", "adduser", "passwd",
        "base-files", "tar", "coreutils", "login", "lsb-base", "debianutils", "readline-common", "ucf", "dbus",
        "dbus-bin", "dbus-daemon", "dbus-system-bus-common", "dbus-session-bus-common", "netbase", "systemd",
        "libpam-modules", "libpam-modules-bin", "libpam-runtime"}
KEEP = ["usr/lib/x86_64-linux-gnu", "usr/lib64", "usr/share/vulkan", "usr/share/glvnd", "usr/share/drirc.d",
        "usr/share/libdrm", "usr/share/glmark2", "usr/bin", "etc/vulkan"]
DROP = ["usr/lib/x86_64-linux-gnu/libvulkan_asahi.so", "usr/share/vulkan/icd.d/asahi_icd.json",
        "usr/lib/x86_64-linux-gnu/libvulkan_gfxstream.so", "usr/share/vulkan/icd.d/gfxstream_vk_icd.json",
        "usr/lib/x86_64-linux-gnu/gconv", "usr/lib/x86_64-linux-gnu/audit", "usr/lib/x86_64-linux-gnu/perl",
        "usr/lib/x86_64-linux-gnu/glib-2.0", "usr/lib/x86_64-linux-gnu/gstreamer1.0",
        "usr/lib/x86_64-linux-gnu/gio", "usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0"]
FIRMWARE_PACKAGES = {"amd": "linux-firmware-amd-graphics", "intel": "linux-firmware-intel-graphics",
                     "nvidia": "linux-firmware-nvidia-graphics"}
TEST_PROGRAMS = ["glmark2-es2-drm", "vulkaninfo", "vkcube", "eglinfo", "es2_info", "peglgears"]
ARCH_SUFFIX = ".x86_64-linux-gnu"


def fetch(url):
    import subprocess
    for attempt in range(5):
        try:
            r = subprocess.run(["curl", "-fsSL", "--connect-timeout", "20", "--max-time", "600", url],
                               capture_output=True, check=True)
            return r.stdout
        except (OSError, subprocess.SubprocessError) as e:
            if attempt == 4:
                raise
            print("retry %s (%s)" % (url, e))
            time.sleep(2 + attempt * 3)


def parse_packages(text, into):
    for para in text.split("\n\n"):
        fields, last = {}, None
        for line in para.splitlines():
            if line.startswith((" ", "\t")) and last:
                fields[last] += " " + line.strip()
            elif ":" in line:
                last, val = line.split(":", 1)
                fields[last] = val.strip()
        name = fields.get("Package")
        if name and name not in into:
            into[name] = fields


def load_index(cache):
    index = {}
    for suite in SUITES:
        for comp in COMPONENTS:
            path = os.path.join(cache, "Packages-%s-%s" % (suite, comp))
            if not os.path.exists(path):
                data = lzma.decompress(fetch("%s/dists/%s/%s/binary-%s/Packages.xz" % (MIRROR, suite, comp, ARCH)))
                with open(path, "wb") as f:
                    f.write(data)
            with open(path, encoding="utf-8", errors="replace") as f:
                parse_packages(f.read(), index)
    provides = {}
    for name, fields in index.items():
        for p in fields.get("Provides", "").split(","):
            p = p.split("(")[0].strip()
            if p:
                provides.setdefault(p, name)
    return index, provides


def closure(roots, index, provides):
    seen, todo = set(), list(roots)
    while todo:
        name = todo.pop()
        if name in seen or name in SKIP:
            continue
        if name not in index:
            if name in provides:
                todo.append(provides[name])
                continue
            sys.exit("package not found: " + name)
        seen.add(name)
        fields = index[name]
        for key in ("Pre-Depends", "Depends"):
            for group in fields.get(key, "").split(","):
                if not group.strip():
                    continue
                for alt in group.split("|"):
                    dep = alt.split("(")[0].split("[")[0].strip().split(":")[0]
                    if dep in SKIP:
                        break
                    if dep in index or dep in provides:
                        todo.append(dep if dep in index else provides[dep])
                        break
    return sorted(seen)


def ar_members(data):
    if data[:8] != b"!<arch>\n":
        raise ValueError("not a .deb")
    off = 8
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[:16].decode().strip().rstrip("/")
        size = int(hdr[48:58].decode().strip())
        yield name, data[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)


def extract_deb(path, dest):
    with open(path, "rb") as f:
        data = f.read()
    for name, body in ar_members(data):
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".zst"):
            body = subprocess.run(["zstd", "-dc"], input=body, capture_output=True, check=True).stdout
            mode = "r:"
        elif name.endswith(".xz"):
            mode = "r:xz"
        elif name.endswith(".gz"):
            mode = "r:gz"
        else:
            mode = "r:"
        with tarfile.open(fileobj=io.BytesIO(body), mode=mode) as t:
            for m in t.getmembers():
                rel = os.path.normpath(m.name).lstrip("./")
                if not rel or rel.startswith(".."):
                    continue
                target = os.path.join(dest, rel)
                if m.isdir():
                    os.makedirs(target, exist_ok=True)
                elif m.issym():
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    if os.path.lexists(target):
                        os.remove(target)
                    os.symlink(m.linkname, target)
                elif m.islnk():
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    src = os.path.join(dest, os.path.normpath(m.linkname).lstrip("./"))
                    if os.path.lexists(target):
                        os.remove(target)
                    shutil.copy2(src, target)
                elif m.isfile():
                    os.makedirs(os.path.dirname(target), exist_ok=True)
                    if os.path.lexists(target):
                        os.remove(target)
                    with t.extractfile(m) as src, open(target, "wb") as out:
                        shutil.copyfileobj(src, out)
                    os.chmod(target, m.mode & 0o7777)
        return
    raise ValueError("no data member in " + path)


def elf_needed(path):
    with open(path, "rb") as f:
        if f.read(4) != b"\x7fELF":
            return None
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    return [l.split("[", 1)[1].split("]", 1)[0] for l in out.splitlines() if "(NEEDED)" in l]


def check_needed(tree):
    libdir = os.path.join(tree, "usr/lib/x86_64-linux-gnu")
    have = set(os.listdir(libdir)) if os.path.isdir(libdir) else set()
    missing = {}
    for dirpath, _, files in os.walk(tree):
        for f in files:
            p = os.path.join(dirpath, f)
            if os.path.islink(p):
                continue
            for lib in elf_needed(p) or []:
                if lib not in have and lib != "ld-linux-x86-64.so.2" and not lib.startswith("ld-musl"):
                    missing.setdefault(lib, []).append(os.path.relpath(p, tree))
    for lib, users in sorted(missing.items()):
        print("initfs: warning: %s (needed by %s) is not in the image" % (lib, ", ".join(users[:3])))


def prune(raw, tree, profile):
    for keep in KEEP:
        src = os.path.join(raw, keep)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(tree, keep), symlinks=True, dirs_exist_ok=True)
    for d in DROP:
        p = os.path.join(tree, d)
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p, ignore_errors=True)
        elif os.path.lexists(p):
            os.remove(p)
    bindir = os.path.join(tree, "usr/bin")
    if os.path.isdir(bindir):
        for f in os.listdir(bindir):
            path = os.path.join(bindir, f)
            name = f[:-len(ARCH_SUFFIX)] if f.endswith(ARCH_SUFFIX) else f
            if profile != "tests" or name not in TEST_PROGRAMS:
                os.remove(path)
            elif name != f:
                os.replace(path, os.path.join(bindir, name))
    for top, target in (("lib", "usr/lib"), ("lib64", "usr/lib64"), ("bin", "usr/bin"), ("sbin", "usr/sbin")):
        os.symlink(target, os.path.join(tree, top))
    os.makedirs(os.path.join(tree, "etc"), exist_ok=True)
    with open(os.path.join(tree, "etc/ld.so.conf"), "w") as f:
        f.write("/usr/lib/x86_64-linux-gnu\n")
    check_needed(tree)
    if shutil.which("ldconfig"):
        subprocess.run(["ldconfig", "-r", tree, "-X"], check=False)


def fetch_deb(name, index, cache):
    fname = index[name]["Filename"]
    local = os.path.join(cache, os.path.basename(fname))
    if not os.path.exists(local):
        print("fetch", os.path.basename(fname))
        data = fetch(MIRROR + "/" + fname)
        with open(local + ".part", "wb") as f:
            f.write(data)
        os.rename(local + ".part", local)
    return local


def firmware_list(groups):
    wanted = []
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware.list")) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) == 2 and line[0] in groups:
                wanted.append((line[0], line[1]))
    return wanted


# files that are stored RAW in the image even though they are large: the firmware archive
# carries its own deflate compression, zstd on top of it would gain ~1% and would force the
# kernel to materialize the whole archive in RAM just to serve one entry
RAW_STORE = set()


def add_firmware(groups, index, cache, work, tree, vbios_dirs=()):
    unknown = [g for g in groups if g not in FIRMWARE_PACKAGES]
    if unknown:
        sys.exit("unknown firmware group: " + ", ".join(unknown))
    used = []
    files = []
    total = 0
    for group in groups:
        pkg = FIRMWARE_PACKAGES[group]
        raw = os.path.join(work, "fw-" + group)
        os.makedirs(raw)
        extract_deb(fetch_deb(pkg, index, cache), raw)
        used.append(pkg)
        src_root = os.path.join(raw, "lib/firmware")
        for _, name in (w for w in firmware_list([group])):
            src = os.path.join(src_root, name)
            if os.path.exists(src):
                with open(src, "rb") as f:
                    data = f.read()
            elif os.path.exists(src + ".zst"):
                with open(src + ".zst", "rb") as f:
                    data = subprocess.run(["zstd", "-dc"], input=f.read(), capture_output=True, check=True).stdout
            else:
                print("initfs: firmware %s is not in %s (optional, skipped)" % (name, pkg))
                continue
            files.append((name, data))
            total += len(data)
        if group == "amd":
            import vbios
            roms, _ = vbios.collect(list(vbios_dirs))
            for name in sorted(roms):
                files.append((name, roms[name]))
                total += len(roms[name])
            if roms:
                print("initfs: %d AMD VBIOS image(s) from %s" % (len(roms), ", ".join(vbios_dirs)))
        doc = os.path.join(raw, "usr/share/doc", pkg, "copyright")
        if os.path.exists(doc):
            os.makedirs(os.path.join(tree, "usr/share/doc", pkg), exist_ok=True)
            shutil.copyfile(doc, os.path.join(tree, "usr/share/doc", pkg, "copyright"))

    # the whole firmware directory as ONE compressed archive in the image (0.10.27): the
    # kernel indexes its central directory once and reads every blob entry by entry, on
    # the fly — the archive is never unpacked (src/components/fwzip.c). Rooted at
    # lib/firmware, the layout of LuOS_gpu_firmware.zip, which the kernel indexes
    # firmware-relative, and stored raw in the image (the zip's deflate is the compression).
    arc = os.path.join(tree, "usr/lib/firmware.zip")
    os.makedirs(os.path.dirname(arc), exist_ok=True)
    with zipfile.ZipFile(arc, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(files):
            z.writestr("lib/firmware/" + name, data)
    os.chmod(arc, 0o644)
    RAW_STORE.add("usr/lib/firmware.zip")
    packed = os.path.getsize(arc)
    print("initfs: firmware %s, %d files, %.1f MiB -> compressed directory /usr/lib/firmware.zip, %.1f MiB" %
          (",".join(groups), len(files), total / 1048576, packed / 1048576))
    return used


def mesa_host_cmd(cc, sysroot, src, out):
    """One cc invocation for the Mesa executor host, portable across host compilers: a gcc that is
    not Debian multiarch-aware (or clang) does not search usr/include/x86_64-linux-gnu under
    --sysroot, so the multiarch include and lib dirs are named explicitly."""
    inc_multi = os.path.join(sysroot, "usr/include/x86_64-linux-gnu")
    lib_multi = os.path.join(sysroot, "usr/lib/x86_64-linux-gnu")
    return [cc, "-Os", "-s", "-fno-ident", "-fomit-frame-pointer",
            "--sysroot=" + sysroot,
            "-isystem", inc_multi,
            "-L", lib_multi,
            "-o", out, src,
            "-Wl,--dynamic-linker=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
            "-Wl,--hash-style=gnu", "-ldl"]


def build_mesa_host(cache, index, work, tree):
    """Compiles the Mesa executor host against the image's own glibc and drops it into /usr/bin."""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mesa", "mesa_host.c")
    if not os.path.exists(src):
        sys.exit("mesa host source missing: " + src)
    sysroot = os.path.join(work, "hostroot")
    os.makedirs(sysroot, exist_ok=True)
    for name in HOST_BUILD + ["libc6"]:
        extract_deb(fetch_deb(name, index, cache), sysroot)
    inc_root = os.path.join(sysroot, "usr/include")
    lib_root = os.path.join(sysroot, "usr/lib")
    inc_multi = os.path.join(sysroot, "usr/include/x86_64-linux-gnu")
    lib_multi = os.path.join(sysroot, "usr/lib/x86_64-linux-gnu")
    for name in ("bits", "gnu"):
        if os.path.isdir(os.path.join(inc_multi, name)) and not os.path.lexists(os.path.join(inc_root, name)):
            os.symlink("x86_64-linux-gnu/" + name, os.path.join(inc_root, name))
    for name in ("crt1.o", "Scrt1.o", "crti.o", "crtn.o", "libc.so", "libdl.so", "libm.so", "libpthread.so"):
        if os.path.exists(os.path.join(lib_multi, name)) and not os.path.lexists(os.path.join(lib_root, name)):
            os.symlink("x86_64-linux-gnu/" + name, os.path.join(lib_root, name))
    for tgt, name in (("usr/lib", "lib"), ("usr/lib64", "lib64")):
        if os.path.isdir(os.path.join(sysroot, tgt)) and not os.path.lexists(os.path.join(sysroot, name)):
            os.symlink(tgt, os.path.join(sysroot, name))
    out = os.path.join(tree, "usr/bin", "luos-mesa-host")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cc = os.environ.get("CC", "cc")
    cmd = mesa_host_cmd(cc, sysroot, src, out)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("mesa host build failed:\n" + r.stderr)
    os.chmod(out, 0o755)
    print("initfs: built-in Mesa executor %s (%d KiB, linked against the image glibc)"
          % ("usr/bin/luos-mesa-host", os.path.getsize(out) // 1024))


class Node:
    def __init__(self, name, kind, mode, path=None, target=None):
        self.name, self.kind, self.mode, self.path, self.target = name, kind, mode, path, target
        self.children = []


def scan(path, name):
    st = os.lstat(path)
    if stat.S_ISLNK(st.st_mode):
        return Node(name, 4, 0o777, target=os.readlink(path))
    if stat.S_ISDIR(st.st_mode):
        n = Node(name, 1, st.st_mode & 0o7777)
        for child in sorted(os.listdir(path)):
            n.children.append(scan(os.path.join(path, child), child))
        return n
    return Node(name, 2, st.st_mode & 0o7777, path=path)


def flatten_dfs(node, out):
    out.append(node)
    node.index = len(out) - 1
    for c in node.children:
        flatten_dfs(c, out)
    node.end = len(out)


def compress(path, level):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        raw = f.read()
    if size < 1024:
        return 2, raw
    packed = subprocess.run(["zstd", "-q", "-c", "-%d" % level, "--ultra" if level > 19 else "-q",
                             "--content-size", "-T0", path], capture_output=True, check=True).stdout
    if len(packed) > size * 9 // 10:
        return 2, raw
    return 3, packed


def write_image(tree, out, level):
    root = scan(tree, "")
    nodes = []
    flatten_dfs(root, nodes)
    names = bytearray()
    blobs = []
    for n in nodes:
        n.name_off = len(names)
        names += n.name.encode()
        if n.kind == 2:
            n.size = os.path.getsize(n.path)
            rel = os.path.relpath(n.path, tree).replace(os.sep, "/")
            if rel in RAW_STORE:
                with open(n.path, "rb") as f:
                    data = f.read()  # already deflate-compressed inside; stored raw
            else:
                n.kind, data = compress(n.path, level)
        elif n.kind == 4:
            data = n.target.encode()
            n.size = len(data)
        else:
            data = b""
            n.size = 0
        blobs.append(data)
    header = 40
    table = header + 40 * len(nodes)
    names_off = table
    data_off = (names_off + len(names) + 15) & ~15
    body = bytearray()
    entries = bytearray()
    for n, data in zip(nodes, blobs):
        off = data_off + len(body) if data else 0
        body += data
        body += b"\0" * ((-len(body)) & 15)
        entries += struct.pack("<IHHIIQQQ", n.name_off, len(n.name.encode()), n.kind, n.mode, n.end, n.size, off,
                               len(data))
    total = data_off + len(body)
    with open(out, "wb") as f:
        f.write(struct.pack("<8sIIQQQ", b"LUOSIFS1", len(nodes), 0, names_off, len(names), total))
        f.write(entries)
        f.write(names)
        f.write(b"\0" * (data_off - names_off - len(names)))
        f.write(body)
    raw = sum(n.size for n in nodes if n.kind in (2, 3))
    print("initfs: %d entries, %.1f MiB of files in a %.1f MiB image -> %s" %
          (len(nodes), raw / 1048576, total / 1048576, out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--profile", choices=["mesa", "tests"], default="mesa")
    ap.add_argument("--firmware", default="amd,intel")
    ap.add_argument("--extra", action="append", default=[])
    ap.add_argument("--extra-bin", action="append", default=[])
    ap.add_argument("--cache", default=None)
    ap.add_argument("--vbios", action="append", default=[])
    ap.add_argument("--level", type=int, default=19)
    args = ap.parse_args()
    cache = args.cache or os.path.join(os.path.dirname(os.path.abspath(args.out)), "debs")
    os.makedirs(cache, exist_ok=True)
    index, provides = load_index(cache)
    roots = MESA + (TESTS if args.profile == "tests" else []) + HOST_BUILD
    pkgs = closure(roots, index, provides)
    work = tempfile.mkdtemp(prefix="luos-initfs-")
    raw, tree = os.path.join(work, "raw"), os.path.join(work, "tree")
    os.makedirs(raw)
    os.makedirs(tree)
    for name in pkgs:
        if name in HOST_BUILD:
            continue
        extract_deb(fetch_deb(name, index, cache), raw)
    prune(raw, tree, args.profile)
    groups = [g for g in args.firmware.replace(",", " ").split() if g != "none"]
    if groups:
        pkgs = pkgs + add_firmware(groups, index, cache, work, tree, args.vbios)
    build_mesa_host(cache, index, work, tree)
    for extra in args.extra:
        if os.path.isdir(extra):
            subprocess.run(["cp", "-r", extra + "/.", tree], check=True)
    for extra in args.extra_bin:
        if os.path.isdir(extra):
            os.makedirs(os.path.join(tree, "usr/bin"), exist_ok=True)
            for f in sorted(os.listdir(extra)):
                src = os.path.join(extra, f)
                if os.path.isfile(src) and not f.endswith("_dyn"):
                    shutil.copy2(src, os.path.join(tree, "usr/bin", f))
    with open(os.path.join(cache, "packages.txt"), "w") as f:
        f.write("\n".join("%s %s" % (p, index[p]["Version"]) for p in pkgs) + "\n")
    write_image(tree, args.out, args.level)
    shutil.rmtree(work)


if __name__ == "__main__":
    main()
