# ChCore Lab2: 伙伴系统（Buddy System）自顶向下分析

> 源码位置：`kernel/mm/buddy.c`, `kernel/include/mm/buddy.h`

---

## 1. 伙伴系统概览（顶层设计）

### 1.1 伙伴系统在内存管理中的位置

ChCore 的内存管理采用**分层架构**：

```
┌─────────────────────────────────────────────┐
│  上层：kmalloc / kfree                       │  ← 内核通用接口
│  （小对象用 slab，大对象用伙伴系统）          │
├─────────────────────────────────────────────┤
│  中层：slab 分配器                            │  ← 处理 ≤8KB 的小对象
│  （基于伙伴系统分配的页构建缓存）               │
├─────────────────────────────────────────────┤
│  底层：伙伴系统 (Buddy System)                │  ← 本文分析的对象
│  （管理物理页的分配与回收，处理 ≥4KB 请求）    │
├─────────────────────────────────────────────┤
│  最底层：物理内存池 (phys_mem_pool)           │  ← 实际硬件内存
└─────────────────────────────────────────────┘
```

**伙伴系统的职责**：以 2 的幂次方个连续物理页（4KB 的倍数）为单位，对外提供分配与回收服务。它能有效减少**外部碎片**（即避免大量不连续的小空闲块导致无法分配大块连续内存）。

### 1.2 核心设计思想

伙伴系统的核心思想非常简单：**将空闲内存按块大小分类管理，分配时拆分大块，释放时合并伙伴**。

具体来说：

1. **按 order 分类**：内存块按大小分为 `order = 0, 1, 2, ...` 等级别
   - `order = 0`：1 页 = 4KB
   - `order = 1`：2 页 = 8KB
   - `order = 2`：4 页 = 16KB
   - ...
   - `order = 13`：8192 页 = 32MB（ChCore 支持的最大块）

2. **每个 order 维护一个空闲链表**：所有该大小的空闲块挂在一个双向链表中

3. **分配策略（拆分）**：
   - 需要 `order = k` 的块时，先检查 `free_lists[k]`
   - 如果没有，向上查找更大的 order
   - 找到后**拆分（split）**：一分为二，一半继续拆分（如果需要），另一半挂入低一阶的空闲链表

4. **释放策略（合并）**：
   - 释放一个块时，检查它的**伙伴（buddy）**是否也空闲
   - 如果伙伴空闲且大小相同，则**合并（merge/coalesce）**成一个更大的块
   - 递归向上合并，直到无法继续

### 1.3 "伙伴"的定义

两个内存块是**伙伴**，当且仅当：
- 它们大小相同（相同的 order）
- 它们地址相邻，且合并后形成的更大块是按该大小对齐的

**伙伴地址计算**（这是伙伴系统的精妙之处）：

```
给定块地址 addr 和 order k：
buddy_addr = addr ^ (1 << (k + 12))

// 其中 12 是因为页大小 4KB = 2^12，
// 所以 order=k 的块大小 = 2^k * 4KB = 2^(k+12) 字节
```

用**异或运算**的原因是：两个伙伴块的地址只在第 `(k+12)` 位上不同，其余位完全相同。翻转这一位就得到伙伴地址。

---

## 2. 数据结构组织

### 2.1 三层数据结构关系

```
┌─────────────────────────────────────────────────────────────┐
│                    struct phys_mem_pool                       │
│  ┌─────────────────┐  ┌─────────────────────────────────────┐ │
│  │  元数据指针      │  │  free_lists[0] ~ free_lists[13]      │ │
│  │  page_metadata ──┼─→│  ┌──────────────────────────────┐   │ │
│  │                 │  │  │ free_list (链表头)            │   │ │
│  │  pool_start_addr│  │  │ nr_free (该order空闲块数)      │   │ │
│  │                 │  │  └──────────────────────────────┘   │ │
│  │  pool_mem_size  │  │  ... (共 BUDDY_MAX_ORDER=14 个)     │ │
│  └─────────────────┘  └─────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
         │
         │ page_metadata 指向
         v
┌─────────────────────────────────────────────────────────────┐
│              struct page page_metadata[N]                     │
│  ┌──────────┐  ┌──────────┐        ┌──────────┐             │
│  │ page[0]  │  │ page[1]  │  ...   │ page[N-1]│             │
│  │  node    │  │  node    │        │  node    │             │
│  │ allocated│  │ allocated│        │ allocated│             │
│  │  order   │  │  order   │        │  order   │             │
│  │  pool    │  │  pool    │        │  pool    │             │
│  └──────────┘  └──────────┘        └──────────┘             │
│       ↑                                              ↑      │
└───────┼──────────────────────────────────────────────┼──────┘
        │                                              │
        └──────── 通过 node 字段链接到 free_lists ─────┘
```

