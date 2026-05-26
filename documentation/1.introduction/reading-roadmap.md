@page page_reading_roadmap RT-Thread Source Reading Roadmap

# RT-Thread 源码阅读路线

这份文档面向想系统学习 RT-Thread 的读者，目标不是先学会“怎么调用 API”，而是先建立对内核、配置系统、构建系统、BSP 和组件层的整体理解。

如果你的目标是学习 RT-Thread 这个操作系统本身，建议优先阅读内核和启动主线，不要一开始就陷入具体 BSP 外设驱动或组件示例。

# 阅读目标

读完这条路线后，应该能够回答下面几个问题：

- RT-Thread 的代码为什么分成 src、include、components、libcpu、bsp 这些目录。
- 系统启动时，代码是怎样从启动文件走到内核初始化，再进入用户 main 的。
- 线程、调度器、定时器、IPC、内存管理分别落在哪些源码文件里。
- BSP 的 rtconfig.py、rtconfig.h、SConstruct、SConscript 分别控制什么。
- 什么时候该看内核，什么时候该看 libcpu，什么时候该看 BSP 或 components。

# 阅读原则

1. 先看结构，再看实现。
2. 先看头文件暴露的接口，再看源文件里的实现细节。
3. 先抓主链路：启动、线程、调度、IPC、定时器。
4. 先读单核路径，再考虑 SMP、多架构移植和复杂组件。
5. 每看完一个模块，都回到实际代码验证，不要只停留在文档层面。

# 总体顺序

推荐按下面的顺序阅读：

1. 仓库整体结构和配置入口
2. 内核公共头文件和核心数据结构
3. 内核对象系统
4. 线程与调度
5. 时钟、定时器、IPC
6. 内存管理与中断管理
7. 系统启动流程
8. 架构移植层 libcpu
9. BSP 目录和板级初始化
10. 组件层和示例

# 第一阶段：建立全局地图

先阅读下面这些文件，只需要弄清楚每个目录的职责，不需要立刻深挖实现：

- [README.md](../../README.md)
- [Kconfig](../../Kconfig)
- [src/Kconfig](../../src/Kconfig)
- [components/Kconfig](../../components/Kconfig)
- [documentation/INDEX.md](../INDEX.md)
- [documentation/3.kernel/basic/basic.md](../3.kernel/basic/basic.md)

这一阶段重点回答：

- 哪些目录属于内核本体。
- 哪些目录属于硬件适配。
- 哪些目录属于可选组件。
- Kconfig 和 SCons 在整个系统里分别扮演什么角色。

# 第二阶段：先读头文件，建立接口认知

在进入具体实现之前，先看内核公共头文件：

- [include/rtthread.h](../../include/rtthread.h)
- [include/rtdef.h](../../include/rtdef.h)
- [include/rthw.h](../../include/rthw.h)
- [include/rtservice.h](../../include/rtservice.h)
- [include/rtatomic.h](../../include/rtatomic.h)

这一阶段重点不是逐行背 API，而是先弄清楚：

- 内核对象有哪些类型。
- 线程、定时器、IPC 的外部接口长什么样。
- 哪些接口属于通用内核，哪些接口明显依赖硬件层。

建议把下面几类接口单独记出来：

- 对象管理接口
- 线程管理接口
- 定时器接口
- IPC 接口
- 临界区与中断接口

# 第三阶段：对象系统是入口

建议先从对象系统开始，因为 RT-Thread 很多内核资源都统一挂在对象体系上。

优先阅读：

- [src/object.c](../../src/object.c)
- [documentation/3.kernel/object/object.md](../3.kernel/object/object.md)

这一阶段重点理解：

- 什么是 rt_object。
- 线程、信号量、定时器为什么都能被统一管理。
- 对象初始化、查找、删除、遍历的统一模式是什么。

如果对象系统没看懂，后面再看线程和 IPC 会比较碎。

# 第四阶段：线程与调度是核心主线

这是最重要的一段，建议花最多时间。

优先阅读：

