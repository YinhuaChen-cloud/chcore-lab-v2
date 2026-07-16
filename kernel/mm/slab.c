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

#include <common/macro.h>
#include <common/types.h>
#include <common/kprint.h>
#include <mm/kmalloc.h>
#include <mm/buddy.h>
#include <mm/slab.h>

/* local variables */
slab_header_t *slabs[SLAB_MAX_ORDER + 1] = {NULL};

/* local functions */
/*
 * 函数意图：把给定大小向上转换为 2 的幂次 order，使 2^order 能容纳 size。
 * 参数：size 表示需要转换的字节数或数量。
 * 返回值：返回最小的 order，满足 2^order >= size。
 */
static inline u64 size_to_order(u64 size)
{
        u64 order = 0;
        int tmp = size;

        while (tmp > 1) {
                tmp >>= 1;
                order += 1;
        }
        if (size > (1 << order))
                order += 1;

        return order;
}

/*
 * 函数意图：把 order 转换为对应的 2 的幂大小。
 * 参数：order 表示 2 的指数。
 * 返回值：返回 2^order 对应的大小。
 */
static inline u64 order_to_size(u64 order)
{
        return 1UL << order;
}

/*
 * 函数意图：从 buddy 分配器申请一段连续页作为 slab 使用，并把这些页标记到同一个 slab。
 * 参数：size 表示希望为 slab 申请的总字节数。
 * 返回值：返回申请到的 slab 内存起始虚拟地址；失败时触发 BUG。
 */
static void *alloc_slab_memory(u64 size)
{
        struct page *p_page, *page;
        void *addr;
        u64 order, page_num;
        void *page_addr;
        int i;

        order = size_to_order(size / BUDDY_PAGE_SIZE);
        addr = get_pages(order);
        p_page = virt_to_page(addr);
        if (p_page == NULL) {
                kwarn("failed to alloc_slab_memory: out of memory\n");
                BUG_ON(1);
        }

        // BUG_ON(check_alignment((u64)addr, SLAB_INIT_SIZE));
        // 把这块 slab 内存（连续内存页）都标记到同一个 slab (起始地址)
        page_num = order_to_size(order);
        for (i = 0; i < page_num; i++) {
                page_addr = (void *)((u64)addr + i * BUDDY_PAGE_SIZE);
                page = virt_to_page(page_addr);
                page->slab = addr;
        }

        return addr;
}

/*
 * 函数意图：初始化一个指定对象大小的 slab，把其中可用槽位串成空闲链表。
 * 参数：order 表示单个 slab 对象的大小为 2^order 字节；size 表示该 slab 占用的总字节数。
 * 返回值：返回新初始化的 slab 头部指针 slab_header_t。
 */
static slab_header_t *init_slab_cache(int order, int size)
{
        void *addr;
        slab_slot_list_t *slot;
        slab_header_t *slab;
        u64 cnt, obj_size;
        int i;

        // 申请一块连续内存页作为 slab 使用
        addr = alloc_slab_memory(size);
        // slab 的元数据放在 slab 内存块的起始位置
        slab = (slab_header_t *)addr;

        // slab 的第一个 slot 被用来存放 slab 的元数据，所以剩余的槽位数量为 size / obj_size - 1
        obj_size = order_to_size(order);
        /* the first slot is used as metadata */
        cnt = size / obj_size - 1;

        slot = (slab_slot_list_t *)(addr + obj_size);
        slab->free_list_head = (void *)slot;
        slab->next_slab = NULL;
        slab->order = order;

        // 把剩余内存块划分为 cnt 个槽位，并把它们串成空闲链表，链表头为 slab->free_list_head
        for (i = 0; i < cnt - 1; i++) {
                slot->next_free = (void *)((u64)slot + obj_size);
                slot = (slab_slot_list_t *)((u64)slot + obj_size);
        }
        /* the last slot has no next one */
        slot->next_free = NULL;

        return slab;
}

/*
 * 函数意图：在指定 order 的 slab 链表中取出一个空闲槽位；若没有空闲槽位则新建 slab。
 * 参数：slab_header 表示当前 order 的 slab 链表头；order 表示要分配的槽位大小为 2^order 字节。
 * 返回值：返回分配出的槽位起始地址，调用者可使用该槽位的完整 2^order 字节。
 */
