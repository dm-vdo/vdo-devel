# This is a script to make gdb set up a breakpoint in vdoDebugMetadata, run
# vdoDebugMetadata, and then print help text.

break vdoDebugMetadata.c:doNothing
run
printf "%s", helpString

define getSlab
  set $pbnOffsetWithinPartition = $arg0 - vdo->depot->firstBlock 
  set $slabSize       = (1 << vdo->depot->slabSizeShift)
  set $slabNumber     = $pbnOffsetWithinPartition / $slabSize
  set $slabOffset     = $pbnOffsetWithinPartition % $slabSize
  set $slab           = slabs[$slabNumber]
end

document getSlab
  For a given PBN, sets up variables about the slab it belongs to.
  Sets the variables $pbnOffsetWithinPartition, $slabSize, $slabNumber,
  $slabOffset, and $slab.
end

define getRefCountForPBN
  set $pbn            = $arg0
  getSlab $pbn
  set $refBlockNumber = $slabOffset / COUNTS_PER_BLOCK
  set $refBlockOffset = $slabOffset % COUNTS_PER_BLOCK
  set $refBlock       = $slab.referenceBlocks[$refBlockNumber]
  set $sectorNumber   = $refBlockOffset / COUNTS_PER_SECTOR
  set $sectorOffset   = $refBlockOffset % COUNTS_PER_SECTOR
  set $sector         = $refBlock->sectors[$sectorNumber]
  set $refCount       = $sector.counts[$sectorOffset]
  print $refCount
end

document getRefCountForPBN
  Prints the reference count recorded for a given PBN.
  Calls getSlab, and also sets the variables:
    $pbn;
    $refBlockNumber, $refBlockOffset, $refBlock;
    $sectorNumber, $sectorOffset, $sector;
    $refCount.
end

define searchSlabJournalForPBN
  getSlab $arg0
  set $i = 0
  while $i < vdo->depot->slabConfig.slabJournalBlocks
    set $j = 0
    set $sjb = $slab->slabJournalBlocks[$i]
    while $j < $sjb->entryCount
      if ($sjb->entries[$j].offset == $slabOffset)
        printf "Found at %u %u: ", $i, $j
        print $sjb->entries[$j]
      end
      set $j = $j + 1
    end
    set $i = $i + 1
  end
end

document searchSlabJournalForPBN
  Searches the slab journal where the given PBN might appear for entries
  relating to that PBN. Calls getSlab, and uses the iteration variables
  $i, j, $sjb. This function is fairly slow.
end

define unpackJournalPoint
  printf "%u %u\n", ($arg0 >> 16), ($arg0 & 0xffff)
