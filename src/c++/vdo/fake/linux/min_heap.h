/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MIN_HEAP_H
#define _LINUX_MIN_HEAP_H

#include <linux/string.h>
#include <linux/types.h>

#undef VDO_USE_NEXT
#if defined(RHEL_RELEASE_CODE)
#if defined(LINUX_VERSION_CODE) && (LINUX_VERSION_CODE > KERNEL_VERSION(6, 10, 0))
#define VDO_USE_NEXT
#endif
#if (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(9, 5)) && defined(RHEL_MINOR) && (RHEL_MINOR < 50)
#define VDO_USE_NEXT
#endif
#else /* RHEL_RELEASE_CODE */
#if (LINUX_VERSION_CODE > KERNEL_VERSION(6, 10, 0))
#define VDO_USE_NEXT
#endif
#endif /* RHEL_RELEASE_CODE */
#ifndef VDO_USE_NEXT
/**
 * struct min_heap - Data structure to hold a min-heap.
 * @data: Start of array holding the heap elements.
 * @nr: Number of elements currently in the heap.
 * @size: Maximum number of elements that can be held in current storage.
 */
struct min_heap {
	void *data;
	int nr;
	int size;
};
#else
/**
 * Data structure to hold a min-heap.
 * @nr: Number of elements currently in the heap.
 * @size: Maximum number of elements that can be held in current storage.
 * @data: Pointer to the start of array holding the heap elements.
 * @preallocated: Start of the static preallocated array holding the heap elements.
 */
#define MIN_HEAP_PREALLOCATED(_type, _name, _nr)	\
struct _name {	\
	int nr;	\
	int size;	\
	_type *data;	\
	_type preallocated[_nr];	\
}

#define DEFINE_MIN_HEAP(_type, _name) MIN_HEAP_PREALLOCATED(_type, _name, 0)

typedef DEFINE_MIN_HEAP(char, min_heap_char) min_heap_char;

#define __minheap_cast(_heap)		(typeof((_heap)->data[0]) *)
#define __minheap_obj_size(_heap)	sizeof((_heap)->data[0])
#endif

/**
 * struct min_heap_callbacks - Data/functions to customise the min_heap.
 * @elem_size: The nr of each element in bytes.
 * @less: Partial order function for this heap.
 * @swp: Swap elements function.
 */
struct min_heap_callbacks {
#ifndef VDO_USE_NEXT
	int elem_size;
	bool (*less)(const void *lhs, const void *rhs);
	void (*swp)(void *lhs, void *rhs);
#else /* VDO_USE_NEXT */
	bool (*less)(const void *lhs, const void *rhs, void *args);
	void (*swp)(void *lhs, void *rhs, void *args);
#endif
};

#ifndef VDO_USE_NEXT
/* Sift the element at pos down the heap. */
static __always_inline
void min_heapify(struct min_heap *heap, int pos,
		const struct min_heap_callbacks *func)
{
	void *left, *right, *parent, *smallest;
	void *data = heap->data;

	for (;;) {
		if (pos * 2 + 1 >= heap->nr)
			break;

		left = data + ((pos * 2 + 1) * func->elem_size);
		parent = data + (pos * func->elem_size);
		smallest = parent;
		if (func->less(left, smallest))
			smallest = left;

		if (pos * 2 + 2 < heap->nr) {
			right = data + ((pos * 2 + 2) * func->elem_size);
			if (func->less(right, smallest))
				smallest = right;
		}
		if (smallest == parent)
			break;
		func->swp(smallest, parent);
		if (smallest == left)
			pos = (pos * 2) + 1;
		else
			pos = (pos * 2) + 2;
	}
}

/* Floyd's approach to heapification that is O(nr). */
static __always_inline
void min_heapify_all(struct min_heap *heap,
		const struct min_heap_callbacks *func)
{
	int i;

	for (i = heap->nr / 2; i >= 0; i--)
		min_heapify(heap, i, func);
}

/* Remove minimum element from the heap, O(log2(nr)). */
static __always_inline
void min_heap_pop(struct min_heap *heap,
		const struct min_heap_callbacks *func)
{
	void *data = heap->data;

	if (WARN_ONCE(heap->nr <= 0, "Popping an empty heap"))
		return;

	/* Place last element at the root (position 0) and then sift down. */
	heap->nr--;
	memcpy(data, data + (heap->nr * func->elem_size), func->elem_size);
	min_heapify(heap, 0, func);
}

