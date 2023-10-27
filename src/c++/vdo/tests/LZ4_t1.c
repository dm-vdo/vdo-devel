/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include <dirent.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>

#include "albtest.h"
#include "fileUtils.h"
#include "lz4.h"
#include "memory-alloc.h"
#include "syscalls.h"
#include "vdoAsserts.h"

/**********************************************************************/

const char *SHAKESPEARE_SONNET_2
  = "When forty winters shall besiege thy brow\n"
    "And dig deep trenches in thy beauty's field,\n"
    "Thy youth's proud livery, so gazed on now,\n"
    "Will be a tottered weed of small worth held.\n"
    "Then, being asked where all thy beauty lies,\n"
    "Where all the treasure of thy lusty days;\n"
    "To say within thine own deep-sunken eyes,\n"
    "Were an all-eating shame, and thriftless praise.\n"
    "How much more praise deserved thy beauty's use,\n"
    "If thou couldst answer, \"This fair child of mine\n"
    "Shall sum my count, and make my old excuse,\"\n"
    "Proving his beauty by succession thine.\n"
    "  This were to be new made when thou art old,\n"
    "  And see thy blood warm when thou feel'st it cold.\n";

const char *SHAKESPEARE_SONNET_3
  = "Look in thy glass and tell the face thou viewest,\n"
    "Now is the time that face should form another,\n"
    "Whose fresh repair if now thou not renewest,\n"
    "Thou dost beguile the world, unbless some mother.\n"
    "For where is she so fair whose uneared womb\n"
    "Disdains the tillage of thy husbandry?\n"
    "Or who is he so fond will be the tomb\n"
    "Of his self-love, to stop posterity?\n"
    "Thou art thy mother's glass, and she in thee\n"
    "Calls back the lovely April of her prime;\n"
    "So thou through windows of thine age shalt see,\n"
    "Despite of wrinkles, this thy golden time.\n"
    "  But if thou live rememb'red not to be,\n"
    "  Die single and thine image dies with thee.\n";

/**********************************************************************/
static void uncompressRandomData(const char* source, int isize, int osize)
{
  // Create a large frame around the uncompressed result
  char *uncompressed, *frame;
  size_t frameSize = 3 * osize;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(frameSize, char, __func__, &frame));
  uncompressed = frame + osize;
  // Test that uncompressing does not write into the frame around the
  // output array.
  memset(frame, 0, frameSize);
  LZ4_uncompress_unknownOutputSize(source, uncompressed, isize, osize);
  memset(uncompressed, 0, osize);
  for (size_t i = 0; i < frameSize; i++) {
    CU_ASSERT(frame[i] == 0);
  }
  uds_free(frame);
}

/**********************************************************************/
static void compressString(const char *source)
{
  int sourceLen = strlen(source);
  char *compressed, *copy, *ctx;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(sourceLen, char, __func__, &compressed));
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(sourceLen + 1, char, __func__, &copy));
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(LZ4_context_size(), char, __func__, &ctx));
  // Test the data are compressed
  int compressedLen = LZ4_compress_ctx_limitedOutput(ctx, source, compressed,
                                                     sourceLen, sourceLen);
  CU_ASSERT(compressedLen > 0);
  CU_ASSERT(compressedLen < sourceLen);
  // Test the data cannot be uncompressed when the destination is too small
  int copyLen = LZ4_uncompress_unknownOutputSize(compressed, copy,
                                                 compressedLen, sourceLen - 1);
  CU_ASSERT(copyLen < 0);
  // Test the data can be uncompressed when the destination is just right
  copyLen = LZ4_uncompress_unknownOutputSize(compressed, copy, compressedLen,
                                             sourceLen);
  CU_ASSERT(copyLen == sourceLen);
  UDS_ASSERT_EQUAL_BYTES(source, copy, sourceLen);
  // Test the data can be uncompressed when the destination is too large
  copyLen = LZ4_uncompress_unknownOutputSize(compressed, copy, compressedLen,
                                             sourceLen + 1);
  CU_ASSERT(copyLen == sourceLen);
  UDS_ASSERT_EQUAL_BYTES(source, copy, sourceLen);
  // Test that uncompressing the source data does not do bad writes
  uncompressRandomData(source, sourceLen, sourceLen);
  // Test that uncompressing the compressed data does not do bad writes
  uncompressRandomData(compressed, compressedLen, sourceLen - 1);
  uncompressRandomData(compressed, compressedLen, sourceLen);
  uncompressRandomData(compressed, compressedLen, sourceLen + 1);
  uds_free(compressed);
  uds_free(copy);
  uds_free(ctx);
}

