# ChCore-Lab 内存来源与物理页分配器源码分析

> 分析对象：SJTU ChCore-Lab v2，当前工作区代码。  
> 结论先行：这个项目在 raspi3/AArch64 平台上**不是运行时探测内存大小**，而是由平台源码里的常量、链接脚本符号和少量固件/QEMU 配置共同约定出“可用物理内存范围”。物理页分配器管理的是这段**配置出来的可用物理内存**，不是链接脚本里单独划出的一块静态 heap。

---

## 1. 总体结论

### 1.1 系统怎么知道自己有多少内存？

源码路径：[`kernel/arch/aarch64/plat/raspi3/mm/mmparse.c`](../kernel/arch/aarch64/plat/raspi3/mm/mmparse.c)

核心代码：

```c
/*
 * The usable physical memory: 0x0 - 0x3f000000.
 * The top 32M is reserved for gpu_mem, i.e., [0x3f000000 - 32M, 0x3f000000).
 * The bottom from 0x80000 is for kernel image.
 * So the real usable physical memory is [img_end, 0x3f000000 - 32M)
 */

extern char img_end;
#define USABLE_MEM_START (ROUND_UP((paddr_t)(&img_end), PAGE_SIZE))
#define USABLE_MEM_END   (0x3f000000)
#define RESERVED_FOR_GPU (32 << 20) /* 32M */

void parse_mem_map(void)
{
        physmem_map_num = 1;
        physmem_map[0][0] = USABLE_MEM_START; /* 4K-aligned */
        physmem_map[0][1] = USABLE_MEM_END - RESERVED_FOR_GPU; /* 4K-aligned */
        kinfo("physmem_map: [0x%lx, 0x%lx)\n",
              physmem_map[0][0],
              physmem_map[0][1]);
}
```

这说明：

- `USABLE_MEM_END` 写死为 `0x3f000000`。
- GPU 保留内存写死为 `32 MiB`。
- 可用内存起点来自链接脚本符号 `img_end`，再按页对齐。
- `parse_mem_map()` 只生成一个物理内存区间。
- 没有看到 FDT/DTB/device tree 或 firmware memory map 的解析路径。

因此 raspi3 平台交给内核内存管理器的物理范围是：

```text
[ ROUND_UP(&img_end, 4 KiB), 0x3f000000 - 32 MiB )
```

也就是：

```text
[ ROUND_UP(&img_end, 0x1000), 0x3d000000 )
```

### 1.2 是内存探测还是 hardcode？

**是 hardcode + 链接脚本符号，不是运行时内存探测。**

更准确地说：

```text
可用 RAM 下界 = 链接脚本产生的 img_end，表示 kernel image 结束位置
可用 RAM 上界 = 平台常量 0x3f000000 - 32 MiB
```

源码中没有发现类似以下机制：

- 解析 DTB/FDT 的 `/memory` 节点；
- 调 firmware/mailbox 查询内存大小；
- 扫描物理地址做 RAM probe；
- 从 QEMU `-m size=1G` 参数读取内存大小。

QEMU 启动参数里确实给了 `-m size=1G`，但内核代码没有读取这个值：

源码路径：[`kernel/arch/aarch64/boot/raspi3/CMakeLists.txt`](../kernel/arch/aarch64/boot/raspi3/CMakeLists.txt)

```cmake
chcore_generate_emulate_sh(
    "qemu-system-aarch64"
    "-machine raspi3b -nographic -serial null -serial mon:stdio -m size=1G -kernel \$basedir/kernel.img"
)
```

这个配置只影响 QEMU 提供的机器资源；内核内部仍然按 `mmparse.c` 里的平台常量来构造 `physmem_map`。

### 1.3 物理页分配器管理什么？

物理页分配器管理的是：

```text
parse_mem_map() 给出的可用物理内存
  - 前端一段 struct page 元数据区
  - 对齐 padding
= 剩余的物理页全部加入 buddy free lists
```

不是管理链接脚本里显式划出的 `.heap` 段；链接脚本只告诉内核 image 到哪里结束，从那里之后才可用。

ASCII 总览：

