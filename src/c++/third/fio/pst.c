/*
 * Prunable Segment Tree implementation
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

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

#include "pst.h"

/**********************************************************************/
// Assumes .nodeOffset contains the absolute offset.
static size_t findMiddle(PstNode *nodeArray,
                         size_t   startIndex,
                         size_t   endIndex)
{
  uint64_t target = (nodeArray[endIndex].nodeOffset
                     + nodeArray[endIndex].length
                     + nodeArray[startIndex].nodeOffset) / 2;
  size_t min      = startIndex;
  size_t max      = endIndex;

  while (true) {
    size_t mid = (min + max) / 2; // could overflow, but not likely
    if (target < nodeArray[mid].nodeOffset) {
      max = mid - 1;
    } else if (target >= (nodeArray[mid].nodeOffset
                          + nodeArray[mid].length)) {
      min = mid + 1;
    } else {
      return mid;
    }
  }
}

/**********************************************************************/
// Assumes .nodeOffset's of the current working set contain the absolute offset
// of each node.
static PstNode *setupRoot(PstNode *nodeArray,
                          size_t   startIndex,
                          size_t   endIndex,
                          uint64_t leftAncestorSegmentEnd)
{
  PstNode *root = NULL;
  size_t rootIndex;

  if (startIndex == endIndex) {
    nodeArray[startIndex].nodeOffset = 0;
    return &nodeArray[startIndex];
  }

  // do a binary search for the "middle"
  rootIndex = findMiddle(nodeArray, startIndex, endIndex);
  root = &nodeArray[rootIndex];
  // make the absolute offset into a relative offset
  root->nodeOffset -= leftAncestorSegmentEnd;
  if (rootIndex > startIndex) {
    // setup left subtree
    root->left = setupRoot(nodeArray, startIndex, rootIndex - 1,
                           leftAncestorSegmentEnd);
    root->left->parent         = root;
    root->left->selfFromParent = &root->left;
  }
  if (rootIndex < endIndex) {
    // setup right subtree
    root->right = setupRoot(nodeArray, rootIndex + 1, endIndex,
                            leftAncestorSegmentEnd
                            + root->nodeOffset + root->length);
    root->right->parent         = root;
    root->right->selfFromParent = &root->right;
  }
  return root;
}

/**********************************************************************/
// Assumes children are initialized to NULL for all array elements
// returns the root.
PstNode *pstConstruct(PstNode *nodeArray, size_t size)
{
  assert(size >= 1);

  // Temporarily set the nodeOffsets to the absolute offset of each node.
  assert(nodeArray[0].length > 0);
  nodeArray[0].nodeOffset = 0;
  for (size_t i = 1; i < size; i++) {
    assert(nodeArray[i].length > 0);
    nodeArray[i].nodeOffset =
      nodeArray[i - 1].nodeOffset + nodeArray[i - 1].length;
  }

  return (size == 1) ? nodeArray : setupRoot(nodeArray, 0, size - 1, 0);
}

/**********************************************************************/
PstNode *pstSearch(PstNode *tree, uint64_t offset)
{
  PstNode  *iTree               = tree;
  uint64_t  subtreeSearchOffset = offset;

  while (true) {
    if (subtreeSearchOffset < iTree->nodeOffset) {
      assert(iTree->left != NULL);
      iTree = iTree->left;
    } else {
      uint64_t iTreeNodeEndOffset = iTree->nodeOffset + iTree->length;
      if (subtreeSearchOffset < iTreeNodeEndOffset) {
        return iTree;
      } else {
        subtreeSearchOffset -= iTreeNodeEndOffset;
        assert(iTree->right != NULL);
        iTree = iTree->right;
      }
    }
  }
}

/**********************************************************************/
uint64_t pstGetOffset(PstNode *node)
{
  uint64_t offset = node->nodeOffset;

  while (node->parent != NULL) {
    if (node->selfFromParent == &(node->parent->right)) {
      offset += node->parent->nodeOffset + node->parent->length;
    }
    node = node->parent;
  }

  return offset;
}

/**********************************************************************/
void pstPrune(PstNode *node)
{
  for (PstNode *iNode = node; iNode->parent != NULL; iNode = iNode->parent) {
    if (iNode->selfFromParent == &(iNode->parent->left)) {
      iNode->parent->nodeOffset -= node->length;
    }
  }
  node->length = 0;
}
