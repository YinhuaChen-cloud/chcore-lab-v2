# ChCore Lab2: 伙伴系统（Buddy System）图文码对照分析

> 源码位置：`kernel/mm/buddy.c`, `kernel/include/mm/buddy.h`

---

## 阅读指南

本文采用**图文码一体**的写法：每个概念先给出示意图和文字说明，随后紧跟对应的源码片段，关键行用注释标注。读者可以对照理解。

---

## 1. 伙伴系统在内存管理中的位置

### 1.1 层级架构

```
┌─────────────────────────────────────────────┐
│  上层：kmalloc / kfree                       │
│  （小对象用 slab，大对象用伙伴系统）          │
├─────────────────────────────────────────────┤
│  中层：slab 分配器                            │
│  （基于伙伴系统分配的页构建缓存）               │
├─────────────────────────────────────────────┤
│  底层：伙伴系统 (Buddy System)                │  ← 本文分析对象
│  （管理物理页的分配与回收，≥4KB）              │
├─────────────────────────────────────────────┤
│  最底层：物理内存池 (phys_mem_pool)           │
└─────────────────────────────────────────────┘
```

**kmalloc 如何选择分配器**（对应源码）：

```c
// kernel/mm/kmalloc.c:47
void *kmalloc(size_t size)
{
        if (size <= _SIZE) {              // ← 小对象（≤8KB）
                return alloc_in_slab(size);   // 走 slab，无内部碎片
        }
        // 大对象：走伙伴系统
        order = size_to_page_order(size);
        return get_pages(order);          // ← 最终调用 buddy_get_pages
}
```

---

## 2. 核心设计思想：分阶管理 + 拆分合并

### 2.1 内存块按 order 分级

```
order │ 页数   │ 大小      │ 管理结构
──────┼────────┼───────────┼────────────────────────
  0   │   1    │   4 KB    │ free_lists[0] 链表
  1   │   2    │   8 KB    │ free_lists[1] 链表
  2   │   4    │  16 KB    │ free_lists[2] 链表
  3   │   8    │  32 KB    │ free_lists[3] 链表
 ...  │  ...   │   ...     │ ...
 13   │ 8192   │  32 MB    │ free_lists[13] 链表   ← 最大块
```

### 2.2 分配：向上查找 → 拆分

```
用户请求 order=1（8KB）

  free_lists[0]: empty
  free_lists[1]: empty        ← 目标 order 没有？向上找！
  free_lists[2]: [A:B:C:D]    ← 找到 order=2 的块（16KB）
  ...

  取出 [A:B:C:D]，拆分成两个 order=1：
    ┌──────────────┬──────────────┐
    │   [A:B]      │   [C:D]      │
    │ 返回给用户    │ 挂入free_lists[1]│
    │ (order=1)    │  (order=1)   │
    └──────────────┴──────────────┘
```

### 2.3 释放：找伙伴 → 合并

```
用户释放 [A:B]（order=1）

  伙伴是 [C:D]（order=1，空闲）→ 合并成 [A:B:C:D]（order=2）

  [A:B:C:D] 的伙伴是 [E:F:G:H]（order=2，空闲）→ 合并成 [A..H]（order=3）

  ...递归直到无法合并
```

### 2.4 "伙伴"的数学定义

两个块是伙伴，当且仅当：
- 大小相同（同 order）
- 地址相邻
- 合并后按该大小对齐

**地址计算**：翻转第 `(order + 12)` 位（因为页大小 = 4KB = 2^12）

```
示例：order=1（8KB），addr = 0x10000

  addr           = 0b 0001 0000 0000 0000 0000  (0x10000)
  buddy = addr ^ (1 << (1+12))
                   = addr ^ 0x2000
                   = 0b 0001 0010 0000 0000 0000  (0x12000)
                          ↑
                    第13位翻转

  两个伙伴相差 8KB，合并后 16KB 起始于 0x10000（16KB 对齐）✓
```

