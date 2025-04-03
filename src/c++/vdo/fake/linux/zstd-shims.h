/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
/*
 * Essentially a stripped down version of the kernel zstd.h.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of https://github.com/facebook/zstd) and
 * the GPLv2 (found in the COPYING file in the root directory of
 * https://github.com/facebook/zstd). You may select, at your option, one of the
 * above-listed licenses.
 *
 * Copyright 2025 Google LLC
 */

#ifndef FAKE_ZSTD_SHIMS_H
#define FAKE_ZSTD_SHIMS_H

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#define zstd_min_clevel ZSTD_minCLevel
#define zstd_max_clevel ZSTD_maxCLevel
#define zstd_parameters ZSTD_parameters
#define zstd_compression_parameters ZSTD_compressionParameters

/**
 * zstd_get_params() - returns zstd_parameters for selected level
 * @level:              The compression level
 * @estimated_src_size: The estimated source size to compress or 0
 *                      if unknown.
 *
 * Return:              The selected zstd_parameters.
 */
static inline zstd_parameters zstd_get_params(int level,
	unsigned long long estimated_src_size)
{
	return ZSTD_getParams(level, estimated_src_size, 0);
}

static inline size_t zstd_cctx_workspace_bound(const zstd_compression_parameters *cparams)
{
	return ZSTD_estimateCCtxSize_usingCParams(*cparams);
}

#define zstd_dctx_workspace_bound ZSTD_estimateDCtxSize
#define zstd_dctx ZSTD_DCtx
#define zstd_cctx ZSTD_CCtx

static zstd_cctx *zstd_init_cctx(void *workspace, size_t workspace_size)
{
	if (workspace == NULL)
		return NULL;
	return ZSTD_initStaticCCtx(workspace, workspace_size);
}

static zstd_dctx *zstd_init_dctx(void *workspace, size_t workspace_size)
{
	if (workspace == NULL)
		return NULL;
	return ZSTD_initStaticDCtx(workspace, workspace_size);
}

#endif // FAKE_ZSTD_SHIMS_H
