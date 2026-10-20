#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

"""lkgen.py - derives how a Linux GPU driver is built from the driver's own Kbuild files.

  lkgen.py DRIVER [--src DIR]        flags and object list of one driver
  lkgen.py all [--src DIR]           every driver, then the header closure written into upstream.list

DRIVER is amdgpu, nouveau or drm-common (the shared DRM, lib and kernel files they need that i915 did not). The driver's Kbuild/Makefile (and the ones it includes) are evaluated by GNU make with
the CONFIG_ symbols below, which gives the object list, the include paths and defines (ccflags-y) and the per-file
flags (CFLAGS_x.o, CFLAGS_REMOVE_x.o; the display core's floating point code needs them). Written:

  linux/gen/DRIVER.mk       per-driver compiler flags for linux.mk
  linux/gen/DRIVER.list     the .c files
  linux/gen/DRIVER.json     the same as data (tools/lkdev uses it)

"all" also runs the compiler's preprocessor over every object (gcc -M, the include paths of the real build, but
against the full Linux tree) and writes every file that is read, and is not listed above the marker, into the
generated block at the end of upstream.list. sync-upstream.py then copies exactly those files.

The Linux tree is --src, default $LINUX_SRC or ~/linux-src (the version in linux/VERSION). CONFIG_ symbols that
kpi/include/generated/autoconf.h already defines are not repeated as -D: they are global to every driver.
"""
import argparse, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LINUX = os.path.dirname(HERE)

DRIVERS = {
    'drm-common': dict(
        dir='drivers/gpu/drm', mod='drm', config='', force_include=[], header_dirs=[],
        # what amdgpu and nouveau need from the DRM core, lib/ and kernel/ and i915 did not
        objs=['drivers/gpu/drm/drm_exec.c', 'drivers/gpu/drm/drm_suballoc.c', 'drivers/gpu/drm/drm_gpuvm.c',
              'drivers/gpu/drm/drm_gem_ttm_helper.c', 'drivers/gpu/drm/scheduler/sched_main.c',
              'drivers/gpu/drm/scheduler/sched_entity.c', 'drivers/gpu/drm/scheduler/sched_fence.c',
              'lib/maple_tree.c', 'lib/kfifo.c', 'lib/interval_tree.c', 'kernel/time/timeconv.c',
              'drivers/gpu/drm/drm_panel_backlight_quirks.c'],
    ),
    'amdgpu': dict(
        dir='drivers/gpu/drm/amd/amdgpu', mod='amdgpu',
        # DRM_AMDGPU selects HMM_MIRROR and MMU_NOTIFIER; the notifier is a stub that refuses (see lkpi/gpu.h)
        config='DRM_AMDGPU DRM_AMD_DC DRM_AMD_DC_FP HMM_MIRROR MMU_NOTIFIER ACPI',
        # TRUE/FALSE come from ACPICA's types in Linux, which amdgpu gets through <linux/acpi.h> somewhere
        force_include=['lkpi/acpi.h'],
        # drivers/gpu/drm/amd/amdxcp is a separate module in Linux (the partitions of MI300 are DRM devices of their own)
        extra_objs=['drivers/gpu/drm/amd/amdxcp/amdgpu_xcp_drv.c'],
        # every header of the driver's own trees is listed (they are needed whole: register headers are included
        # by file, and the object list above is the only thing that decides what is compiled)
        header_dirs=[],
    ),
    'nouveau': dict(
        dir='drivers/gpu/drm/nouveau', mod='nouveau',
        # NOUVEAU_DEBUG / _DEFAULT are the Kconfig defaults (maximum and default log level). Off: NOUVEAU_PLATFORM_DRIVER
        # (Tegra), DRM_NOUVEAU_SVM (needs HMM), DRM_NOUVEAU_BACKLIGHT, HWMON, DEBUG_FS, AGP.
        config='DRM_NOUVEAU NOUVEAU_DEBUG=5 NOUVEAU_DEBUG_DEFAULT=3 ACPI',
        # the GSP code reads the firmware's ELF headers and takes <linux/elf.h> from <linux/module.h> in Linux
        force_include=['linux/elf.h'],
        header_dirs=[],
    ),
}


