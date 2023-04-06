/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef PARSE_UTILS_H
#define PARSE_UTILS_H

#include <stdint.h>
#include <stdbool.h>

#include "encodings.h"

typedef struct {
  char *sparse;
  char *memorySize;
} UdsConfigStrings;

/**
 * Parse a string argument as an unsigned int.
 *
 * @param [in]  arg      The argument to parse
 * @param [in]  lowest   The lowest allowed value
 * @param [in]  highest  The highest allowed value
 * @param [out] numPtr   A pointer to return the parsed integer.
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE.
 **/
int __must_check parseUInt(const char *arg,
			   unsigned int lowest,
			   unsigned int highest,
			   unsigned int *numPtr);

/**
 * Parse a string argument as a signed int.
 *
 * @param [in]  arg      The argument to parse
 * @param [out] numPtr   A pointer to return the parsed integer.
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE.
 **/
int parseInt(const char *arg, int *numPtr);

/**
 * Parse a string argument as a decimal uint64_t.
 *
 * @param [in]  arg      The argument to parse
 * @param [out] numPtr   A pointer to return the parsed value.
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE.
 **/
int __must_check parseUInt64(const char *arg, uint64_t *numPtr);

/**
 * Parse a string argument as a size, optionally using LVM's concept
 * of size suffixes.
 *
 * @param [in]  arg      The argument to parse
 * @param [in]  lvmMode  Whether to parse suffixes as LVM or SI.
 * @param [out] sizePtr  A pointer to return the parsed size, in bytes
 *
 * @return VDO_SUCCESS or VDO_OUT_OF_RANGE.
 **/
int __must_check parseSize(const char *arg, bool lvmMode, uint64_t *sizePtr);

/**
 * Parse UdsConfigStrings into a index_config.
 *
 * @param [in]  configStrings  The UDS config strings read.
 * @param [out] configPtr      A pointer to return the struct index_config.
 **/
int __must_check parseIndexConfig(UdsConfigStrings *configStrings,
				  struct index_config *configPtr);

#endif // PARSE_UTILS_H