---

## 3. 数据结构：三层组织

### 3.1 整体结构图

```
┌─────────────────────────────────────────────────────────────┐
│                    struct phys_mem_pool                       │
│  ┌─────────────────┐  ┌─────────────────────────────────────┐│
│  │ pool_start_addr │  │ free_lists[0] ──→ page[3].node      ││
│  │ page_metadata ──┼──┤ free_lists[1] ──→ page[0].node      ││
│  │ pool_mem_size   │  │ free_lists[2] ──→ page[4].node      ││
│  │ ...             │  │ ...                                 ││
│  └─────────────────┘  └─────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
         ↑
         │ page_metadata 指向
         │
┌─────────────────────────────────────────────────────────────┐
│              struct page page_metadata[N]                     │
│  ┌──────────┐  ┌──────────┐        ┌──────────┐             │
│  │ page[0]  │  │ page[1]  │  ...   │ page[N-1]│             │
│  │  node ───┼──┼──→ 链表  │        │  node    │             │
│  │allocated │  │  order   │        │  pool    │             │
│  │  order   │  │  pool    │        │          │             │
│  └──────────┘  └──────────┘        └──────────┘             │
└─────────────────────────────────────────────────────────────┘
         ↑                                              ↑
         └────── page - page_metadata = 页索引 ──────────┘
                    × 4KB + pool_start_addr = 物理地址
```

### 3.2 结构体定义（对应源码）

```c
// kernel/include/mm/buddy.h:23-25 ── 常量定义
#define BUDDY_PAGE_SIZE       (0x1000)    // ← 4KB
#define BUDDY_MAX_ORDER       (14UL)      // ← 支持 order 0~13
#define BUDDY_PAGE_SIZE_ORDER (12)        // ← 1<<12 = 4096，用于伙伴地址计算

// kernel/include/mm/buddy.h:30-41 ── 每页元数据
struct page {
        struct list_head node;              // ← 链入 free_list 的节点
        int allocated;                      // ← 1=已分配, 0=空闲
        int order;                          // ← 当前块所属 order
        void *slab;                         // ← slab 分配器使用（本文不涉及）
        struct phys_mem_pool *pool;         // ← 所属内存池（用于地址转换）
};

// kernel/include/mm/buddy.h:43-46 ── 每个 order 的空闲链表头
struct free_list {
        struct list_head free_list;         // ← 双向循环链表头
        u64 nr_free;                        // ← 该 order 空闲块数量
};

// kernel/include/mm/buddy.h:49-71 ── 物理内存池
struct phys_mem_pool {
        u64 pool_start_addr;                // ← 可用内存起始虚拟地址
        u64 pool_mem_size;                  // ← 池总大小（字节）
        u64 pool_phys_page_num;             // ← 总页数（仅单元测试用）
        struct page *page_metadata;         // ← 元数据数组起始地址
        struct free_list free_lists[BUDDY_MAX_ORDER];  // ← 14 个空闲链表
};
```

### 3.3 页索引 ↔ 物理地址转换

```
转换关系（线性映射）：

  page_metadata ────────────────────────────────────── pool_start_addr
       │                                                    │
       │  page_idx = page - page_metadata                    │
       │  addr = page_idx × 4KB + pool_start_addr           │
       │                                                    │
       ↓                                                    ↓
  ┌─────────┐                                       ┌─────────┐
  │ page[0] │←──────────────────────────────────────│ 物理页0 │
  │ page[1] │←──────────────────────────────────────│ 物理页1 │
  │   ...   │                                       │   ...   │
  │page[N-1]│←──────────────────────────────────────│物理页N-1│
  └─────────┘                                       └─────────┘
```