def autoconf_symbols():
    return set(autoconf_values())


def autoconf_values():
    """CONFIG_ symbols every driver is built with (kpi/include/generated/autoconf.h) -> make value"""
    text = open(os.path.join(LINUX, 'kpi/include/generated/autoconf.h')).read()
    return {m.group(1): ('y' if m.group(2) in ('', '1') else m.group(2))
            for m in re.finditer(r'^#define\s+CONFIG_(\w+)[ \t]*(\S*)', text, re.M)}


def eval_kbuild(src, drv):
    if 'objs' in drv:
        return {'objs': [o[:-2] + '.o' for o in drv['objs']], 'ccflags': [], 'cf': {}, 'cr': {}}
    out = subprocess.run(['make', '-s', '-C', src, '-f', os.path.join(HERE, 'kbuild-objs.mk'),
                          'DRVDIR=' + drv['dir'], 'MOD=' + drv['mod'],
                          # the Kbuild files see the global configuration too (CONFIG_X86 decides nouveau_acpi.o)
                          'CFG=' + drv['config'] + ' ' + ' '.join('%s=%s' % kv for kv in autoconf_values().items())],
                         capture_output=True, text=True)
    if out.returncode:
        sys.exit(out.stderr)
    res = {'objs': [], 'ccflags': [], 'cf': {}, 'cr': {}}
    for line in out.stdout.splitlines():
        if line.startswith('OBJS='):
            res['objs'] = sorted(os.path.normpath(os.path.join(drv['dir'], o)) for o in line[5:].split())
        elif line.startswith('CCFLAGS='):
            toks, fl, i = line[8:].split(), [], 0
            while i < len(toks):                      # "-I path" and "-Ipath" are the same
                if toks[i] in ('-I', '-D', '-U') and i + 1 < len(toks):
                    fl.append(toks[i] + toks[i + 1]); i += 2
                else:
                    fl.append(toks[i]); i += 1
            res['ccflags'] = fl
        elif line[:3] in ('CF:', 'CR:'):
            k, v = line[3:].split('=', 1)
            res['cf' if line[1] == 'F' else 'cr'][os.path.normpath(os.path.join(drv['dir'], k))] = v.split()
    res['objs'] = sorted(res['objs'] + [o[:-2] + '.o' for o in drv.get('extra_objs', [])])
    return res


def upstream_flag(f):
    """-Ipath (relative to the Linux tree root) -> -I$(LINUX_DIR)/upstream/path"""
    if f.startswith('-I'):
        return '-I$(LINUX_DIR)/upstream/' + os.path.normpath(f[2:])
    return f


def gen_mk(name, drv, res):
    if 'objs' in drv:
        return '# generated by tools/lkgen.py %s; the flags of these files are the defaults in linux.mk\n' % name
    auto = autoconf_symbols()
    defs = []
    for c in drv['config'].split():
        name, _, value = c.partition('=')
        if name not in auto:
            defs.append('-DCONFIG_%s=%s' % (name, value or '1'))
    flags = [upstream_flag(f) for f in res['ccflags']] + defs
    flags += ['-include $(LINUX_DIR)/kpi/include/' + h for h in drv['force_include']]
    root = '$(LINUX_BUILD)/upstream/'
    dirs = sorted({os.path.dirname(o) for o in res['objs']})
    # amdgpu's objects are in amd/amdgpu, amd/pm and amd/display: one pattern for everything below their common parent
    top = os.path.commonpath(dirs)
    lines = ['# generated by tools/lkgen.py %s from the driver\'s Kbuild files; do not edit' % name, '']
    lines.append('%s%s/%%.o: LINUX_MODNAME = %s' % (root, top, drv['mod']))
    lines.append('%s%s/%%.o: LINUX_EXTRA_CFLAGS = %s' % (root, top, ' '.join(flags)))
    lines.append('')
    for o in res['objs']:
        if o in res['cf']:
            lines.append('%s%s: LINUX_FILE_CFLAGS = %s' % (root, o, ' '.join(upstream_flag(f) for f in res['cf'][o])))
        if o in res['cr']:
            lines.append('%s%s: LINUX_FILE_CFLAGS_REMOVE = %s' % (root, o, ' '.join(res['cr'][o])))
    return '\n'.join(lines) + '\n'