```text
物理地址空间，raspi3 平台约定

0x00000000
    |
    | 低地址区域，含 boot/kernel image 等，不交给 buddy
    |
0x00080000  TEXT_OFFSET，kernel image 大致从这里加载
    |
    | kernel image: init/text/data/rodata/bss ...
    |
img_end  <--- 链接脚本产生；parse_mem_map() 从这里之后开始
    |
    | parse_mem_map() 声称可用的内存区间
    v
+-------------------------------+  physmem_map[0][0] = ROUND_UP(img_end, 4K)
| struct page metadata          |  mm_init() 放在可用区间最前面
+-------------------------------+
| alignment padding             |
+-------------------------------+  start_vaddr，buddy 真正管理的页起点
|                               |
| buddy managed physical pages  |
|                               |
+-------------------------------+  physmem_map[0][1] = 0x3d000000
| GPU reserved 32 MiB           |
+-------------------------------+  0x3f000000
| peripherals / MMIO            |
+-------------------------------+  0x40000000
```

---

## 2. 链接脚本如何给出 `img_end`

源码路径：[`kernel/arch/aarch64/boot/linker.tpl.ld`](../kernel/arch/aarch64/boot/linker.tpl.ld)

关键代码：

```ld
. = TEXT_OFFSET;
img_start = .;
init : {
    ${init_objects}
}

. = ALIGN(SZ_16K);

init_end = ABSOLUTE(.);

.text KERNEL_VADDR + init_end : AT(init_end) {
    *(.text*)
}

. = ALIGN(SZ_64K);
.data : {
    *(.data*)
}
. = ALIGN(SZ_64K);

.rodata : {
    *(.rodata*)
}
_edata = . - KERNEL_VADDR;

_bss_start = . - KERNEL_VADDR;
.bss : {
    *(.bss*)
}
_bss_end = . - KERNEL_VADDR;
. = ALIGN(SZ_64K);

img_end = . - KERNEL_VADDR;
```

这里的 `img_end` 是物理地址意义上的 kernel image 末尾：

- 链接脚本从 `TEXT_OFFSET` 开始摆放镜像。
- `.text`、`.data`、`.rodata`、`.bss` 都被放完后，按 `SZ_64K` 对齐。
- `img_end = . - KERNEL_VADDR` 把高半内核虚拟地址换回物理地址。

这解释了为什么 `mmparse.c` 可以用：

```c
extern char img_end;
#define USABLE_MEM_START (ROUND_UP((paddr_t)(&img_end), PAGE_SIZE))
```

即：内核自己知道“我这份镜像到物理地址哪里结束”。但这不等于知道整机内存大小；整机上界仍然是写死的 `0x3f000000`。

---

## 3. 启动页表也体现了 hardcode 的平台内存布局

源码路径：[`kernel/arch/aarch64/boot/raspi3/init/mmu.c`](../kernel/arch/aarch64/boot/raspi3/init/mmu.c)

关键常量：

```c
/* Physical memory address space: 0-1G */
#define PHYSMEM_START   (0x0UL)
#define PERIPHERAL_BASE (0x3F000000UL)
#define PHYSMEM_END     (0x40000000UL)
```

低地址恒等映射：

```c
/* Normal memory: PHYSMEM_START ~ PERIPHERAL_BASE */
/* Map with 2M granularity */
for (; vaddr < PERIPHERAL_BASE; vaddr += SIZE_2M) {
        boot_ttbr0_l2[GET_L2_INDEX(vaddr)] =
                (vaddr) /* low mem, va = pa */
                | UXN
                | ACCESSED
                | NG
                | INNER_SHARABLE
                | NORMAL_MEMORY
                | IS_VALID;
}

/* Peripheral memory: PERIPHERAL_BASE ~ PHYSMEM_END */
/* Map with 2M granularity */
for (vaddr = PERIPHERAL_BASE; vaddr < PHYSMEM_END; vaddr += SIZE_2M) {
        boot_ttbr0_l2[GET_L2_INDEX(vaddr)] =
                (vaddr) /* low mem, va = pa */
                | UXN
                | ACCESSED
                | NG
                | DEVICE_MEMORY
                | IS_VALID;
}
```

高半内核映射：

