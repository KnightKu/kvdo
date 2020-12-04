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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/journalPoint.h#6 $
 */

#ifndef JOURNAL_POINT_H
#define JOURNAL_POINT_H

#include "numeric.h"
#include "types.h"

typedef uint16_t JournalEntryCount;

/**
 * The absolute position of an entry in a recovery journal or slab journal.
 **/
struct journal_point {
	sequence_number_t sequence_number;
	JournalEntryCount entry_count;
};

/**
 * A packed, platform-independent encoding of a struct journal_point.
 **/
struct packed_journal_point {
	/**
	 * The packed representation is the little-endian 64-bit representation
	 * of the low-order 48 bits of the sequence number, shifted up 16 bits,
	 * or'ed with the 16-bit entry count.
	 *
	 * Very long-term, the top 16 bits of the sequence number may not always
	 * be zero, as this encoding assumes--see BZ 1523240.
	 **/
	byte encoded_point[8];
} __packed;

/**
 * Move the given journal point forward by one entry.
 *
 * @param point              the journal point to adjust
 * @param entries_per_block  the number of entries in one full block
 **/
static inline void advance_journal_point(struct journal_point *point,
					 JournalEntryCount entries_per_block)
{
	point->entry_count++;
	if (point->entry_count == entries_per_block) {
		point->sequence_number++;
		point->entry_count = 0;
	}
}

/**
 * Check whether a journal point is valid.
 *
 * @param point  the journal point
 *
 * @return <code>true</code> if the journal point is valid
 **/
static inline bool is_valid_journal_point(const struct journal_point *point)
{
	return ((point != NULL) && (point->sequence_number > 0));
}

/**
 * Check whether the first point precedes the second point.
 *
 * @param first   the first journal point
 * @param second  the second journal point

 *
 * @return <code>true</code> if the first point precedes the second point.
 **/
static inline bool before_journal_point(const struct journal_point *first,
					const struct journal_point *second)
{
	return ((first->sequence_number < second->sequence_number) ||
		((first->sequence_number == second->sequence_number) &&
		 (first->entry_count < second->entry_count)));
}

/**
 * Check whether the first point is the same as the second point.
 *
 * @param first   the first journal point
 * @param second  the second journal point
 *
 * @return <code>true</code> if both points reference the same logical
 *         position of an entry the journal
 **/
static inline bool
are_equivalent_journal_points(const struct journal_point *first,
			      const struct journal_point *second)
{
	return ((first->sequence_number == second->sequence_number) &&
		(first->entry_count == second->entry_count));
}

/**
 * Encode the journal location represented by a journal_point into a
 * packed_journal_point.
 *
 * @param unpacked  The unpacked input point
 * @param packed    The packed output point
 **/
static inline void pack_journal_point(const struct journal_point *unpacked,
				      struct packed_journal_point *packed)
{
	uint64_t native =
		((unpacked->sequence_number << 16) | unpacked->entry_count);
	put_unaligned_le64(native, packed->encoded_point);
}

/**
 * Decode the journal location represented by a packed_journal_point into a
 * journal_point.
 *
 * @param packed    The packed input point
 * @param unpacked  The unpacked output point
 **/
static inline void
unpack_journal_point(const struct packed_journal_point *packed,
		     struct journal_point *unpacked)
{
	uint64_t native = get_unaligned_le64(packed->encoded_point);
	unpacked->sequence_number = (native >> 16);
	unpacked->entry_count = (native & 0xffff);
}

#endif // JOURNAL_POINT_H