### 2.2 `struct page` — 每页的元数据

```c
// kernel/include/mm/buddy.h:30
struct page {
        struct list_head node;       // 用于链接到某个 free_list 的双向链表
        int allocated;               // 1=已分配，0=空闲
        int order;                   // 当前页所属内存块的 order
        void *slab;                  // slab 分配器使用（本文不涉及）
        struct phys_mem_pool *pool;  // 所属的物理内存池
};
```

**关键理解**：`struct page` 不是**一个**物理页，而是**一个物理页的描述符（metadata）**。所有 `struct page` 集中存放在内存池的元数据区，与实际的物理页是**一一对应**关系。

### 2.3 `struct free_list` — 每个 order 的空闲链表

```c
// kernel/include/mm/buddy.h:43
struct free_list {
        struct list_head free_list;  // 双向循环链表头
        u64 nr_free;                 // 该 order 下有多少个空闲块
};
```

### 2.4 `struct phys_mem_pool` — 物理内存池

```c
// kernel/include/mm/buddy.h:49
struct phys_mem_pool {
        u64 pool_start_addr;          // 可用物理内存的起始虚拟地址
        u64 pool_mem_size;            // 池的总大小（字节）
        u64 pool_phys_page_num;       // 总页数（仅用于单元测试）
        struct page *page_metadata;   // 元数据区起始地址
        struct free_list free_lists[BUDDY_MAX_ORDER];  // 14 个空闲链表
};
```

**为什么需要 `phys_mem_pool`？** 实际物理内存可能是不连续的（如 NUMA 架构或有保留区域），ChCore 用多个 `phys_mem_pool` 分别管理不连续的物理内存区域。

### 2.5 常量定义

```c
// kernel/include/mm/buddy.h
#define BUDDY_PAGE_SIZE       (0x1000)    // 4KB
#define BUDDY_MAX_ORDER       (14UL)      // 支持 order 0~13
#define BUDDY_PAGE_SIZE_ORDER (12)        // 1 << 12 = 4096
```

最大可分配连续内存：`2^13 * 4KB = 8192 * 4KB = 32MB`。

---

## 3. 内存池布局

### 3.1 物理内存池的线性布局

一个物理内存池在虚拟地址空间中这样排列：

```
低地址
│
│    元数据区 (page_metadata)          │ 对齐填充 │    可用物理内存区
│  (npages × sizeof(struct page))      │  (pad)   │  (npages × 4KB)
│                                      │          │
v                                      v          v
+──────────────────────────────────────+──────────+─────────────────...──+
│ page[0] │ page[1] │ ... │ page[N-1]  │   ...    │ 4K页0 │ 4K页1 │ ...   │
+──────────────────────────────────────+──────────+─────────────────...──+
↑                                      ↑          ↑
│                                      │          │
page_metadata                     对齐边界    pool_start_addr
(free_mem_start)                            (start_vaddr)
                                              = ROUND_UP(free_mem_start
                                                + npages×sizeof(page),
                                                PAGE_SIZE)
```

> 初始化逻辑在 `kernel/mm/mm.c:29-66`

```c
// mm.c:51-55
npages = (free_mem_end - free_mem_start) / (PAGE_SIZE + sizeof(struct page));
start_vaddr = ROUND_UP(free_mem_start + npages * sizeof(struct page), PAGE_SIZE);
```

**关键点**：
1. 内核把整块可用内存划分为两部分：前面存放 `struct page` 元数据数组，后面是真正的可用物理页
2. 元数据区**本身不参与伙伴系统的管理**
3. 伙伴系统管理的对象是后面的物理页
4. 元数据区和物理页之间可能有填充（为了按页对齐）

### 3.2 页索引与物理地址的转换

由于元数据数组和物理页是**线性一一对应**的，可以通过简单算术在两者之间转换：