def compile_deps(src, rel, res, drv, name):
    """files read while preprocessing one object: the include paths of linux.mk with the full tree for upstream/"""
    flags = []
    for f in res['ccflags']:
        flags.append('-I' + os.path.join(src, f[2:]) if f.startswith('-I') else f)
    obj = rel[:-2] + '.o'
    auto = autoconf_symbols()
    defs = []
    for c in drv['config'].split():
        n, _, v = c.partition('=')
        if n not in auto:
            defs.append('-DCONFIG_%s=%s' % (n, v or '1'))
    cmd = ['gcc', '-std=gnu11', '-m64', '-nostdinc', '-D__KERNEL__', '-ffreestanding',
           '-include', os.path.join(LINUX, 'kpi/include/lkpi/prelude.h')]
    for h in drv['force_include']:
        cmd += ['-include', os.path.join(LINUX, 'kpi/include', h)]
    cmd += defs + flags + res['cf'].get(obj, [])
    cmd += ['-I' + os.path.join(LINUX, 'kpi/include'), '-I' + os.path.join(src, 'include'),
            '-I' + os.path.join(LINUX, 'kpi/include/uapi'), '-I' + os.path.join(src, 'include/uapi'),
            '-I' + os.path.join(src, 'arch/x86/include/uapi'), '-I' + os.path.dirname(os.path.join(src, rel)),
            '-DKBUILD_MODNAME="%s"' % drv['mod'], '-DKBUILD_BASENAME="x"', '-M', os.path.join(src, rel)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('%s: preprocessing failed\n%s' % (rel, r.stderr[:2000]))
    files = set()
    for tok in r.stdout.replace('\\\n', ' ').split():
        if tok.startswith(src + '/'):
            files.add(os.path.normpath(tok[len(src) + 1:]))
    return files


MARK_BEGIN = '# >>> generated by tools/lkgen.py all (amdgpu, nouveau, drm-common): do not edit between the markers'
MARK_END = '# <<< end of generated block'


def update_upstream_list(closure):
    path = os.path.join(LINUX, 'upstream.list')
    text = open(path).read()
    if MARK_BEGIN in text:
        text = text[:text.index(MARK_BEGIN)].rstrip('\n') + '\n'
    listed = set()
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            listed.add(line)
    new = sorted(f for f in closure if f not in listed)
    open(path, 'w').write(text + '\n' + MARK_BEGIN + '\n' + ''.join(f + '\n' for f in new) + MARK_END + '\n')
    return len(new)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('driver', choices=sorted(DRIVERS) + ['all'])
    ap.add_argument('--src', default=os.environ.get('LINUX_SRC', os.path.expanduser('~/linux-src')))
    a = ap.parse_args()
    names = sorted(DRIVERS) if a.driver == 'all' else [a.driver]
    closure = set()
    os.makedirs(os.path.join(LINUX, 'gen'), exist_ok=True)
    for name in names:
        drv = DRIVERS[name]
        res = eval_kbuild(a.src, drv)
        open(os.path.join(LINUX, 'gen', name + '.mk'), 'w').write(gen_mk(name, drv, res))
        open(os.path.join(LINUX, 'gen', name + '.list'), 'w').write(''.join(o[:-2] + '.c\n' for o in res['objs']))
        json.dump(dict(res, cfg=drv['config'].split(), mod=drv['mod'], force_include=drv['force_include']),
                  open(os.path.join(LINUX, 'gen', name + '.json'), 'w'), indent=1)
        print('%s: %d objects, %d per-file flag sets' % (name, len(res['objs']), len(res['cf'])))
        if a.driver == 'all':
            for o in res['objs']:
                closure |= compile_deps(a.src, o[:-2] + '.c', res, drv, name)
    if a.driver == 'all':
        print('upstream.list: %d files added to the generated block' % update_upstream_list(closure))


if __name__ == '__main__':
    main()
