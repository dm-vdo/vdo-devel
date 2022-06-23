/*
 * Prunable Segment Tree
 *
 * Copyright (c) 2021 Red Hat
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
 */

#ifndef PST_H
#define PST_H

#include <inttypes.h>
#include <sys/types.h>

/*
 * Given a sequence of n contiguous segments of arbitrary lengths (forming a
 * search space equal to the sum of those lengths), this structure provides a
 * means to:
 * - look up in which segment a particular offset falls, in O(log(n));
 * - prune a segment from the search, effectively reducing the search space by
 *   its length and adjusting all the offsets of the segments following it, in
 *   O(log(n));
 * - given a segment reference, derive its absolute offset in the current
 *   search space (accounting for removed segments) in O(log(n)).
 *
 * The structure is created as a binary search tree, initially balanced with
 * respect to the relative lengths of the segments. Specifically, each tree
 * node has the initial property that the middle of the range spanned by its
 * subtree falls within the segment represented by the node itself.
 *
 * The tree is not dynamically balanced (and as segments are pruned, the above
 * property is lost). Thus, lookup, pruning and offset derivation continue to
 * take O(log(n)) where n is the initial number of segments, regardless of how
 * many segments are pruned.
 *
 * Each tree node stores a 'nodeOffset' value, which is the offset from the
 * segment end of the first left ancestor of that node (in other words, a
 * node's right child has a nodeOffset relative to the node's own nodeOffset,
 * whereas its left child has an offset relative to the first left ancestor of
 * the node, just like the node's own nodeOffset). The current absolute offset
 * of the node in the search space is derived as the sum of the node's
 * nodeOffset and all its left ancestors' nodeOffsets. Pruning a segment in the
 * tree consists of walking up the tree and subtracting the node's length from
 * all the node's right ancestors' nodeOffsets, and setting the node's length
 * to zero.
 */

typedef struct pstNode PstNode;
struct pstNode {
  /** offset relative to the first left ancestor */
  uint64_t   nodeOffset;
  /** length of this node's segment */
  uint64_t   length;
  /** left child (NULL if none) */
  PstNode   *left;
  /** right child (NULL if none) */
  PstNode   *right;
  /** parent node (NULL for the root node) */
  PstNode   *parent;
  /** pointer to the parent's pointer to this node (NULL for the root node) */
  PstNode  **selfFromParent;
  /** user data associated with this segment */
  void      *payload;
};

/**
 * Build the tree.
 *
 * @nodeArray  array of PstNode instances, with non-zero node lengths and
 *             payloads set, and all other pointers set to NULL
 * @size       size of the array
 *
 * @return     a pointer to the root node of the tree.
 **/
PstNode *pstConstruct(PstNode *nodeArray, size_t size);

/**
 * Find the segment that contains a given offset
 *
 * @tree       root of the tree to search
 * @offset     offset to search for. MUST be within the tree's range.
 *
 * @return     a pointer to the node representing the segment
 **/
PstNode *pstSearch(PstNode *tree, uint64_t offset);

/**
 * Derive the (current) absolute offset of a segment
 *
 * @node       tree node representing the segment
 *
 * @return     the segment's absolute offset
 **/
uint64_t pstGetOffset(PstNode *node);

/**
 * Prune a segment from the search
 *
 * @node       tree node representing the segment
 **/
void pstPrune(PstNode *node);

#endif /* PST_H */
