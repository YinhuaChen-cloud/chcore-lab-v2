# ChCore Lab2: 伙伴系统（Buddy System）设计与实现分析

> 源码位置：`kernel/mm/buddy.c`, `kernel/include/mm/buddy.h`, `kernel/mm/mm.c`

---

## 1. 核心数据结构

### 1.1 `struct page` — 每页的元数据

```c
// kernel/include/mm/buddy.h:30
struct page {
        struct list_head node;       // 用于链接到空闲链表
        int allocated;               // 是否已分配
        int order;                   // 当前页所属块的 order
        void *slab;                  // slab 分配器使用（与本篇无关）
        struct phys_mem_pool *pool;  // 所属的物理内存池
};
```

每个物理页面（4KB）都对应一个 `struct page` 结构体。**元数据区位于可用物理内存之前**。

### 1.2 `struct free_list` — 每个 order 的空闲链表头

```c
// kernel/include/mm/buddy.h:43
struct free_list {
        struct list_head free_list;  // 双向循环链表头
        u64 nr_free;                 // 该 order 下有多少个空闲块
};
```

### 1.3 `struct phys_mem_pool` — 物理内存池

```c
// kernel/include/mm/buddy.h:49
struct phys_mem_pool {
        u64 pool_start_addr;          // 可用内存的起始虚拟地址
        u64 pool_mem_size;            // 池的总大小（字节）
        u64 pool_phys_page_num;       // 总页数（仅用于单元测试）
        struct page *page_metadata;   // 元数据区起始地址
        struct free_list free_lists[BUDDY_MAX_ORDER];  // 14 个空闲链表
};
```

### 1.4 常量定义

```c
// kernel/include/mm/buddy.h
#define BUDDY_PAGE_SIZE       (0x1000)   // 4KB
#define BUDDY_MAX_ORDER       (14UL)     // 支持 order 0~13
#define BUDDY_PAGE_SIZE_ORDER (12)       // 1 << 12 = 4096
```

**最大可分配连续内存**：`2^13 * 4KB = 32KB * 4KB = 32 * 4KB = 128KB? 不对...`  
`2^13 * 4KB = 8192 * 4KB = 32768KB = 32MB`。  
源码注释写 16M，但 `2^13 * 4KB = 32MB`。可能是注释有误，或我算错。`2^13 = 8192`，`8192 * 4096 = 33554432 = 32MB`。

---

## 2. 内存池整体布局（ASCII 图）

```
                        低地址
                        |
                        |    page_metadata 区域               | 对齐填充 | 可用物理内存区域
                        |  (npages * sizeof(struct page))     |  (pad)   |  (npages * 4KB)
                        |                                     |          |
                        v                                     v          v
                        +-------------------------------------+----------+------------------...--+
  0 ~ free_mem_start    | page[0] | page[1] | ... | page[N-1] |   ...    |  4K页0 | 4K页1 | ...  |
is kernel .text & .data +-------------------------------------+----------+------------------...--+
                        ^                                                ^
                        |                                                |
                        page_metadata (free_mem_start)            pool_start_addr (start_addr)
                                                                    = ROUND_UP(free_mem_start + npages*sizeof(page))
```

> 引自 `kernel/mm/mm.c:29-66`

```c
// mm.c:51-55
npages = (free_mem_end - free_mem_start) / (PAGE_SIZE + sizeof(struct page));
start_vaddr = ROUND_UP(free_mem_start + npages * sizeof(struct page), PAGE_SIZE);
```

**关键点**：内核在初始化时把整块可用内存划分为两部分：前面存放 `struct page` 元数据，后面才是真正的可用物理页。元数据区**不参与伙伴系统管理**，管理对象是后面的物理页。

---

## 3. 伙伴系统初始化：`init_buddy`

```c
// buddy.c:25
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, u64 page_num)
{
        // 1. 初始化内存池基本信息
        pool->pool_start_addr = start_addr;
        pool->page_metadata = start_page;
        pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;

        // 2. 初始化 14 个空闲链表（全部为空）
        for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
                pool->free_lists[order].nr_free = 0;
                init_list_head(&(pool->free_lists[order].free_list));
        }

        // 3. 清零所有页元数据
        memset((char *)start_page, 0, page_num * sizeof(struct page));

        // 4. 把每个页标记为 "allocated=1, order=0"，并设置 pool 指针
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                page->allocated = 1;   // 标记为已分配（这样 free 时会合并）
                page->order = 0;
                page->pool = pool;
        }

        // 5. 逐页释放到伙伴系统中（会触发自动合并！）
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                buddy_free_pages(pool, page);
        }
}
```

**为什么初始化时要把 `allocated` 设为 1？**