```c
vaddr = KERNEL_VADDR + PHYSMEM_START;

boot_ttbr1_l0[GET_L0_INDEX(vaddr)] = ((u64)boot_ttbr1_l1) | IS_TABLE
                                     | IS_VALID | NG;
boot_ttbr1_l1[GET_L1_INDEX(vaddr)] = ((u64)boot_ttbr1_l2) | IS_TABLE
                                     | IS_VALID | NG;

for (; vaddr < KERNEL_VADDR + PERIPHERAL_BASE; vaddr += SIZE_2M) {
        boot_ttbr1_l2[GET_L2_INDEX(vaddr)] =
                (vaddr - KERNEL_VADDR) /* high mem, pa = va - KERNEL_VADDR */
                | UXN
                | ACCESSED
                | NG
                | INNER_SHARABLE
                | NORMAL_MEMORY
                | IS_VALID;
}
```

这部分代码同样没有探测内存；它直接按照 raspi3 的平台布局映射：

```text
0x00000000 .. 0x3f000000  Normal memory
0x3f000000 .. 0x40000000  Device / peripheral memory
0x40000000 ..             Local peripherals 等设备空间
```

---

## 4. `mm_init()` 如何消费内存范围

源码路径：[`kernel/mm/mm.c`](../kernel/mm/mm.c)

全局结构：

```c
/* On raspi3, the size of physical memory pool only need to be 1 */
#define PHYS_MEM_POOL_SIZE 1
struct phys_mem_pool global_mem[PHYS_MEM_POOL_SIZE];
int physmem_map_num;
u64 physmem_map[PHYS_MEM_POOL_SIZE][2]; /* [start, end) */
```

初始化流程：

```c
void mm_init(void)
{
        vaddr_t free_mem_start = 0;
        vaddr_t free_mem_end = 0;
        struct page *page_meta_start = NULL;
        u64 npages = 0;
        u64 start_vaddr = 0;

        physmem_map_num = 0;
        parse_mem_map();

        if (physmem_map_num == 1) {
                free_mem_start = phys_to_virt(physmem_map[0][0]);
                free_mem_end = phys_to_virt(physmem_map[0][1]);

                npages = (free_mem_end - free_mem_start)
                         / (PAGE_SIZE + sizeof(struct page));
                start_vaddr =
                        ROUND_UP(free_mem_start + npages * sizeof(struct page),
                                 PAGE_SIZE);

                page_meta_start = (struct page *)free_mem_start;

                /* buddy alloctor for managing physical memory */
                init_buddy(
                        &global_mem[0], page_meta_start, start_vaddr, npages);
        } else {
                BUG("Unsupported physmem_map_num\n");
        }

        /* slab alloctor for allocating small memory regions */
        init_slab();
}
```

这个函数做了几件关键事情：

1. 调用 `parse_mem_map()`，获得物理可用区间。
2. 用 `phys_to_virt()` 把物理区间变成内核可访问的高半虚拟地址区间。
3. 在这段可用内存的最前面放 `struct page` 数组。
4. 把 `struct page` 元数据之后、页对齐后的区域交给 buddy 管理。
5. 当前 raspi3 实现只支持 `physmem_map_num == 1`。

对应布局：

```text
free_mem_start = phys_to_virt(physmem_map[0][0])
free_mem_end   = phys_to_virt(physmem_map[0][1])

[free_mem_start, free_mem_end)

+-------------------------------------------------------------+
| page metadata: npages * sizeof(struct page)                 |
+-------------------------------------------------------------+
| padding to PAGE_SIZE                                        |
+-------------------------------------------------------------+ <- start_vaddr
| page 0 | page 1 | page 2 | ... | page npages-1             |
+-------------------------------------------------------------+
```

注意 `npages` 的计算：

```c
npages = (free_mem_end - free_mem_start)
         / (PAGE_SIZE + sizeof(struct page));
```

它不是简单地把整个区间除以 `PAGE_SIZE`，而是把每个可管理物理页需要的一份 `struct page` 元数据也计入空间成本。这样 `metadata + managed pages` 都能放进同一段可用内存里。

---

## 5. `phys_to_virt()` 是固定偏移直接映射

