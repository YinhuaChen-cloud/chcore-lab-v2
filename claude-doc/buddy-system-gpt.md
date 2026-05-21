# SJTU ChCore Lab2 伙伴系统源码分析：从 `kmalloc` 往下看

> 关注范围：伙伴系统（buddy system）的设计与实现。  
> 主要源码：
>
> - `kernel/mm/kmalloc.c`
> - `kernel/mm/buddy.c`
> - `kernel/include/mm/buddy.h`
> - `kernel/mm/mm.c`
> - `kernel/arch/aarch64/plat/raspi3/mm/mmparse.c`
>
> 本文按调用链自顶向下阅读：先从 `kmalloc` 入口看它如何进入页级分配，再逐层分析 `get_pages`、`buddy_get_pages`、`split_page`、`merge_page`、`buddy_free_pages` 和初始化逻辑。

---

## 0. 一句话概括

ChCore 的内核动态内存分配是一个“两层分配器”：

```text
kmalloc / kfree
│
├── 小对象：slab allocator
│
└── 大对象：buddy system
        │
        ├── 以 4KB 页为最小单位
        ├── 按 2^order 个连续页分级管理
        ├── 分配时：向上找大块，再递归拆分
        └── 释放时：找同阶 buddy，递归合并
```

本文只在必要处提到 slab。真正的分析对象是 buddy：它负责管理页级连续物理内存块。

---

## 1. 从 `kmalloc` 开始：什么时候进入伙伴系统？

`kmalloc` 的实现位于 `kernel/mm/kmalloc.c`：

```c
void *kmalloc(size_t size)
{
        u64 order;

        if (size <= _SIZE) {
                return alloc_in_slab(size);
        }

        if (size <= BUDDY_PAGE_SIZE)
                order = 0;
        else
                order = size_to_page_order(size);

        return get_pages(order);
}
```

这里的路由规则是：

```text
请求 size
│
├── size <= _SIZE
│       └── alloc_in_slab(size)
│           小对象走 slab，本文不展开
│
└── size > _SIZE
        │
        ├── size <= 4KB
        │       └── order = 0
        │
        └── size > 4KB
                └── order = size_to_page_order(size)

        最后：get_pages(order) → buddy_get_pages(...)
```

其中：

```c
#define _SIZE (1UL << SLAB_MAX_ORDER)
#define SLAB_MAX_ORDER (11)
```

所以 `_SIZE = 2048` 字节。也就是说：

- `size <= 2KB`：走 slab。
- `size > 2KB`：走 buddy，至少分配一个 4KB 页。

这意味着 `kmalloc(3000)` 虽然只要 3000 字节，但由于超过 slab 最大对象大小，会从 buddy 分配一个完整 4KB 页。

```text
kmalloc(3000)
│
├── 3000 > 2048
├── 3000 <= 4096
└── get_pages(order = 0)     // 分配 1 个物理页
```

---

## 2. `size_to_page_order`：字节数如何变成 buddy 的 order？

buddy 不直接按字节分配，而按 order 分配：

```text
order = k  ⇒  块大小 = 2^k * 4KB
```

`size_to_page_order` 负责把字节数转换为“至少能容纳它”的最小 order：

```c
u64 size_to_page_order(u64 size)
{
        u64 order;
        u64 pg_num;
        u64 tmp;

        order = 0;
        pg_num = ROUND_UP(size, BUDDY_PAGE_SIZE) / BUDDY_PAGE_SIZE;
        tmp = pg_num;

        while (tmp > 1) {
                tmp >>= 1;
                order += 1;
        }

        if (pg_num > (1 << order))
                order += 1;

        return order;
}
```

步骤如下：

```text
size
│
├── ROUND_UP(size, 4KB)
│       把字节数向上取整到页大小
│
├── pg_num = 需要多少个 4KB 页
│
└── order = ceil(log2(pg_num))
```

例子：

```text
size = 0x2000 = 8192 bytes
pg_num = 2
order = 1
块大小 = 2^1 * 4KB = 8KB
```

```text
size = 9000 bytes
pg_num = ceil(9000 / 4096) = 3
order = ceil(log2(3)) = 2
块大小 = 2^2 * 4KB = 16KB
```

ASCII 表：