因为 `buddy_free_pages` 会检查 `BUG_ON(page->allocated == 0)`（`buddy.c:243`）。只有标记为 allocated 的页才能被释放。这是一个**内部不变式（invariant）**：free 操作只接收已分配的页。

**初始化时逐页释放的智慧**：

虽然一开始把所有页标记为 order=0，但逐页调用 `buddy_free_pages` 时，伙伴系统会**自动尝试合并相邻的空闲伙伴**。最终，物理内存会被合并成尽可能大的连续块，挂在对应 order 的空闲链表上。

---

## 4. 核心操作

### 4.1 伙伴查找：`get_buddy_chunk`

这是伙伴系统的**核心数学**，用异或运算找到伙伴：

```c
// buddy.c:65
static struct page *get_buddy_chunk(struct phys_mem_pool *pool, struct page *chunk)
{
        chunk_addr = (u64)page_to_virt(chunk);
        order = chunk->order;
        // 关键：用异或找到伙伴地址！
        buddy_chunk_addr = chunk_addr ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));
        // ... 边界检查
        return virt_to_page((void *)buddy_chunk_addr);
}
```

**异或原理**：两个 order 为 `k` 的伙伴块，地址相差 `2^k * 4KB`。异或的第 `(k+12)` 位会翻转，正好得到伙伴地址。

```
示例：order = 1，块大小 = 8KB (2^1 * 4KB)

chunk_addr  = 0x10000  (64KB)
             = 0b 0001 0000 0000 0000 0000

buddy_addr  = 0x10000 ^ (1 << (1 + 12))
             = 0x10000 ^ 0x2000
             = 0x12000  (72KB)
             = 0b 0001 0010 0000 0000 0000

             ^^^^第13位翻转（0->1）^^^^
```

**地址 vs 页索引**：

```c
// buddy.c:260
void *page_to_virt(struct page *page) {
        addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE + pool->pool_start_addr;
        return (void *)addr;
}

// buddy.c:272
struct page *virt_to_page(void *ptr) {
        page = pool->page_metadata + (((u64)addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);
        return page;
}
```

页索引到地址的转换很简单：`index * 4KB + start_addr`。这是**线性映射**，因为伙伴系统管理的物理页是连续的。

---

### 4.2 分配页：`buddy_get_pages`

```c
// buddy.c:142
struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
        if(order >= BUDDY_MAX_ORDER || order < 0)
                return NULL;

        // 找到满足要求的最小 order
        for (cur_order = order; cur_order < BUDDY_MAX_ORDER; cur_order++) {
                list = &pool->free_lists[cur_order];
                if (list->nr_free > 0) {
                        // 从空闲链表取出一个块
                        page = list_entry(list->free_list.next, struct page, node);
                        list_del(&page->node);
                        pool->free_lists[page->order].nr_free--;
                        // 拆分到目标 order
                        page = split_page(pool, order, page);
                        page->allocated = 1;
                        return page;
                }
        }
        return NULL;
}
```

**分配策略**：First-fit，从目标 order 开始向上扫描，找到第一个非空链表。

---

### 4.3 拆分页：`split_page`

```c
// buddy.c:100
static struct page *split_page(struct phys_mem_pool *pool, u64 order, struct page *page)
{
        BUG_ON(page == NULL);
        BUG_ON(page->order < order);
        BUG_ON(page->allocated);

        if (page->order == order) {
                return page;   // 已经满足要求
        }

        // 1. 降阶
        page->order--;

        // 2. 找到伙伴
        buddy = get_buddy_chunk(pool, page);
        buddy->order = page->order;
        buddy->allocated = 0;   // 标记为空闲

        // 3. 把伙伴放入低一阶的空闲链表
        list_add(&buddy->node, &pool->free_lists[buddy->order].free_list);
        pool->free_lists[buddy->order].nr_free++;

        // 4. 递归拆分
        return split_page(pool, order, page);
}
```

**拆分过程图解**（分配一个 order=1 的块，但 free_lists[2] 有一个可用块）：

```
初始状态：order=2 的块 [A:B:C:D] 在 free_lists[2] 中
（每个字母代表一个 4KB 页，4页 = order=2 = 16KB）

free_lists:
  [0]: empty
  [1]: empty
  [2]: [A:B:C:D]  <-- 这里有块
  ...

Step 1: split_page(order=1, page=[A:B:C:D])
  page->order 从 2 降到 1  [A:B]
  伙伴 chunk = [C:D]，放入 free_lists[1]
  递归: split_page(order=1, page=[A:B])

        free_lists:
          [0]: empty
          [1]: [C:D]       <-- 伙伴挂在这里
          [2]: empty

Step 2: page->order == 1 == target_order，返回 [A:B]

最终结果：
  返回 page [A:B] (order=1, 8KB)
  free_lists[1] 中有 [C:D] (order=1, 8KB)
```