源码路径：[`kernel/include/arch/aarch64/arch/mmu.h`](../kernel/include/arch/aarch64/arch/mmu.h)

```c
#ifndef KBASE
#define KBASE              0xFFFFFF0000000000
#define PHYSICAL_ADDR_MASK (40)
#endif // KBASE

#define phys_to_virt(x) ((vaddr_t)((paddr_t)(x) + KBASE))
#define virt_to_phys(x) ((paddr_t)((vaddr_t)(x)-KBASE))
```

也就是说，buddy 管理的页虽然用内核虚拟地址访问，但本质上对应直接映射的物理内存：

```text
virtual = physical + KBASE
physical = virtual - KBASE
```

ASCII：

```text
physical address        kernel virtual address
0x00000000       --->   KBASE + 0x00000000
0x00100000       --->   KBASE + 0x00100000
0x3d000000       --->   KBASE + 0x3d000000
```

---

## 6. Buddy 分配器的数据结构

源码路径：[`kernel/include/mm/buddy.h`](../kernel/include/mm/buddy.h)

关键宏：

```c
/*
 * Supported Order: [0, BUDDY_MAX_ORDER).
 * The max allocated size (continous physical memory size) is
 * 2^(BUDDY_MAX_ORDER - 1) * 4K, i.e., 16M.
 */
#define BUDDY_PAGE_SIZE (0x1000)
#define BUDDY_MAX_ORDER (14UL)
```

这里代码实际含义是：

```text
order 范围: 0 .. 13
order n 块大小: 2^n * 4 KiB
最大 order 13: 2^13 * 4 KiB = 32 MiB
```

注释里写 `16M`，但以 `BUDDY_MAX_ORDER = 14` 和 `[0, BUDDY_MAX_ORDER)` 来算，最大 chunk 实际是 `32 MiB`。

每个物理页的元数据：

```c
/* `struct page` is the metadata of one physical 4k page. */
struct page {
        /* Free list */
        struct list_head node;
        /* Whether the correspond physical page is free now. */
        int allocated;
        /* The order of the memory chunck that this page belongs to. */
        int order;
        /* Used for ChCore slab allocator. */
        void *slab;
        /* The physical memory pool this page belongs to */
        struct phys_mem_pool *pool;
};
```

每个 order 一个 freelist：

```c
struct free_list {
        struct list_head free_list;
        u64 nr_free;
};
```

物理内存池：

```c
/* Disjoint physical memory can be represented by several phys_mem_pool. */
struct phys_mem_pool {
        u64 pool_start_addr;
        u64 pool_mem_size;
        u64 pool_phys_page_num;
        struct page *page_metadata;
        struct free_list free_lists[BUDDY_MAX_ORDER];
};
```

结构关系：

```text
struct phys_mem_pool

+--------------------+
| pool_start_addr    |----> buddy managed area start, kernel virtual address
| pool_mem_size      |
| pool_phys_page_num |
| page_metadata      |----> struct page array
| free_lists[0]      |----> chunks of 1 page
| free_lists[1]      |----> chunks of 2 pages
| free_lists[2]      |----> chunks of 4 pages
| ...                |
| free_lists[13]     |----> chunks of 8192 pages = 32 MiB
+--------------------+
```

---

## 7. Buddy 初始化：把所有页先释放进去，自动合并

源码路径：[`kernel/mm/buddy.c`](../kernel/mm/buddy.c)

`init_buddy()`：

```c
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
```

这里有个很典型的技巧：

```text
初始化时每个 page 先标成 allocated = 1
然后逐页调用 buddy_free_pages()
释放过程中 merge_page() 会尽可能与 buddy 合并
最终形成大块的空闲块，进入各级 free_list
```

初始化过程图：

```text
Step 1: metadata 清零

page_metadata:
+-----+-----+-----+-----+-----+-----+
| p0  | p1  | p2  | p3  | p4  | ... |
+-----+-----+-----+-----+-----+-----+

Step 2: 全部暂记为 allocated, order=0

p0 allocated=1 order=0
p1 allocated=1 order=0
...

Step 3: 逐页 buddy_free_pages()

free p0 -> free_lists[0]
free p1 -> 与 p0 合并 -> free_lists[1]
free p2 -> free_lists[0]
free p3 -> 与 p2 合并，再与 p0-p1 合并 -> free_lists[2]
...
```

