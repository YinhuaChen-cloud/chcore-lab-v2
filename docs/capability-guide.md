# Capability：从概念到 SJTU-chcore 实现

本文面向第一次接触 capability 的读者，先解释它在操作系统设计中的意义，再结合 SJTU-chcore 的源码说明这些概念如何落地。

> 相关源码主要位于：
>
> - [`kernel/include/object/object.h`](../kernel/include/object/object.h)：内核对象的统一抽象。
> - [`kernel/include/object/cap_group.h`](../kernel/include/object/cap_group.h)：进程的 capability 空间。
> - [`kernel/object/capability.c`](../kernel/object/capability.c)：capability 的分配、复制、移动、释放。
> - [`kernel/object/cap_group.c`](../kernel/object/cap_group.c)：`cap_group` 的创建与 slot table 管理。
> - [`kernel/object/memory.c`](../kernel/object/memory.c)：PMO capability 如何参与内存映射。
> - [`kernel/object/thread.c`](../kernel/object/thread.c)：线程、进程与 capability 的关系。
> - [`libchcore/src/capability/capability.c`](../libchcore/src/capability/capability.c)：用户态 capability API。

---

## 1. 一句话理解 capability

**Capability 是一种“可被内核验证的访问凭证”：谁持有某个 capability，谁就被允许访问它指向的某个内核对象。**

它可以粗略类比为 Linux 的文件描述符：

```text
Linux 文件描述符：

    int fd = open("/tmp/a", O_RDWR);

    用户态看到：fd = 3
    内核态维护：进程 fd table[3] -> struct file -> inode/device/pipe/...


Capability：

    int cap = sys_create_pmo(...);

    用户态看到：cap = 5
    内核态维护：进程 slot_table[5] -> object_slot -> kernel object
```

用户程序看到的 capability 通常只是一个整数，例如 `5`。但这个整数本身不是对象地址，也不是裸权限位。它只有在当前进程的 capability table 中被内核解释时才有意义。

因此 capability 的关键点是：

1. **对象由内核保存**：用户态不能直接拿到内核对象指针。
2. **capability 是对象引用**：用户态只能把 capability 编号传给系统调用。
3. **内核检查 capability table**：只有 table 中存在对应 slot，且对象类型符合要求，操作才会继续。
4. **权限随 capability 传递**：如果一个进程没有某个 capability，它原则上就不能操作对应对象。

---

## 2. capability 不是“权限检查函数”，而是一种系统结构

传统系统常见的权限判断方式是：

```text
请求：进程 P 想操作对象 O

内核：
    1. 查 P 的 uid/gid/label/namespace
    2. 查 O 的 owner/mode/ACL/security label
    3. 根据全局策略判断是否允许
```

capability 的思路更像：

```text
请求：进程 P 想操作对象 O

内核：
    1. P 是否持有指向 O 的 capability？
    2. capability 的类型和权限是否满足本次操作？
```

也就是说，capability 把“是否有权访问对象”变成“是否持有对象的访问凭证”。

### 2.1 为什么 capability 不能简单理解为普通整数

在 SJTU-chcore 中，用户态确实把 capability 当作整数传给系统调用。但这个整数只在对应进程的 `cap_group` 中有效：

```text
Process A 的 cap 3：

    A.cap_group.slot_table[3] -> PMO object

Process B 的 cap 3：

    B.cap_group.slot_table[3] -> Thread object

或者：
    B.cap_group.slot_table[3] -> NULL
```

所以 capability 编号不是全局对象 ID。它更像“某个进程本地 capability table 的索引”。

用户程序即使猜到了一个整数，也只能让内核在**自己的** `cap_group` 中查这个整数。如果 slot 不存在，或者类型不对，内核会返回 `-ECAPBILITY`。

### 2.2 capability 的安全性来自哪里

capability 的安全性主要来自三点：

```text
             用户态                         内核态
    +----------------------+        +-----------------------+
    | 只能持有 cap number   | -----> | cap_group.slot_table  |
    | 例如 3、4、5          | syscall| object_slot           |
    +----------------------+        | kernel object         |
                                    +-----------------------+

    用户态不能直接：
      - 构造 object_slot
      - 修改 slot_table
      - 获得 kernel object 指针
```

也就是说，用户态持有的是“名字”或“句柄”；真正的绑定关系由内核维护。

---

## 3. capability 为什么适合微内核

微内核的核心目标是：**把尽可能多的系统服务放到用户态，只把最基础的机制留在内核中。**

典型微内核会把下面这些组件拆出去：

```text
传统宏内核：

    +---------------------------------------------------+
    | Kernel                                            |
    |  scheduler | memory | fs | network | driver | ... |
    +---------------------------------------------------+
                         ^
                         |
                      user apps


微内核：

    +--------------------+       +---------+  +---------+  +---------+
    | Microkernel        | <---> | FS srv  |  | Net srv |  | Driver  |
    | scheduling         |       +---------+  +---------+  +---------+
    | address spaces     |              ^          ^            ^
    | IPC                |              |          |            |
    | capabilities       |          user apps talk to services via IPC
    +--------------------+
```

但这样会带来一个问题：如果文件系统、驱动、网络栈都在用户态，那么它们如何安全地访问资源？

例如：

1. 文件系统服务需要访问磁盘设备。
2. 网络服务需要访问网卡设备。
3. 用户进程需要让文件系统服务读写自己的缓冲区。
4. 进程管理器需要创建新进程和线程。

如果没有 capability，系统往往需要依赖全局身份、全局名字空间和复杂访问控制策略。微内核中服务之间频繁交互，这会变得很难维护。

capability 提供了一种自然模型：

```text
资源访问 = 持有 capability
授权别人访问 = 复制/移动 capability
撤销访问 = 删除 capability
隔离边界 = 每个进程有自己的 cap_group
```