```text
请求大小范围                 页数需求          buddy order     实际分配
──────────────────────────  ───────────────  ─────────────  ─────────────
(2KB, 4KB]                  1                0              4KB
(4KB, 8KB]                  2                1              8KB
(8KB, 16KB]                 3~4              2              16KB
(16KB, 32KB]                5~8              3              32KB
...                         ...              ...            ...
```

---

## 3. `get_pages`：从所有物理内存池中找页

`kmalloc` 计算出 order 后调用 `get_pages(order)`：

```c
void *get_pages(int order)
{
        struct page *p_page = NULL;
        int i;

        for (i = 0; i < physmem_map_num; ++i) {
                p_page = buddy_get_pages(&global_mem[i], order);
                if (p_page) {
                        break;
                }
        }

        if (!p_page) {
                kwarn("[OOM] Cannot get page from any memory pool!\n");
                return NULL;
        }
        return page_to_virt(p_page);
}
```

设计上，ChCore 支持多个物理内存池：

```text
global_mem[0]  ── 一个连续可用物理内存区域
global_mem[1]  ── 另一个连续可用物理内存区域
...
```

`get_pages` 的策略很直接：

```text
get_pages(order)
│
├── for each pool in global_mem[]
│       │
│       ├── buddy_get_pages(pool, order)
│       │
│       └── 如果成功，停止搜索
│
├── 如果所有 pool 都失败：返回 NULL
│
└── 如果成功：page_to_virt(page)，返回内核可访问的虚拟地址
```

当前 raspi3 平台初始化时只建立一个内存池，但这层接口已经为多个不连续物理内存区域预留了结构。

---

## 4. buddy 管理的基本单位：`struct page`

伙伴系统不是直接把裸地址塞进链表，而是为每个 4KB 物理页维护一个 `struct page` 元数据：

```c
struct page {
        struct list_head node;
        int allocated;
        int order;
        void *slab;
        struct phys_mem_pool *pool;
};
```

字段含义：

```text
struct page
├── node
│       链入某个 free_list[order] 的双向循环链表
│
├── allocated
│       1：这个块已分配
│       0：这个块空闲
│
├── order
│       这个 page 所代表的块的阶数
│       块大小 = 2^order * 4KB
│
├── slab
│       slab 分配器使用，本文不关心
│
└── pool
        指向所属 phys_mem_pool
```

需要特别注意：`struct page` 是“页描述符”，不是物理页本身。

```text
page_metadata 数组                         实际可分配物理页区域
┌──────────────┐                           ┌──────────────┐
│ struct page0 │  描述第 0 个 4KB 页  ───▶ │ physical pg0 │
├──────────────┤                           ├──────────────┤
│ struct page1 │  描述第 1 个 4KB 页  ───▶ │ physical pg1 │
├──────────────┤                           ├──────────────┤
│ ...          │                           │ ...          │
└──────────────┘                           └──────────────┘
```

一个 order 大于 0 的块由多个页组成，但通常只用块起始页的 `struct page` 表示这个块。

例如，一个 order=2 的块占 4 页：

```text
order=2 block

page[8]     page[9]     page[10]    page[11]
┌────────┬────────┬────────┬────────┐
│ start  │ inside │ inside │ inside │
│ order=2│        │        │        │
└────────┴────────┴────────┴────────┘
   ▲
   └── free_list[2] 中挂的是 page[8].node
```

内部页的元数据不一定都能独立代表块，算法主要依赖块首地址对应的 `struct page`。

---

## 5. `struct phys_mem_pool`：一个连续物理内存池

`struct phys_mem_pool` 定义如下：

```c
struct phys_mem_pool {
        u64 pool_start_addr;
        u64 pool_mem_size;
        u64 pool_phys_page_num;
        struct page *page_metadata;
        struct free_list free_lists[BUDDY_MAX_ORDER];
};
```

它把一个连续内存区域抽象成：

```text
phys_mem_pool
│
├── pool_start_addr
│       buddy 真正管理的第一个物理页的内核虚拟地址
│
├── pool_mem_size
│       buddy 管理的总字节数
│
├── pool_phys_page_num
│       该 pool 中 4KB 页数量，主要用于测试
│
├── page_metadata
│       struct page 数组起点
│
└── free_lists[0..13]
        每个 order 一个空闲链表
```

