/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#include "journalWritingUtils.h"

#include "block-map.h"
#include "data-vio.h"
#include "recovery-journal.h"
#include "slab.h"
#include "slab-depot.h"
#include "types.h"
#include "vdo-component-states.h"

#include "blockMapUtils.h"
#include "vdoAsserts.h"
#include "vdoTestBase.h"

enum {
  BAD_SLOT = 0x3ff,
};

static struct block_map        *blockMap;
static block_count_t            journalSize;
static slab_count_t             slabsToReference;

static physical_block_number_t  badPBN;

/**********************************************************************/
void initializeJournalWritingUtils(block_count_t journalBlocks,
                                   block_count_t logicalBlocks,
                                   slab_count_t  slabs)
{
  initializeBlockMapUtils(logicalBlocks);
  blockMap         = vdo->block_map;
  journalSize      = journalBlocks;
  slabsToReference = slabs;
  badPBN           = getTestConfig().config.physical_blocks + 1;
}

/**********************************************************************/
void tearDownJournalWritingUtils(void)
{
  tearDownBlockMapUtils();
}

/**********************************************************************/
physical_block_number_t computePBNFromLBN(logical_block_number_t lbn,
                                          block_count_t          offset)
{
  struct slab_depot        *depot       = vdo->depot;
  const struct slab_config *slabConfig  = &depot->slab_config;
  physical_block_number_t   firstPBN    = depot->slabs[1]->start;
  slab_count_t              slabIndex   = ((lbn / slabConfig->data_blocks)
                                           % slabsToReference);
  block_count_t             blockOffset = (lbn % slabConfig->data_blocks);
  physical_block_number_t   pbn
    = (firstPBN + (slabIndex * slabConfig->slab_blocks) + blockOffset + offset);
  if (!vdo_is_physical_data_block(depot, pbn)) {
    pbn -= (blockOffset + offset);
  }
  CU_ASSERT_TRUE(vdo_is_physical_data_block(depot, pbn));
  return pbn;
}

/**********************************************************************/
void makeJournalEntry(struct packed_recovery_journal_entry *packed,
                      logical_block_number_t                lbn,
                      bool                                  isIncRef,
                      physical_block_number_t               pbn,
                      CorruptionType                        corruption)
{
  page_count_t pageIndex = lbn / VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

  struct recovery_journal_entry entry = {
    .operation = (isIncRef ? VDO_JOURNAL_DATA_INCREMENT
                           : VDO_JOURNAL_DATA_DECREMENT),
    .slot = {
      .pbn  = vdo_find_block_map_page_pbn(blockMap, pageIndex),
      .slot = lbn % VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
    },
    .mapping = {
      .pbn   = pbn,
      .state = VDO_MAPPING_STATE_UNCOMPRESSED,
    },
  };

  switch (corruption) {
  case CORRUPT_LBN_PBN:
    entry.slot.pbn = badPBN;
    break;

  case CORRUPT_LBN_SLOT:
    entry.slot.slot = BAD_SLOT;
    break;

  case CORRUPT_PBN:
    entry.mapping.pbn = badPBN;
    break;

  default:
    break;
  }

  *packed = vdo_pack_recovery_journal_entry(&entry);
}

/**
 * Implements PopulateBlockMapConfigurator.
 **/
static void putBlocksInMapConfigurator(struct data_vio *dataVIO)
{
  dataVIO->new_mapped.pbn = computePBNFromLBN(dataVIO->logical.lbn, 0);
  dataVIO->new_mapped.state = VDO_MAPPING_STATE_UNCOMPRESSED;
  dataVIO->recovery_sequence_number = (dataVIO->new_mapped.pbn
                                       / RECOVERY_JOURNAL_ENTRIES_PER_BLOCK);
  setBlockMapping(dataVIO->logical.lbn,
                  dataVIO->new_mapped.pbn,
                  VDO_MAPPING_STATE_UNCOMPRESSED);
}

/**********************************************************************/
void putBlocksInMap(logical_block_number_t start, block_count_t count)
{
  populateBlockMap(start, count, putBlocksInMapConfigurator);
}