### 3.1 capability 支撑微内核的四个设计点

#### 设计点一：资源对象化

内核不再暴露“随便传地址即可访问”的资源，而是把资源封装成对象：

```text
PMO       -> 物理内存对象
Thread    -> 线程对象
VMSpace   -> 虚拟地址空间对象
CapGroup  -> capability 空间，也就是进程抽象
```

#### 设计点二：访问显式化

调用系统调用时，用户态必须传 capability：

```text
sys_map_pmo(target_cap_group_cap, pmo_cap, addr, perm, len)
            ^                     ^
            |                     |
            |                     +-- 我是否有这个 PMO？
            +-- 我是否有目标进程的 cap_group？
```

内核不会因为某个进程“名字上应该能访问”就放行，而是检查它是否真的持有相关 capability。

#### 设计点三：授权可传递

如果服务 A 想让服务 B 使用某个对象，可以通过 `cap_copy()` 或相关系统调用把 capability 复制到 B 的 `cap_group`。

```text
Before:

    Process A slot_table             Process B slot_table
    +------+                         +------+
    |  7   | ---> PMO object         |      |
    +------+                         +------+

cap_copy(A, B, 7)

After:

    Process A slot_table             Process B slot_table
    +------+                         +------+
    |  7   | ---+                    |  3   | ---+
    +------+    |                    +------+    |
                +----> PMO object <--------------+
```

#### 设计点四：最小内核只负责机制

微内核可以只提供这些机制：

```text
- 创建对象
- 创建 capability
- 检查 capability
- 复制/移动/删除 capability
- IPC 时传递 capability
- 地址空间和线程调度等基础机制
```

而“谁应该获得哪些 capability”可以由用户态系统服务决定。例如进程管理器创建新进程时，把初始内存、主线程、通信端点等 capability 分发给新进程。

需要注意：**capability 不会自动变出一个微内核。** 它只是让微内核的对象隔离、授权传递和服务拆分更容易实现。

---

## 4. SJTU-chcore 如何把复杂功能放到用户态，同时保证内核安全

前面讲的是 capability 与微内核的设计关系。本节进一步回答一个更具体的问题：

> SJTU-chcore 到底如何把内存管理、文件系统、进程管理等复杂功能尽量放到用户态，让普通应用程序使用，同时还不破坏内核安全？

先给出结论：**ChCore 的安全边界不是靠“用户态服务自觉不要乱来”，而是靠内核只暴露少量可验证的机制；复杂策略运行在用户态，但每次触碰内核对象时都必须提交 capability，由内核统一检查。**

可以把整个系统理解成三层：

```text
普通应用程序
    |
    |  libc / libchcore API
    v
用户态系统服务
    例如：进程管理器、文件系统服务、设备服务、网络服务
    |
    |  syscall + capability + PMO/IPC
    v
微内核
    只负责：线程、地址空间、PMO、capability、异常/系统调用、基础调度
```

### 4.1 “复杂策略在用户态，基础机制在内核态”是什么意思

宏内核里，文件系统、驱动、网络协议栈、进程管理策略等大量代码都在内核中运行。它们一旦出错，常常就是内核崩溃或任意内核内存破坏。

微内核的思路是把系统拆成两类东西：

```text
机制 mechanism：
    必须由内核掌控的最小能力。
    例如：创建地址空间、切换线程、检查 capability、映射 PMO。

策略 policy：
    可以由普通用户态服务决定的复杂逻辑。
    例如：哪个进程应该启动、文件路径如何解析、缓存如何替换、设备请求如何排队。
```

在 ChCore 中，这种拆分大致表现为：

```text
微内核保留：

    - cap_group / capability table
    - thread / scheduler 基础机制
    - vmspace / page table 操作
    - PMO 这种可授权的内存对象
    - syscall 入口与参数检查
    - copy_from_user / copy_to_user 这类用户态访问边界

用户态服务负责：

    - 创建和管理普通进程的策略
    - 把 ELF、文件内容或请求数据组织成 PMO
    - 通过 IPC 响应其它应用请求
    - 决定是否把某个 capability 转交给另一个进程
    - 文件系统、设备、网络等更复杂服务的业务逻辑
```

注意当前 SJTU-chcore lab 代码并没有完整实现真实文件系统和完整 IPC 子系统，很多服务只是在后续实验或完整 ChCore 中展开；但是当前代码已经把核心安全模型搭好了：**对象必须通过 capability 访问，跨进程资源共享必须通过 capability 复制或映射。**

### 4.2 普通应用如何使用用户态系统服务

一个应用程序并不应该直接知道磁盘驱动怎么工作，也不应该直接修改别的进程页表。它通常通过用户态库和系统服务间接完成复杂操作：

```text
Application
    |
    |  open/read/write/spawn 等高级 API，完整系统中由 libc 或系统库包装
    v
User-level server
    |
    |  用 syscall 操作 PMO、thread、cap_group 等内核对象
    v
Microkernel
```

以“读文件到用户缓冲区”为例，微内核风格的流程不是让文件系统代码跑进内核，而是让文件系统服务作为普通用户进程运行：

```text
App                         FS Server                         Kernel
 |                              |                                |
 | 1. 创建/准备 buffer PMO       |                                |
 |----------------------------->|                                |
 |   请求 read(file, buffer_cap) |                                |
 |                              | 2. 检查请求、解析路径、读设备     |
 |                              |    这些复杂逻辑在用户态完成       |
 |                              |                                |
 |                              | 3. 使用被授权的 PMO 写入数据      |
 |                              |------------------------------->|
 |                              |   sys_write_pmo(buffer_cap, ...) |
 |                              |                                |
 | 4. App 从 buffer 中看到数据    |                                |
```