---

## 8. buddy 的地址关系：用 XOR 找 buddy 块

源码路径：[`kernel/mm/buddy.c`](../kernel/mm/buddy.c)

```c
static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                    struct page *chunk)
{
        u64 chunk_addr;
        u64 buddy_chunk_addr;
        int order;

        /* Get the address of the chunk. */
        chunk_addr = (u64)page_to_virt(chunk);
        order = chunk->order;
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
```

公式：

```text
buddy_addr = chunk_addr XOR (1 << (order + 12))
```

因为 `BUDDY_PAGE_SIZE = 4 KiB = 2^12`，所以：

```text
order 0: chunk size = 2^(0+12) = 4 KiB，翻转 bit 12
order 1: chunk size = 2^(1+12) = 8 KiB，翻转 bit 13
order 2: chunk size = 2^(2+12) = 16 KiB，翻转 bit 14
...
```

ASCII 示例：

```text
order 2，chunk size = 16 KiB

base + 0x00000  chunk A
base + 0x04000  chunk B

A 的 buddy = A ^ 0x4000 = B
B 的 buddy = B ^ 0x4000 = A
```

`get_buddy_chunk()` 还会检查算出来的 buddy 地址是否仍在当前 `phys_mem_pool` 范围内，避免跨 pool 合并。

---

## 9. 分配流程：从合适 order 取块，不够就向上找再 split

源码路径：[`kernel/mm/buddy.c`](../kernel/mm/buddy.c)

```c
struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
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
}
```

`split_page()`：

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

        /* Split: decrease order, put buddy into lower free list */
        page->order--;
        buddy = get_buddy_chunk(pool, page);
        BUG_ON(buddy == NULL);
        buddy->order = page->order;
        buddy->allocated = 0;
        list_add(&buddy->node, &pool->free_lists[buddy->order].free_list);
        pool->free_lists[buddy->order].nr_free++;

        /* Recursively split until target order */
        return split_page(pool, order, page);
}
```

分配图：

```text
请求 order = 1，即 8 KiB

free_lists[1] 为空
free_lists[2] 为空
free_lists[3] 有一个 32 KiB 块

32 KiB order 3
+-------------------------------+
|                               |
+-------------------------------+

split to order 2:
+---------------+---------------+
| keep splitting| buddy -> FL[2]|
+---------------+---------------+

split to order 1:
+-------+-------+
| alloc | FL[1] |
+-------+-------+

返回左边 order 1 块，右边碎片进入对应 freelist。
```

---

## 10. 释放流程：标记为空闲，然后尽可能向上合并

源码路径：[`kernel/mm/buddy.c`](../kernel/mm/buddy.c)

```c
void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        /* Mark as free */
        BUG_ON(page->allocated == 0);
        page->allocated = 0;

        /* Merge with buddy as much as possible */
        page = merge_page(pool, page);

        BUG_ON(page->allocated);

        /* Insert into the appropriate free list */
        list_add(&page->node, &pool->free_lists[page->order].free_list);
        pool->free_lists[page->order].nr_free++;
}
```

`merge_page()`：

```c
static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
        struct page *buddy;

        /* Cannot merge beyond max order */
        BUG_ON(page->order > BUDDY_MAX_ORDER - 1 || page->allocated);
        if (page->order == BUDDY_MAX_ORDER - 1) {
                return page;
        }

        buddy = get_buddy_chunk(pool, page);

        /* Buddy must exist, be free, and same order */
        if (buddy == NULL || buddy->allocated || buddy->order < page->order) {
                return page;
        }

        BUG_ON(buddy->order > page->order);
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
}
```

释放合并图：

```text
释放一个 order 0 页

Case A: buddy 已分配
+----+----+
|free|used|
+----+----+
结果：free 进入 free_lists[0]

Case B: buddy 空闲且同 order
+----+----+
|free|free|
+----+----+
结果：合并为 order 1
+---------+
|  free   |
+---------+
然后继续尝试和 order 1 的 buddy 合并
```

---

## 11. `struct page` 与虚拟地址互转

源码路径：[`kernel/mm/buddy.c`](../kernel/mm/buddy.c)

```c
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
```

```c
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
```

关系图：

```text
page_metadata array                         managed pages