```c
// kernel/mm/buddy.c:260-270 ── page → 虚拟地址
void *page_to_virt(struct page *page)
{
        u64 addr;
        struct phys_mem_pool *pool = page->pool;
        BUG_ON(pool == NULL);
        // page_idx × PAGE_SIZE + start_addr
        addr = (page - pool->page_metadata) * BUDDY_PAGE_SIZE    // ← 计算页索引 × 4KB
               + pool->pool_start_addr;                          // ← 加上基地址
        return (void *)addr;
}

// kernel/mm/buddy.c:272-293 ── 虚拟地址 → page
struct page *virt_to_page(void *ptr)
{
        struct page *page;
        struct phys_mem_pool *pool = NULL;
        u64 addr = (u64)ptr;
        int i;

        // 遍历所有 pool，找到 addr 所属的那个
        for (i = 0; i < physmem_map_num; ++i) {                   // ← 支持多内存池
                if (addr >= global_mem[i].pool_start_addr
                    && addr < global_mem[i].pool_start_addr
                                       + global_mem[i].pool_mem_size) {
                        pool = &global_mem[i];
                        break;
                }
        }

        BUG_ON(pool == NULL);
        page = pool->page_metadata                                // ← 元数据基址
               + (((u64)addr - pool->pool_start_addr) / BUDDY_PAGE_SIZE);  // ← 页索引
        return page;
}
```

---

## 4. 内存池布局

### 4.1 物理内存池的虚拟地址布局

```
低地址
│
v
+──────────────────────────────────────+──────────+─────────────────...──+
│ page[0] │ page[1] │ ... │ page[N-1]  │   pad    │ 4K页0 │ 4K页1 │ ...   │
+──────────────────────────────────────+──────────+─────────────────...──+
↑                                      ↑          ↑
│                                      │          │
page_metadata                          │    pool_start_addr
(free_mem_start)                       │    = ROUND_UP(free_mem_start
                                       │              + N*sizeof(page),
                                       │              PAGE_SIZE)
                                       │
                                    对齐边界

总大小 = N×sizeof(struct page) + pad + N×4KB
```

**初始化时的页数计算**（来自 `mm.c`）：

```c
// kernel/mm/mm.c:51-55
npages = (free_mem_end - free_mem_start)
         / (PAGE_SIZE + sizeof(struct page));   // ← 每个页需要：4KB 物理页 + 一个 struct page
start_vaddr = ROUND_UP(free_mem_start
                       + npages * sizeof(struct page),
                       PAGE_SIZE);              // ← 元数据区之后按页对齐
```

---

## 5. 核心操作：初始化

### 5.1 初始化流程

```
输入: pool, start_page(元数据区首地址), start_addr(物理内存首地址), page_num(页数)

┌──────────────────────────────────────────────────────┐
│ Step 1: 设置内存池基本信息                            │
│   pool->pool_start_addr = start_addr                  │
│   pool->page_metadata   = start_page                  │
│   pool->pool_mem_size   = page_num × 4KB              │
├──────────────────────────────────────────────────────┤
│ Step 2: 初始化 14 个空闲链表（全部为空）               │
│   for order = 0 to 13:                                │
│     free_lists[order].nr_free = 0                     │
│     init_list_head(&free_lists[order].free_list)      │
├──────────────────────────────────────────────────────┤
│ Step 3: 清零所有页元数据                              │
│   memset(start_page, 0, page_num × sizeof(struct page))│
├──────────────────────────────────────────────────────┤
│ Step 4: 标记每个页为"已分配"                          │
│   for i = 0 to page_num-1:                            │
│     page[i].allocated = 1   ← 必须设为1，否则free会报错│
│     page[i].order = 0                                 │
│     page[i].pool = pool                               │
├──────────────────────────────────────────────────────┤
│ Step 5: 逐页释放（触发自动合并成大块）                 │
│   for i = 0 to page_num-1:                            │
│     buddy_free_pages(pool, &page[i])                  │
│     // 每次释放都会尝试与邻居合并                       │
└──────────────────────────────────────────────────────┘
```

### 5.2 初始化源码

