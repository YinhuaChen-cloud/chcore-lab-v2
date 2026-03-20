# ChCore 在 VS Code 中使用 GDB + QEMU 调试

本文档说明工作区中新增的两个配置文件：

- [.vscode/tasks.json](.vscode/tasks.json)：定义“构建 / 启动 QEMU / 启动命令行 GDB”等任务。
- [.vscode/launch.json](.vscode/launch.json)：定义 VS Code 图形调试器如何连接到 QEMU 提供的 GDB 远程端口。

## 一、整体原理

这套调试链路的核心是 **QEMU 的 GDB remote stub**。

当你执行 `make qemu-gdb` 时，仓库里的 Makefile 会让 QEMU 以类似下面的方式启动：

- `-S`：CPU 启动后立刻暂停，不继续执行。
- `-gdb tcp::1234`：QEMU 在本地 `1234` 端口提供一个 GDB 调试服务。

此时 QEMU 相当于“被调试目标”，而 `gdb-multiarch` 或 VS Code 的 C/C++ 调试插件相当于“调试器前端”。前端连上 `localhost:1234` 后，就可以：

- 下断点
- 单步执行
- 查看寄存器
- 查看内存
- 查看调用栈

对于内核调试，**符号文件**尤其关键。GDB 必须读取带符号的内核镜像，才能把地址翻译成函数名、源码行号和变量信息。本仓库中使用的是：

- `build/kernel.img`

因此，每次改完代码后，应该先重新构建，再开始调试。

## 二、各文件的作用

### 1. [.vscode/tasks.json](.vscode/tasks.json)

这个文件定义了几个常用任务：

#### `ChCore: build`

作用：编译内核。

原理：调试器读取最新构建产物中的符号表；如果源码已经改了，但镜像没重新编译，断点位置、函数地址和源码行号可能都不准确。

#### `ChCore: qemu-gdb`

作用：启动支持 GDB 调试的 QEMU。

原理：这个任务调用 `make qemu-gdb`，而仓库 Makefile 已经封装好了 QEMU 的调试参数，所以不需要你手工写长命令。

#### `ChCore: qemu`

作用：普通启动，不启用 GDB。

原理：有时你需要先确认系统能否正常运行，再进入调试模式；或者排查“只在调试暂停时出现”的时序问题。

#### `ChCore: gdb-cli`

作用：在终端里直接打开 `gdb-multiarch`。

原理：它会复用仓库已有的 `.gdbinit`，适合喜欢命令行调试的场景，也方便和 VS Code 图形调试相互验证。

### 2. [.vscode/launch.json](.vscode/launch.json)

这个文件定义 VS Code 如何连接到 QEMU。

其中最关键的几个字段是：

#### `program`

指定带符号的内核镜像路径。GDB 不仅要“连上目标”，还要知道“这个目标的符号来自哪个文件”。

#### `miDebuggerPath`

指定使用哪个 GDB。这里是：

- `gdb-multiarch`

原因是当前实验环境目标架构为 AArch64，而宿主机通常不是同一架构，`gdb-multiarch` 更稳妥。

#### `miDebuggerServerAddress`

指定远程调试地址：

- `localhost:1234`

它对应的就是 `make qemu-gdb` 启动出来的 QEMU 调试端口。

#### `setupCommands`

连接后自动执行初始化命令。

例如：

- `set architecture aarch64`

这能明确告诉 GDB 当前目标是 AArch64，避免某些情况下架构识别不准确。

## 三、推荐使用流程

### 方案 A：VS Code 图形界面调试

1. 先运行任务 `ChCore: qemu-gdb`
2. 打开“运行和调试”视图
3. 选择 `ChCore: Attach gdb-multiarch to QEMU(:1234)`
4. 按 `F5`
5. 在源码中打断点，开始单步调试

这个流程里：

- 第一步负责“启动被调试目标”
- 第二步到第五步负责“让调试器附加进去”

### 方案 B：命令行 GDB 调试

1. 运行任务 `ChCore: qemu-gdb`
2. 再运行任务 `ChCore: gdb-cli`

这样会直接进入仓库已有的 `.gdbinit` 调试流程。

## 四、建议在哪些位置下断点

如果你在做启动阶段实验，建议优先在下面这些位置下断点：

- [kernel/arch/aarch64/boot/raspi3/init/start.S](kernel/arch/aarch64/boot/raspi3/init/start.S)
- [kernel/arch/aarch64/main.c](kernel/arch/aarch64/main.c)

典型观察点包括：

- 是否成功从高异常级切到 EL1
- 栈指针是否初始化正确
- MMU / 页表初始化前后的寄存器状态
- 是否正确跳转到 `main`

## 五、常见问题

### 1. VS Code 无法启动调试

先检查是否安装了 C/C++ 调试扩展：

- `ms-vscode.cpptools`

如果没有这个扩展，`cppdbg` 类型的调试配置不会工作。

### 2. 提示找不到 `gdb-multiarch`

说明宿主机未安装该工具。需要先在系统里安装 `gdb-multiarch`。

### 3. 断点是灰色的，或断不住

通常有几个原因：

- 没有重新编译
- `program` 指向的不是当前最新镜像
- 当前代码还没执行到该位置
- 早期汇编阶段的断点需要在 QEMU 暂停后尽早附加

### 4. 1234 端口连接失败

说明 `make qemu-gdb` 还没启动成功，或者已经退出。先看任务终端输出是否正常。

## 六、进一步优化建议

如果你后续希望做到“一键 F5 自动拉起 QEMU 再自动附加 GDB”，也可以继续扩展当前配置，例如：

- 增加专门的后台任务启动 QEMU
- 配置更严格的后台任务就绪匹配
- 增加 `postDebugTask` 在结束调试后自动清理 QEMU

不过对实验环境来说，当前这套“两步法”更稳定，也更容易理解问题出在哪一层。