这里最重要的是：FS Server 并不是“天然能访问 App 的内存”。它必须拿到 App 显式转交的 PMO capability，才能读写这块共享缓冲区。

### 4.3 安全边界一：用户态服务仍然只是普通用户进程

把文件系统或进程管理器放到用户态，并不意味着它们可以任意操作内核。

它们和普通应用一样运行在 EL0：

```text
EL0 用户态：

    App process
    FS server
    Process manager
    Network server
    Driver-like user service

EL1 内核态：

    Microkernel
```

因此用户态服务天然受到硬件隔离：

```text
用户态服务不能直接：

    - 写内核内存
    - 修改页表
    - 伪造 struct object
    - 伪造 object_slot
    - 修改别的进程 cap_group
    - 直接访问别的进程地址空间
```

它只能通过系统调用请求内核代为执行操作。而每个系统调用都会重新回到内核的检查逻辑。

### 4.4 安全边界二：系统调用入口只接受 capability，不接受裸对象指针

ChCore 的很多系统调用都不让用户态传内核对象指针，而是传 capability 编号。

例如内存映射：

```text
sys_map_pmo(target_cap_group_cap, pmo_cap, addr, perm, len)
```

这个接口不会说“请给我一个 `struct pmobject *`”。用户态根本拿不到这个指针。它只能传：

```text
target_cap_group_cap：我是否有权操作目标进程？
pmo_cap：我是否有权使用这段 PMO？
```

内核侧再调用：

```text
obj_get(current_cap_group, pmo_cap, TYPE_PMO)
obj_get(current_cap_group, target_cap_group_cap, TYPE_CAP_GROUP)
```

这一步同时完成两类检查：

```text
1. capability 是否存在？
2. capability 指向的对象类型是否正确？
```

所以，即使一个恶意用户态服务猜到某个整数，也不能绕过当前进程的 `cap_group`：

```text
Malicious Server
    |
    | sys_map_pmo(guess_cap, guess_pmo, ...)
    v
Kernel
    |
    | 在 Malicious Server 自己的 cap_group 中查 guess_cap / guess_pmo
    | 查不到或类型不对：-ECAPBILITY
```

### 4.5 安全边界三：跨进程共享必须显式转交 capability

用户态服务要给其它进程提供功能，通常需要共享某些对象。ChCore 不鼓励通过“全局名字 + 隐式权限”来共享，而是通过 capability 复制或移动。

典型模型如下：

```text
App cap_group                       FS Server cap_group

cap 5 -> buffer PMO                 cap 3 -> FS internal object

App 请求 FS 读文件，并把 cap 5 转交给 FS：

App cap_group                       FS Server cap_group

cap 5 --------+                     cap 9 --------+
              |                                    |
              +------------> buffer PMO <----------+
```

在源码层面，这类动作由这些函数支撑：

```text
cap_copy(src_cap_group, dest_cap_group, src_slot_id)
cap_move(src_cap_group, dest_cap_group, src_slot_id)
sys_cap_copy_to(dest_cap_group_cap, src_slot_id)
sys_cap_copy_from(src_cap_group_cap, src_slot_id)
sys_transfer_caps(dest_group_cap, src_caps_buf, nr_caps, dst_caps_buf)
```

因此，“授权 FS Server 写入我的 buffer”不是靠 FS Server 知道我的虚拟地址，而是靠 App 把 PMO capability 交给 FS Server。

### 4.6 安全边界四：共享内存用 PMO 表达，而不是暴露任意地址空间

用户态服务之间需要高效传输数据时，不能简单把一个进程的裸虚拟地址交给另一个进程。虚拟地址只在各自 `vmspace` 中有意义，而且直接共享地址空间会破坏隔离。

ChCore 使用 PMO 作为共享内存对象：

```text
PMO = Physical Memory Object

    - 是一个内核对象
    - 有自己的 capability
    - 可以被映射到一个或多个进程的 vmspace
    - 映射时可指定权限：读 / 写 / 执行
```

共享流程可以画成：

```text
         PMO object
        /          \
       /            \
      v              v
App vmspace      Server vmspace
addr A           addr B

同一个 PMO 可以映射到不同进程的不同虚拟地址。
两个进程看到的虚拟地址不同，但背后是同一个内存对象。
```

源码里，用户态库提供了 PMO 包装接口：

```text
chcore_pmo_create(size, type)
chcore_pmo_map(target_cap_group_cap, pmo_cap, addr, perm)
chcore_pmo_write(pmo_cap, offset, buf, len)
chcore_pmo_read(pmo_cap, offset, buf, len)
chcore_pmo_auto_map(pmo_cap, size, perm)
```

内核侧则由 `sys_create_pmo()`、`sys_map_pmo()`、`sys_write_pmo()`、`sys_read_pmo()` 等系统调用实现。

这使用户态服务能够高效传输大块数据，同时仍由内核检查：

```text
是否持有 PMO capability？
是否持有目标 cap_group capability？
映射权限是否合法？
映射范围是否合法？
```

### 4.7 以“进程管理器创建应用”为例

完整微内核系统中，进程管理器通常是用户态服务。它负责决定创建哪些进程、加载哪些程序、给新进程哪些初始资源。

在 ChCore 的 capability 模型里，可以把流程理解为：

```text
Process Manager
    |
    | 1. 请求内核创建 cap_group
    v
Kernel
    |
    | 返回 new_process_cap_group_cap
    v
Process Manager
    |
    | 2. 创建 PMO，装入 ELF 代码/数据
    | 3. sys_map_pmo(new_process_cap_group_cap, code_pmo_cap, ...)
    | 4. sys_create_thread(new_process_cap_group_cap, entry, stack, ...)
    v
New Process
```

安全点在于：进程管理器不是直接改内核调度队列或目标页表，而是通过 capability 调用内核机制：

