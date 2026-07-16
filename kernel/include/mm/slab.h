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

#pragma once

#include <common/types.h>

#define SLAB_INIT_SIZE (2 * 1024 * 1024) // 2M

/* order range: [SLAB_MIN_ORDER, SLAB_MAX_ORDER] */
#define SLAB_MIN_ORDER (5)
#define SLAB_MAX_ORDER (11)

/*
 * 每个 slab 内存块开头的元数据。
 * 一个 slab 管理一批固定大小（2^order 字节）的对象槽位，
 * 并通过 free_list_head 记录本 slab 内还没分配出去的槽位链表。
 */
typedef struct slab_header slab_header_t;
struct slab_header {
        void *free_list_head;      /* 指向本 slab 中第一个空闲槽位 */
        slab_header_t *next_slab;  /* 串起同一 order 的下一个 slab */
        int order;                 /* 本 slab 中每个槽位的大小为 2^order 字节 */
};

/*
 * 空闲槽位复用自身开头空间保存的链表节点。
 * 只有槽位处于空闲状态时，该结构才有效；槽位分配给调用者后，
 * 这片内存就是普通对象空间，不再保存 next_free 语义。
 */
typedef struct slab_slot_list slab_slot_list_t;
struct slab_slot_list {
        void *next_free;           /* 指向同一 slab 中下一个空闲槽位 */
};

void init_slab(void);

void *alloc_in_slab(u64);
void free_in_slab(void *addr);

u64 get_free_mem_size_from_slab(void);
