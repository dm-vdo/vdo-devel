// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include "blockTestUtils.h"

#include "assertions.h"
#include "geometry.h"
#include "index.h"
#include "volume.h"

/**********************************************************************/
unsigned int getBlocksPerChapter(struct uds_index_session *session)
{
  struct index_geometry *geometry = session->index->volume->geometry;
  return geometry->records_per_chapter;
}

/**********************************************************************/
unsigned long getBlocksPerIndex(struct uds_index_session *session)
{
  struct index_geometry *geometry = session->index->volume->geometry;
  return geometry->records_per_volume;
}

/**********************************************************************/
unsigned int getChaptersPerIndex(struct uds_index_session *session)
{
  struct index_geometry *geometry = session->index->volume->geometry;
  return geometry->chapters_per_volume;
}

/**********************************************************************/
bool isIndexSparse(struct uds_index_session *session)
{
  return session->parameters.sparse;
}