```text
创建进程：必须有创建 cap_group 的授权
映射内存：必须同时有 PMO cap 和目标 cap_group cap
创建线程：必须持有目标 cap_group cap
传递初始资源：必须通过 cap_copy / transfer_caps
```

当前 lab 中 `sys_create_cap_group()` 还限制为只有 `ROOT_PID` 可以调用，这可以理解成教学版的“只有根服务 / 进程管理器可以创建新进程”。

### 4.8 以“文件系统服务”为例

当前 lab 尚未实现完整文件系统，但 capability 机制已经能解释完整 ChCore 或类似微内核中 FS Server 的安全模型。

假设应用想读文件：

```text
App:
    1. 创建 buffer PMO
    2. 把 buffer PMO capability 随请求发给 FS Server
    3. 等待 FS Server 返回

FS Server:
    1. 收到请求和 buffer PMO capability
    2. 在用户态完成路径解析、权限策略、缓存逻辑
    3. 通过 PMO 写入数据
    4. 返回结果

Kernel:
    1. 只负责检查 capability 是否真实存在
    2. 只负责 PMO 映射/读写/传递等基础机制
    3. 不需要理解复杂文件系统逻辑
```

对应的安全性质是：

```text
FS Server 出 bug 时：

    - 可能破坏自己的用户态地址空间
    - 可能返回错误文件内容
    - 可能错误处理自己已经持有的 capability

但它不能直接：

    - 写任意内核内存
    - 修改 App 未授权的 PMO
    - 操作它没有 capability 的进程
    - 随意伪造其它对象 capability
```

这就是“把复杂功能放到用户态还能保证内核安全”的核心。

### 4.9 内核安全性来自哪些源码机制

回到 SJTU-chcore 源码，可以把安全性归纳为五个机制：

```text
1. 硬件特权级隔离
   用户服务在 EL0，内核在 EL1。

2. 每个进程独立 cap_group
   capability 编号只在自己的 slot_table 中解释。

3. 系统调用中使用 obj_get() 做对象查找和类型检查
   错误 capability 返回 -ECAPBILITY。

4. PMO / VMSpace / Thread / CapGroup 都是 object
   内核统一维护 refcount、copies_head 和对象释放流程。

5. 跨进程资源共享必须调用 cap_copy() / sys_transfer_caps() / sys_map_pmo()
   授权关系由内核建立，而不是用户态自己写内核表。
```

可以用一张总图总结：

```text
                    +-----------------------------+
                    |        Microkernel          |
                    |-----------------------------|
                    | object / capability checks  |
                    | PMO mapping                 |
                    | thread + vmspace mechanism  |
                    | syscall boundary            |
                    +--------------+--------------+
                                   ^
                                   |
                    syscall(cap numbers only)
                                   |
        +--------------------------+--------------------------+
        |                                                     |
        v                                                     v
+-------------------+                               +-------------------+
| User App          |     IPC / shared PMO / caps    | User FS Server    |
|-------------------| <---------------------------> |-------------------|
| own cap_group     |                               | own cap_group     |
| own vmspace       |                               | own vmspace       |
| no kernel pointer |                               | no kernel pointer |
+-------------------+                               +-------------------+
```

### 4.10 一句话总结这一节

SJTU-chcore 的微内核安全思路是：**把文件系统、进程管理、设备服务等复杂策略放到用户态；内核只提供对象、capability、PMO、地址空间和线程等基础机制；用户态服务每次想操作资源，都必须通过 capability 让内核验证它是否真的被授权。**

---

## 5. SJTU-chcore 中的核心模型

SJTU-chcore 的 capability 模型可以用一张图概括：

```text
                         current_thread
                              |
                              v
                        current_cap_group
                              |
                              v
                    +---------------------+
                    | struct cap_group    |
                    |                     |
                    | slot_table          |
                    | thread_list         |
                    | thread_cnt          |
                    | pid                 |
                    | cap_group_name      |
                    +----------+----------+
                               |
                               v
                    +---------------------+
                    | struct slot_table   |
                    |                     |
                    | slots_size          |
                    | slots[]             |
                    | slots_bmp           |
                    | full_slots_bmp      |
                    +----------+----------+
                               |
                  cap number   |   slots[cap]
                  e.g. 5       v
                    +---------------------+
                    | struct object_slot  |
                    |                     |
                    | slot_id = 5         |
                    | cap_group           |
                    | isvalid             |
                    | rights              |
                    | object ------------+|
                    | copies              ||
                    +---------------------+|
                                           |
                                           v
                    +-------------------------------+
                    | struct object                 |
                    |                               |
                    | type                          |
                    | size                          |
                    | copies_head                   |
                    | refcount                      |
                    | opaque[] ------------------+  |
                    +----------------------------|--+
                                                 v
                         actual object data: cap_group / thread / pmo / vmspace
```

这张图里最重要的是四层：

```text
cap_group -> slot_table -> object_slot -> object -> real kernel object
```

用户态传入的整数 capability 对应的是 `slot_table` 的下标。

---

## 6. 内核对象：`struct object`

SJTU-chcore 把 capability 能指向的资源统一抽象为 `struct object`。定义在 [`kernel/include/object/object.h`](../kernel/include/object/object.h)：

```text
struct object
    type         对象类型
    size         真实对象数据大小
    copies_head  所有指向该对象的 capability slot 链表
    refcount     引用计数
    opaque[]     真实对象数据起点
```

对象类型包括：

```text
TYPE_CAP_GROUP   进程 / capability 空间
TYPE_THREAD      线程
TYPE_CONNECTION  IPC 连接，实验代码中预留
TYPE_PMO         物理内存对象
TYPE_VMSPACE     虚拟地址空间
TYPE_SEMAPHORE   信号量，后续实验使用
```

