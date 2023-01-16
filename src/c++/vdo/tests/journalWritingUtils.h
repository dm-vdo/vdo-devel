/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 *
 * $Id$
 */

#ifndef JOURNAL_WRITING_UTILS_H
#define JOURNAL_WRITING_UTILS_H

#include "types.h"
#include "vdo-component-states.h"

enum {
  FULL_BLOCK     = -1, // Value indicating a full block of entries
  SHORT_BLOCK    = 99, // Number of journal entries in a partially filled block
  FULL_SECTOR    = -1, // Value indicating a full sector of entries
  EMPTY_SECTOR   =  0, // Number of journal entries in an empty sector
  SHORT_SECTOR   =  7, // Number of journal entries in a partial sector
  LAST_SECTOR    = 35, // Number of journal entries in the last full sector
  APPLY_ALL      = -1, // All sector entries should be applied
  APPLY_NONE     = EMPTY_SECTOR, // No sector entries should be applied
  APPLY_PART     = SHORT_SECTOR, // Only some sector entries should be applied
  USE_NONCE      = -1, // Value indicating the VDO nonce should be used
  BAD_NONCE      = 0x01,
  GOOD_COUNT     = 0,
  BAD_COUNT      = 0xff,
};

/** Possible types of corruption */
typedef enum {
  CORRUPT_NOTHING,
  CORRUPT_LBN_PBN,
  CORRUPT_LBN_SLOT,
  CORRUPT_PBN,
} CorruptionType;

/** Possible types of torn writes */
typedef enum {
  NO_TEAR,
  TEAR_OLD,
  TEAR_NEW,
} TearType;

/** A pattern to describe a recovery journal sector */
typedef struct {
  TearType tearType;
  int      entryCount;
  uint8_t  recoveryCount;
  int      applicableEntries;
} SectorPattern;

/** A pattern to describe a recovery journal block */
typedef struct {
  sequence_number_t     head;
  sequence_number_t     sequenceNumber;
  uint8_t               recoveryCount;
  int                   nonceState;
  int                   blockLength;
  bool                  applicable;
  const SectorPattern  *sector;
} BlockPattern;

/**
 * Initialize journal writing utilities.
 *
 * @param journalBlocks  The number of journal blocks; block patterns
 *                       are expected to be this long.
 * @param logicalBlocks  The number of logical blocks in the VDO
 * @param slabs          The number of slabs to reference in journal entries
 **/
void initializeJournalWritingUtils(block_count_t journalBlocks,
                                   block_count_t logicalBlocks,
                                   slab_count_t  slabs);

/**
 * Free up resources allocated in initializeJournalWritingUtils().
 **/
void tearDownJournalWritingUtils(void);

/**
 * Compute an arbitrary (but deterministic) PBN to which to map a given LBN.
 *
 * @param lbn     The lbn to map
 * @param offset  The offset from the default PBN value for the given LBN
 *
 * @return The PBN
 **/
physical_block_number_t computePBNFromLBN(logical_block_number_t lbn,
                                          block_count_t          offset);

/**
 * Make an entry in a journal block.
 *
 * @param entry       A pointer to the entry to encode
 * @param lbn         The lbn to make an entry for
 * @param isIncRef    Whether the entry is an incref
 * @param pbn         The pbn to make an entry for
 * @param corruption  The type of corruption to include
 **/
void makeJournalEntry(struct packed_recovery_journal_entry *entry,
                      logical_block_number_t                lbn,
                      bool                                  isIncRef,
                      physical_block_number_t               pbn,
                      CorruptionType                        corruption);

/**
 * Fill the block map with patterned data so the test can determine
 * if a mapping has been overwritten. The data pattern must ensure
 * that each mapped PBN is actually a data block since some test
 * scenarios try to reference count the mapped blocks.
 *
 * @param start  The lbn of the first entry
 * @param count  The number of entries to make
 **/
void putBlocksInMap(logical_block_number_t start, block_count_t count);

/**
 * Set the headers of a journal block according to a pattern.
 *
 * @param header        The header to modify
 * @param blockPattern  The pattern to model the header after
 */
void setBlockHeader(struct packed_journal_header *header,
                    BlockPattern                 *blockPattern);

/**
 * Set journal sector headers according to a pattern.
 *
 * @param sector         The sector to modify
 * @param checkByte      The check byte for the block
 * @param sectorPattern  The pattern to model the sector headers after
 */
void setSectorHeader(struct packed_journal_sector *sector,
                     uint8_t                       checkByte,
                     const SectorPattern          *sectorPattern);

/**
 * Create journal blocks that represent the given journal pattern
 * and write then to the layer.
 *
 * @param  corruption      the type of corruption to apply to journal entries
 * @param  readOnly        whether this as a rebuild out of read-only mode
 * @param  journalPattern  the journal block pattern
 **/
void writeJournalBlocks(CorruptionType  corruption,
                        bool            readOnly,
                        BlockPattern   *journalPattern);

#endif /* JOURNAL_WRITING_UTILS_H */