```c
// kernel/mm/buddy.c:25-61 ── 伙伴系统初始化
void init_buddy(struct phys_mem_pool *pool, struct page *start_page,
                vaddr_t start_addr, u64 page_num)
{
        int order;
        int page_idx;
        struct page *page;

        /* ===== Step 1: 初始化内存池基本信息 ===== */
        pool->pool_start_addr = start_addr;              // ← 物理内存起始地址
        pool->page_metadata = start_page;                // ← 元数据区起始地址
        pool->pool_mem_size = page_num * BUDDY_PAGE_SIZE;// ← 总字节数
        pool->pool_phys_page_num = page_num;             // ← 仅单元测试用

        /* ===== Step 2: 初始化 14 个空闲链表 ===== */
        for (order = 0; order < BUDDY_MAX_ORDER; ++order) {
                pool->free_lists[order].nr_free = 0;     // ← 初始无空闲块
                init_list_head(&(pool->free_lists[order].free_list));
        }

        /* ===== Step 3: 清零所有页元数据 ===== */
        memset((char *)start_page, 0, page_num * sizeof(struct page));

        /* ===== Step 4: 标记每个页为已分配 ===== */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                page->allocated = 1;   // ← 关键！free 要求 allocated==1
                page->order = 0;       // ← 初始每页都是 order=0
                page->pool = pool;     // ← 设置所属池（地址转换需要）
        }

        /* ===== Step 5: 逐页释放（自动合并） ===== */
        for (page_idx = 0; page_idx < page_num; ++page_idx) {
                page = start_page + page_idx;
                buddy_free_pages(pool, page);  // ← 释放时会自动合并伙伴
        }
}
```

**为什么 Step 4 要设 `allocated = 1`？**

因为 `buddy_free_pages` 第 243 行会检查 `BUG_ON(page->allocated == 0)`——它只接受已分配的页。这是一个**不变式（invariant）**。

---

## 6. 核心操作：分配页

### 6.1 分配流程

```
输入: pool, order
输出: struct page* (已分配), 或 NULL

┌─────────────────────────────────────────────────────────────┐
│ Step 1: 检查 order 合法性 [0, BUDDY_MAX_ORDER)              │
│   非法 → 返回 NULL                                          │
├─────────────────────────────────────────────────────────────┤
│ Step 2: 向上扫描空闲链表                                     │
│   for cur = order; cur < 14; cur++:                         │
│     if free_lists[cur].nr_free > 0:                         │
│       找到一个可用大块，跳出循环                             │
│   全空 → 返回 NULL                                          │
├─────────────────────────────────────────────────────────────┤
│ Step 3: 从链表中取出块                                       │
│   page = 链表第一个元素                                      │
│   list_del(&page->node)      ← 从链表摘除                   │
│   nr_free--                  ← 计数减一                     │
├─────────────────────────────────────────────────────────────┤
│ Step 4: 拆分到目标 order（如需）                             │
│   if page->order > order:                                   │
│     page = split_page(pool, order, page)                    │
├─────────────────────────────────────────────────────────────┤
│ Step 5: 标记已分配并返回                                     │
│   page->allocated = 1                                       │
│   return page                                               │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 分配源码

```c
// kernel/mm/buddy.c:142-179 ── 分配页
struct page *buddy_get_pages(struct phys_mem_pool *pool, u64 order)
{
        int cur_order;
        struct free_list *list;
        struct page *page;

        /* Step 1: 检查 order 合法性 */
        if(order >= BUDDY_MAX_ORDER || order < 0) {      // ← 越界检查
                return NULL;
        }

