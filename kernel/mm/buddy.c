/*
 * Copyright (c) 2022 Institute of Parallel And Distributed Systems (IPADS)
 * ChCore-Lab is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan
 * PSL v1. You may obtain a copy of Mulan PSL v1 at:
 *     http://license.coscl.org.cn/MulanPSL
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY
 * KIND, EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE. See the
 * Mulan PSL v1 for more details.
 */

#include <common/util.h>
#include <common/macro.h>
#include <common/kprint.h>
#include <mm/buddy.h>

/*
 * The layout of a phys_mem_pool:
 * | page_metadata are (an array of struct page) | alignment pad | usable memory
 * |
 *
 * The usable memory: [pool_start_addr, pool_start_addr + pool_mem_size).
 */
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, u64 page_num)
{
        int order;
        int page_idx;
        struct page *page;

        /* Init the physical memory pool. */
        pool->pool_start_addr = start_addr;
        pool->page_metadata = start_page;
        pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;
        /* This field is for unit test only. */
        pool->pool_phys_page_num = page_num;

        /* Init the free lists */
        for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
                pool->free_lists[order].nr_free = 0;
                init_list_head(&(pool->free_lists[order].free_list));
        }

        /* Clear the page_metadata area. */
        memset((char *)start_page, 0, page_num * sizeof(struct page));

        /* Init the page_metadata area. */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                page->allocated = 1;
                page->order = 0;
                page->pool = pool;
        }

        /* Put each physical memory page into the free lists. */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                buddy_free_pages(pool, page);
        }
}

// Get the buddy chunk of the given chunk. 
// Return NULL if the buddy chunk does not belong to the same pool.
static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                    struct page *chunk)
{
        u64 chunk_addr;
        u64 buddy_chunk_addr;
        int order;

        /* Get the address of the chunk. */
        chunk_addr = (u64)page_to_virt(chunk);
        order = chunk->order;
/*
 * Calculate the address of the buddy chunk according to the address
 * relationship between buddies.
 */
#define BUDDY_PAGE_SIZE_ORDER (12)
        buddy_chunk_addr = chunk_addr
                           ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));

        /* Check whether the buddy_chunk_addr belongs to pool. */
        if ((buddy_chunk_addr < pool->pool_start_addr)
            || (buddy_chunk_addr
                >= (pool->pool_start_addr + pool->pool_mem_size))) {
                return NULL;
        }

        return virt_to_page((void *)buddy_chunk_addr);
}

// Recursively split the chunk 
// pool: the physical memory pool this chunk belongs to
// order: the target order to split to
// page: the chunk to split, which should be of order >= target order
// return val: the chunk of target order after splitting
// Assumptions:
// 1. The input chunk is not allocated, but it is not in the free list of its order.
static struct page *split_page(struct phys_mem_pool *pool, u64 order,
                               struct page *page)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Recursively put the buddy of current chunk into
         * a suitable free list.
         */
        struct page *buddy;

        BUG_ON(page == NULL);
        BUG_ON(page->order < order);
        BUG_ON(page->allocated);
        if (page->order == order) {
                return page;
        }

        /* We do not remove page->node from free list here */
        /* Otherwise, we need to add page->node to the free list after splitting, */
        /* which increase unnecessary overhead */

        /* Split: decrease order, put buddy into lower free list */
        page->order--;
        buddy = get_buddy_chunk(pool, page);
        BUG_ON(buddy == NULL);
        buddy->order = page->order;
        buddy->allocated = 0;
        list_add(&buddy->node, &pool->free_lists[buddy->order].free_list);
        pool->free_lists[buddy->order].nr_free++;

        /* Since we do not remove page->node from free list before, */
        /* we do not need to add it back after splitting, which */
        /* reduces unnecessary overhead. */

        /* Recursively split until target order */
        return split_page(pool, order, page);
        /* LAB 2 TODO 2 END */
}