### 6.1 为什么需要统一 object 头

统一对象头让 capability 子系统可以用同一套逻辑管理不同资源：

```text
capability.c 不需要知道 PMO 里面长什么样
capability.c 只需要知道：

    object->type
    object->refcount
    object->copies_head
    object->opaque
```

真实对象存在 `opaque[]` 后面：

```text
内存布局：

    +-------------------------+
    | struct object metadata  |
    +-------------------------+
    | opaque[]                |  <- 返回给内核其他模块的真实对象指针
    |                         |
    | struct pmobject         |
    | or struct thread        |
    | or struct cap_group     |
    +-------------------------+
```

例如 `obj_alloc(TYPE_PMO, sizeof(*pmo))` 返回的是 `object->opaque`，也就是 `struct pmobject *`。需要从真实对象反推对象头时，代码使用 `container_of()`。

---

## 7. capability slot：`struct object_slot`

`struct object_slot` 定义在 [`kernel/include/object/cap_group.h`](../kernel/include/object/cap_group.h)：

```text
struct object_slot
    slot_id     这个 capability 在当前 cap_group 中的编号
    cap_group   这个 slot 属于哪个 cap_group
    isvalid     是否有效
    rights      权限位
    object      指向真实内核对象的 object 头
    copies      链入 object->copies_head
```

可以把 `object_slot` 理解成“capability 的内核态实体”。用户态看到的只是 `slot_id`，内核态保存完整 slot。

```text
用户态：

    int cap = 8;

内核态：

    cap_group->slot_table.slots[8]
        -> object_slot {
               slot_id = 8,
               cap_group = 当前进程,
               object = 某个 PMO/Thread/VMSpace/CapGroup,
               rights = ...,
           }
```

### 7.1 `rights` 字段

`object_slot` 中有 `rights` 字段，表示 capability 可以携带权限位。完整 capability 系统通常会用它表达读、写、执行、转授权等权限。

但在当前 SJTU-chcore lab 代码中，大部分 `cap_alloc(..., rights)` 都传 `0`，且源码主要依赖“是否持有 capability”和“对象类型是否匹配”来做检查。因此阅读本项目时，应把 `rights` 理解为**设计上预留的权限字段**，而不是当前实验中已经充分实现的访问控制。

---

## 8. `cap_group`：进程就是 capability 空间

`struct cap_group` 也定义在 [`kernel/include/object/cap_group.h`](../kernel/include/object/cap_group.h)。在 SJTU-chcore 中，它基本就是“进程”的核心抽象：

```text
struct cap_group
    slot_table       该进程拥有的所有 capability
    thread_list      属于该进程的线程链表
    thread_cnt       线程数量
    pid              进程 ID
    cap_group_name   调试用名称
```

这与传统“进程 = 地址空间 + 线程集合 + 资源表”的理解是一致的：

```text
ChCore process / cap_group

    +---------------------------------------------------+
    | cap_group                                         |
    |                                                   |
    |  slot_table                                       |
    |    cap 0 -> self cap_group object                 |
    |    cap 1 -> vmspace object                        |
    |    cap 2 -> main thread object                    |
    |    cap 3 -> PMO object                            |
    |    ...                                            |
    |                                                   |
    |  thread_list                                      |
    |    thread A -> thread B -> ...                    |
    +---------------------------------------------------+
```

### 8.1 两个固定 capability

SJTU-chcore 约定每个 `cap_group` 中：

```text
CAP_GROUP_OBJ_ID = 0   第 0 个 capability 指向自身 cap_group
VMSPACE_OBJ_ID   = 1   第 1 个 capability 指向自身 vmspace
```

这两个固定 slot 非常重要：

```text
slot 0: 我是谁？
    cap_group itself

slot 1: 我的地址空间是什么？
    vmspace
```

例如 [`kernel/object/thread.c`](../kernel/object/thread.c) 中初始化线程时，会通过这两个固定 capability 取出线程所属的 `cap_group` 和 `vmspace`。

---

## 9. slot table：capability 编号从哪里来

每个 `cap_group` 内部有一个 `slot_table`：

```text
struct slot_table
    slots_size      当前 table 容量
    slots[]         object_slot 指针数组
    slots_bmp       哪些 slot 已被占用
    full_slots_bmp  哪些 bitmap word 已满，用于加速查找
```

分配 capability 时，内核调用 `alloc_slot_id()` 找一个空 slot：

```text
alloc_slot_id(cap_group)

    1. 在 full_slots_bmp 中找一个未满的 bitmap word
    2. 在 slots_bmp 中找一个空 bit
    3. 如果找不到，expand_slot_table()
    4. 设置 bit，返回 slot_id
```

释放 capability 时，`free_slot_id()` 会清除 bitmap 并把 `slots[slot_id]` 设为 `NULL`。

```text
slots_bmp:

    bit = 0 表示空闲
    bit = 1 表示该 slot 已分配

slots[]:

    slots[slot_id] -> object_slot
```

---

## 10. capability 生命周期

下面按典型生命周期理解 [`kernel/object/capability.c`](../kernel/object/capability.c)。

### 10.1 创建对象：`obj_alloc()`

`obj_alloc(type, size)` 做三件事：

```text
1. 分配 sizeof(struct object) + size 的内存
2. 初始化 object metadata
3. 返回 object->opaque，即真实对象数据区
```

图示：

```text
obj_alloc(TYPE_PMO, sizeof(struct pmobject))

    +--------------------+
    | struct object      |
    | type = TYPE_PMO    |
    | refcount = 0       |
    | copies_head = empty|
    +--------------------+
    | struct pmobject    |  <- 返回这个地址
    +--------------------+
```

注意：`obj_alloc()` 只创建对象，不自动创建 capability。因此对象刚分配出来时 `refcount = 0`。

