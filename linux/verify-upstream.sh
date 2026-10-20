#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 korbolajnennikolaj

# Checks that every file under upstream/ is byte-identical to the pinned Linux release.
cd "$(dirname "$0")/upstream" && sha256sum -c --quiet ../MANIFEST.sha256 && echo "upstream/ matches Linux $(cat ../VERSION)"
