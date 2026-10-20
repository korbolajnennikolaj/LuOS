#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

# Copies the files listed in upstream.list from a Linux checkout (unmodified) into upstream/ and
# writes MANIFEST.sha256. Usage: sync-upstream.py /path/to/linux   (checkout of the tag in VERSION)
# Lines in upstream.list are paths relative to the Linux tree; a trailing / copies a whole directory.
import hashlib, os, shutil, sys
here = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1]
dst = os.path.join(here, 'upstream')
paths = []
for line in open(os.path.join(here, 'upstream.list')):
    line = line.split('#', 1)[0].strip()
    if not line: continue
    full = os.path.join(src, line)
    if line.endswith('/'):
        for root, _, files in os.walk(full):
            for f in files:
                if f.endswith(('.h', '.c')):
                    paths.append(os.path.relpath(os.path.join(root, f), src))
    else:
        if not os.path.isfile(full): sys.exit('missing in Linux tree: ' + line)
        paths.append(line)
if os.path.isdir(dst): shutil.rmtree(dst)
lines = []
for p in sorted(set(paths)):
    os.makedirs(os.path.dirname(os.path.join(dst, p)), exist_ok=True)
    shutil.copyfile(os.path.join(src, p), os.path.join(dst, p))
    lines.append('%s  %s' % (hashlib.sha256(open(os.path.join(src, p), 'rb').read()).hexdigest(), p))
open(os.path.join(here, 'MANIFEST.sha256'), 'w').write('\n'.join(lines) + '\n')
print('%d files copied' % len(lines))