// Allocate a chunk of the given order. 
// Return NULL if no such chunk is available.
// if order is invalid (>= BUDDY_MAX_ORDER || < 0), return NULL.
struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Find a chunk that satisfies the order requirement
         * in the free lists, then split it if necessary.
         */
        int cur_order;
        struct free_list *list;
        struct page *page;

        if(order >= BUDDY_MAX_ORDER || order < 0) {
                return NULL;
        }

        /* Find the smallest available order >= requested order */
        for (cur_order = order; cur_order < BUDDY_MAX_ORDER; cur_order++) {
                list = &pool->free_lists[cur_order];
                if (list->nr_free > 0) {
                        page = list_entry(list->free_list.next,
                                          struct page, node);
                        BUG_ON(page->allocated);
                        BUG_ON(page->order != cur_order);
                        /* Remove from current free list */
                        list_del(&page->node);
                        pool->free_lists[page->order].nr_free--;
                        /* Split down to the requested order */
                        page = split_page(pool, order, page);
                        BUG_ON(page->order != order);
                        /* Remove from free list and mark allocated */
                        page->allocated = 1;
                        return page;
                }
        }

        return NULL;
        /* LAB 2 TODO 2 END */
}

// Recursively merge the chunk with its buddy if possible.
// Return the merged chunk after merging.
// The caller function of this function should put the returned chunk into the free list.
// This function will not put the merged chunk into the free list.
// Assumptions:
// 1. The input chunk is not allocated, but it is not in the free list
static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Recursively merge current chunk with its buddy
         * if possible.
         */
        struct page *buddy;

        /* Cannot merge beyond max order */
        BUG_ON(page->order > BUDDY_MAX_ORDER - 1 || page->allocated);
        if (page->order == BUDDY_MAX_ORDER - 1) {
                return page;
        }

        buddy = get_buddy_chunk(pool, page);

        /* Buddy must exist, be free, and same order */
        /* It is possible for buddy to have a lower order since it might be splitted */
        if (buddy == NULL || buddy->allocated || buddy->order < page->order) {
                return page;
        }

        /* It is impossible for buddy's order to be greater than page's order */
        /* since buddy needs to merge page to form a higher order chunk */ 
        BUG_ON(buddy->order > page->order);
        /* Buddy must have the same order now */
        BUG_ON(buddy->order != page->order);

        /* Remove buddy from its free list */
        list_del(&buddy->node);
        pool->free_lists[buddy->order].nr_free--;

        /* Use the lower-addressed page as the merged chunk */
        if (buddy < page) {
                page = buddy;
        }
        page->order++;

        /* Recursively try to merge further */
        return merge_page(pool, page);
        /* LAB 2 TODO 2 END */
}

// Free the chunk and recursively merge it with its buddy if possible.
// This function assumes that the input chunk is already marked as allocated, 
// and it is not in the free list.
void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        /* LAB 2 TODO 2 BEGIN */
        /*
         * Hint: Merge the chunk with its buddy and put it into
         * a suitable free list.
         */
        /* Mark as free */
        // We assume that page is not in free list
        BUG_ON(page->allocated == 0);
        page->allocated = 0;

        /* We do not put it into the freelist since it might be removed soon later, */
        /* which reduces unnecessary overhead. */

        /* Merge with buddy as much as possible */
        page = merge_page(pool, page);

        BUG_ON(page->allocated);

        /* Insert into the appropriate free list */
        list_add(&page->node, &pool->free_lists[page->order].free_list);
        pool->free_lists[page->order].nr_free++;
        /* LAB 2 TODO 2 END */
}

void *page_to_virt(struct page *page)
{
        u64 addr;
        struct phys_mem_pool *pool = page->pool;

        BUG_ON(pool == NULL);
        /* page_idx * BUDDY_PAGE_SIZE + start_addr */
        addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE
               + pool->pool_start_addr;
        return (void *)addr;
}

struct page *virt_to_page(void *ptr)
{
        struct page *page;
        struct phys_mem_pool *pool = NULL;
        u64 addr = (u64)ptr;
        int i;

        /* Find the corresponding physical memory pool. */
        for (i = 0; i < physmem_map_num; ++i) {
                if (addr >= global_mem[i].pool_start_addr
                    && addr < global_mem[i].pool_start_addr
                                       + global_mem[i].pool_mem_size) {
                        pool = &global_mem[i];
                        break;
                }
        }