order 分级：

```text
BUDDY_PAGE_SIZE = 0x1000 = 4KB
BUDDY_MAX_ORDER = 14
支持 order: [0, 13]

order   页数             块大小
─────   ──────────────   ─────────────
0       1                4KB
1       2                8KB
2       4                16KB
3       8                32KB
...     ...              ...
13      8192             32MB
```

源码注释里写“最大连续分配 16M”，但按当前常量 `BUDDY_MAX_ORDER = 14` 计算，最大 order 是 13，最大块为：

```text
2^13 * 4KB = 8192 * 4096 = 32MB
```

所以这里注释与当前常量存在不一致，实际应以代码常量为准。

---

## 6. 空闲链表：每个 order 一条链

每个 order 有一个 `struct free_list`：

```c
struct free_list {
        struct list_head free_list;
        u64 nr_free;
};
```

整体形态如下：

```text
pool->free_lists

order 0:  head ──▶ [4KB block] ──▶ [4KB block] ──▶ head
order 1:  head ──▶ [8KB block] ──▶ head
order 2:  head ──▶ [16KB block] ─▶ [16KB block] ─▶ head
...
order13:  head ──▶ [32MB block] ─▶ head
```

`nr_free` 是该 order 链表中的空闲块数量。分配和释放都会同步维护它：

```text
取出一个块：nr_free--
放回一个块：nr_free++
```

---

## 7. 地址转换：`page_to_virt` 与 `virt_to_page`

buddy 内部操作的是 `struct page *`，但 `kmalloc` 对外返回的是内核虚拟地址。两者之间由 `page_to_virt` 和 `virt_to_page` 转换。

### 7.1 `page_to_virt`

```c
void *page_to_virt(struct page *page)
{
        u64 addr;
        struct phys_mem_pool *pool = page->pool;

        BUG_ON(pool == NULL);
        addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE
               + pool->pool_start_addr;
        return (void *)addr;
}
```

核心公式：

```text
page_idx = page - pool->page_metadata
addr     = pool->pool_start_addr + page_idx * 4KB
```

图示：

```text
page_metadata:  page[0]   page[1]   page[2]   ...
                  │         │         │
                  │ idx=0   │ idx=1   │ idx=2
                  v         v         v
pool_start:     addr+0K   addr+4K   addr+8K   ...
```

### 7.2 `virt_to_page`

```c
struct page *virt_to_page(void *ptr)
{
        ...
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
```

反向公式：

```text
page_idx = (addr - pool_start_addr) / 4KB
page     = page_metadata + page_idx
```

它先扫描 `global_mem[]`，确定地址属于哪个 pool，再计算页号。

---

## 8. 伙伴地址：`get_buddy_chunk` 的异或技巧

伙伴系统的关键是快速找到某个块的“伙伴块”。ChCore 用 `get_buddy_chunk` 实现：

```c
static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                    struct page *chunk)
{
        u64 chunk_addr;
        u64 buddy_chunk_addr;
        int order;

        chunk_addr = (u64)page_to_virt(chunk);
        order = chunk->order;

#define BUDDY_PAGE_SIZE_ORDER (12)
        buddy_chunk_addr = chunk_addr
                           ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));

        if ((buddy_chunk_addr < pool->pool_start_addr)
            || (buddy_chunk_addr
                >= (pool->pool_start_addr + pool->pool_mem_size))) {
                return NULL;
        }

        return virt_to_page((void *)buddy_chunk_addr);
}
```

因为：

```text
4KB = 2^12
order = k 的块大小 = 2^k * 4KB = 2^(k+12)
```

两个 buddy 块的地址只差第 `k + 12` 位。翻转这一位即可得到伙伴地址：

```text
buddy_addr = chunk_addr ^ (1 << (order + 12))
```

例子：order=1，块大小为 8KB。

```text
chunk_addr = 0x10000
order      = 1
mask       = 1 << (1 + 12) = 0x2000

buddy_addr = 0x10000 ^ 0x2000 = 0x12000
```

ASCII 二进制示意：