static void *_alloc_in_slab_nolock(slab_header_t *slab_header, int order)
{
        slab_slot_list_t *first_slot;
        void *next_slot;
        slab_header_t *next_slab;
        slab_header_t *new_slab;

        // 现在第一个 slab 查看是否有空闲的 slot
        first_slot = (slab_slot_list_t *)(slab_header->free_list_head);
        if (likely(first_slot != NULL)) {
                next_slot = first_slot->next_free;
                slab_header->free_list_head = next_slot;
                return first_slot;
        }

        // 走到这里，说明第一个 slab 没有空闲的 slot，找下一个 slab，循环直到没有 slab
        next_slab = slab_header->next_slab;
        while (next_slab != NULL) {
                first_slot = (slab_slot_list_t *)(next_slab->free_list_head);
                if (likely(first_slot != NULL)) {
                        next_slot = first_slot->next_free;
                        next_slab->free_list_head = next_slot;
                        return first_slot;
                }
                next_slab = next_slab->next_slab;
        }

        // 走到这里，说明 slab-list 里没有空闲内存，需要分配一个新的，插到 slab-list 链表头
        /* Allocate a new slab */
        new_slab = init_slab_cache(order, SLAB_INIT_SIZE);
        new_slab->next_slab = slab_header;
        slabs[order] = new_slab;

        return _alloc_in_slab_nolock(new_slab, order);
}

/*
 * 函数意图：封装 slab 内部的实际分配逻辑，当前版本不额外加锁。
 * 参数：slab_header 表示当前 order 的 slab 链表头；order 表示要分配的槽位大小为 2^order 字节。
 * 返回值：返回分配出的槽位起始地址。
 */
static void *_alloc_in_slab(slab_header_t *slab_header, int order)
{
        void *free_slot;

        free_slot = _alloc_in_slab_nolock(slab_header, order);
        return free_slot;
}

/*
 * exported functions
 */

/*
 * 函数意图：初始化 slab 分配器，为支持的每个对象大小创建初始 slab。
 * 参数：无。
 * 返回值：无。
 */
void init_slab()
{
        int order;

        /* slab obj size: 32, 64, 128, 256, 512, 1024, 2048 */
        for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
                slabs[order] = init_slab_cache(order, SLAB_INIT_SIZE);
        }
        kdebug("mm: finish initing slab allocators\n");
}

/*
 * 函数意图：按请求大小从 slab 分配器中分配一块小对象内存。
 * 参数：size 表示调用者请求分配的字节数，必须不超过 slab 支持的最大对象大小。
 * 返回值：返回可用内存的起始地址；实际槽位大小为不小于 size 的 2 的幂，且至少为 SLAB_MIN_ORDER 对应大小。
 */
void *alloc_in_slab(u64 size)
{
        int order;

        BUG_ON(size > order_to_size(SLAB_MAX_ORDER));

        order = (int)size_to_order(size);
        if (order < SLAB_MIN_ORDER)
                order = SLAB_MIN_ORDER;

        return _alloc_in_slab(slabs[order], order);
}

/*
 * 函数意图：把之前由 slab 分配器分配出的槽位归还到所属 slab 的空闲链表。
 * 参数：addr 表示要释放的槽位起始地址，必须是 slab 分配器返回过的地址。
 * 返回值：无。
 */
void free_in_slab(void *addr)
{
        struct page *page;
        slab_header_t *slab;
        slab_slot_list_t *slot;

        slot = (slab_slot_list_t *)addr;
        page = virt_to_page(addr);
        BUG_ON(page == NULL);

        slab = page->slab;
        slot->next_free = slab->free_list_head;
        slab->free_list_head = slot;
}

/* Get the size of free memory in slab */
/*
 * 函数意图：遍历所有 slab，统计当前 slab 分配器中仍处于空闲状态的内存总量。
 * 参数：无。
 * 返回值：返回所有空闲槽位大小之和，单位为字节。
 */
u64 get_free_mem_size_from_slab(void)
{
        int order;
        slab_header_t *current_slab;
        slab_slot_list_t *slot;
        u64 current_slot_size;
        u64 total_size = 0;
        u64 current_slot_num; /* used for debug */

        for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
                current_slab = slabs[order];
                current_slot_size = order_to_size(order);
                current_slot_num = 0;
                /* walk throught all the slabs of a certain order */
                while (current_slab != NULL) {
                        slot = current_slab->free_list_head;

                        /* walk through all the slots of a certain slab */
                        while (slot != NULL) {
                                total_size += current_slot_size;
                                current_slot_num++;
                                slot = slot->next_free;
                        }
                        current_slab = current_slab->next_slab;
                }
                kdebug("slab memory chunk size : 0x%lx, num : %d\n",
                       current_slot_size,
                       current_slot_num);
        }

        return total_size;
}