```c
// buddy.c:260
// page 结构体指针 → 物理页虚拟地址
void *page_to_virt(struct page *page) {
        addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE + pool->pool_start_addr;
        return (void *)addr;
}

// buddy.c:272
// 物理页虚拟地址 → page 结构体指针
struct page *virt_to_page(void *ptr) {
        // 先找到属于哪个 pool
        for (i = 0; i < physmem_map_num; ++i) {
                if (addr >= global_mem[i].pool_start_addr
                    && addr < global_mem[i].pool_start_addr + global_mem[i].pool_mem_size) {
                        pool = &global_mem[i];
                        break;
                }
        }
        page = pool->page_metadata + ((addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);
        return page;
}
```

---

## 4. 核心操作流程（算法层面）

### 4.1 初始化流程

```
输入: pool, start_page, start_addr, page_num

Step 1: 设置内存池基本信息
        pool->pool_start_addr = start_addr
        pool->page_metadata = start_page
        pool->pool_mem_size = page_num * 4KB

Step 2: 初始化 14 个空闲链表（全部为空）
        for order = 0 to 13:
                free_lists[order].nr_free = 0
                初始化链表头

Step 3: 清零所有页元数据
        memset(start_page, 0, page_num * sizeof(struct page))

Step 4: 标记每个页为"已分配"并设置 pool 指针
        for i = 0 to page_num-1:
                page[i].allocated = 1    // 标记为已分配
                page[i].order = 0
                page[i].pool = pool

Step 5: 逐页释放（触发自动合并！）
        for i = 0 to page_num-1:
                buddy_free_pages(pool, &page[i])
                // 每次释放都会尝试与邻居合并
                // 最终形成尽可能大的连续空闲块
```

**为什么初始化时 `allocated = 1`？**

因为 `buddy_free_pages` 会检查 `BUG_ON(page->allocated == 0)`——它只接受已分配的页。这是一个**不变式（invariant）**：free 操作的对象必须是已分配的。

**逐页释放的智慧**：虽然每页初始 order=0，但释放时会自动尝试合并相邻的伙伴块。例如第 0 页释放时（无伙伴可合），第 1 页释放时与第 0 页合并成 order=1，第 2 页释放时再与合并块继续合并... 最终所有连续内存会合并成尽可能大的块。

### 4.2 分配流程

```
输入: pool, order（请求的大小等级）
输出: 分配的 struct page*，或 NULL

Step 1: 检查 order 合法性 [0, BUDDY_MAX_ORDER)
        不合法则返回 NULL

Step 2: 从请求 order 开始向上扫描空闲链表
        for cur_order = order; cur_order < BUDDY_MAX_ORDER; cur_order++:
                if free_lists[cur_order] 非空:
                        找到一个可用大块，跳出循环
        如果全空，返回 NULL

Step 3: 从找到的链表中取出一个块
        page = 链表第一个元素
        从链表中摘除
        nr_free--

Step 4: 如果需要，拆分到目标 order
        while page->order > 目标 order:
                // 拆分成两个 order-1 的块
                page->order--
                buddy = 找到 page 的伙伴
                buddy->order = page->order
                buddy->allocated = 0
                把 buddy 挂入 free_lists[buddy->order]
                nr_free++
                // page 继续循环（可能还需继续拆分）

Step 5: 标记为已分配
        page->allocated = 1
        返回 page
```

**分配策略**：First-fit，从目标 order 开始向上扫描，找到第一个非空链表。

### 4.3 释放流程

```
输入: pool, page（要释放的页）

Step 1: 检查 page 确实已分配
        BUG_ON(page->allocated == 0)
        page->allocated = 0

Step 2: 尝试与伙伴合并（递归）
        while True:
                if page->order == 最大 order:
                        break  // 无法继续合并

                buddy = 找到 page 的伙伴

                if buddy 不存在 或 buddy 已分配 或 buddy->order < page->order:
                        break  // 无法合并

                // 伙伴存在、空闲、且 order 相同
                BUG_ON(buddy->order != page->order)  // 安全检查

                // 从伙伴所在链表摘除
                从 free_lists[buddy->order] 摘除 buddy
                nr_free--

                // 合并：取低地址页作为合并后的块
                if buddy 的地址 < page 的地址:
                        page = buddy
                page->order++

                // 继续尝试向上合并

Step 3: 把合并后的块挂入对应 order 的空闲链表
        插入 free_lists[page->order]
        nr_free++
```