```text
chunk_addr:  0001 0000 0000 0000 0000   0x10000
mask:        0000 0010 0000 0000 0000   0x02000
             ------------------------ ^
buddy_addr:  0001 0010 0000 0000 0000   0x12000
                    ▲
                    翻转 order+12 位
```

它还做了边界检查：如果算出的 buddy 地址不属于当前 pool，就返回 `NULL`。这对 pool 边界附近的块很重要。

---

## 9. 分配主流程：`buddy_get_pages`

`buddy_get_pages(pool, order)` 是 buddy 分配入口：

```c
struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
        int cur_order;
        struct free_list *list;
        struct page *page;

        if(order >= BUDDY_MAX_ORDER || order < 0) {
                return NULL;
        }

        for (cur_order = order; cur_order < BUDDY_MAX_ORDER; cur_order++) {
                list = &pool->free_lists[cur_order];
                if (list->nr_free > 0) {
                        page = list_entry(list->free_list.next,
                                          struct page, node);
                        BUG_ON(page->allocated);
                        BUG_ON(page->order != cur_order);

                        list_del(&page->node);
                        pool->free_lists[page->order].nr_free--;

                        page = split_page(pool, order, page);
                        BUG_ON(page->order != order);

                        page->allocated = 1;
                        return page;
                }
        }

        return NULL;
}
```

它的策略是“找最小可用大块”：

```text
请求 order = k
│
├── 检查 free_lists[k]
│       ├── 有：直接取
│       └── 无：继续检查 free_lists[k+1]
│
├── 检查 free_lists[k+1]
│       ├── 有：取出，再拆成 order k
│       └── 无：继续向上
│
├── ...
│
└── 如果一直到 BUDDY_MAX_ORDER - 1 都没有：返回 NULL
```

示例：请求 order=1，但只有 order=3 有空闲块。

```text
分配前：

free_lists[0]: empty
free_lists[1]: empty    ← 目标
free_lists[2]: empty
free_lists[3]: [ A B C D E F G H ]

buddy_get_pages(order=1)
│
├── 在 order=3 找到一个 8 页块
├── 从 free_lists[3] 删除该块
└── split_page(..., target_order=1)
```

接下来由 `split_page` 负责递归拆分。

---

## 10. 拆分：`split_page`

`split_page(pool, target_order, page)` 的前提是：

- `page` 指向一个空闲块。
- 该块已经从原 free list 中取出。
- `page->order >= target_order`。

源码：

```c
static struct page *split_page(struct phys_mem_pool *pool, u64 order,
                               struct page *page)
{
        struct page *buddy;

        BUG_ON(page == NULL);
        BUG_ON(page->order < order);
        BUG_ON(page->allocated);
        if (page->order == order) {
                return page;
        }

        page->order--;
        buddy = get_buddy_chunk(pool, page);
        BUG_ON(buddy == NULL);
        buddy->order = page->order;
        buddy->allocated = 0;
        list_add(&buddy->node, &pool->free_lists[buddy->order].free_list);
        pool->free_lists[buddy->order].nr_free++;

        return split_page(pool, order, page);
}
```

拆分一个 order=m 的块，会得到两个 order=m-1 的块：

```text
拆分前：order=3

[ A B C D E F G H ]

拆分后：两个 order=2

[ A B C D ] [ E F G H ]
     │             │
     │             └── buddy，挂入 free_lists[2]
     │
     └── page，继续用于下一轮拆分或最终返回
```

继续前面的例子，请求 order=1，从 order=3 拿到大块：

```text
step 1: order=3 → order=2

取出的块：
[ A B C D E F G H ]

拆成：
[ A B C D ] [ E F G H ]
     │             │
     │             └── 放回 free_lists[2]
     └── 继续拆

step 2: order=2 → order=1

[ A B C D ]

拆成：
[ A B ] [ C D ]
   │       │
   │       └── 放回 free_lists[1]
   └── 返回给调用者
```

最终状态：

```text
返回给用户：
[ A B ]        order=1, allocated=1

空闲链表新增：
free_lists[1]: [ C D ]
free_lists[2]: [ E F G H ]
```

### 10.1 为什么只把 buddy 放回链表？

`split_page` 中有一条很重要的实现选择：

```text
当前 page 不放回 free list；只把拆出来的 buddy 放回 free list。
```

