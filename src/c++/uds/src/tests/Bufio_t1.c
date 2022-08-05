// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "albtest.h"
#include "assertions.h"
#include "testPrototypes.h"

/*
 * The initial purpose of this test is to ensure that we understand how to use
 * dm-bufio.  We test the dm-bufio features that we plan to use.  The ongoing
 * purpose is to ensure that the features we are using continue to work as
 * expected.
 */

/**********************************************************************/

static const char SHAKESPEARE_SONNET_2[]
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

static const char *SHAKESPEARE_SONNET_3
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
static void writePage(struct io_factory *factory,
                      int                blockOffset,
                      const char        *sonnet,
                      size_t             sonnetSize)
{
  struct dm_bufio_client *client = NULL;
  UDS_ASSERT_SUCCESS(make_uds_bufio(factory, blockOffset * UDS_BLOCK_SIZE,
                                    UDS_BLOCK_SIZE, 1, &client));
  struct dm_buffer *buffer = NULL;
  void *data = dm_bufio_new(client, 0, &buffer);
  UDS_ASSERT_KERNEL_SUCCESS(data);
  memset(data, 0, UDS_BLOCK_SIZE);
  CU_ASSERT(sonnetSize < UDS_BLOCK_SIZE);
  memcpy(data, sonnet, sonnetSize);
  dm_bufio_mark_buffer_dirty(buffer);
  dm_bufio_release(buffer);
  dm_bufio_client_destroy(client);
}

/**********************************************************************/
static void readPage(struct io_factory *factory,
                     int                blockOffset,
                     const char        *sonnet,
                     size_t             sonnetSize)
{
  struct dm_bufio_client *client = NULL;
  UDS_ASSERT_SUCCESS(make_uds_bufio(factory, blockOffset * UDS_BLOCK_SIZE,
                                    UDS_BLOCK_SIZE, 1, &client));
  struct dm_buffer *buffer = NULL;
  void *data = dm_bufio_read(client, 0, &buffer);
  UDS_ASSERT_KERNEL_SUCCESS(data);
  CU_ASSERT(sonnetSize < UDS_BLOCK_SIZE);
  UDS_ASSERT_EQUAL_BYTES(data, sonnet, sonnetSize);
  dm_bufio_release(buffer);
  dm_bufio_client_destroy(client);
}

/**********************************************************************/
static void noioTest(void)
{
  struct io_factory *factory;
  struct dm_bufio_client *client = NULL;
  UDS_ASSERT_SUCCESS(make_uds_io_factory(getTestIndexName(), &factory));
  UDS_ASSERT_SUCCESS(make_uds_bufio(factory, 0, UDS_BLOCK_SIZE, 1, &client));
  dm_bufio_client_destroy(client);
  put_uds_io_factory(factory);
}

/**********************************************************************/
static void singleTest(void)
{
  struct io_factory *factory;
  UDS_ASSERT_SUCCESS(make_uds_io_factory(getTestIndexName(), &factory));
  writePage(factory, 0, SHAKESPEARE_SONNET_2, sizeof(SHAKESPEARE_SONNET_2));
  readPage(factory, 0, SHAKESPEARE_SONNET_2, sizeof(SHAKESPEARE_SONNET_2));
  put_uds_io_factory(factory);
}

/**********************************************************************/
static void doubleTest(void)
{
  struct io_factory *factory;
  UDS_ASSERT_SUCCESS(make_uds_io_factory(getTestIndexName(), &factory));
  writePage(factory, 1, SHAKESPEARE_SONNET_2, sizeof(SHAKESPEARE_SONNET_2));
  writePage(factory, 2, SHAKESPEARE_SONNET_3, sizeof(SHAKESPEARE_SONNET_3));
  readPage(factory, 1, SHAKESPEARE_SONNET_2, sizeof(SHAKESPEARE_SONNET_2));
  readPage(factory, 2, SHAKESPEARE_SONNET_3, sizeof(SHAKESPEARE_SONNET_3));
  put_uds_io_factory(factory);
}

/**********************************************************************/
static const CU_TestInfo tests[] = {
  { "noio",   noioTest },
  { "single", singleTest },
  { "double", doubleTest },
  CU_TEST_INFO_NULL,
};

static const CU_SuiteInfo suite = {
  .name  = "Bufio_t1",
  .tests = tests
};

/**********************************************************************/
const CU_SuiteInfo *initializeModule(void)
{
  return &suite;
}