**关键优化**：先合并再入链表。如果先挂入链表再合并，会导致"入链表→出链表→再入链表"的冗余操作。代码中选择在合并完成后再一次性挂入最终链表。

---

## 5. 完整示例：分配 + 释放的全过程

假设内存池有 **8 个连续的 4KB 页**（共 32KB），观察空闲链表的变化。

### 5.1 初始化后

逐页释放触发合并，最终状态：

```
free_lists[0]: empty
free_lists[1]: empty
free_lists[2]: empty
free_lists[3]: [0:1:2:3:4:5:6:7]   // 8页 = order=3 = 32KB
```

### 5.2 分配 order=1（8KB）

```
扫描: free_lists[1] 空 → free_lists[2] 空 → free_lists[3] 有 [0:1:2:3:4:5:6:7]

从 free_lists[3] 取出 [0:1:2:3:4:5:6:7]

拆分过程:
  1. [0:1:2:3:4:5:6:7] (order=3) 拆成:
     - [0:1:2:3] (order=2) ← 继续拆分
     - [4:5:6:7] (order=2) → 挂入 free_lists[2]

  2. [0:1:2:3] (order=2) 拆成:
     - [0:1] (order=1) ← 目标 order，停止拆分
     - [2:3] (order=1) → 挂入 free_lists[1]

最终状态:
  free_lists[0]: empty
  free_lists[1]: [2:3]
  free_lists[2]: [4:5:6:7]
  free_lists[3]: empty

返回: page[0] (代表 [0:1] 这个 8KB 块)
```

### 5.3 再分配 order=0（4KB）

```
扫描: free_lists[0] 空 → free_lists[1] 有 [2:3]

从 free_lists[1] 取出 [2:3]

拆分过程:
  [2:3] (order=1) 拆成:
    - [2] (order=0) ← 目标 order
    - [3] (order=0) → 挂入 free_lists[0]

最终状态:
  free_lists[0]: [3]
  free_lists[1]: empty
  free_lists[2]: [4:5:6:7]
  free_lists[3]: empty

返回: page[2] (代表 [2] 这个 4KB 页)
```

### 5.4 释放 [0:1]（order=1）

```
page[0] 的 order=1，伙伴是 [2:3]
但 [2] 已被分配（page[2].allocated=1），只有 [3] 空闲
→ 伙伴 [2:3] 不完整，无法合并

最终状态:
  free_lists[0]: [3]
  free_lists[1]: [0:1]      // 新释放的块
  free_lists[2]: [4:5:6:7]
  free_lists[3]: empty
```

### 5.5 释放 [2]（order=0）

```
Step 1: page[2] 的 order=0，伙伴是 [3]
        [3] 空闲（在 free_lists[0] 中），order=0 == page[2]->order
        → 合并成 [2:3] (order=1)

        从 free_lists[0] 摘除 [3]

        现在 [2:3] 的 order=1，伙伴是 [0:1]
        [0:1] 空闲（在 free_lists[1] 中），order=1 == [2:3]->order
        → 合并成 [0:1:2:3] (order=2)

        从 free_lists[1] 摘除 [0:1]

        现在 [0:1:2:3] 的 order=2，伙伴是 [4:5:6:7]
        [4:5:6:7] 空闲（在 free_lists[2] 中），order=2 == [0:1:2:3]->order
        → 合并成 [0:1:2:3:4:5:6:7] (order=3)

        从 free_lists[2] 摘除 [4:5:6:7]

        [0:1:2:3:4:5:6:7] 的 order=3，已到最大 order
        → 停止合并

Step 2: 把 [0:1:2:3:4:5:6:7] 挂入 free_lists[3]

最终状态（完全恢复！）:
  free_lists[0]: empty
  free_lists[1]: empty
  free_lists[2]: empty
  free_lists[3]: [0:1:2:3:4:5:6:7]
```

---

## 6. 代码实现详解

在理解了整体设计和算法流程后，我们来看具体的代码实现。

### 6.1 初始化：`init_buddy`