原因是当前 `page` 是候选返回块，它还可能继续被拆，最后会返回给分配者。若每次都把它放回链表再取出，会产生无意义的链表操作。

```text
更高效的做法：

大块出链
│
├── 左半边：一直拿在手里，继续拆，最后分配出去
└── 右半边：每拆一次就放回对应 order 的 free list
```

---

## 11. 释放入口：`kfree` 与 `free_pages`

### 11.1 `kfree`

`kfree` 会根据 `struct page` 中的 `slab` 字段判断这块内存是否属于 slab：

```c
void kfree(void *ptr)
{
        struct page *p_page;

        p_page = virt_to_page(ptr);
        if (p_page && p_page->slab) {
                free_in_slab(ptr);
        } else {
                buddy_free_pages(p_page->pool, p_page);
        }
}
```

对 buddy 来说，关键路径是：

```text
kfree(ptr)
│
├── virt_to_page(ptr)
│       把地址转回 struct page
│
└── buddy_free_pages(p_page->pool, p_page)
        释放并尝试合并
```

### 11.2 `free_pages`

还有一个更直接的页释放接口：

```c
void free_pages(void *addr)
{
        struct page *p_page;
        p_page = virt_to_page(addr);
        buddy_free_pages(p_page->pool, p_page);
}
```

它不检查 slab，适合释放明确来自 `get_pages` / buddy 的页级分配。

---

## 12. 释放主流程：`buddy_free_pages`

源码：

```c
void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        BUG_ON(page->allocated == 0);
        page->allocated = 0;

        page = merge_page(pool, page);

        BUG_ON(page->allocated);

        list_add(&page->node, &pool->free_lists[page->order].free_list);
        pool->free_lists[page->order].nr_free++;
}
```

流程：

```text
buddy_free_pages(pool, page)
│
├── 检查 page 当前必须是 allocated=1
│       防止重复释放同一块
│
├── page->allocated = 0
│       标记为空闲
│
├── merge_page(pool, page)
│       尽可能向上合并 buddy
│
└── 把最终合并后的块挂入 free_lists[page->order]
```

它也采用了和 `split_page` 类似的优化：释放的块不会马上入链，因为它可能很快被 `merge_page` 合并掉。最终只把合并后的最大块入链一次。

---

## 13. 合并：`merge_page`

`merge_page` 的目标：如果当前块的 buddy 也空闲且同阶，就把二者合成更高一阶的块，并递归尝试继续合并。

源码：

```c
static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
        struct page *buddy;

        BUG_ON(page->order > BUDDY_MAX_ORDER - 1 || page->allocated);
        if (page->order == BUDDY_MAX_ORDER - 1) {
                return page;
        }

        buddy = get_buddy_chunk(pool, page);

        if (buddy == NULL || buddy->allocated || buddy->order < page->order) {
                return page;
        }

        BUG_ON(buddy->order > page->order);
        BUG_ON(buddy->order != page->order);

        list_del(&buddy->node);
        pool->free_lists[buddy->order].nr_free--;

        if (buddy < page) {
                page = buddy;
        }
        page->order++;

        return merge_page(pool, page);
}
```

合并条件可以总结为：

```text
可以合并，当且仅当：

1. 当前块不是最大 order
2. buddy 在同一个 pool 内
3. buddy 当前是空闲的 allocated=0
4. buddy->order == page->order
```

为什么 `buddy->order < page->order` 时直接不能合并？

```text
当前块：order=2，大小 16KB
buddy 区域可能已经被拆成更小块：

当前块                 buddy 区域
[ A B C D ]            [ E F ] [ G ] [ H ]
 order=2               order=1 order=0 order=0

虽然 buddy 区域整体相邻，但它已经不是一个完整 order=2 空闲块，不能合并。
```

为什么 `buddy->order > page->order` 是 BUG？

如果 buddy 真的是当前块按同一 order 算出的伙伴，那么它代表的块不应该比当前块更大。若出现更大的 order，说明元数据状态已经不一致。

### 13.1 合并时为什么选择低地址块作为新块？

```c
if (buddy < page) {
        page = buddy;
}
page->order++;
```

合并后更大块的起始地址应该是两个 buddy 中较低的地址。