+---------+---------+---------+             +---------+---------+---------+
| page[0] | page[1] | page[2] |   <----->   | 4K #0   | 4K #1   | 4K #2   |
+---------+---------+---------+             +---------+---------+---------+
     ^                                         ^
     |                                         |
page_metadata                          pool_start_addr

page_to_virt(page[i]) = pool_start_addr + i * 4096
virt_to_page(addr)    = page_metadata + (addr - pool_start_addr) / 4096
```

---

## 12. 上层如何使用 buddy

源码路径：[`kernel/mm/kmalloc.c`](../kernel/mm/kmalloc.c)

大块 `kmalloc()` 走 buddy，小块走 slab：

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

`get_pages()` 遍历所有物理内存池：

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

释放：

```c
void free_pages(void *addr)
{
        struct page *p_page;
        p_page = virt_to_page(addr);
        buddy_free_pages(p_page->pool, p_page);
}
```

所以从上层看：

```text
kmalloc(size)
    |
    +-- size <= slab max -> slab allocator
    |
    +-- size > slab max
            |
            v
        get_pages(order)
            |
            v
        buddy_get_pages(&global_mem[i], order)
            |
            v
        page_to_virt(page)
```

---

## 13. 是否管理“所有物理内存”？

答案要分层说：

### 13.1 不是全机器地址空间

它不会管理：

```text
0x00000000 .. img_end              kernel image / reserved low region
metadata 区域                       struct page 数组自身
alignment padding                   页对齐浪费
0x3d000000 .. 0x3f000000            GPU reserved 32 MiB
0x3f000000 .. 0x40000000            peripherals / MMIO
0x40000000 ..                       local peripherals / device space
```

### 13.2 是管理“源码配置认为可用的物理内存”

`parse_mem_map()` 给出：

```text
[img_end, 0x3d000000)
```

`mm_init()` 再从这段里切掉 metadata 和 padding：

```text
[img_end, 0x3d000000)

+----------------------+-------------------+-------------------------------+
| struct page metadata | alignment padding | buddy managed physical pages  |
+----------------------+-------------------+-------------------------------+
```

buddy 最终管理的是最后一段：

```text
[start_vaddr, start_vaddr + npages * 4096)
```

其中：

```c
npages = (free_mem_end - free_mem_start)
         / (PAGE_SIZE + sizeof(struct page));
start_vaddr = ROUND_UP(free_mem_start + npages * sizeof(struct page),
                       PAGE_SIZE);
```

### 13.3 不是链接脚本划好的 heap

链接脚本没有声明类似：

```ld
.heap : { ... }
```

buddy 的元数据和实际页都来自 `parse_mem_map()` 给出的可用物理内存。链接脚本只提供 `img_end`，用于避免覆盖内核镜像本身。

---

## 14. 最终回答

### Q1：这个项目怎么得知自己拥有多少内存？

通过 raspi3 平台代码里的 `parse_mem_map()`。该函数使用硬编码常量和链接脚本符号构造内存范围：

```text
start = ROUND_UP(&img_end, PAGE_SIZE)
end   = 0x3f000000 - 32 MiB
```

不是运行时探测。

### Q2：是内存探测还是 hardcode 写死在源码里？

是 hardcode。`0x3f000000`、`32 MiB GPU reserved`、`PHYS_MEM_POOL_SIZE = 1` 都在源码中写死。`img_end` 是链接脚本生成的 kernel image 结束地址。

### Q3：物理页分配器管理一块链接脚本划好的内存，还是管理所有物理内存？

都不是完全准确。它管理的是：

```text
平台代码声明可用的物理内存区间
减去 struct page metadata
减去 alignment padding
```

这更接近“管理所有被源码配置为可用的物理内存”，而不是“管理链接脚本里固定划的一块 heap”。

### Q4：一句话总结

```text
ChCore-Lab raspi3 的内存大小来自平台 hardcode 的物理内存布局；buddy 分配器在 mm_init() 中把该可用区间的前部用作 page metadata，其余全部作为页帧池管理。
```
