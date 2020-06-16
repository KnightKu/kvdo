/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/krusty/src/uds/openChapterZone.c#8 $
 */

#include "openChapterZone.h"

#include "compiler.h"
#include "hashUtils.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

/**********************************************************************/
static INLINE size_t recordsSize(const struct open_chapter_zone *openChapter)
{
  return (sizeof(struct uds_chunk_record) * (1 + openChapter->capacity));
}

/**********************************************************************/
static INLINE size_t slotsSize(size_t slotCount)
{
  return (sizeof(struct open_chapter_zone_slot) * slotCount);
}

/**
 * Round up to the first power of two greater than or equal
 * to the supplied number.
 *
 * @param val  the number to round up
 *
 * @return the first power of two not smaller than val for any
 *         val <= 2^63
 **/
static INLINE size_t nextPowerOfTwo(size_t val)
{
  if (val == 0) {
    return 1;
  }
  return (1 << compute_bits(val - 1));
}

/**********************************************************************/
int makeOpenChapter(const struct geometry     *geometry,
                    unsigned int               zoneCount,
                    struct open_chapter_zone **openChapterPtr)
{
  int result = ASSERT(zoneCount > 0, "zone count must be > 0");
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_WITH_ERROR_CODE(geometry->open_chapter_load_ratio > 1,
                                  UDS_BAD_STATE,
                                  "Open chapter hash table is too small");
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_WITH_ERROR_CODE((geometry->records_per_chapter
                                   <= OPEN_CHAPTER_MAX_RECORD_NUMBER),
                                  UDS_BAD_STATE,
                                  "Too many records (%u) for a single chapter",
                                  geometry->records_per_chapter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (geometry->records_per_chapter < zoneCount) {
    return logUnrecoverable(
      UDS_INVALID_ARGUMENT,
      "zone count: %u is larger than the records per chapter %u",
      zoneCount, geometry->records_per_chapter);
  }
  size_t capacity = geometry->records_per_chapter / zoneCount;

  // The slot count must be at least one greater than the capacity.
  // Using a power of two slot count guarantees that hash insertion
  // will never fail if the hash table is not full.
  size_t slotCount
    = nextPowerOfTwo(capacity * geometry->open_chapter_load_ratio);
  struct open_chapter_zone *openChapter;
  result = ALLOCATE_EXTENDED(struct open_chapter_zone, slotCount,
                             struct open_chapter_zone_slot,
                             "open chapter", &openChapter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  openChapter->slotCount = slotCount;
  openChapter->capacity = capacity;
  result = allocateCacheAligned(recordsSize(openChapter), "record pages",
                                &openChapter->records);
  if (result != UDS_SUCCESS) {
    freeOpenChapter(openChapter);
    return result;
  }

  *openChapterPtr = openChapter;
  return UDS_SUCCESS;
}

/**********************************************************************/
size_t openChapterSize(const struct open_chapter_zone *openChapter)
{
  return openChapter->size - openChapter->deleted;
}

/**********************************************************************/
void resetOpenChapter(struct open_chapter_zone *openChapter)
{
  openChapter->size    = 0;
  openChapter->deleted = 0;

  memset(openChapter->records, 0, recordsSize(openChapter));
  memset(openChapter->slots,   0, slotsSize(openChapter->slotCount));
}

/**********************************************************************/
static struct uds_chunk_record *
probeChapterSlots(struct open_chapter_zone    *openChapter,
                  const struct uds_chunk_name *name,
                  unsigned int                *slotPtr,
                  unsigned int                *recordNumberPtr)
{
  unsigned int slots     = openChapter->slotCount;
  unsigned int probe     = name_to_hash_slot(name, slots);
  unsigned int firstSlot = 0;

  struct uds_chunk_record *record;
  unsigned int probeSlot;
  unsigned int recordNumber;
  unsigned int probeAttempts;

  for (probeAttempts = 1; ; ++probeAttempts) {
    probeSlot = firstSlot + probe;
    recordNumber = openChapter->slots[probeSlot].recordNumber;

    // If the hash slot is empty, we've reached the end of a chain without
    // finding the record and should terminate the search.
    if (recordNumber == 0) {
      record = NULL;
      break;
    }

    // If the name of the record referenced by the slot matches and has not
    // been deleted, then we've found the requested name.
    record = &openChapter->records[recordNumber];
    if ((memcmp(&record->name, name, UDS_CHUNK_NAME_SIZE) == 0)
        && !openChapter->slots[recordNumber].recordDeleted) {
      break;
    }

    // Quadratic probing: advance the probe by 1, 2, 3, etc. and try again.
    // This performs better than linear probing and works best for 2^N slots.
    probe += probeAttempts;
    if (probe >= slots) {
      probe = probe % slots;
    }
  }

  // These NULL checks will be optimized away in callers who don't care about
  // the values when this function is inlined.
  if (slotPtr != NULL) {
    *slotPtr = probeSlot;
  }
  if (recordNumberPtr != NULL) {
    *recordNumberPtr = recordNumber;
  }

  return record;
}

/**********************************************************************/
void searchOpenChapter(struct open_chapter_zone     *openChapter,
                       const struct uds_chunk_name  *name,
                       struct uds_chunk_data        *metadata,
                       bool                         *found)
{
  struct uds_chunk_record *record
    = probeChapterSlots(openChapter, name, NULL, NULL);

  if (record == NULL) {
    *found = false;
  } else {
    *found = true;
    if (metadata != NULL) {
      *metadata = record->data;
    }
  }
}

/**********************************************************************/
int putOpenChapter(struct open_chapter_zone    *openChapter,
                   const struct uds_chunk_name *name,
                   const struct uds_chunk_data *metadata,
                   unsigned int                *remaining)
{
  unsigned int slot;
  struct uds_chunk_record *record
    = probeChapterSlots(openChapter, name, &slot, NULL);

  if (record != NULL) {
    record->data = *metadata;
    *remaining = openChapter->capacity - openChapter->size;
    return UDS_SUCCESS;
  }

  if (openChapter->size >= openChapter->capacity) {
    return makeUnrecoverable(UDS_VOLUME_OVERFLOW);
  }

  unsigned int recordNumber = ++openChapter->size;
  openChapter->slots[slot].recordNumber = recordNumber;
  record                                = &openChapter->records[recordNumber];
  record->name                          = *name;
  record->data                          = *metadata;

  *remaining = openChapter->capacity - openChapter->size;
  return UDS_SUCCESS;
}

/**********************************************************************/
void removeFromOpenChapter(struct open_chapter_zone    *openChapter,
                           const struct uds_chunk_name *name,
                           bool                        *removed)
{
  unsigned int recordNumber;
  struct uds_chunk_record *record
    = probeChapterSlots(openChapter, name, NULL, &recordNumber);

  if (record == NULL) {
    *removed = false;
    return;
  }

  // Set the deleted flag on the recordNumber in the slot array so search
  // won't find it and close won't index it.
  openChapter->slots[recordNumber].recordDeleted = true;
  openChapter->deleted += 1;
  *removed = true;
}

/**********************************************************************/
void freeOpenChapter(struct open_chapter_zone *openChapter)
{
  if (openChapter != NULL) {
    FREE(openChapter->records);
    FREE(openChapter);
  }
}
