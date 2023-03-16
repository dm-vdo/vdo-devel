/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef VDO_HEAP_H
#define VDO_HEAP_H

#include <linux/types.h>

/**
 * typedef heap_comparator - Prototype for functions which compare two array elements.
 * @item1: The first element to compare.
 * @item2: The second element to compare.
 *
 * All the time complexity claims in this module assume this operation has O(1) time complexity.
 *
 * Return: An integer which is less than, equal to, or greater than 0 depending on whether item1 is
 *         less than, equal to, or greater than item2, respectively
 */
typedef int heap_comparator(const void *item1, const void *item2);

/**
 * typedef heap_swapper - Prototype for functions which swap two array elements.
 * @item1: The first element to swap.
 * @item2: The second element to swap.
 */
typedef void heap_swapper(void *item1, void *item2);

/*
 * A heap array can be any array of fixed-length elements in which the heap invariant can be
 * established. In a max-heap, every child of a node must be at least as large as its children.
 * Once that invariant is established in an array by calling vdo_build_heap(), all the other heap
 * operations may be used on that array.
 */
struct heap {
	/* the 1-based array of heap elements (nodes) */
	u8 *array;
	/* the function to use to compare two elements */
	heap_comparator *comparator;
	/* the function to use to swap two elements */
	heap_swapper *swapper;
	/* the maximum number of elements that can be stored */
	size_t capacity;
	/* the size of every element (in bytes) */
	size_t element_size;
	/* the current number of elements in the heap */
	size_t count;
};

void vdo_initialize_heap(struct heap *heap,
			 heap_comparator *comparator,
			 heap_swapper *swapper,
			 void *array,
			 size_t capacity,
			 size_t element_size);

void vdo_build_heap(struct heap *heap, size_t count);

/**
 * vdo_is_heap_empty() - Check whether the heap is currently empty.
 * @heap: The heap to query.
 *
 * Return: true if there are no elements in the heap.
 */
static inline bool vdo_is_heap_empty(const struct heap *heap)
{
	return (heap->count == 0);
}

bool vdo_pop_max_heap_element(struct heap *heap, void *element_ptr);

size_t vdo_sort_heap(struct heap *heap);

void *vdo_sort_next_heap_element(struct heap *heap);

#endif /* VDO_HEAP_H */
