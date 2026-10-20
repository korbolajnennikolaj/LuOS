// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "internal.h"
#include <linux/zstd.h>

unsigned long lkpi_zstd_workspace_size(void)
{
	return zstd_dctx_workspace_bound();
}

long lkpi_zstd_decompress(void *dst, unsigned long cap, const void *src, unsigned long len, void *wksp,
			  unsigned long wksp_size)
{
	zstd_dctx *dctx = zstd_init_dctx(wksp, wksp_size);
	if (!dctx) return -ENOMEM;
	size_t r = zstd_decompress_dctx(dctx, dst, cap, src, len);
	if (zstd_is_error(r)) {
		pr_err("LinuxKPI: zstd: %s\n", zstd_get_error_name(r));
		return -EIO;
	}
	return (long)r;
}