```text
合并前：

page:   [ C D ]
buddy:  [ A B ]

合并后：

[ A B C D ]
  ▲
  └── 新块起点必须是低地址的 [A B]
```

由于 `page_metadata` 数组顺序和物理页地址顺序一致，比较 `struct page *` 指针大小就等价于比较页索引、比较物理地址。

### 13.2 合并示意

```text
释放 [ A B ]，order=1

free_lists[1] 里已有它的 buddy [ C D ]：

[ A B ] + [ C D ] → [ A B C D ]，order=2

如果 free_lists[2] 里又有 [ E F G H ]：

[ A B C D ] + [ E F G H ] → [ A B C D E F G H ]，order=3

继续递归，直到：
- buddy 不在同一 pool
- buddy 已分配
- buddy order 不匹配
- 已到最大 order
```

---

## 14. 初始化：`mm_init` 如何构造 buddy 内存池？

上面分析了分配和释放，但最开始 free list 里的块从哪里来？答案在 `mm_init` 和 `init_buddy`。

### 14.1 平台解析可用内存：`parse_mem_map`

raspi3 平台的 `parse_mem_map`：

```c
extern char img_end;
#define USABLE_MEM_START (ROUND_UP((paddr_t)(&img_end), PAGE_SIZE))
#define USABLE_MEM_END   (0x3f000000)
#define RESERVED_FOR_GPU (32 << 20)

void parse_mem_map(void)
{
        physmem_map_num = 1;
        physmem_map[0][0] = USABLE_MEM_START;
        physmem_map[0][1] = USABLE_MEM_END - RESERVED_FOR_GPU;
}
```

它给出一段可用物理内存：

```text
[ kernel image end, 0x3f000000 - 32MB )
```

底部避开内核镜像，顶部给 GPU 预留 32MB。

### 14.2 `mm_init` 划分 metadata 与可分配页

`mm_init` 中：

```c
free_mem_start = phys_to_virt(physmem_map[0][0]);
free_mem_end = phys_to_virt(physmem_map[0][1]);

npages = (free_mem_end - free_mem_start)
         / (PAGE_SIZE + sizeof(struct page));
start_vaddr = ROUND_UP(free_mem_start + npages * sizeof(struct page),
                       PAGE_SIZE);

page_meta_start = (struct page *)free_mem_start;
init_buddy(&global_mem[0], page_meta_start, start_vaddr, npages);
```

这段代码把一整段空闲内存分成两部分：

```text
低地址
│
│  page metadata 区                           padding        buddy 管理区
│  npages * sizeof(struct page)               页对齐填充      npages * 4KB
│
▼
+-------------------------------------------+-------------+----------------------+
| page[0] | page[1] | ... | page[npages-1] |   unused    | real pages managed   |
+-------------------------------------------+-------------+----------------------+
^                                                         ^
|                                                         |
free_mem_start / page_meta_start                         start_vaddr / pool_start
```

为什么 `npages` 这样算？

```text
每管理 1 个物理页，需要：

sizeof(struct page)  元数据空间
+ PAGE_SIZE          实际可分配物理页空间

所以：
npages = total_available_bytes / (PAGE_SIZE + sizeof(struct page))
```

这样可以保证元数据数组和被管理页都放在同一段可用内存中，并且互不重叠。

---

## 15. `init_buddy`：逐页释放，自动合并成大块

`init_buddy` 做五件事：

```c
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, u64 page_num)
{
        pool->pool_start_addr = start_addr;
        pool->page_metadata = start_page;
        pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;
        pool->pool_phys_page_num = page_num;

        for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
                pool->free_lists[order].nr_free = 0;
                init_list_head(&(pool->free_lists[order].free_list));
        }

        memset((char *)start_page, 0, page_num * sizeof(struct page));

        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                page->allocated = 1;
                page->order = 0;
                page->pool = pool;
        }

        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                buddy_free_pages(pool, page);
        }
}
```

### 15.1 为什么先把每页设为 `allocated = 1`？

因为 `buddy_free_pages` 的入口检查是：

```c
BUG_ON(page->allocated == 0);
```

释放函数只接受“当前已分配”的块。初始化时还没有任何 free block，所以 ChCore 先伪造出“每个 4KB 页都已分配”的状态，然后逐个调用 `buddy_free_pages`，让正常释放逻辑把它们变成空闲块。

