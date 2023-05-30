// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "murmurhash3.h"
#include "string-utils.h"
#include "testPrototypes.h"

/**********************************************************************/
static inline uint32_t numberFromBuffer(const u8 *buffer)
{
  return buffer[0] | (buffer[1] << 8) | (buffer[2] << 16) | (buffer[3] << 24);
}

/**********************************************************************/
uint64_t fillBufferFromSeed(uint64_t seed, void *buffer, size_t size)
{
  enum {
    HASH_SIZE = 16              // for murmurhash3_128
  };

  // hex numbers below all from /dev/urandom...

  u8 hashBuffer[HASH_SIZE] = {
    0x67, 0x08, 0xf3, 0xa9, 0xfe, 0xb8, 0x4e, 0x9f,
    0xd5, 0xc1, 0xc1, 0xc2, 0x27, 0x40, 0xd9, 0x0c,
  };

  const uint32_t mask = ~0;
  uint32_t seed1 = 0xc158be6a ^ (seed & mask);
  uint32_t seed2 = 0xef4d80a3 ^ (seed >> 32);
  uint32_t seed3 = 0x96de0058 ^ ((seed >> 16) & mask);

  u8 outBuffer[HASH_SIZE];

  // do this at least once to generate new seed to return
  unsigned int i = 1;
  u8 *buf = buffer;
  do {
    murmurhash3_128(hashBuffer, sizeof(hashBuffer), seed1, outBuffer);
    uint32_t temp = seed1;
    seed1 = (seed2 ^ numberFromBuffer(&outBuffer[0])) + i;
    seed2 = (seed3 ^ numberFromBuffer(&outBuffer[4])) + 2 * i;
    seed3 = (temp  ^ numberFromBuffer(&outBuffer[8])) + 3 * i;

    size_t n = size < HASH_SIZE ? size : HASH_SIZE;
    if (n > 0) {
      memcpy(hashBuffer, outBuffer, HASH_SIZE);
      memcpy(buf, outBuffer, n);
      buf += n;
      size -= n;
    }
    ++i;
  } while (size > 0);

  return ((uint64_t) seed2 << 32) | seed3;
}