        BUG_ON(pool == NULL);
        page = pool->page_metadata
               + (((u64)addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);
        return page;
}

u64 get_free_mem_size_from_buddy(struct phys_mem_pool *pool)
{
        int order;
        struct free_list *list;
        u64 current_order_size;
        u64 total_size = 0;

        for (order = 0; order < BUDDY_MAX_ORDER; order++) {
                /* 2^order * 4K */
                current_order_size = BUDDY_PAGE_SIZE * (1 << order);
                list = pool->free_lists + order;
                total_size += list->nr_free * current_order_size;

                /* debug : print info about current order */
                kdebug("buddy memory chunk order: %d, size: 0x%lx, num: %d\n",
                       order,
                       current_order_size,
                       list->nr_free);
        }
        return total_size;
}

#ifdef CHCORE_KERNEL_TEST
#include <mm/mm.h>
#include <lab.h>
void lab2_test_buddy(void)
{
        struct phys_mem_pool *pool = &global_mem[0];
        {
                u64 free_mem_size = get_free_mem_size_from_buddy(pool);
                lab_check(pool->pool_phys_page_num == free_mem_size / PAGE_SIZE,
                          "Init buddy");
        }
        {
                lab_check(buddy_get_pages(pool, BUDDY_MAX_ORDER + 1) == NULL
                                  && buddy_get_pages(pool, (u64)-1) == NULL,
                          "Check invalid order");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *page = buddy_get_pages(pool, 0);
                BUG_ON(page == NULL);
                lab_assert(page->order == 0 && page->allocated);
                expect_free_mem -= PAGE_SIZE;
                lab_assert(get_free_mem_size_from_buddy(pool)
                           == expect_free_mem);
                buddy_free_pages(pool, page);
                expect_free_mem += PAGE_SIZE;
                lab_assert(get_free_mem_size_from_buddy(pool)
                           == expect_free_mem);
                lab_check(ok, "Allocate & free order 0");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *page;
                for (int i = 0; i < BUDDY_MAX_ORDER; i++) {
                        page = buddy_get_pages(pool, i);
                        BUG_ON(page == NULL);
                        lab_assert(page->order == i && page->allocated);
                        expect_free_mem -= (1 << i) * PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                        buddy_free_pages(pool, page);
                        expect_free_mem += (1 << i) * PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                }
                for (int i = BUDDY_MAX_ORDER - 1; i >= 0; i--) {
                        page = buddy_get_pages(pool, i);
                        BUG_ON(page == NULL);
                        lab_assert(page->order == i && page->allocated);
                        expect_free_mem -= (1 << i) * PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                        buddy_free_pages(pool, page);
                        expect_free_mem += (1 << i) * PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                }
                lab_check(ok, "Allocate & free each order");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *pages[BUDDY_MAX_ORDER];
                for (int i = 0; i < BUDDY_MAX_ORDER; i++) {
                        pages[i] = buddy_get_pages(pool, i);
                        BUG_ON(pages[i] == NULL);
                        lab_assert(pages[i]->order == i);
                        expect_free_mem -= (1 << i) * PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                }
                for (int i = 0; i < BUDDY_MAX_ORDER; i++) {
                        buddy_free_pages(pool, pages[i]);
                        expect_free_mem += (1 << i) * PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                }
                lab_check(ok, "Allocate & free all orders");
        }
        {
                bool ok = true;
                u64 expect_free_mem = pool->pool_phys_page_num * PAGE_SIZE;
                struct page *page;
                for (int i = 0; i < pool->pool_phys_page_num; i++) {
                        page = buddy_get_pages(pool, 0);
                        BUG_ON(page == NULL);
                        lab_assert(page->order == 0);
                        expect_free_mem -= PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                }
                lab_assert(get_free_mem_size_from_buddy(pool) == 0);
                lab_assert(buddy_get_pages(pool, 0) == NULL);
                for (int i = 0; i < pool->pool_phys_page_num; i++) {
                        page = pool->page_metadata + i;
                        lab_assert(page->allocated);
                        buddy_free_pages(pool, page);
                        expect_free_mem += PAGE_SIZE;
                        lab_assert(get_free_mem_size_from_buddy(pool)
                                   == expect_free_mem);
                }
                lab_assert(pool->pool_phys_page_num * PAGE_SIZE
                           == expect_free_mem);
                lab_check(ok, "Allocate & free all memory");
        }
        printk("[TEST] Buddy tests finished\n");
}
#endif /* CHCORE_KERNEL_TEST */