/**********************************************************************/
static void testPoetry(void)
{
  compressString(SHAKESPEARE_SONNET_2);
  compressString(SHAKESPEARE_SONNET_3);
}

/**********************************************************************/
static int compressBlockFromStream(FILE *stream, int sourceLen)
{
  char *compressed, *copy, *ctx, *source;
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(sourceLen, char, __func__, &compressed));
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(sourceLen, char, __func__, &copy));
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(LZ4_context_size(), char, __func__, &ctx));
  VDO_ASSERT_SUCCESS(UDS_ALLOCATE(sourceLen, char, __func__, &source));
  CU_ASSERT(fread(source, sourceLen, 1, stream) == 1);
  uncompressRandomData(source, sourceLen, sourceLen);
  int compressedLen = LZ4_compress_ctx_limitedOutput(ctx, source, compressed,
                                                     sourceLen, sourceLen);
  if ((compressedLen > 0) && (compressedLen < sourceLen)) {
    int copyLen = LZ4_uncompress_unknownOutputSize(compressed, copy,
                                                   compressedLen, sourceLen);
    CU_ASSERT(copyLen == sourceLen);
    UDS_ASSERT_EQUAL_BYTES(source, copy, sourceLen);
    uncompressRandomData(compressed, compressedLen, sourceLen);
  } else {
    compressedLen = 0;
  }
  uds_free(compressed);
  uds_free(copy);
  uds_free(ctx);
  uds_free(source);
  // Return the length of the compressed data, or zero if the data were not
  // compressible
  return compressedLen;
}

/**********************************************************************/
static void testFiles(void)
{
  enum { BLOCK_SIZE = 4096 };
  // Track how many blocks are compressible, and how much they are compressed
  long numBytes = 0;
  long numBytesCompressed = 0;
  int numBlocksCompressed = 0;
  int numBlocks = 0;
  // This outer loop uses the test directory as a source of test files
  DIR *dir = opendir(".");
  CU_ASSERT_PTR_NOT_NULL(dir);
  struct dirent *entry;

  char *testFileRegexStr = ".*\\.c";
  regex_t testFileRegex;
  CU_ASSERT_EQUAL(regcomp(&testFileRegex, testFileRegexStr, REG_NOSUB), 0);

  while ((entry = readdir(dir)) != NULL) {
    if (regexec(&testFileRegex, entry->d_name, 0, NULL, 0) == REG_NOMATCH) {
      continue;
    }
    struct stat sb;
    UDS_ASSERT_SYSTEM_CALL(lstat(entry->d_name, &sb));
    if (S_ISREG(sb.st_mode)) {
      // And then this inner loop uses each test file as a source of 4K blocks
      FILE *stream = fopen(entry->d_name, "r");
      CU_ASSERT_PTR_NOT_NULL(stream);
      for (int size = sb.st_size; size > 0; size -= BLOCK_SIZE) {
        int sourceLen = BLOCK_SIZE;
        if (size < sourceLen) {
          sourceLen = size;
        }
        ++numBlocks;
        int compressedLen = compressBlockFromStream(stream, sourceLen);
        if (compressedLen > 0) {
          numBytes += sourceLen;
          numBytesCompressed += compressedLen;
          ++numBlocksCompressed;
        }
      }
      UDS_ASSERT_SYSTEM_CALL(fclose(stream));
    }
  }
  UDS_ASSERT_SYSTEM_CALL(closedir(dir));
  // Report how many blocks were compressed, and how much
  double squishedness = 100.0;
  if (numBytes > 0) {
    squishedness *= numBytesCompressed;
    squishedness /= numBytes;
  }
  printf("(%d of %d blocks compressed to %2.0f%%) ",
         numBlocksCompressed, numBlocks, squishedness);
  regfree(&testFileRegex);
}

/**********************************************************************/

static CU_TestInfo theTestInfo[] = {
  { "poetry test", testPoetry },
  { "files test",  testFiles },
  CU_TEST_INFO_NULL
};

static CU_SuiteInfo theSuiteInfo = {
  .name                     = "LZ4 tests (LZ4_t1)",
  .initializerWithArguments = NULL,
  .initializer              = NULL,
  .cleaner                  = NULL,
  .tests                    = theTestInfo
};

CU_SuiteInfo *initializeModule(void)
{
  return &theSuiteInfo;
}