**优化注意**：`split_page` 在 `buddy_get_pages` 中调用时，**不会先把大块从链表摘除再拆分**。原因是：

```c
// buddy.c:117-119 (注释)
/* We do not remove page->node from free list here */
/* Otherwise, we need to add page->node to the free list after splitting, */
/* which increase unnecessary overhead */
```

实际上在 `buddy_get_pages` 中**已经摘除了**（`list_del` 在 `buddy_get_pages:166`），所以拆分后的 page 不需要再挂回链表——它的 `node` 已经被从原链表移除了。

---

### 4.4 释放页：`buddy_free_pages`

```c
// buddy.c:234
void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        BUG_ON(page->allocated == 0);   // 只能释放已分配的页
        page->allocated = 0;

        // 先合并，再挂入链表（减少不必要的链表操作）
        page = merge_page(pool, page);

        list_add(&page->node, &pool->free_lists[page->order].free_list);
        pool->free_lists[page->order].nr_free++;
}
```

**关键优化**：先合并再入链表，避免"入链表→出链表（合并时）→再入链表"的冗余操作。

---

### 4.5 合并页：`merge_page`

```c
// buddy.c:187
static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
        if (page->order == BUDDY_MAX_ORDER - 1) {
                return page;   // 已到最大 order，无法继续合并
        }

        buddy = get_buddy_chunk(pool, page);

        // 伙伴必须存在、空闲、且 order 相同
        if (buddy == NULL || buddy->allocated || buddy->order < page->order) {
                return page;
        }

        BUG_ON(buddy->order > page->order);
        BUG_ON(buddy->order != page->order);

        // 从伙伴所在链表摘除
        list_del(&buddy->node);
        pool->free_lists[buddy->order].nr_free--;

        // 取低地址页作为合并后的块
        if (buddy < page) {
                page = buddy;
        }
        page->order++;

        // 递归尝试继续合并
        return merge_page(pool, page);
}
```

**合并条件**：
1. 当前块不是最大 order
2. 伙伴块存在（在内存池范围内）
3. 伙伴块是空闲的（`allocated == 0`）
4. 伙伴块的 order **等于**当前块的 order

**为什么检查 `buddy->order < page->order`？**

因为伙伴可能已经被拆分成了更小的块，此时不能合并。例如：

```
内存布局：order=1 的块 [A:B]，其伙伴 [C:D] 已经被拆分成两个 order=0 块

如果释放 [A]（order=0），其伙伴是 [B]（order=0，但可能已经被分配）
如果释放 [A:B]（order=1），其伙伴 [C:D] 现在是两个独立的 order=0 块
  → buddy->order = 0 < page->order = 1，不能合并
```

**合并过程图解**：

```
场景：释放 order=0 的页 A，其伙伴 B 也是 order=0 且空闲

初始：
  free_lists[0]: [B]   (B 是空闲的)
  正在释放: [A]

Step 1: merge_page(page=A)
  buddy(B) 存在、空闲、order=0 == page->order=0 ✓
  从 free_lists[0] 摘除 B
  A 和 B 合并，取低地址（假设 A < B）
  page = A, page->order = 1  [A:B]

  free_lists[0]: empty

Step 2: merge_page(page=[A:B]) 递归
  检查 [A:B] 的伙伴 [C:D]
  假设 [C:D] 也是 order=1 且空闲
  合并成 [A:B:C:D]，order=2

  ...继续递归直到无法合并...

最终：[A:B:C:D:E:F:G:H...] 挂在尽可能高的 order 链表上
```

---

## 5. 完整流程：分配 + 释放

### 5.1 分配流程

```
用户请求: buddy_get_pages(pool, order=1)  // 需要 8KB

        +-----------------------------------+
        | 检查 order 是否合法 [0, 14)       |
        +-----------------------------------+
                     |
                     v
        +-----------------------------------+
        | 从 order=1 开始扫描 free_lists    |
        | free_lists[1] 为空？继续          |
        | free_lists[2] 有块？使用它！      |
        +-----------------------------------+
                     |
                     v
        +-----------------------------------+
        | 从 free_lists[2] 摘除大块         |
        | list_del(&page->node)             |
        | nr_free--                        |
        +-----------------------------------+
                     |
                     v
        +-----------------------------------+
        | split_page(pool, order=1, page)   |
        |  大块 order=2 → 拆成两个 order=1  |
        |  一半返回给用户，另一半挂入        |
        |  free_lists[1]                    |
        +-----------------------------------+
                     |
                     v
        +-----------------------------------+
        | 标记 allocated=1                  |
        | 返回 page 给用户                  |
        +-----------------------------------+
```

### 5.2 释放流程