### 10.2 安装 capability：`cap_alloc()`

`cap_alloc(cap_group, obj, rights)` 把一个对象安装进某个 `cap_group` 的 slot table 中。

流程：

```text
cap_alloc(cap_group, obj, rights)

    obj -> container_of -> object metadata
    slot_id = alloc_slot_id(cap_group)
    slot = kmalloc(struct object_slot)

    slot->slot_id   = slot_id
    slot->cap_group = cap_group
    slot->isvalid   = true
    slot->rights    = rights
    slot->object    = object

    list_add(slot->copies, object->copies_head)
    object->refcount = 1
    install_slot(cap_group, slot_id, slot)

    return slot_id
```

安装后：

```text
cap_group.slot_table.slots[slot_id]
        |
        v
    object_slot
        |
        v
    object metadata
        |
        v
    real object data
```

### 10.3 使用 capability：`obj_get()` / `get_opaque()`

大多数系统调用不会直接相信用户传进来的整数，而会用 `obj_get()` 把 capability 转成内核对象指针。

例如 [`kernel/object/memory.c`](../kernel/object/memory.c) 中：

```text
pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO)
```

语义是：

```text
在当前进程的 cap_group 中查 pmo_cap：

    1. slot_id 是否有效？
    2. slot 是否存在？
    3. slot->object->type 是否为 TYPE_PMO？
    4. 如果都满足，返回 object->opaque，并增加 refcount
```

如果失败，返回 `NULL`，系统调用通常返回 `-ECAPBILITY`。

图示：

```text
syscall argument: pmo_cap = 4

current_cap_group
    |
    v
slot_table[4]
    |
    v
object_slot
    |
    v
object(type = TYPE_PMO)
    |
    v
pmobject *
```

### 10.4 释放引用：`obj_put()`

`obj_get()` 会增加对象引用计数，所以用完后要 `obj_put()`。

```text
obj_get()  -> refcount + 1
obj_put()  -> refcount - 1
```

当引用计数变为 0 时，`obj_put()` 会调用 `__free_object()` 释放对象。

### 10.5 复制 capability：`cap_copy()`

`cap_copy(src_cap_group, dest_cap_group, src_slot_id)` 把源进程中的某个 capability 复制到目标进程。

复制前：

```text
src_cap_group                     dest_cap_group

slot 6                            empty
  |
  v
object_slot A
  |
  v
object refcount = 1
```

复制后：

```text
src_cap_group                     dest_cap_group

slot 6                            slot 2
  |                                 |
  v                                 v
object_slot A                    object_slot B
  |                                 |
  +-------------+-------------------+
                v
        object refcount = 2
```

`object->copies_head` 会链接所有指向该对象的 `object_slot`。这让内核可以知道“有哪些 capability 正在指向这个对象”。

### 10.6 移动 capability：`cap_move()`

`cap_move()` 可以理解为：

```text
cap_move(src, dest, cap)

    new_cap = cap_copy(src, dest, cap)
    cap_free(src, cap)
```

也就是把 capability 从源 `cap_group` 转移到目标 `cap_group`。

### 10.7 释放 capability：`cap_free()`

`cap_free()` 删除一个 slot，并减少对象引用计数：

```text
cap_free(cap_group, slot_id)

    1. 检查 slot 是否有效
    2. 从 cap_group 的 slot_table 删除 slot
    3. 从 object->copies_head 删除 slot
    4. kfree(slot)
    5. object->refcount--
    6. 如果 refcount 变为 0，释放 object
```

---

## 11. 创建进程：`cap_group` 也是对象

SJTU-chcore 有一个很重要的设计：**`cap_group` 本身也是 capability 指向的内核对象。**

这意味着：如果进程 A 持有进程 B 的 `cap_group` capability，A 就可以在系统调用中把 B 作为目标对象，例如把 PMO 映射到 B 的地址空间。

```text
Process A holds:

    cap 4 -> Process B's cap_group object
    cap 5 -> PMO object

Then A can call:

    sys_map_pmo(4, 5, addr, perm, len)
```

### 11.1 root 进程创建第一个 `cap_group`

[`kernel/object/cap_group.c`](../kernel/object/cap_group.c) 中的 `create_root_cap_group()` 用于创建第一个用户进程的 `cap_group`。

它需要完成的核心事情是：

```text
1. obj_alloc(TYPE_CAP_GROUP, sizeof(struct cap_group))
2. cap_group_init(root_cap_group, ...)
3. 在 slot 0 安装 root_cap_group 自己
4. obj_alloc(TYPE_VMSPACE, sizeof(struct vmspace))
5. vmspace_init(vmspace)
6. 在 slot 1 安装 vmspace
7. 设置 root_cap_group = cap_group
```

最终布局：

```text
root_cap_group

    slot 0 -> root_cap_group object
    slot 1 -> root vmspace object
```

### 11.2 普通创建：`sys_create_cap_group()`

`sys_create_cap_group()` 是用户态创建新 `cap_group` 的系统调用。

当前代码里它首先检查：

```text
current_cap_group->pid == ROOT_PID
```

也就是说，当前实验版本中只有 root 进程可以创建新的 `cap_group`。这符合微内核中常见的“进程管理器负责创建进程”的思路。

创建新 `cap_group` 后，内核会把新 `cap_group` 的 capability 返回给调用者：

```text
current/root cap_group

    cap N -> new cap_group object


new cap_group

    cap 0 -> new cap_group object
    cap 1 -> new vmspace object
```

图示：

```text
root process                              new process

slot N ---------------------------------> cap_group object <--- slot 0
                                            |
                                            v
                                      slot_table
                                      slot 0: self
                                      slot 1: vmspace
```

