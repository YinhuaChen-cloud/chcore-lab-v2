# ChCore-Lab slab 分配器设计与实现梳理

本文结合当前源码梳理 `kmalloc`、slab allocator 与 buddy allocator 的关系，重点解释小对象分配路径、slab 的内部布局、初始化/分配/释放流程。

## 1. 先纠正一个关键边界：不是 `< 4KB` 走 slab

`kmalloc` 的分流逻辑在 [kernel/mm/kmalloc.c:46-60](kernel/mm/kmalloc.c#L46-L60)：

```c
#define _SIZE (1UL << SLAB_MAX_ORDER)

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

而 `SLAB_MAX_ORDER` 定义在 [kernel/include/mm/slab.h:18-20](kernel/include/mm/slab.h#L18-L20)：

```c
#define SLAB_MIN_ORDER (5)
#define SLAB_MAX_ORDER (11)
```

所以：

```text
_SIZE = 1UL << SLAB_MAX_ORDER
      = 1UL << 11
      = 2048 bytes
      = 2KB
```

因此当前项目里的实际分流是：

```text
kmalloc(size)
│
├── size <= 2048 bytes
│   └── alloc_in_slab(size)       // slab 小对象分配
│
└── size > 2048 bytes
    ├── 2049 ~ 4096 bytes
    │   └── get_pages(0)          // buddy 分配 1 个 4KB page
    │
    └── > 4096 bytes
        └── get_pages(order)      // buddy 分配 2^order 个连续 4KB page
```

也就是说，你理解中的“`< 4KB` 走 slab，`>= 4KB` 走 buddy”在这个源码版本中并不成立；这里是“`<= 2KB` 走 slab，`> 2KB` 走 buddy”。

## 2. 整体关系：slab 建在 buddy 之上

内存子系统初始化顺序在 [kernel/mm/mm.c:68-81](kernel/mm/mm.c#L68-L81)：

```text
mm_init()
│
├── init_buddy(...)
│   └── 初始化物理页管理器，管理 4KB page 及其 2^order 连续页块
│
└── init_slab()
    └── 从 buddy 中拿 2MB 大块，切成固定大小 slot，用于 kmalloc 小对象
```

可以把二者关系理解为：

```text
                 kmalloc(size)
                      │
          ┌───────────┴───────────┐
          │                       │
   size <= 2KB              size > 2KB
          │                       │
          v                       v
   slab allocator          buddy allocator
          │                       │
          │                       v
          │               4KB page / 2^order pages
          │
          v
   slab 内部没有直接管理物理页分裂合并，
   它向 buddy 申请 2MB slab memory，
   再切成 32/64/.../2048 字节 slot。
```

## 3. slab 支持的对象大小等级

slab 相关宏定义在 [kernel/include/mm/slab.h:16-20](kernel/include/mm/slab.h#L16-L20)：

```c
#define SLAB_INIT_SIZE (2 * 1024 * 1024) // 2M
#define SLAB_MIN_ORDER (5)
#define SLAB_MAX_ORDER (11)
```

每个 order 对应的 slot/object 大小为：

```text
object_size = 1 << order
```

所以 slab allocator 管理如下 7 个大小等级：

```text
order:       5    6    7     8     9      10      11
size:       32   64   128   256   512    1024    2048 bytes
```

初始化代码在 [kernel/mm/slab.c:145-153](kernel/mm/slab.c#L145-L153)：

```c
void init_slab()
{
        int order;

        /* slab obj size: 32, 64, 128, 256, 512, 1024, 2048 */
        for (order = SLAB_MIN_ORDER; order <= SLAB_MAX_ORDER; order++) {
                slabs[order] = init_slab_cache(order, SLAB_INIT_SIZE);
        }
        kdebug("mm: finish initing slab allocators\n");
}
```

全局数组在 [kernel/mm/slab.c:19-21](kernel/mm/slab.c#L19-L21)：

```c
slab_header_t *slabs[SLAB_MAX_ORDER + 1] = {NULL};
```

它以 `order` 为下标，指向对应大小等级的 slab 链表头：

```text
slabs[]
│
├── slabs[0]   unused
├── ...
├── slabs[5]  ──► slab list for 32-byte objects
├── slabs[6]  ──► slab list for 64-byte objects
├── slabs[7]  ──► slab list for 128-byte objects
├── slabs[8]  ──► slab list for 256-byte objects
├── slabs[9]  ──► slab list for 512-byte objects
├── slabs[10] ──► slab list for 1024-byte objects
└── slabs[11] ──► slab list for 2048-byte objects
```

## 4. 核心数据结构

定义在 [kernel/include/mm/slab.h:22-32](kernel/include/mm/slab.h#L22-L32)：

```c
typedef struct slab_header slab_header_t;
struct slab_header {
        void *free_list_head;
        slab_header_t *next_slab;
        int order;
};

typedef struct slab_slot_list slab_slot_list_t;
struct slab_slot_list {
        void *next_free;
};
```

含义如下：

```text
struct slab_header
┌────────────────┬──────────────────────────────┐
│ free_list_head │ 当前 slab 内第一个空闲 slot    │
├────────────────┼──────────────────────────────┤
│ next_slab      │ 同一个 order 的下一个 slab     │
├────────────────┼──────────────────────────────┤
│ order          │ 该 slab 的 slot 大小等级       │
└────────────────┴──────────────────────────────┘

struct slab_slot_list
┌────────────────┬──────────────────────────────┐
│ next_free      │ 下一个空闲 slot               │
└────────────────┴──────────────────────────────┘
```

这里的设计很简洁：

- 一个 slab 管一个固定大小的对象等级，例如 64B 或 512B。
- 每个 slab 是一块 2MB 连续内存。
- slab 的第一个 slot 被当作 `slab_header_t` 元数据使用。
- 剩余 slot 通过 `next_free` 串成单链表。
- 同一大小等级的多个 slab 通过 `next_slab` 串成 slab 链表。

## 5. 一个 slab 的内存布局

`init_slab_cache(order, size)` 负责把一块 2MB 内存初始化成某个 order 的 slab cache，源码在 [kernel/mm/slab.c:70-98](kernel/mm/slab.c#L70-L98)。核心逻辑：

```c
addr = alloc_slab_memory(size);
slab = (slab_header_t *)addr;

obj_size = order_to_size(order);
/* the first slot is used as metadata */
cnt = size / obj_size - 1;

slot = (slab_slot_list_t *)(addr + obj_size);
slab->free_list_head = (void *)slot;
slab->next_slab = NULL;
slab->order = order;
```

内存布局如下：

```text
一块 slab memory：2MB = SLAB_INIT_SIZE

addr
│
▼
┌──────────────────────┬──────────────────────┬──────────────────────┬─────┬──────────────────────┐
│ slot 0               │ slot 1               │ slot 2               │ ... │ slot N               │
│ used as slab_header  │ free object slot     │ free object slot     │     │ free object slot     │
└──────────────────────┴──────────────────────┴──────────────────────┴─────┴──────────────────────┘
  <── obj_size ──────>  <── obj_size ──────>  <── obj_size ──────>

slot 0:
┌─────────────────────────────────────────────┐
│ slab_header_t                               │
│ - free_list_head ───────┐                   │
│ - next_slab             │                   │
│ - order                 │                   │
└─────────────────────────┼───────────────────┘
                          │
                          ▼
slot 1 ──next_free──► slot 2 ──next_free──► slot 3 ──► ... ──► NULL
```

注意：第一个 slot 被占作元数据，因此一个 slab 中可分配对象数是：

```text
usable_slot_count = SLAB_INIT_SIZE / object_size - 1
```

具体数量为：

```text
object size    total slots in 2MB    usable slots
32 B           65536                 65535
64 B           32768                 32767
128 B          16384                 16383
256 B          8192                  8191
512 B          4096                  4095
1024 B         2048                  2047
2048 B         1024                  1023
```

## 6. slab 如何从 buddy 获得 2MB 内存

`alloc_slab_memory(size)` 在 [kernel/mm/slab.c:43-68](kernel/mm/slab.c#L43-L68)：

```c
order = size_to_order(size / BUDDY_PAGE_SIZE);
addr = get_pages(order);
p_page = virt_to_page(addr);
...
page_num = order_to_size(order);
for (i = 0; i < page_num; i++) {
        page_addr = (void *)((u64)addr + i * BUDDY_PAGE_SIZE);
        page = virt_to_page(page_addr);
        page->slab = addr;
}
```

对于 `SLAB_INIT_SIZE = 2MB`：

```text
size / BUDDY_PAGE_SIZE = 2MB / 4KB = 512 pages
order = log2(512) = 9
get_pages(9) => 从 buddy 申请 2^9 = 512 个连续 4KB pages
```

因此每个 slab cache 实际上向 buddy 申请一个 order-9 的连续页块：

```text
buddy allocator
│
└── get_pages(9)
    │
    ▼
    512 continuous pages = 512 * 4KB = 2MB
    │
    ▼
slab allocator 将这 2MB 切成固定大小 slot
```

`alloc_slab_memory` 还会把这 2MB 覆盖到的每个 `struct page` 的 `page->slab` 字段都设置成该 slab 的起始地址：

```text
2MB slab memory covers 512 pages

page metadata:
┌─────────────┬─────────────┬─────────────┬─────┬─────────────┐
│ page[0]     │ page[1]     │ page[2]     │ ... │ page[511]   │
│ slab = addr │ slab = addr │ slab = addr │     │ slab = addr │
└─────────────┴─────────────┴─────────────┴─────┴─────────────┘
```

这对 `kfree` 判断释放路径非常关键，见后文。

## 7. size 如何映射到 slab order

`alloc_in_slab(size)` 在 [kernel/mm/slab.c:156-167](kernel/mm/slab.c#L156-L167)：

```c
void *alloc_in_slab(u64 size)
{
        int order;

        BUG_ON(size > order_to_size(SLAB_MAX_ORDER));

        order = (int)size_to_order(size);
        if (order < SLAB_MIN_ORDER)
                order = SLAB_MIN_ORDER;

        return _alloc_in_slab(slabs[order], order);
}
```

`size_to_order` 在 [kernel/mm/slab.c:23-36](kernel/mm/slab.c#L23-L36)，本质是向上取整到 2 的幂：

```text
order = ceil(log2(size))
slot_size = 1 << order
如果 order < 5，则提升到 5，即最小 slot 为 32B。
```

映射示例：

```text
request size     raw ceil pow2     final slab order     slot size
1                1                 5                    32 B
16               16                5                    32 B
32               32                5                    32 B
33               64                6                    64 B
100              128               7                    128 B
400              512               9                    512 B
1025             2048              11                   2048 B
2048             2048              11                   2048 B
2049             -                 -                    不走 slab，走 buddy
```

图示：

```text
alloc_in_slab(size)
│
├── BUG_ON(size > 2048)
│
├── order = ceil_log2(size)
│
├── if order < 5: order = 5
│
└── 从 slabs[order] 对应的 slab 链表中取一个空闲 slot
```

## 8. 分配流程：优先当前 slab，再找 next_slab，最后扩容

真正取 slot 的逻辑在 `_alloc_in_slab_nolock`，源码在 [kernel/mm/slab.c:100-131](kernel/mm/slab.c#L100-L131)。流程如下：

```text
_alloc_in_slab_nolock(slab_header, order)
│
├── 1. 看当前 slab_header->free_list_head
│      │
│      ├── 非空：弹出第一个 free slot，返回
│      └── 空：继续
│
├── 2. 遍历 slab_header->next_slab 链表
│      │
│      ├── 找到某个 slab 有 free slot：弹出并返回
│      └── 都没有：继续
│
└── 3. 当前 order 没有空闲 slot
       │
       ├── init_slab_cache(order, SLAB_INIT_SIZE) 新建一个 2MB slab
       ├── new_slab->next_slab = slab_header
       ├── slabs[order] = new_slab
       └── 递归调用 _alloc_in_slab_nolock(new_slab, order)
```

弹出 free slot 的代码模式是：

```c
first_slot = (slab_slot_list_t *)(slab_header->free_list_head);
if (likely(first_slot != NULL)) {
        next_slot = first_slot->next_free;
        slab_header->free_list_head = next_slot;
        return first_slot;
}
```

对应图示：

```text
分配前：

slab->free_list_head
        │
        ▼
      slot A ──► slot B ──► slot C ──► NULL

分配 slot A：

返回 slot A
slab->free_list_head
        │
        ▼
      slot B ──► slot C ──► NULL
```

当当前 slab 满了，会沿 `next_slab` 查找同 order 的其他 slab：

```text
slabs[order]
   │
   ▼
┌────────────┐   next_slab   ┌────────────┐   next_slab   ┌────────────┐
│ slab #3    │ ────────────► │ slab #2    │ ────────────► │ slab #1    │
│ maybe full │              │ maybe full │              │ maybe free │
└────────────┘              └────────────┘              └────────────┘
```

如果整个链表都没有空闲 slot，则新建 slab，并把新 slab 插到链表头：

```text
扩容前：

slabs[order] ──► old head ──► old slab ──► NULL

扩容后：

slabs[order] ──► new slab ──► old head ──► old slab ──► NULL
```

## 9. 释放流程：通过 page->slab 找回所属 slab

`kfree` 的分流逻辑在 [kernel/mm/kmalloc.c:76-86](kernel/mm/kmalloc.c#L76-L86)：

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

释放时并没有记录“这个指针当初是 kmalloc 了多少字节”。它通过地址所在 page 的元数据判断：

```text
kfree(ptr)
│
├── p_page = virt_to_page(ptr)
│
├── if p_page->slab != NULL
│      └── 说明 ptr 落在某个 slab memory 里，调用 free_in_slab(ptr)
│
└── else
       └── 说明 ptr 是 buddy 直接分配的 page chunk，调用 buddy_free_pages(...)
```

`free_in_slab` 在 [kernel/mm/slab.c:169-182](kernel/mm/slab.c#L169-L182)：

```c
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
```

也就是把释放的 slot 头插回该 slab 的 free list：

```text
释放前：

slab->free_list_head
        │
        ▼
      slot B ──► slot C ──► NULL

释放 slot A：

slot A->next_free = slab->free_list_head
slab->free_list_head = slot A

释放后：

slab->free_list_head
        │
        ▼
      slot A ──► slot B ──► slot C ──► NULL
```

这里能找回 `slab`，是因为创建 slab memory 时，`alloc_slab_memory` 已经把覆盖到的每个 page 的 `page->slab` 设置为 slab 起始地址，见 [kernel/mm/slab.c:60-65](kernel/mm/slab.c#L60-L65)。

## 10. 与 buddy 的边界和协作

buddy allocator 的页大小为 4KB，定义在 [kernel/include/mm/buddy.h:17-23](kernel/include/mm/buddy.h#L17-L23)：

```c
#define BUDDY_PAGE_SIZE (0x1000)
#define BUDDY_MAX_ORDER (14UL)
```

`get_pages(order)` 在 [kernel/mm/kmalloc.c:88-105](kernel/mm/kmalloc.c#L88-L105)：

```c
for (i = 0; i < physmem_map_num; ++i) {
        p_page = buddy_get_pages(&global_mem[i], order);
        if (p_page) {
                break;
        }
}
...
return page_to_virt(p_page);
```

因此两层分配器的边界可以画成：

```text
请求大小                         分配器路径                 实际分配粒度
────────────────────────────────────────────────────────────────────────
1 ~ 32 B                         slab order 5              32 B slot
33 ~ 64 B                        slab order 6              64 B slot
65 ~ 128 B                       slab order 7              128 B slot
129 ~ 256 B                      slab order 8              256 B slot
257 ~ 512 B                      slab order 9              512 B slot
513 ~ 1024 B                     slab order 10             1024 B slot
1025 ~ 2048 B                    slab order 11             2048 B slot
2049 ~ 4096 B                    buddy order 0             1 page = 4KB
4097 ~ 8192 B                    buddy order 1             2 pages = 8KB
8193 ~ 16384 B                   buddy order 2             4 pages = 16KB
...                              ...                       ...
```

从层次上看：

```text
小对象：

kmalloc(100)
│
├── size <= 2048
├── alloc_in_slab(100)
├── size_to_order(100) = 7
├── use slabs[7]
└── 返回一个 128B slot

大对象：

kmalloc(5000)
│
├── size > 2048
├── size_to_page_order(5000) = 1
├── get_pages(1)
└── 返回 2 个连续 4KB pages，即 8KB
```

## 11. slab 分配器“不做”的事情

这个实现是教学/实验性质的简化 slab allocator。结合源码可以看到它没有实现一些更复杂的机制：

```text
当前实现没有：

1. 没有 per-CPU cache
2. 没有 partial/full/empty slab 分类链表
3. 没有对象构造/析构函数
4. 没有回收全空 slab 给 buddy 的逻辑
5. 没有显式记录每个 slot 的分配大小
6. 没有锁；函数名里有 _nolock，但外层 _alloc_in_slab 也没有加锁
```

尤其是第 4 点值得注意：`free_in_slab` 只是把 slot 插回 free list，并不会在某个 slab 全空时调用 `buddy_free_pages` 归还整块 2MB。因此 slab 从 buddy 申请的 2MB 一旦扩容出来，就长期留在 slab allocator 中复用。

图示：

```text
buddy ──get_pages(9)──► slab 2MB
                         │
                         ├── slot allocated/free/allocated/free ...
                         │
                         └── 即使所有 slot 都 free，当前代码也不会：
                             slab 2MB ──free_pages──► buddy
```

## 12. 小结

当前项目 slab allocator 的核心设计可以概括为：

```text
1. kmalloc 的小对象边界是 2KB，不是 4KB。

2. slab 管理 7 个固定大小等级：
   32, 64, 128, 256, 512, 1024, 2048 bytes。

3. 每个大小等级至少有一个 2MB slab memory。
   init_slab() 会初始化 7 个 slab，意味着启动时会从 buddy 预先拿走：
   7 * 2MB = 14MB。

4. 每个 slab 的第一个 slot 存 slab_header_t，剩余 slot 形成 free list。

5. 分配时根据 size 向上取整到 2 的幂，找到 slabs[order]，从 free list 弹出一个 slot。

6. 释放时通过 virt_to_page(ptr)->slab 找回 slab header，把 slot 头插回 free list。

7. slab 扩容时继续从 buddy 申请 2MB，并插到 slabs[order] 链表头。

8. 当前实现不会把空 slab 归还给 buddy。
```

整体结构图：

```text
                         ┌────────────────────────────┐
                         │          kmalloc           │
                         └─────────────┬──────────────┘
                                       │
                   ┌───────────────────┴───────────────────┐
                   │                                       │
            size <= 2048                              size > 2048
                   │                                       │
                   ▼                                       ▼
          ┌─────────────────┐                    ┌─────────────────┐
          │ slab allocator  │                    │ buddy allocator │
          └────────┬────────┘                    └────────┬────────┘
                   │                                      │
                   │                                      ▼
                   │                         4KB page chunks, 2^order pages
                   │
                   ▼
         slabs[5..11], one list per object size
                   │
        ┌──────────┼──────────┬──────────┬──────────┐
        ▼          ▼          ▼          ▼          ▼
      32B        64B        128B       ...       2048B
        │          │          │                     │
        ▼          ▼          ▼                     ▼
   2MB slab   2MB slab   2MB slab              2MB slab
        │          │          │                     │
        ▼          ▼          ▼                     ▼
   free slot  free slot  free slot             free slot
   linked list linked list linked list         linked list
```