```
用户释放: buddy_free_pages(pool, page)  // page 是 order=1

        +-----------------------------------+
        | 检查 page->allocated == 1         |
        | 标记 allocated = 0                |
        +-----------------------------------+
                     |
                     v
        +-----------------------------------+
        | merge_page(pool, page)            |
        | 找伙伴 → 检查条件 → 合并          |
        | 递归直到无法合并                   |
        +-----------------------------------+
                     |
                     v
        +-----------------------------------+
        | 把合并后的块挂入对应 order 的链表  |
        | list_add(&page->node, ...)        |
        | nr_free++                        |
        +-----------------------------------+
```

---

## 6. 空闲链表状态变化示例

假设初始有 4 个连续的 4KB 页（共 16KB），初始化后全部空闲：

```
初始化后（全部合并为 order=2 块）：
  free_lists[0]: empty
  free_lists[1]: empty
  free_lists[2]: [0:1:2:3]

分配 order=1（8KB）：
  free_lists[0]: empty
  free_lists[1]: [2:3]      <-- 拆分后剩余的一半
  free_lists[2]: empty
  返回: [0:1] (allocated)

再分配 order=0（4KB）：
  free_lists[0]: [3]        <-- 从 [2:3] 拆分
  free_lists[1]: empty
  free_lists[2]: empty
  返回: [2] (allocated)

释放 [0:1]（order=1）：
  free_lists[0]: [3]
  free_lists[1]: [0:1]      <-- 伙伴 [2:3] 不完整（[2]已分配），无法合并
  free_lists[2]: empty

释放 [2]（order=0）：
  [2] 的伙伴 [3] 在 free_lists[0] 中，order 相同 → 合并为 [2:3] (order=1)
  然后 [0:1] 的伙伴 [2:3] 现在也是 order=1 且空闲 → 合并为 [0:1:2:3] (order=2)

  free_lists[0]: empty
  free_lists[1]: empty
  free_lists[2]: [0:1:2:3]  <-- 完全恢复！
```

---

## 7. 内存碎片分析

伙伴系统通过**强制对齐和分裂/合并**，有效减少了**外部碎片**（无法分配大块连续内存），但引入了**内部碎片**（分配比请求稍大的块）。

```
内部碎片示例：
  请求 5KB → 分配 order=1（8KB）→ 浪费 3KB
  请求 12KB → 分配 order=2（16KB）→ 浪费 4KB
```

ChCore 通过 **slab 分配器**（在 `kernel/mm/slab.c`）处理小内存分配（≤8KB），避免内部碎片。

```c
// kernel/mm/kmalloc.c:47
void *kmalloc(size_t size)
{
        if (size <= _SIZE) {       // <= 8KB
                return alloc_in_slab(size);   // 用 slab
        }
        // 大内存：用伙伴系统
        order = size_to_page_order(size);
        return get_pages(order);
}
```

---

## 8. 关键源码佐证速查

| 功能 | 文件 | 行号 |
|------|------|------|
| 常量定义（`BUDDY_MAX_ORDER=14`） | `kernel/include/mm/buddy.h` | 24 |
| `struct page` 定义 | `kernel/include/mm/buddy.h` | 30 |
| `struct phys_mem_pool` 定义 | `kernel/include/mm/buddy.h` | 49 |
| 内存池初始化入口 | `kernel/mm/mm.c` | 36 |
| 伙伴系统初始化 | `kernel/mm/buddy.c` | 25 |
| 页分配 | `kernel/mm/buddy.c` | 142 |
| 页拆分 | `kernel/mm/buddy.c` | 100 |
| 页释放 | `kernel/mm/buddy.c` | 234 |
| 页合并 | `kernel/mm/buddy.c` | 187 |
| 伙伴查找（异或） | `kernel/mm/buddy.c` | 65 |
| 页地址转换 | `kernel/mm/buddy.c` | 260, 272 |
| 空闲内存统计 | `kernel/mm/buddy.c` | 295 |
| 单元测试 | `kernel/mm/buddy.c` | 320 |
| 链表操作 | `kernel/include/common/list.h` | 18-46 |
| kmalloc 封装 | `kernel/mm/kmalloc.c` | 47 |

---

## 9. 总结

ChCore Lab2 的伙伴系统实现是一个**经典且简洁的教科书式实现**：

1. **线性元数据**：每个物理页一个 `struct page`，放在物理内存之前，通过索引快速寻址。
2. **异或找伙伴**：利用地址对齐特性，用 `addr ^ (1 << (order+12))` 快速定位伙伴，无需额外存储。
3. **延迟合并**：释放时递归向上合并，尽可能恢复大块连续内存。
4. **按需拆分**：分配时从大到小拆分，避免浪费。
5. **链表优化**：合并后再入链表，减少冗余操作。
6. **与 slab 配合**：小对象走 slab，大对象走伙伴，兼顾碎片和效率。

整个实现约 **250 行 C 代码**（不含测试），结构清晰，非常适合教学。