- [src/thread.c](../../src/thread.c)
- [src/scheduler_up.c](../../src/scheduler_up.c)
- [src/scheduler_comm.c](../../src/scheduler_comm.c)
- [src/cpu_up.c](../../src/cpu_up.c)
- [documentation/3.kernel/thread/thread.md](../3.kernel/thread/thread.md)

如果后续再看多核，可补充：

- [src/scheduler_mp.c](../../src/scheduler_mp.c)
- [src/cpu_mp.c](../../src/cpu_mp.c)
- [documentation/3.kernel/smp-startup/smp-startup.md](../3.kernel/smp-startup/smp-startup.md)

这一阶段重点理解：

- 线程对象是怎么初始化的。
- 栈是怎么准备的。
- 线程为什么能被挂起、恢复、切换。
- 优先级调度和时间片轮转在代码里怎么体现。
- 线程退出后为什么还需要 defunct 或清理流程。

建议你画一条最小调用链：

rt_thread_create -> rt_thread_startup -> rt_schedule

把这条链看懂，内核主干就算入门了。

# 第五阶段：时钟、定时器、IPC

线程之后，接着看内核里最常用的服务模块。

优先阅读：

- [src/clock.c](../../src/clock.c)
- [src/timer.c](../../src/timer.c)
- [src/ipc.c](../../src/ipc.c)
- [documentation/3.kernel/timer/timer.md](../3.kernel/timer/timer.md)
- [documentation/3.kernel/thread-sync/thread-sync.md](../3.kernel/thread-sync/thread-sync.md)
- [documentation/3.kernel/thread-comm/thread-comm.md](../3.kernel/thread-comm/thread-comm.md)

这一阶段重点理解：

- tick 在系统里是什么。
- 线程阻塞超时为什么依赖定时器。
- 信号量、互斥锁、事件、邮箱、消息队列在内核中的共同点和差异。
- 为什么 mutex 需要优先级继承，而 semaphore 不强调这一点。

这部分建议边看源码边对照文档，不然会比较容易陷入函数细节。

# 第六阶段：内存管理与中断管理

这部分决定你对 RTOS 底层质量的理解深度。

优先阅读：

- [src/mem.c](../../src/mem.c)
- [src/memheap.c](../../src/memheap.c)
- [src/mempool.c](../../src/mempool.c)
- [src/slab.c](../../src/slab.c)
- [src/irq.c](../../src/irq.c)
- [documentation/3.kernel/memory/memory.md](../3.kernel/memory/memory.md)
- [documentation/3.kernel/interrupt/interrupt.md](../3.kernel/interrupt/interrupt.md)

这一阶段重点理解：

- RT-Thread 为什么提供多种内存管理方案。
- mem、memheap、mempool、slab 分别适合什么场景。
- 中断上下文和线程上下文在代码中如何区分。
- 哪些 API 可以在中断里调用，哪些不应该在中断里调用。

# 第七阶段：启动流程

当你已经理解线程和调度，再回头看启动流程，会更容易连起来。

优先阅读：

- [src/components.c](../../src/components.c)
- [src/idle.c](../../src/idle.c)
- [documentation/3.kernel/basic/basic.md](../3.kernel/basic/basic.md)

然后选择一个具体 BSP 来串启动路径，推荐从结构较清晰、资料较多的 BSP 开始，例如：

- [bsp/stm32/stm32f103-blue-pill](../../bsp/stm32/stm32f103-blue-pill)
- [bsp/qemu-vexpress-a9](../../bsp/qemu-vexpress-a9)

在 BSP 里重点看：

- README
- startup 汇编文件
- board 目录
- applications 目录
- rtconfig.py
- rtconfig.h
- SConstruct
- SConscript

这一阶段重点理解：

- 系统从复位后第一个 C 函数开始做了什么。
- rt_hw_board_init 负责什么。
- rtthread_startup 做了哪些固定步骤。
- 用户 main 是作为普通函数执行，还是作为线程执行。

# 第八阶段：libcpu 和架构移植

如果你已经理解了线程切换、调度和启动，再进入 libcpu 才更有效。

建议按你最关心的架构选一个目录深入，例如：

- [libcpu/arm](../../libcpu/arm)
- [libcpu/risc-v](../../libcpu/risc-v)

