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

#define ZSTD_FORWARD_IF_ERR(ret)            \
	do {                                \
		size_t const __ret = (ret); \
		if (ZSTD_isError(__ret))    \
			return __ret;       \
	} while (0)

static size_t zstd_cctx_init(zstd_cctx *cctx, const zstd_parameters *parameters,
	unsigned long long pledged_src_size)
{
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_reset(
		cctx, ZSTD_reset_session_and_parameters));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setPledgedSrcSize(
		cctx, pledged_src_size));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_windowLog, parameters->cParams.windowLog));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_hashLog, parameters->cParams.hashLog));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_chainLog, parameters->cParams.chainLog));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_searchLog, parameters->cParams.searchLog));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_minMatch, parameters->cParams.minMatch));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_targetLength, parameters->cParams.targetLength));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_strategy, parameters->cParams.strategy));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_contentSizeFlag, parameters->fParams.contentSizeFlag));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_checksumFlag, parameters->fParams.checksumFlag));
	ZSTD_FORWARD_IF_ERR(ZSTD_CCtx_setParameter(
		cctx, ZSTD_c_dictIDFlag, !parameters->fParams.noDictIDFlag));
	return 0;
}

static size_t zstd_compress_cctx(zstd_cctx *cctx, void *dst, size_t dst_capacity,
	const void *src, size_t src_size, const zstd_parameters *parameters)
{
	ZSTD_FORWARD_IF_ERR(zstd_cctx_init(cctx, parameters, src_size));
	return ZSTD_compress2(cctx, dst, dst_capacity, src, src_size);
}

#define zstd_decompress_dctx ZSTD_decompressDCtx
#define zstd_is_error ZSTD_isError
#endif // FAKE_ZSTD_SHIMS_H