这体现了 capability 的一个核心特征：**进程管理器能否管理某个进程，取决于它是否持有该进程的 `cap_group` capability。**

---

## 12. 创建线程：线程也是对象

线程对象类型是 `TYPE_THREAD`。创建线程时，SJTU-chcore 会为线程分配一个 capability。

[`kernel/object/thread.c`](../kernel/object/thread.c) 中的 `thread_init()` 会从目标 `cap_group` 的固定 slot 中取出：

```text
slot 0 -> cap_group
slot 1 -> vmspace
```

然后把它们记录在线程结构中：

```text
thread->cap_group = target cap_group
thread->vmspace   = target vmspace
```

创建完成后，线程会被加入该 `cap_group` 的 `thread_list`，并通过 `cap_alloc()` 获得线程 capability。

```text
cap_group

    thread_list:
        thread A -> thread B -> ...

    slot_table:
        slot 0 -> cap_group
        slot 1 -> vmspace
        slot 2 -> thread A
        slot 3 -> thread B
```

在 `sys_create_thread()` 中，调用者传入目标 `cap_group` 的 capability。内核先通过：

```text
obj_get(current_cap_group, args.cap_group_cap, TYPE_CAP_GROUP)
```

确认调用者确实持有目标进程的 `cap_group` capability，然后才创建线程。

---

## 13. 内存对象 PMO：用 capability 映射内存

PMO 是 physical memory object，表示一段物理内存对象。它是 SJTU-chcore 中理解 capability 很好的例子。

### 13.1 创建 PMO

[`kernel/object/memory.c`](../kernel/object/memory.c) 中 `create_pmo()` 的核心流程是：

```text
1. pmo = obj_alloc(TYPE_PMO, sizeof(*pmo))
2. pmo_init(pmo, type, size, ...)
3. cap = cap_alloc(cap_group, pmo, 0)
4. return cap
```

用户态拿到的是 PMO 的 capability 编号：

```text
Process A

slot 5 -> PMO object
```

### 13.2 映射 PMO：`sys_map_pmo()`

`sys_map_pmo(target_cap_group_cap, pmo_cap, addr, perm, len)` 是 capability 设计的典型体现。

它要求调用者同时持有两个 capability：

```text
1. pmo_cap
   表示调用者有权使用这个 PMO

2. target_cap_group_cap
   表示调用者有权操作目标进程的地址空间
```

源码流程可以概括为：

```text
sys_map_pmo(target_cap_group_cap, pmo_cap, addr, perm, len)

    pmo = obj_get(current_cap_group, pmo_cap, TYPE_PMO)
        如果失败：-ECAPBILITY

    target_cap_group = obj_get(current_cap_group,
                               target_cap_group_cap,
                               TYPE_CAP_GROUP)
        如果失败：-ECAPBILITY

    vmspace = obj_get(target_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE)

    vmspace_map_range(vmspace, addr, len, perm, pmo)

    如果 target_cap_group != current_cap_group：
        cap_copy(current_cap_group, target_cap_group, pmo_cap)
```

图示：

```text
Process A wants to map PMO into Process B

Process A slot_table:

    cap 4 -> B's cap_group
    cap 5 -> PMO

System call:

    sys_map_pmo(4, 5, addr, perm, len)

Kernel:

    1. Check A has cap 5 and it is TYPE_PMO
    2. Check A has cap 4 and it is TYPE_CAP_GROUP
    3. Get B's vmspace from B.slot_table[1]
    4. Map PMO into B's vmspace
    5. Copy PMO cap into B's cap_group

After:

Process A                         Process B

cap 5 ----+                       cap k ----+
          |                                |
          +---------> PMO object <---------+
```

这就是 capability 支持微内核的关键动作之一：**一个服务可以把某个内存对象显式授权给另一个进程。**

---

## 14. 用户态 API 到内核 syscall 的路径

用户态 API 在 [`libchcore/src/capability/capability.c`](../libchcore/src/capability/capability.c)：

```text
chcore_cap_copy_to(dest_cap_group_cap, src_cap)
    -> __chcore_sys_cap_copy_to(dest_cap_group_cap, src_cap)
    -> SYS_cap_copy_to
    -> sys_cap_copy_to(dest_cap_group_cap, src_slot_id)
```

内核 syscall table 在 [`kernel/syscall/syscall.c`](../kernel/syscall/syscall.c) 中注册：

```text
SYS_cap_copy_to     -> sys_cap_copy_to
SYS_cap_copy_from   -> sys_cap_copy_from
SYS_transfer_caps   -> sys_transfer_caps
```

### 14.1 `sys_cap_copy_to()`

语义：把当前进程的某个 capability 复制到目标 `cap_group`。

```text
sys_cap_copy_to(dest_cap_group_cap, src_slot_id)

    dest_cap_group = obj_get(current_cap_group,
                             dest_cap_group_cap,
                             TYPE_CAP_GROUP)

    cap_copy(current_cap_group, dest_cap_group, src_slot_id)
```

调用者必须持有目标 `cap_group` 的 capability，否则无法向目标进程复制能力。

### 14.2 `sys_cap_copy_from()`

语义：从某个源 `cap_group` 中复制 capability 到当前进程。

```text
sys_cap_copy_from(src_cap_group_cap, src_slot_id)

    src_cap_group = obj_get(current_cap_group,
                            src_cap_group_cap,
                            TYPE_CAP_GROUP)

    cap_copy(src_cap_group, current_cap_group, src_slot_id)
```

调用者必须持有源 `cap_group` 的 capability。

### 14.3 `sys_transfer_caps()`

`sys_transfer_caps()` 批量复制 capability。它从用户缓冲区读入一组源 capability，将它们复制到目标 `cap_group`，再把目标中的新 capability 编号写回用户缓冲区。