/**********************************************************************/
void setBlockHeader(struct packed_journal_header *header,
                    BlockPattern                 *blockPattern)
{
  struct recovery_journal *journal = vdo->recovery_journal;
  struct recovery_block_header unpacked;
  vdo_unpack_recovery_block_header(header, &unpacked);

  unpacked.block_map_head    = blockPattern->head;
  unpacked.slab_journal_head = blockPattern->head;
  unpacked.sequence_number   = blockPattern->sequenceNumber;
  unpacked.metadata_type     = VDO_METADATA_RECOVERY_JOURNAL;
  unpacked.recovery_count    = blockPattern->recoveryCount;
  unpacked.check_byte
    = vdo_compute_recovery_journal_check_byte(journal, unpacked.sequence_number);

  unpacked.nonce = journal->nonce;
  if (blockPattern->nonceState == BAD_NONCE) {
    unpacked.nonce = BAD_NONCE;
  }

  unpacked.entry_count = journal->entries_per_block;
  if (blockPattern->blockLength == SHORT_BLOCK) {
    unpacked.entry_count = SHORT_BLOCK;
  }

  vdo_pack_recovery_block_header(&unpacked, header);
}

/**********************************************************************/
void setSectorHeader(struct packed_journal_sector *sector,
                     uint8_t                       check_byte,
                     const SectorPattern          *sectorPattern)
{
  sector->entry_count = sectorPattern->entryCount;
  if (sectorPattern->entryCount == FULL_SECTOR) {
    sector->entry_count = RECOVERY_JOURNAL_ENTRIES_PER_SECTOR;
  }
  sector->recovery_count = sectorPattern->recoveryCount;

  if (sectorPattern->tearType == TEAR_OLD) {
    sector->check_byte = check_byte - 1;
  } else if (sectorPattern->tearType == TEAR_NEW) {
    sector->check_byte = check_byte + 1;
  } else {
    sector->check_byte = check_byte;
  }
}

/**********************************************************************/
void writeJournalBlocks(CorruptionType  corruption,
                        bool            readOnly,
                        BlockPattern   *journalPattern)
{
  struct recovery_journal *journal = vdo->recovery_journal;

  physical_block_number_t journalStart;
  VDO_ASSERT_SUCCESS(vdo_translate_to_pbn(journal->partition, 0,
                                          &journalStart));

  char block[VDO_BLOCK_SIZE];

  logical_block_number_t nextLBN                = 0;
  sequence_number_t      maxValidSequenceNumber = 1;
  journal_entry_count_t  blockEntries           = 0;
  for (block_count_t i = 0; i < journalSize; i++) {
    VDO_ASSERT_SUCCESS(layer->reader(layer, journalStart + i, 1, block));
    struct packed_journal_header *header
      = (struct packed_journal_header *) block;
    BlockPattern *blockPattern = &journalPattern[i];
    setBlockHeader(header, blockPattern);
    // Keep track of the highest block with a valid nonce, so we can make
    // the real journal claim it has saved this many blocks.
    if ((journalPattern[i].nonceState == USE_NONCE)
        && (journalPattern[i].sequenceNumber > maxValidSequenceNumber)) {
      maxValidSequenceNumber = journalPattern[i].sequenceNumber;
    }

    // Set all entries, valid and not, to known unique values. Ensure that
    // all PBNs fall in the data block part of a slab.
    blockEntries = 0;
    for (uint8_t j = 1; j < VDO_SECTORS_PER_BLOCK; j++) {
      struct packed_journal_sector *sector
        = vdo_get_journal_block_sector(header, j);
      const SectorPattern *sectorPattern = &blockPattern->sector[j];
      setSectorHeader(sector, header->check_byte, sectorPattern);
      for (journal_entry_count_t k = 0;
           k < RECOVERY_JOURNAL_ENTRIES_PER_SECTOR;
           k++, blockEntries++) {
        struct packed_recovery_journal_entry *entry = &sector->entries[k];
        logical_block_number_t lbn = nextLBN++;
        // Bias the offset by one so these mappings will always be distinct
        // from the ones generated by putBlocksInMap().
        physical_block_number_t pbn = computePBNFromLBN(lbn, 1);

        int entryCorruption
          = ((blockEntries == 1) ? corruption : CORRUPT_NOTHING);
        makeJournalEntry(entry, lbn, true, pbn, entryCorruption);

        // Update the mapping array only if the journal entry will be applied.
        if (blockPattern->applicable && (k < sector->entry_count)
            && ((sectorPattern->applicableEntries == APPLY_ALL)
                || (k < sectorPattern->applicableEntries))
            && (readOnly || (corruption == CORRUPT_NOTHING))
            && (entryCorruption == CORRUPT_NOTHING)) {
          setBlockMapping(lbn, pbn, VDO_MAPPING_STATE_UNCOMPRESSED);
        }
      }
    }

    VDO_ASSERT_SUCCESS(layer->writer(layer, journalStart + i, 1, block));
  }

  // Pretend that the super block was last saved long ago.
  journal->tail = 1;
}