### 15.2 为什么逐页释放能够形成大块？

因为每次释放都会尝试和 buddy 合并。

假设有 8 个页面：

```text
初始：全部 marked allocated=1，还不在任何 free list 中

page:  0 1 2 3 4 5 6 7
state: A A A A A A A A
```

逐页 `buddy_free_pages`：

```text
free page0:
[0] 进入 free_lists[0]

free page1:
[1] 的 buddy 是 [0]，合并成 [0 1] order=1

free page2:
[2] 进入 free_lists[0]

free page3:
[3] + [2] → [2 3] order=1
[2 3] + [0 1] → [0 1 2 3] order=2

free page4:
[4] 进入 free_lists[0]

free page5:
[5] + [4] → [4 5] order=1

free page6:
[6] 进入 free_lists[0]

free page7:
[7] + [6] → [6 7] order=1
[6 7] + [4 5] → [4 5 6 7] order=2
[4 5 6 7] + [0 1 2 3] → [0 1 2 3 4 5 6 7] order=3
```

最终：

```text
free_lists[3]: [0..7]
其他 order: empty
```

如果总页数不是 2 的幂，最终会形成若干个不同 order 的最大可合并块。

---

## 16. 一次完整分配与释放示例

假设初始化后 pool 中有一个 order=3 的空闲块：

```text
free_lists[3]: [0 1 2 3 4 5 6 7]
```

### 16.1 分配 8KB：`kmalloc(8192)`

调用链：

```text
kmalloc(8192)
│
├── size > 2KB，走 buddy
├── size_to_page_order(8192) = 1
├── get_pages(1)
└── buddy_get_pages(pool, 1)
```

buddy 操作：

```text
目标 order=1

free_lists[1] empty
free_lists[2] empty
free_lists[3] has [0..7]

取出 [0..7]
split order=3 → order=2:
    [0..3] 继续
    [4..7] 放入 free_lists[2]

split order=2 → order=1:
    [0..1] 返回
    [2..3] 放入 free_lists[1]
```

分配后：

```text
返回给 kmalloc: [0 1]

free_lists[1]: [2 3]
free_lists[2]: [4 5 6 7]
```

### 16.2 释放 8KB：`kfree(ptr)`

调用链：

```text
kfree(ptr)
│
├── virt_to_page(ptr) → page[0]
└── buddy_free_pages(pool, page[0])
```

释放操作：

```text
释放 [0 1]

它的 buddy [2 3] 空闲且 order=1
→ 合并为 [0 1 2 3] order=2

它的新 buddy [4 5 6 7] 空闲且 order=2
→ 合并为 [0 1 2 3 4 5 6 7] order=3

放入 free_lists[3]
```

最终恢复：

```text
free_lists[3]: [0..7]
```

---

## 17. 关键不变式

为了让 split/merge 正确，代码依赖这些不变式：

### 17.1 一个空闲块只出现在对应 order 的 free list 中

```text
page->allocated == 0
page->order == k
page->node ∈ pool->free_lists[k]
```

例外：正在 split 或 merge 的临时块可能“不在任何链表中”，这是刻意的优化。

### 17.2 分配出去的块不在 free list 中

```text
page->allocated == 1
page->node 不应在任何 free list 中
```

`buddy_get_pages` 先 `list_del`，再把 `allocated` 设为 1。

### 17.3 只有块起始页代表整个块

对于 order=k 的块，链表中挂的是起始页的 `node`。

```text
[ page i ... page i + 2^k - 1 ]
   ▲
   └── page i 的 struct page 表示整个块
```

### 17.4 合并只发生在“同 pool、同 order、都空闲”的 buddy 之间

```text
same pool
same order
both free
address relationship matches buddy formula
```

---

## 18. 错误检查与边界处理

代码中大量使用 `BUG_ON` 维护内部一致性：

```text
split_page:
- page != NULL
- page->order >= target_order
- page 未分配

buddy_get_pages:
- order < BUDDY_MAX_ORDER
- 取出的块未分配
- 取出的块 order 与链表 order 一致

merge_page:
- page 未分配
- page order 不超过最大 order
- buddy order 不应大于 page order

buddy_free_pages:
- 不能释放 already-free 的块
```