        /* Step 2: 向上扫描，找第一个非空闲链表 */
        for (cur_order = order; cur_order < BUDDY_MAX_ORDER; cur_order++) {
                list = &pool->free_lists[cur_order];
                if (list->nr_free > 0) {                 // ← 该 order 有空闲块？

                        /* Step 3: 从链表取出 */
                        page = list_entry(list->free_list.next,
                                          struct page, node);   // ← 取第一个
                        BUG_ON(page->allocated);               // ← 安全检查
                        BUG_ON(page->order != cur_order);      // ← 一致性检查
                        list_del(&page->node);                   // ← 从链表摘除
                        pool->free_lists[page->order].nr_free--; // ← 计数减一

                        /* Step 4: 拆分到目标 order */
                        page = split_page(pool, order, page);    // ← 可能需要拆分
                        BUG_ON(page->order != order);            // ← 确保拆分正确

                        /* Step 5: 标记已分配 */
                        page->allocated = 1;                     // ← 标记为已分配
                        return page;
                }
        }

        return NULL;  // ← 内存不足
}
```

---

## 7. 核心操作：拆分页

### 7.1 拆分过程图解

```
场景：需要 order=1（8KB），但 free_lists[2] 有一个 order=2（16KB）块

初始状态:
  free_lists[0]: empty
  free_lists[1]: empty
  free_lists[2]: [A:B:C:D]  ← 取出这个块

Step 1: split_page(order=1, page=[A:B:C:D])
        page->order 从 2 降到 1
        ┌─────────┬─────────┐
        │ [A:B]   │ [C:D]   │
        │ page    │ buddy   │
        │ order=1 │ order=1 │
        └─────────┴─────────┘
        buddy 挂入 free_lists[1]

        free_lists[1]: [C:D]   ← 新挂入

Step 2: page->order == 1 == target_order，返回 [A:B]

最终结果:
  free_lists[0]: empty
  free_lists[1]: [C:D]
  free_lists[2]: empty
  返回: page[A] (代表 [A:B] 块)
```

### 7.2 拆分源码

```c
// kernel/mm/buddy.c:100-137 ── 拆分页
static struct page *split_page(struct phys_mem_pool *pool, u64 order,
                               struct page *page)
{
        struct page *buddy;

        BUG_ON(page == NULL);
        BUG_ON(page->order < order);        // ← 块必须足够大
        BUG_ON(page->allocated);            // ← 必须是空闲块

        /* 已经满足要求，无需拆分 */
        if (page->order == order) {
                return page;                // ← 直接返回
        }

        /* ===== 拆分操作 ===== */
        page->order--;                      // ← Step 1: 降阶（当前块变小）
        buddy = get_buddy_chunk(pool, page);// ← Step 2: 找到伙伴
        BUG_ON(buddy == NULL);
        buddy->order = page->order;         // ← Step 3: 伙伴同阶
        buddy->allocated = 0;               // ←        伙伴标记为空闲
        list_add(&buddy->node,             // ← Step 4: 伙伴挂入低一阶链表
                 &pool->free_lists[buddy->order].free_list);
        pool->free_lists[buddy->order].nr_free++;  // ← 计数加一

        /* 递归拆分，直到目标 order */
        return split_page(pool, order, page);// ← Step 5: 继续拆分（如需）
}
```

**优化细节**：调用 `split_page` 前，`buddy_get_pages` 已经把 `page` 从原链表摘除（`list_del`）。所以 `split_page` 只需要处理"把 buddy 挂入低阶链表"，不需要处理 `page` 本身——它已经在链表外了。

---

## 8. 核心操作：释放页 + 合并

### 8.1 合并过程图解

```
场景：释放 [A]（order=0），伙伴 [B] 也是 order=0 且空闲

初始:
  free_lists[0]: [B]    ← B 空闲
  正在释放: [A]

Step 1: merge_page(page=A)
        buddy = [B]，存在 ✓、空闲 ✓、order=0 == page->order=0 ✓
        从 free_lists[0] 摘除 B
        A 和 B 合并，取低地址（假设 A < B）
        page = A, page->order = 1  →  [A:B]

        free_lists[0]: empty