/*
 * Remove the minimum element and then push the given element. The
 * implementation performs 1 sift (O(log2(nr))) and is therefore more
 * efficient than a pop followed by a push that does 2.
 */
static __always_inline
void min_heap_pop_push(struct min_heap *heap,
		const void *element,
		const struct min_heap_callbacks *func)
{
	memcpy(heap->data, element, func->elem_size);
	min_heapify(heap, 0, func);
}

/* Push an element on to the heap, O(log2(nr)). */
static __always_inline
void min_heap_push(struct min_heap *heap, const void *element,
		const struct min_heap_callbacks *func)
{
	void *data = heap->data;
	void *child, *parent;
	int pos;

	if (WARN_ONCE(heap->nr >= heap->size, "Pushing on a full heap"))
		return;

	/* Place at the end of data. */
	pos = heap->nr;
	memcpy(data + (pos * func->elem_size), element, func->elem_size);
	heap->nr++;

	/* Sift child at pos up. */
	for (; pos > 0; pos = (pos - 1) / 2) {
		child = data + (pos * func->elem_size);
		parent = data + ((pos - 1) / 2) * func->elem_size;
		if (func->less(parent, child))
			break;
		func->swp(parent, child);
	}
}
#else /* VDO_USE_NEXT */
/* Sift the element at pos down the heap. */
static __always_inline
void __min_heap_sift_down(min_heap_char *heap, int pos, size_t elem_size,
		const struct min_heap_callbacks *func, void *args)
{
	void *left, *right;
	void *data = heap->data;
	void *root = data + pos * elem_size;
	int i = pos, j;

	/* Find the sift-down path all the way to the leaves. */
	for (;;) {
		if (i * 2 + 2 >= heap->nr)
			break;
		left = data + (i * 2 + 1) * elem_size;
		right = data + (i * 2 + 2) * elem_size;
		i = func->less(left, right, args) ? i * 2 + 1 : i * 2 + 2;
	}

	/* Special case for the last leaf with no sibling. */
	if (i * 2 + 2 == heap->nr)
		i = i * 2 + 1;

	/* Backtrack to the correct location. */
	while (i != pos && func->less(root, data + i * elem_size, args))
		i = (i - 1) / 2;

	/* Shift the element into its correct place. */
	j = i;
	while (i != pos) {
		i = (i - 1) / 2;
		func->swp(data + i * elem_size, data + j * elem_size, args);
	}
}

#define min_heap_sift_down(_heap, _pos, _func, _args)	\
	__min_heap_sift_down((min_heap_char *)_heap, _pos, __minheap_obj_size(_heap), _func, _args)

/* Floyd's approach to heapification that is O(nr). */
static __always_inline
void __min_heapify_all(min_heap_char *heap, size_t elem_size,
		const struct min_heap_callbacks *func, void *args)
{
	int i;

	for (i = heap->nr / 2 - 1; i >= 0; i--)
		__min_heap_sift_down(heap, i, elem_size, func, args);
}

#define min_heapify_all(_heap, _func, _args)	\
	__min_heapify_all((min_heap_char *)_heap, __minheap_obj_size(_heap), _func, _args)

/* Remove minimum element from the heap, O(log2(nr)). */
static __always_inline
bool __min_heap_pop(min_heap_char *heap, size_t elem_size,
		const struct min_heap_callbacks *func, void *args)
{
	void *data = heap->data;

	if (WARN_ONCE(heap->nr <= 0, "Popping an empty heap"))
		return false;

	/* Place last element at the root (position 0) and then sift down. */
	heap->nr--;
	memcpy(data, data + (heap->nr * elem_size), elem_size);
	__min_heap_sift_down(heap, 0, elem_size, func, args);

	return true;
}

#define min_heap_pop(_heap, _func, _args)	\
	__min_heap_pop((min_heap_char *)_heap, __minheap_obj_size(_heap), _func, _args)
#endif /* VDO_USE_NEXT */

#endif /* _LINUX_MIN_HEAP_H */