```c
// buddy.c:25
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, u64 page_num)
{
        int order;
        int page_idx;
        struct page *page;

        /* Step 1: 初始化内存池基本信息 */
        pool->pool_start_addr = start_addr;
        pool->page_metadata = start_page;
        pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;
        pool->pool_phys_page_num = page_num;  // 仅用于单元测试

        /* Step 2: 初始化 14 个空闲链表 */
        for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
                pool->free_lists[order].nr_free = 0;
                init_list_head(&(pool->free_lists[order].free_list));
        }

        /* Step 3: 清零所有页元数据 */
        memset((char *)start_page, 0, page_num * sizeof(struct page));

        /* Step 4: 标记每个页为已分配 */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                page->allocated = 1;   // 必须设为 1，因为 free 会检查
                page->order = 0;
                page->pool = pool;
        }

        /* Step 5: 逐页释放（自动合并成大块） */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                buddy_free_pages(pool, page);
        }
}
```

### 6.2 伙伴查找：`get_buddy_chunk`

这是伙伴系统的**核心数学**：

```c
// buddy.c:65
static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                    struct page *chunk)
{
        u64 chunk_addr;
        u64 buddy_chunk_addr;
        int order;

        /* 获取块的虚拟地址 */
        chunk_addr = (u64)page_to_virt(chunk);
        order = chunk->order;

        /* 用异或计算伙伴地址 */
#define BUDDY_PAGE_SIZE_ORDER (12)
        buddy_chunk_addr = chunk_addr ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));

        /* 检查伙伴是否在内存池范围内 */
        if ((buddy_chunk_addr < pool->pool_start_addr)
            || (buddy_chunk_addr >= (pool->pool_start_addr + pool->pool_mem_size))) {
                return NULL;
        }

        return virt_to_page((void *)buddy_chunk_addr);
}
```

**异或原理详解**：

```
示例：order = 1，块大小 = 8KB (2^1 * 4KB)

chunk_addr  = 0x10000  (64KB)
             = 0b 0001 0000 0000 0000 0000

偏移位 = 1 << (1 + 12) = 1 << 13 = 0x2000

buddy_addr  = 0x10000 ^ 0x2000
             = 0x12000  (72KB)
             = 0b 0001 0010 0000 0000 0000
                  ↑
                  第13位翻转（0→1）

两个伙伴块相差正好 8KB，且合并后的 16KB 块起始于 0x10000，是 16KB 对齐的 ✓
```

### 6.3 分配页：`buddy_get_pages`

```c
// buddy.c:142
struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
        int cur_order;
        struct free_list *list;
        struct page *page;

        /* Step 1: 检查 order 合法性 */
        if(order >= BUDDY_MAX_ORDER || order < 0) {
                return NULL;
        }

        /* Step 2: 向上扫描找到可用块 */
        for (cur_order = order; cur_order < BUDDY_MAX_ORDER; cur_order++) {
                list = &pool->free_lists[cur_order];
                if (list->nr_free > 0) {
                        /* Step 3: 从链表取出 */
                        page = list_entry(list->free_list.next,
                                          struct page, node);
                        BUG_ON(page->allocated);
                        BUG_ON(page->order != cur_order);
                        list_del(&page->node);
                        pool->free_lists[page->order].nr_free--;

                        /* Step 4: 拆分到目标 order */
                        page = split_page(pool, order, page);
                        BUG_ON(page->order != order);

                        /* Step 5: 标记已分配 */
                        page->allocated = 1;
                        return page;
                }
        }

        return NULL;  // 内存不足
}
```

### 6.4 拆分页：`split_page`

```c
// buddy.c:100
static struct page *split_page(struct phys_mem_pool *pool, u64 order,
                               struct page *page)
{
        struct page *buddy;

        BUG_ON(page == NULL);
        BUG_ON(page->order < order);
        BUG_ON(page->allocated);

        /* 已经满足要求 */
        if (page->order == order) {
                return page;
        }

        /* 拆分：降阶，把伙伴放入低一阶的空闲链表 */
        page->order--;
        buddy = get_buddy_chunk(pool, page);
        BUG_ON(buddy == NULL);
        buddy->order = page->order;
        buddy->allocated = 0;
        list_add(&buddy->node, &pool->free_lists[buddy->order].free_list);
        pool->free_lists[buddy->order].nr_free++;

        /* 递归拆分 */
        return split_page(pool, order, page);
}
```

**实现细节**：`split_page` 假设调用者已经将该块从原空闲链表中摘除。这样在递归过程中，当前 `page` 不需要反复从链表移除再插入——它已经在链表外了。这是性能优化。

### 6.5 合并页：`merge_page`