这一阶段重点理解：

- 上下文切换真正依赖哪些寄存器保存和恢复逻辑。
- 异常入口、中断入口和 PendSV 之类的机制如何与内核调度对接。
- 为什么同样的内核 API，最终能跑在不同 CPU 上。

# 第九阶段：BSP 和组件层

到这一步再看 BSP、驱动框架和组件，效率会高很多。

建议顺序：

1. 一个具体 BSP 的 board 和 drivers
2. [components/drivers](../../components/drivers)
3. [components/finsh](../../components/finsh)
4. [components/dfs](../../components/dfs)
5. [components/net](../../components/net)

这一阶段要区分清楚两件事：

- 哪些代码在提供“操作系统能力”。
- 哪些代码只是在“使用操作系统能力”。

例如你当前打开的 [examples/rt-link/rtlink_dev_example.c](../../examples/rt-link/rtlink_dev_example.c)，更适合在了解组件框架之后再看，不适合作为理解 RT-Thread 内核的起点。

# 一份适合初学者的 7 天阅读安排

如果你希望按天推进，可以直接按下面执行。

## Day 1

- [README.md](../../README.md)
- [Kconfig](../../Kconfig)
- [src/Kconfig](../../src/Kconfig)
- [components/Kconfig](../../components/Kconfig)

目标：搞清楚仓库分层和配置入口。

## Day 2

- [include/rtthread.h](../../include/rtthread.h)
- [include/rtdef.h](../../include/rtdef.h)
- [src/object.c](../../src/object.c)

目标：理解对象系统和核心数据结构。

## Day 3

- [src/thread.c](../../src/thread.c)
- [documentation/3.kernel/thread/thread.md](../3.kernel/thread/thread.md)

目标：理解线程的创建、启动、挂起、退出。

## Day 4

- [src/scheduler_up.c](../../src/scheduler_up.c)
- [src/scheduler_comm.c](../../src/scheduler_comm.c)
- [src/cpu_up.c](../../src/cpu_up.c)

目标：理解单核调度主线。

## Day 5

- [src/timer.c](../../src/timer.c)
- [src/clock.c](../../src/clock.c)
- [src/ipc.c](../../src/ipc.c)

目标：理解超时机制、定时器与 IPC 的关系。

## Day 6

- [src/mem.c](../../src/mem.c)
- [src/memheap.c](../../src/memheap.c)
- [src/mempool.c](../../src/mempool.c)
- [src/slab.c](../../src/slab.c)
- [src/irq.c](../../src/irq.c)

目标：理解内存与中断上下文约束。

## Day 7

- [src/components.c](../../src/components.c)
- [bsp/qemu-vexpress-a9](../../bsp/qemu-vexpress-a9)
  或 [bsp/stm32/stm32f103-blue-pill](../../bsp/stm32/stm32f103-blue-pill)

目标：把启动流程和具体 BSP 串起来。

# 每个阶段都建议做的事

每看完一个阶段，建议至少做 3 件事：

1. 写出这个模块的核心数据结构。
2. 画出 1 条关键调用链。
3. 记下 2 到 3 个“这个模块为什么要这样设计”的问题。

比如在看线程模块时，至少要自己回答：

- 为什么线程退出后不是立刻直接释放所有资源。
- 为什么阻塞等待通常会和线程定时器绑定。
- 为什么调度器要和 CPU/架构层分开。

# 不建议的阅读方式

下面这些路径对初学者通常效率较低：

- 一开始就看具体外设驱动。
- 一开始就看 examples 目录，希望通过示例反推内核。
- 一开始就看某个厂商 BSP 的全部代码。
- 直接从汇编启动文件开始一路硬啃到底。

这些内容不是不重要，而是它们依赖你先对内核主干有最基本的理解。

# 下一步建议

如果你下次继续学习，建议直接从下面这组文件重新开始：

1. [include/rtthread.h](../../include/rtthread.h)
2. [src/object.c](../../src/object.c)
3. [src/thread.c](../../src/thread.c)
4. [src/scheduler_up.c](../../src/scheduler_up.c)

这四个位置是重新进入状态最快的入口。