还有一些边界行为：

- `buddy_get_pages` 找不到合适块时返回 `NULL`。
- `get_pages` 遍历所有 pool 后仍失败，会打印 OOM 警告并返回 `NULL`。
- `get_buddy_chunk` 算出的 buddy 不在当前 pool 时返回 `NULL`，不能跨 pool 合并。
- `virt_to_page` 找不到所属 pool 会触发 `BUG_ON(pool == NULL)`，所以传给 `kfree` / `free_pages` 的地址必须来自被 buddy 管理的区域。

一个小细节：`buddy_get_pages` 的参数类型是 `u64 order`，所以判断 `order < 0` 在 C 语义上不会真正成立。无效的大 order 主要由 `order >= BUDDY_MAX_ORDER` 拦住。

---

## 19. 时间复杂度

设最大 order 数为 `M = BUDDY_MAX_ORDER`，当前代码中 `M = 14`。

### 分配

```text
buddy_get_pages:
- 最坏向上扫描 M 个 free list
- 最坏拆分 M 次

复杂度：O(M)
```

因为 `M` 是常数 14，所以实际非常快。

### 释放

```text
buddy_free_pages:
- 最坏向上合并 M 次

复杂度：O(M)
```

同样实际是常数级。

---

## 20. 设计优点与代价

### 优点

1. **结构简单**：每个 order 一条链表，分配/释放逻辑清晰。
2. **适合连续物理页分配**：页表页、大页映射、DMA 类需求通常需要连续页。
3. **合并能力强**：释放时自动递归合并，缓解外部碎片。
4. **地址计算快**：buddy 由一次 XOR 得到。
5. **多 pool 可扩展**：`global_mem[]` 和 `phys_mem_pool` 抽象支持不连续内存区域。

### 代价

1. **内部碎片**：只能按 2 的幂页数分配，例如 9000 字节会分配 16KB。
2. **最大连续块受限**：当前最大 order=13，即最大 32MB 连续块。
3. **并发保护缺失**：当前代码没有锁；实验环境下通常足够，但多核并发分配需要额外同步。
4. **metadata 占用内存**：每个 4KB 页都要一个 `struct page`。
5. **释放接口依赖正确地址**：错误地址会导致 `virt_to_page` 找不到 pool 或破坏元数据。

---

## 21. 总结：从 `kmalloc` 到 buddy 的完整链路

最终调用链可以画成：

```text
kmalloc(size)
│
├── size <= 2KB
│       └── alloc_in_slab(size)
│
└── size > 2KB
        │
        ├── size_to_page_order(size)
        │       把字节数变成 buddy order
        │
        └── get_pages(order)
                │
                ├── 遍历 global_mem[]
                │
                └── buddy_get_pages(pool, order)
                        │
                        ├── 向上查找非空 free list
                        ├── 取出一个足够大的块
                        ├── split_page 拆到目标 order
                        └── 返回 struct page

get_pages 再调用 page_to_virt(page)，把 struct page 转成内核虚拟地址返回。
```

释放链路：

```text
kfree(ptr)
│
├── virt_to_page(ptr)
│       地址转 struct page
│
├── 如果 page->slab != NULL
│       └── free_in_slab(ptr)
│
└── 否则
        └── buddy_free_pages(pool, page)
                │
                ├── 标记为空闲
                ├── merge_page 递归合并 buddy
                └── 把最终块挂回对应 free list
```

初始化链路：

```text
mm_init()
│
├── parse_mem_map()
│       获取平台可用物理内存区间
│
├── 划分 metadata 区和 buddy 管理区
│
└── init_buddy(pool, page_metadata, start_vaddr, npages)
        │
        ├── 初始化 pool 信息和 free_lists
        ├── 把每个 page 标成 allocated=1, order=0
        └── 逐页 buddy_free_pages
                自动合并成尽可能大的空闲块
```

一句话收尾：ChCore 的 buddy system 以 `struct page` 作为页级元数据，以 `phys_mem_pool` 管理连续物理内存区域，以 `free_lists[order]` 组织不同大小的空闲块；分配时“向上找、向下拆”，释放时“找伙伴、向上合”，最终为 `kmalloc` 的大对象分配提供连续页级内存。