```c
// buddy.c:187
static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
        struct page *buddy;

        /* 不能超出最大 order */
        BUG_ON(page->order > BUDDY_MAX_ORDER - 1 || page->allocated);
        if (page->order == BUDDY_MAX_ORDER - 1) {
                return page;
        }

        buddy = get_buddy_chunk(pool, page);

        /* 检查合并条件 */
        if (buddy == NULL || buddy->allocated || buddy->order < page->order) {
                return page;  // 无法合并
        }

        /* 安全检查 */
        BUG_ON(buddy->order > page->order);
        BUG_ON(buddy->order != page->order);

        /* 从伙伴的链表摘除 */
        list_del(&buddy->node);
        pool->free_lists[buddy->order].nr_free--;

        /* 取低地址页作为合并后的块 */
        if (buddy < page) {
                page = buddy;
        }
        page->order++;

        /* 递归尝试继续合并 */
        return merge_page(pool, page);
}
```

### 6.6 释放页：`buddy_free_pages`

```c
// buddy.c:234
void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        /* 安全检查：只能释放已分配的页 */
        BUG_ON(page->allocated == 0);
        page->allocated = 0;

        /* 先合并，再入链表（减少冗余操作） */
        page = merge_page(pool, page);

        BUG_ON(page->allocated);

        /* 挂入最终空闲链表 */
        list_add(&page->node, &pool->free_lists[page->order].free_list);
        pool->free_lists[page->order].nr_free++;
}
```

---

## 7. 碎片问题与应对

### 7.1 外部碎片 vs 内部碎片

```
外部碎片（伙伴系统解决）:
  内存中有足够总空间，但没有足够大的连续块
  [已用][4K空闲][已用][4K空闲] → 无法分配 8K 连续块

内部碎片（伙伴系统引入）:
  分配比请求稍大的块造成的浪费
  请求 5KB → 分配 order=1（8KB）→ 浪费 3KB
  请求 12KB → 分配 order=2（16KB）→ 浪费 4KB
```

### 7.2 ChCore 的分层策略

ChCore 通过 **slab 分配器**处理小内存分配（≤8KB），避免内部碎片：

```c
// kernel/mm/kmalloc.c:47
void *kmalloc(size_t size)
{
        if (size <= _SIZE) {       // ≤ 8KB
                return alloc_in_slab(size);   // 用 slab（无内部碎片）
        }
        // 大内存：用伙伴系统
        order = size_to_page_order(size);
        return get_pages(order);
}
```

---

## 8. 关键源码速查表

| 功能 | 文件 | 行号 |
|------|------|------|
| 常量定义（`BUDDY_MAX_ORDER=14`） | `kernel/include/mm/buddy.h` | 24 |
| `struct page` 定义 | `kernel/include/mm/buddy.h` | 30 |
| `struct phys_mem_pool` 定义 | `kernel/include/mm/buddy.h` | 49 |
| 伙伴系统初始化 | `kernel/mm/buddy.c` | 25 |
| 页分配 | `kernel/mm/buddy.c` | 142 |
| 页拆分 | `kernel/mm/buddy.c` | 100 |
| 页释放 | `kernel/mm/buddy.c` | 234 |
| 页合并 | `kernel/mm/buddy.c` | 187 |
| 伙伴查找（异或） | `kernel/mm/buddy.c` | 65 |
| 页地址转换 | `kernel/mm/buddy.c` | 260, 272 |
| 空闲内存统计 | `kernel/mm/buddy.c` | 295 |
| 单元测试 | `kernel/mm/buddy.c` | 320 |
| 内存池初始化入口 | `kernel/mm/mm.c` | 36 |
| kmalloc 封装 | `kernel/mm/kmalloc.c` | 47 |

---

## 9. 总结

ChCore Lab2 的伙伴系统是一个**经典且简洁的教科书式实现**，核心设计要点：

1. **分层管理**：按 order 分类，每个 order 一个空闲链表
2. **异或找伙伴**：利用地址对齐特性，用 `addr ^ (1 << (order+12))` 快速定位伙伴
3. **分配时拆分**：找不到精确匹配的块时，拆分更大的块
4. **释放时合并**：递归向上合并伙伴，恢复大块连续内存
5. **链表操作优化**：合并完成后再入链表，减少冗余操作
6. **与 slab 配合**：小对象（≤8KB）走 slab 分配器，大对象走伙伴系统

整个实现约 **250 行 C 代码**（不含测试），结构清晰，非常适合教学。