```text
current process                       dest process

src_caps[] = [3, 5, 8]

sys_transfer_caps(dest_group_cap, src_caps, 3, dst_caps)

                                      dst_caps[] = [2, 4, 7]
```

这类接口常用于 IPC 或服务间传递一组资源句柄。

---

## 15. capability 与微内核交互的典型场景

下面用一个抽象场景串起来：用户进程请求文件系统服务读取数据到一段内存。

```text
Actors:

    App process
    FS server process
    Kernel

Objects:

    PMO buffer
    IPC endpoint / connection，当前 lab 中部分预留
```

一种 capability 风格的流程是：

```text
1. App 创建一个 PMO 作为缓冲区

    App slot 5 -> PMO buffer

2. App 通过 IPC 请求 FS server 读文件

    请求中附带：PMO buffer 的 capability

3. Kernel 把 PMO capability 复制到 FS server

    App slot 5 ----+
                  |
                  v
               PMO buffer
                  ^
                  |
    FS slot 9 -----+

4. FS server 持有 PMO capability，因此可以把数据写入该 PMO

5. App 读取自己的 buffer
```

这里没有全局“谁能访问谁的内存”的隐式规则。App 明确地把 buffer capability 交给 FS server，FS server 才能访问。

这就是 capability 对微内核的重要意义：

```text
服务拆到用户态之后，仍然可以通过 capability 安全地传递资源访问权。
```

---

## 16. 和普通指针、全局 ID、文件描述符的区别

### 16.1 capability vs 普通指针

```text
普通指针：

    用户态如果拿到地址，就可能直接读写。

Capability：

    用户态只拿到编号。
    编号必须经过内核的 cap_group.slot_table 解释。
```

在 SJTU-chcore 中，真实对象指针只在内核中存在。用户态 capability 编号不能直接解引用。

### 16.2 capability vs 全局对象 ID

```text
全局 ID：

    object_id = 10086
    所有进程看到同一个 ID，内核再判断谁能访问。

Capability：

    cap = 5 只在当前 cap_group 中有意义。
    另一个进程的 cap = 5 可以指向完全不同的对象。
```

capability 更强调“持有即授权”，而不是“知道全局名字后再查权限”。

### 16.3 capability vs Linux 文件描述符

二者非常相似，但 capability 更一般化：

```text
Linux fd：

    主要用于 file/socket/pipe/eventfd 等文件抽象

ChCore capability：

    用于 PMO、thread、vmspace、cap_group、IPC connection 等内核对象
```

可以说：**文件描述符是 capability 思想在文件对象上的一种体现；微内核 capability 则把这种思想推广到几乎所有内核资源。**

---

## 17. 阅读源码时的几个易混点

### 17.1 `cap_group` 既是“进程”，也是“对象”

`cap_group` 表示一个进程的 capability 空间。但它自己也通过 `obj_alloc(TYPE_CAP_GROUP, ...)` 创建，因此也能被别的 capability 指向。

这会形成一种看似递归的结构：

```text
cap_group object
    contains slot_table
        slot 0 -> itself
```

这不是 bug，而是设计：每个进程都持有“指向自己”的 capability。

### 17.2 slot 0 和 slot 1 是约定，不是随机分配

`CAP_GROUP_OBJ_ID = 0`，`VMSPACE_OBJ_ID = 1`。创建 `cap_group` 时必须保证：

```text
slot 0 -> cap_group
slot 1 -> vmspace
```

否则线程初始化、地址空间切换、内存映射都会出问题。

### 17.3 `obj_get()` 返回的是 `opaque`，不是 `struct object *`

`obj_get()` 返回真实对象指针，例如：

```text
TYPE_PMO       -> struct pmobject *
TYPE_THREAD    -> struct thread *
TYPE_CAP_GROUP -> struct cap_group *
TYPE_VMSPACE   -> struct vmspace *
```

如果需要从真实对象指针回到对象头，源码使用 `container_of()`。

### 17.4 `refcount` 同时受 capability 和临时引用影响

对象引用计数不只表示“有多少 capability 指向它”。`obj_get()` 也会临时增加引用计数，`obj_put()` 再减少。

因此可以粗略理解为：

```text
refcount = capability slots 数量 + 当前内核临时引用数量
```

### 17.5 当前 lab 的 capability 还不是完整工业级实现

SJTU-chcore lab 有不少教学留空和简化，例如：

1. `rights` 字段没有被充分使用。
2. 部分 IPC、连接对象、权限传播策略在当前实验中尚未完整展开。
3. `cap_group.c` 中存在 LAB TODO，需要学生补齐初始化和创建逻辑。

因此阅读时应区分：

```text
capability 的完整设计理念
        !=
当前实验代码已经实现的全部功能
```

---

## 18. 总结

可以用下面几句话概括 SJTU-chcore 的 capability 机制：

```text
1. 所有重要内核资源都被包装成 object。

2. 用户进程不能直接访问 object，只能持有 capability 编号。

3. capability 编号是 cap_group.slot_table 的下标。

4. cap_group 是进程的资源表，也是一个可被 capability 指向的 object。

5. 系统调用通过 obj_get() 把 capability 编号翻译成内核对象指针。

6. cap_copy() / cap_move() / cap_free() 实现授权传递和撤销。

7. 这种“对象 + capability + 显式传递”的模型，让微内核可以安全地把服务拆到用户态。
```

如果只记一张图，建议记住这张：

```text
User process

    cap number
        |
        v
Kernel

    current_cap_group
        |
        v
    slot_table[cap]
        |
        v
    object_slot
        |
        v
    object metadata
        |
        v
    real kernel object
```

如果只记一句话：

> Capability 是内核维护的、可传递的对象访问权；在 SJTU-chcore 中，它表现为每个进程 `cap_group` 里的 slot 编号。