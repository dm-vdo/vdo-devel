/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Unit test implementations of linux/highmem.h (and mm.h)
 *
 * Copyright 2023 Red Hat
 *
 */

#ifndef __LINUX_HIGHMEM_H
#define __LINUX_HIGHMEM_H

#include "permassert.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE  (((unsigned long) 1) << PAGE_SHIFT)
#define PAGE_MASK  (~(PAGE_SIZE - 1))

struct page {
  char page_data[PAGE_SIZE];
} __packed;

#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)
#define is_vmalloc_addr(x) (true)
#define virt_to_page(addr) vmalloc_to_page(addr)

/**********************************************************************/
static inline struct page *vmalloc_to_page(void *addr)
{
  return addr;
}

/**********************************************************************/
static inline void memcpy_to_page(struct page *page,
                                  size_t offset,
                                  const char *from,
                                  size_t len)
{
  ASSERT_LOG_ONLY(((offset + len) <= PAGE_SIZE), "page overflow");
  memcpy(page->page_data + offset, from, len);
}

static inline void memcpy_from_page(char *to,
                                    struct page *page,
				    size_t offset,
                                    size_t len)
{
  ASSERT_LOG_ONLY(((offset + len) <= PAGE_SIZE), "page overflow");
  memcpy(to, page->page_data + offset, len);
}

#endif // __LINUX_HIGHMEM_H