Step 2: merge_page(page=[A:B]) 递归
        buddy = [C:D]，也是 order=1 且空闲
        从 free_lists[1] 摘除 [C:D]
        合并成 [A:B:C:D]，order=2

        ...继续递归直到无法合并...

最终：合并后的块挂在尽可能高的 order 链表上
```

### 8.2 释放流程

```
输入: pool, page

┌─────────────────────────────────────────────────────────────┐
│ Step 1: 安全检查并标记空闲                                   │
│   BUG_ON(page->allocated == 0)   ← 只能释放已分配的页        │
│   page->allocated = 0                                       │
├─────────────────────────────────────────────────────────────┤
│ Step 2: 尝试合并（递归）                                     │
│   page = merge_page(pool, page)                             │
│   // merge_page 返回合并后的块，可能 order 更大              │
├─────────────────────────────────────────────────────────────┤
│ Step 3: 挂入空闲链表                                         │
│   list_add(&page->node, &free_lists[page->order])           │
│   nr_free++                                                 │
└─────────────────────────────────────────────────────────────┘
```

### 8.3 释放源码

```c
// kernel/mm/buddy.c:234-258 ── 释放页
void buddy_free_pages(struct phys_mem_pool *pool, struct page *page)
{
        /* Step 1: 安全检查 + 标记空闲 */
        BUG_ON(page->allocated == 0);   // ← 只能释放已分配的页
        page->allocated = 0;             // ← 标记为空闲

        /* Step 2: 先合并，再入链表（减少冗余操作） */
        page = merge_page(pool, page);   // ← 递归合并伙伴

        BUG_ON(page->allocated);

        /* Step 3: 挂入最终空闲链表 */
        list_add(&page->node,            // ← 插入对应 order 的链表
                 &pool->free_lists[page->order].free_list);
        pool->free_lists[page->order].nr_free++;  // ← 计数加一
}
```

### 8.4 合并源码

```c
// kernel/mm/buddy.c:187-229 ── 合并页（递归）
static struct page *merge_page(struct phys_mem_pool *pool, struct page *page)
{
        struct page *buddy;

        /* 不能合并超过最大 order */
        BUG_ON(page->order > BUDDY_MAX_ORDER - 1 || page->allocated);
        if (page->order == BUDDY_MAX_ORDER - 1) {
                return page;                    // ← 已到最大，无法合并
        }

        buddy = get_buddy_chunk(pool, page);    // ← 找到伙伴

        /* 检查合并条件：伙伴必须存在、空闲、且 order 相同 */
        if (buddy == NULL                       // ← 伙伴不存在（越界）
            || buddy->allocated                 // ← 伙伴已分配
            || buddy->order < page->order) {    // ← 伙伴已被拆分（更小）
                return page;                    // ← 无法合并，直接返回
        }

        /* 安全检查 */
        BUG_ON(buddy->order > page->order);
        BUG_ON(buddy->order != page->order);    // ← 到这里必须同阶

        /* 从伙伴所在链表摘除 */
        list_del(&buddy->node);                 // ← 摘除伙伴
        pool->free_lists[buddy->order].nr_free--;

        /* 取低地址页作为合并后的块 */
        if (buddy < page) {                     // ← 地址比较
                page = buddy;                   // ← 保留低地址
        }
        page->order++;                          // ← 升阶

        /* 递归尝试继续合并 */
        return merge_page(pool, page);          // ← 继续向上合并
}
```

**为什么检查 `buddy->order < page->order`？**

因为伙伴可能已经被拆分成了更小的块。例如：

```
内存布局：order=1 的块 [A:B]，其伙伴 [C:D] 已经被拆分成两个 order=0

  free_lists[0]: [C], [D]
  free_lists[1]: empty

如果释放 [A:B]（order=1），其伙伴应该是 [C:D]
但 [C] 和 [D] 现在是独立的 order=0 块
→ buddy->order = 0 < page->order = 1，不能合并
```

---

## 9. 核心操作：伙伴查找

### 9.1 异或原理详解

```
order = k 的块大小 = 2^k × 4KB = 2^(k+12) 字节

两个伙伴块的地址只在第 (k+12) 位不同：

示例：order=2（16KB）

  chunk_addr    = 0x10000
                = 0b 0001 0000 0000 0000 0000

  偏移位        = 1 << (2 + 12) = 1 << 14 = 0x4000

  buddy_addr    = 0x10000 ^ 0x4000
                = 0b 0001 0100 0000 0000 0000
                = 0x14000
                       ↑
                 第14位翻转（0→1）

  两个伙伴相差 16KB，合并后 32KB 起始于 0x10000（32KB 对齐）✓
```

### 9.2 伙伴查找源码

```c
// kernel/mm/buddy.c:65-91 ── 伙伴查找
static struct page *get_buddy_chunk(struct phys_mem_pool *pool,
                                    struct page *chunk)
{
        u64 chunk_addr;
        u64 buddy_chunk_addr;
        int order;

        /* 获取块的虚拟地址 */
        chunk_addr = (u64)page_to_virt(chunk);
        order = chunk->order;

        /* ===== 核心：用异或计算伙伴地址 ===== */
#define BUDDY_PAGE_SIZE_ORDER (12)
        buddy_chunk_addr = chunk_addr
                           ^ (1UL << (order + BUDDY_PAGE_SIZE_ORDER));
                        //  ↑
                        //  翻转第 (order+12) 位

        /* 检查伙伴是否在内存池范围内 */
        if ((buddy_chunk_addr < pool->pool_start_addr)
            || (buddy_chunk_addr
                >= (pool->pool_start_addr + pool->pool_mem_size))) {
                return NULL;                    // ← 伙伴在池外
        }

        return virt_to_page((void *)buddy_chunk_addr);  // ← 地址 → page 结构体
}
```

---

## 10. 完整示例：分配与释放的完整时序

假设内存池有 **8 页 = 32KB**，观察 free_lists 状态变化。

### 10.1 状态变化表

| 操作 | free_lists[0] | free_lists[1] | free_lists[2] | free_lists[3] | 说明 |
|------|:-------------:|:-------------:|:-------------:|:-------------:|------|
| 初始化后 | empty | empty | empty | `[0:1:2:3:4:5:6:7]` | 全部合并为 order=3 |
| 分配 order=1 | empty | `[2:3]` | `[4:5:6:7]` | empty | [0:1] 返回，[2:3] 剩余，[4:5:6:7] 未动 |
| 分配 order=0 | `[3]` | empty | `[4:5:6:7]` | empty | 从 [2:3] 拆分：[2] 返回，[3] 剩余 |
| 释放 [0:1] | `[3]` | `[0:1]` | `[4:5:6:7]` | empty | [0:1] 伙伴 [2:3] 不完整（[2]已分配），无法合并 |
| 释放 [2] | empty | empty | empty | `[0:1:2:3:4:5:6:7]` | [2]↔[3] 合并→[2:3]；[0:1]↔[2:3] 合并→[0:1:2:3]；[0:1:2:3]↔[4:5:6:7] 合并→order=3 |

### 10.2 可视化

```
初始化后（全部合并）:
  ┌─────────────────────────────────────────┐
  │ [0:1:2:3:4:5:6:7]  order=3 (32KB)      │ ← free_lists[3]
  └─────────────────────────────────────────┘

分配 order=1:
  ┌───────────┬───────────┬─────────────────┐
  │ [0:1]     │ [2:3]     │ [4:5:6:7]       │
  │ 已分配    │ free_lists[1]│ free_lists[2] │
  │ (返回)    │           │                 │
  └───────────┴───────────┴─────────────────┘

再分配 order=0:
  ┌─────┬─────┬───────────┬─────────────────┐
  │ [0:1]│[2] │ [3]       │ [4:5:6:7]       │
  │已分配│已分配│free_lists[0]│ free_lists[2] │
  │     │(返回)│           │                 │
  └─────┴─────┴───────────┴─────────────────┘

释放 [0:1]:
  ┌─────┬─────┬─────┬───────────────────────┐
  │     │ [2] │ [3] │ [0:1]    [4:5:6:7]    │
  │     │已分配│free │free_lists[1] free_lists[2]│
  │     │     │_lists[0]│                    │
  └─────┴─────┴─────┴───────────────────────┘

释放 [2]（触发级联合并）:
  Step 1: [2] ↔ [3]  →  [2:3]  (order=1)
  Step 2: [0:1] ↔ [2:3] → [0:1:2:3] (order=2)
  Step 3: [0:1:2:3] ↔ [4:5:6:7] → [0..7] (order=3)

  ┌─────────────────────────────────────────┐
  │ [0:1:2:3:4:5:6:7]  order=3 (32KB)      │ ← free_lists[3]
  └─────────────────────────────────────────┘
```

---

## 11. 碎片问题与应对

### 11.1 两种碎片

```
外部碎片（伙伴系统解决）:
  [已用][4K空闲][已用][4K空闲] → 无法分配 8K 连续块
  伙伴系统通过合并邻居来避免

内部碎片（伙伴系统引入）:
  请求 5KB → 分配 order=1（8KB）→ 浪费 3KB
  请求 12KB → 分配 order=2（16KB）→ 浪费 4KB
```

### 11.2 ChCore 的分层策略

```c
// kernel/mm/kmalloc.c:47
void *kmalloc(size_t size)
{
        if (size <= _SIZE) {              // ← ≤8KB 小对象
                return alloc_in_slab(size);   // slab 无内部碎片
        }
        order = size_to_page_order(size);
        return get_pages(order);          // ← >8KB 大对象走伙伴系统
}
```

---

## 12. 关键源码速查表

| 功能 | 文件 | 行号 |
|------|------|------|
| 常量定义（`BUDDY_MAX_ORDER=14`） | `kernel/include/mm/buddy.h` | 24 |
| `struct page` 定义 | `kernel/include/mm/buddy.h` | 30 |
| `struct phys_mem_pool` 定义 | `kernel/include/mm/buddy.h` | 49 |
| 伙伴系统初始化 | `kernel/mm/buddy.c` | 25 |
| **页分配** | `kernel/mm/buddy.c` | **142** |
| **页拆分** | `kernel/mm/buddy.c` | **100** |
| **页释放** | `kernel/mm/buddy.c` | **234** |
| **页合并** | `kernel/mm/buddy.c` | **187** |
| **伙伴查找（异或）** | `kernel/mm/buddy.c` | **65** |
| 页 → 虚拟地址 | `kernel/mm/buddy.c` | 260 |
| 虚拟地址 → 页 | `kernel/mm/buddy.c` | 272 |
| 空闲内存统计 | `kernel/mm/buddy.c` | 295 |
| 内存池初始化入口 | `kernel/mm/mm.c` | 36 |
| kmalloc 路由 | `kernel/mm/kmalloc.c` | 47 |

---

## 13. 总结

| 设计要点 | 实现方式 | 对应源码 |
|---------|---------|---------|
| 分阶管理 | 14 个空闲链表 | `free_lists[BUDDY_MAX_ORDER]` |
| 快速定位伙伴 | 地址异或 | `addr ^ (1 << (order+12))` |
| 分配时拆分 | 递归降阶 | `split_page()` |
| 释放时合并 | 递归升阶 | `merge_page()` |
| 减少链表操作 | 合并后再入链表 | `buddy_free_pages()` 先 merge 后 add |
| 避免内部碎片 | 小对象走 slab | `kmalloc()` 中的 `size <= _SIZE` 判断 |

整个实现约 **250 行 C 代码**（不含测试），结构清晰，非常适合教学。
