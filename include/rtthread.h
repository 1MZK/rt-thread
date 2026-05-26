/*
 * Copyright (c) 2006-2024 RT-Thread Development Team
 * 
 * 版权声明：归 RT-Thread 开发团队所有，时间跨度从 2006 年至 2024 年。
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * 许可证标识：遵循 Apache 2.0 开源许可证。
 *
 * Change Logs:
 * 变更日志：记录了文件历次修改的时间、作者及修改内容摘要。
 * Date           Author       Notes
 * 2006-03-18     Bernard      the first version (首次版本)
 * 2006-04-26     Bernard      add semaphore APIs (增加信号量API)
 * 2006-08-10     Bernard      add version information (增加版本信息)
 * 2007-01-28     Bernard      rename RT_OBJECT_Class_Static to RT_Object_Class_Static (重命名静态对象宏)
 * 2007-03-03     Bernard      clean up the definitions to rtdef.h (清理定义，移至rtdef.h)
 * 2010-04-11     yi.qiu       add module feature (增加模块特性)
 * 2013-06-24     Bernard      add rt_kprintf re-define when not use RT_USING_CONSOLE. (无控制台时重定义打印函数)
 * 2016-08-09     ArdaFu       add new thread and interrupt hook. (增加线程和中断钩子)
 * 2018-11-22     Jesven       add all cpu's lock and ipi handler (增加多CPU锁和核间中断处理)
 * 2021-02-28     Meco Man     add RT_KSERVICE_USING_STDLIB (增加使用标准C库的宏)
 * 2021-11-14     Meco Man     add rtlegacy.h for compatibility (增加向后兼容头文件)
 * 2022-06-04     Meco Man     remove strnlen (移除strnlen函数)
 * 2023-05-20     Bernard      add rtatomic.h header file to included files. (增加原子操作头文件)
 * 2023-06-30     ChuShicheng  move debug check from the rtdebug.h (从rtdebug.h移入调试检查)
 * 2023-10-16     Shell        Support a new backtrace framework (支持新的回溯框架)
 * 2023-12-10     xqyjlj       fix spinlock in up (修复单核下的自旋锁问题)
 * 2024-01-25     Shell        Add rt_susp_list for IPC primitives (为IPC原语增加挂起列表)
 * 2024-03-10     Meco Man     move std libc related functions to rtklibc (将标准C库相关函数移至rtklibc)
 */

/* 头文件保护宏，防止头文件被重复包含 */
#ifndef __RT_THREAD_H__
#define __RT_THREAD_H__

/* 包含 RT-Thread 的配置头文件，用于裁剪系统功能 */
#include <rtconfig.h>
/* 包含内核基本数据类型和宏的定义 */
#include <rtdef.h>
/* 包含内核服务函数的声明（如链表操作等） */
#include <rtservice.h>
/* 包含模块相关的声明 */
#include <rtm.h>
/* 包含原子操作相关的声明 */
#include <rtatomic.h>
/* 包含内核C库函数（如字符串/内存操作）的声明 */
#include <rtklibc.h>
/* 如果开启了兼容旧版本API的宏，则包含兼容性头文件 */
#ifdef RT_USING_LEGACY
#include <rtlegacy.h>
#endif
/* 如果开启了 FinSH 控制台组件，则包含 FinSH 头文件 */
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif /* RT_USING_FINSH */

/* 如果是 C++ 编译器，则使用 extern "C" 确保 C 编译器链接规范 */
#ifdef __cplusplus
extern "C" {
#endif

/* 如果使用 GNU C 编译器，声明一个默认的入口函数 */
#ifdef __GNUC__
int entry(void);
#endif

/*
 * kernel object interface
 * 内核对象接口：RT-Thread的线程、信号量等均继承自内核对象
 */
/* 获取指定类型内核对象的信息结构体指针 */
struct rt_object_information *
rt_object_get_information(enum rt_object_class_type type);
/* 获取指定类型内核对象的数量 */
int rt_object_get_length(enum rt_object_class_type type);
/* 获取指定类型内核对象的指针列表 */
int rt_object_get_pointers(enum rt_object_class_type type, rt_object_t *pointers, int maxlen);

/* 静态初始化内核对象 */
void rt_object_init(struct rt_object         *object,
                    enum rt_object_class_type type,
                    const char               *name);
/* 脱离静态内核对象（从对象容器中移除） */
void rt_object_detach(rt_object_t object);

/* 如果开启了堆内存管理（动态内存分配） */
#ifdef RT_USING_HEAP
/* 动态分配一个内核对象 */
rt_object_t rt_object_allocate(enum rt_object_class_type type, const char *name);
/* 删除动态分配的内核对象（释放内存） */
void rt_object_delete(rt_object_t object);
/* custom object 自定义对象 */
/* 创建自定义对象，允许绑定自定义数据和销毁函数 */
rt_object_t rt_custom_object_create(const char *name, void *data, rt_err_t (*data_destroy)(void *));
/* 销毁自定义对象 */
rt_err_t rt_custom_object_destroy(rt_object_t obj);
#endif /* RT_USING_HEAP */

/* 判断一个对象是否是系统静态对象 */
rt_bool_t rt_object_is_systemobject(rt_object_t object);
/* 获取内核对象的类型 */
rt_uint8_t rt_object_get_type(rt_object_t object);
/* 遍历指定类型的内核对象 */
rt_err_t rt_object_for_each(rt_uint8_t type, rt_object_iter_t iter, void *data);
/* 根据名称查找内核对象 */
rt_object_t rt_object_find(const char *name, rt_uint8_t type);
/* 获取内核对象的名称 */
rt_err_t rt_object_get_name(rt_object_t object, char *name, rt_uint8_t name_size);

/* 如果开启了钩子函数（Hook）机制 */
#ifdef RT_USING_HOOK
/* 设置对象附加(初始化/分配)时的钩子函数 */
void rt_object_attach_sethook(void (*hook)(struct rt_object *object));
/* 设置对象脱离时的钩子函数 */
void rt_object_detach_sethook(void (*hook)(struct rt_object *object));
/* 设置对象尝试获取(占用)时的钩子函数 */
void rt_object_trytake_sethook(void (*hook)(struct rt_object *object));
/* 设置对象成功获取(占用)时的钩子函数 */
void rt_object_take_sethook(void (*hook)(struct rt_object *object));
/* 设置对象释放时的钩子函数 */
void rt_object_put_sethook(void (*hook)(struct rt_object *object));
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_clock-management
 * 时钟与定时器管理分组
 * @{
 */

/*
 * clock & timer interface
 * 时钟与定时器接口
 */
/* 获取当前系统的 tick 数 */
rt_tick_t rt_tick_get(void);
/* 获取相对于基准 tick 的增量 */
rt_tick_t rt_tick_get_delta(rt_tick_t base);
/* 设置当前系统的 tick 数 */
void rt_tick_set(rt_tick_t tick);
/* 系统 tick 递增 1（通常在时钟中断中调用） */
void rt_tick_increase(void);
/* 系统 tick 递增指定数值 */
void rt_tick_increase_tick(rt_tick_t tick);
/* 将毫秒数转换为 tick 数 */
rt_tick_t  rt_tick_from_millisecond(rt_int32_t ms);
/* 获取当前系统 tick 对应的毫秒数 */
rt_tick_t rt_tick_get_millisecond(void);

/* 设置 tick 递增时的钩子函数 */
#ifdef RT_USING_HOOK
void rt_tick_sethook(void (*hook)(void));
#endif /* RT_USING_HOOK */

/* 系统定时器初始化 */
void rt_system_timer_init(void);
/* 系统定时器线程初始化（用于软定时器） */
void rt_system_timer_thread_init(void);

/* 静态初始化一个定时器 */
void rt_timer_init(rt_timer_t  timer,
                   const char *name,
                   void (*timeout)(void *parameter),
                   void       *parameter,
                   rt_tick_t   time,
                   rt_uint8_t  flag);
/* 脱离一个静态定时器 */
rt_err_t rt_timer_detach(rt_timer_t timer);

/* 动态创建一个定时器 */
#ifdef RT_USING_HEAP
rt_timer_t rt_timer_create(const char *name,
                           void (*timeout)(void *parameter),
                           void       *parameter,
                           rt_tick_t   time,
                           rt_uint8_t  flag);
/* 删除一个动态定时器 */
rt_err_t rt_timer_delete(rt_timer_t timer);
#endif /* RT_USING_HEAP */

/* 启动定时器 */
rt_err_t rt_timer_start(rt_timer_t timer);
/* 停止定时器 */
rt_err_t rt_timer_stop(rt_timer_t timer);
/* 控制定时器（如设置周期、获取状态等） */
rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg);
/* 获取下一个即将超时的定时器的超时 tick */
rt_tick_t rt_timer_next_timeout_tick(void);
/* 检查并执行软件定时器超时（通常在 tick 中断调用） */
void rt_timer_check(void);

/* 设置定时器进入超时处理函数前的钩子 */
#ifdef RT_USING_HOOK
void rt_timer_enter_sethook(void (*hook)(struct rt_timer *timer));
/* 设置定时器退出超时处理函数后的钩子 */
void rt_timer_exit_sethook(void (*hook)(struct rt_timer *timer));
#endif /* RT_USING_HOOK */

/**@}*/ /* 时钟与定时器管理分组结束 */

/*
 * thread interface
 * 线程接口
 */
/* 静态初始化一个线程 */
rt_err_t rt_thread_init(struct rt_thread *thread,
                        const char       *name,
                        void (*entry)(void *parameter),
                        void             *parameter,
                        void             *stack_start,
                        rt_uint32_t       stack_size,
                        rt_uint8_t        priority,
                        rt_uint32_t       tick);
/* 脱离静态线程 */
rt_err_t rt_thread_detach(rt_thread_t thread);

/* 动态创建一个线程 */
#ifdef RT_USING_HEAP
rt_thread_t rt_thread_create(const char *name,
                             void (*entry)(void *parameter),
                             void       *parameter,
                             rt_uint32_t stack_size,
                             rt_uint8_t  priority,
                             rt_uint32_t tick);
/* 删除动态线程 */
rt_err_t rt_thread_delete(rt_thread_t thread);
#endif /* RT_USING_HEAP */

/* 关闭线程（不再调度该线程） */
rt_err_t rt_thread_close(rt_thread_t thread);
/* 获取当前正在运行的线程 */
rt_thread_t rt_thread_self(void);
/* 根据名称查找线程 */
rt_thread_t rt_thread_find(char *name);
/* 启动一个线程，将其加入就绪队列 */
rt_err_t rt_thread_startup(rt_thread_t thread);
/* 让出 CPU 给其他同优先级线程 */
rt_err_t rt_thread_yield(void);
/* 线程延时指定 tick 数（进入挂起状态） */
rt_err_t rt_thread_delay(rt_tick_t tick);
/* 线程绝对延时（延时到绝对 tick 值） */
rt_err_t rt_thread_delay_until(rt_tick_t *tick, rt_tick_t inc_tick);
/* 线程毫秒级延时 */
rt_err_t rt_thread_mdelay(rt_int32_t ms);
/* 控制线程（如修改优先级、获取状态等） */
rt_err_t rt_thread_control(rt_thread_t thread, int cmd, void *arg);
/* 挂起线程 */
rt_err_t rt_thread_suspend(rt_thread_t thread);
/* 带有特定挂起标志地挂起线程（用于IPC） */
rt_err_t rt_thread_suspend_with_flag(rt_thread_t thread, int suspend_flag);
/* 恢复线程 */
rt_err_t rt_thread_resume(rt_thread_t thread);

/* 如果开启了智能操作系统（如进程/内存隔离等特性） */
#ifdef RT_USING_SMART
/* 唤醒线程 */
rt_err_t rt_thread_wakeup(rt_thread_t thread);
/* 设置线程的唤醒回调函数及用户数据 */
void rt_thread_wakeup_set(struct rt_thread *thread, rt_wakeup_func_t func, void* user_data);
#endif /* RT_USING_SMART */

/* 获取线程名称 */
rt_err_t rt_thread_get_name(rt_thread_t thread, char *name, rt_uint8_t name_size);

/* 如果开启了 CPU 使用率追踪 */
#ifdef RT_USING_CPU_USAGE_TRACER
/* 获取指定线程的 CPU 使用率 */
rt_uint8_t rt_thread_get_usage(rt_thread_t thread);
#endif /* RT_USING_CPU_USAGE_TRACER */

/* 如果开启了信号功能 */
#ifdef RT_USING_SIGNALS
/* 为线程分配信号相关的资源 */
void rt_thread_alloc_sig(rt_thread_t tid);
/* 释放线程的信号资源 */
void rt_thread_free_sig(rt_thread_t tid);
/* 向指定线程发送信号 */
int  rt_thread_kill(rt_thread_t tid, int sig);
#endif /* RT_USING_SIGNALS */

/* 线程相关的钩子函数声明 */
#ifdef RT_USING_HOOK
/* 设置线程挂起时的钩子 */
void rt_thread_suspend_sethook(void (*hook)(rt_thread_t thread));
/* 设置线程恢复时的钩子 */
void rt_thread_resume_sethook (void (*hook)(rt_thread_t thread));

/**
 * @ingroup group_thread_management
 * 线程管理分组
 *
 * @brief Sets a hook function when a thread is initialized.
 * 设置线程初始化时的钩子函数。
 *
 * @param thread is the target thread that initializing
 * 参数 thread 是正在初始化的目标线程
 */
/* 定义线程初始化完成时的钩子函数指针类型 */
typedef void (*rt_thread_inited_hookproto_t)(rt_thread_t thread);
/* 声明线程初始化钩子列表 */
RT_OBJECT_HOOKLIST_DECLARE(rt_thread_inited_hookproto_t, rt_thread_inited);

#endif /* RT_USING_HOOK */

/*
 * idle thread interface
 * 空闲线程接口
 */
/* 初始化空闲线程 */
void rt_thread_idle_init(void);

/* 如果开启了钩子机制或空闲钩子 */
#if defined(RT_USING_HOOK) || defined(RT_USING_IDLE_HOOK)
// FIXME: 必须在这里为 rt_thread_idle_sethook 写 doxygen 注释，而不是在 src/idle.c 中。
//        因为 src/idle.c 中的 `rt_align(RT_ALIGN_SIZE)` 会导致 doxygen 构建失败。
/**
 * @ingroup group_thread_management
 * 线程管理分组
 *
 * @brief This function sets a hook function to idle thread loop. When the system performs
 *        idle loop, this hook function should be invoked.
 * 此函数用于设置空闲线程循环中的钩子函数。当系统执行空闲循环时，将调用此钩子函数。
 *
 * @param hook the specified hook function.
 * 参数 hook 为指定的钩子函数。
 *
 * @return `RT_EOK`: set OK. 设置成功。
 *         `-RT_EFULL`: hook list is full. 钩子列表已满。
 *
 * @note the hook function must be simple and never be blocked or suspend.
 * 注意：钩子函数必须简短，绝对不能被阻塞或挂起。
 */
/* 设置空闲线程钩子 */
rt_err_t rt_thread_idle_sethook(void (*hook)(void));
/* 删除空闲线程钩子 */
rt_err_t rt_thread_idle_delhook(void (*hook)(void));
#endif /* defined(RT_USING_HOOK) || defined(RT_USING_IDLE_HOOK) */

/* 获取空闲线程的句柄 */
rt_thread_t rt_thread_idle_gethandler(void);
/* 判断指定线程是否是空闲线程 */
rt_bool_t rt_thread_is_idle_thread(rt_thread_t thread);

/*
 * schedule service
 * 调度器服务接口
 */
/* 调度器初始化 */
void rt_system_scheduler_init(void);
/* 启动调度器 */
void rt_system_scheduler_start(void);

/* 执行线程调度（触发上下文切换） */
void rt_schedule(void);
/* 在中断上下文中请求调度切换 */
void rt_scheduler_do_irq_switch(void *context);

/* 如果开启了栈溢出检查 */
#ifdef RT_USING_OVERFLOW_CHECK
/* 检查线程栈是否溢出 */
void rt_scheduler_stack_check(struct rt_thread *thread);
/* 宏封装：检查栈溢出 */
#define RT_SCHEDULER_STACK_CHECK(thr) rt_scheduler_stack_check(thr)

#else /* !RT_USING_OVERFLOW_CHECK 未开启栈溢出检查 */
/* 宏定义为空操作 */
#define RT_SCHEDULER_STACK_CHECK(thr)

#endif /* RT_USING_OVERFLOW_CHECK */

/* 进入临界区（关闭调度/中断），返回之前的临界区状态 */
rt_base_t rt_enter_critical(void);
/* 退出临界区（恢复调度/中断） */
void rt_exit_critical(void);
/* 安全退出临界区（考虑中断嵌套等情况） */
void rt_exit_critical_safe(rt_base_t critical_level);
/* 获取当前临界区嵌套层数 */
rt_uint16_t rt_critical_level(void);

/* 调度器相关的钩子函数 */
#ifdef RT_USING_HOOK
/* 设置栈溢出时的钩子函数 */
void rt_scheduler_stack_overflow_sethook(rt_err_t (*hook)(struct rt_thread *thread));
/* 设置线程切换时的钩子函数（记录从哪个线程切换到哪个线程） */
void rt_scheduler_sethook(void (*hook)(rt_thread_t from, rt_thread_t to));
/* 设置调度器切换时的钩子（仅记录切入的线程） */
void rt_scheduler_switch_sethook(void (*hook)(struct rt_thread *tid));
#endif /* RT_USING_HOOK */

/* 如果开启了多核（SMP）支持 */
#ifdef RT_USING_SMP
/* 从核启动入口函数 */
void rt_secondary_cpu_entry(void);
/* 调度器核间中断（IPI）处理函数 */
void rt_scheduler_ipi_handler(int vector, void *param);
#endif /* RT_USING_SMP */

/**
 * @addtogroup group_signal
 * 信号分组
 * @{
 */
/* 信号相关接口 */
#ifdef RT_USING_SIGNALS
/* 屏蔽（阻塞）指定信号 */
void rt_signal_mask(int signo);
/* 解除屏蔽指定信号 */
void rt_signal_unmask(int signo);
/* 检查信号（通常在上下文恢复时调用） */
void *rt_signal_check(void* context);
/* 安装（注册）信号处理函数 */
rt_sighandler_t rt_signal_install(int signo, rt_sighandler_t handler);
/* 等待信号 */
int rt_signal_wait(const rt_sigset_t *set, rt_siginfo_t *si, rt_int32_t timeout);
/* 信号子系统初始化 */
int rt_system_signal_init(void);
#endif /* RT_USING_SIGNALS */
/**@}*/ /* 信号分组结束 */

/**
 * @addtogroup group_memory_management
 * 内存管理分组
 * @{
 */

/*
 * memory management interface
 * 内存管理接口
 */
/* 如果开启了内存池 */
#ifdef RT_USING_MEMPOOL
/*
 * memory pool interface
 * 内存池接口
 */
/* 静态初始化内存池 */
rt_err_t rt_mp_init(struct rt_mempool *mp,
                    const char        *name,
                    void              *start,
                    rt_size_t          size,
                    rt_size_t          block_size);
/* 脱离静态内存池 */
rt_err_t rt_mp_detach(struct rt_mempool *mp);

/* 动态创建内存池 */
#ifdef RT_USING_HEAP
rt_mp_t rt_mp_create(const char *name,
                     rt_size_t   block_count,
                     rt_size_t   block_size);
/* 删除动态内存池 */
rt_err_t rt_mp_delete(rt_mp_t mp);
#endif /* RT_USING_HEAP */

/* 从内存池分配一个内存块，time为等待时间 */
void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time);
/* 释放内存块回内存池 */
void rt_mp_free(void *block);

/* 内存池分配和释放的钩子 */
#ifdef RT_USING_HOOK
void rt_mp_alloc_sethook(void (*hook)(struct rt_mempool *mp, void *block));
void rt_mp_free_sethook(void (*hook)(struct rt_mempool *mp, void *block));
#endif /* RT_USING_HOOK */

#endif /* RT_USING_MEMPOOL */

/* 如果开启了堆内存管理（动态内存分配） */
#ifdef RT_USING_HEAP
/*
 * heap memory interface
 * 堆内存接口
 */
/* 系统堆内存初始化 */
void rt_system_heap_init(void *begin_addr, void *end_addr);
/* 通用堆内存初始化 */
void rt_system_heap_init_generic(void *begin_addr, void *end_addr);

/* 动态分配指定大小的内存 */
void *rt_malloc(rt_size_t size);
/* 释放动态分配的内存 */
void rt_free(void *ptr);
/* 重新分配内存大小 */
void *rt_realloc(void *ptr, rt_size_t newsize);
/* 分配并清零指定数量的内存块 */
void *rt_calloc(rt_size_t count, rt_size_t size);
/* 分配按指定字节对齐的内存 */
void *rt_malloc_align(rt_size_t size, rt_size_t align);
/* 释放按对齐分配的内存 */
void rt_free_align(void *ptr);

/* 获取内存使用信息（总大小、已用大小、最大使用大小） */
void rt_memory_info(rt_size_t *total,
                    rt_size_t *used,
                    rt_size_t *max_used);

/* 如果同时使用了 SLAB 并将其作为系统堆 */
#if defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP)
/* 分配连续的内存页 */
void *rt_page_alloc(rt_size_t npages);
/* 释放连续的内存页 */
void rt_page_free(void *addr, rt_size_t npages);
#endif /* defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP) */

/**
 * @ingroup group_hook
 * 钩子函数分组
 * @{
 */
/* 堆内存操作相关的钩子 */
#ifdef RT_USING_HOOK
/* 设置 malloc 时的钩子 */
void rt_malloc_sethook(void (*hook)(void **ptr, rt_size_t size));
/* 设置 realloc 入口时的钩子 */
void rt_realloc_set_entry_hook(void (*hook)(void **ptr, rt_size_t size));
/* 设置 realloc 出口时的钩子 */
void rt_realloc_set_exit_hook(void (*hook)(void **ptr, rt_size_t size));
/* 设置 free 时的钩子 */
void rt_free_sethook(void (*hook)(void **ptr));
#endif /* RT_USING_HOOK */
/**@}*/ /* 钩子分组结束 */

#endif /* RT_USING_HEAP */

/* 如果开启了小内存管理算法 */
#ifdef RT_USING_SMALL_MEM
/**
 * small memory object interface
 * 小内存管理对象接口
 */
/* 初始化小内存堆 */
rt_smem_t rt_smem_init(const char    *name,
                     void          *begin_addr,
                     rt_size_t      size);
/* 脱离小内存堆 */
rt_err_t rt_smem_detach(rt_smem_t m);
/* 从小内存堆分配内存 */
void *rt_smem_alloc(rt_smem_t m, rt_size_t size);
/* 重新分配小内存堆中的内存 */
void *rt_smem_realloc(rt_smem_t m, void *rmem, rt_size_t newsize);
/* 释放小内存堆中的内存 */
void rt_smem_free(void *rmem);
#endif /* RT_USING_SMALL_MEM */

/* 如果开启了 MemHeap 管理（可将多个内存块连成一个大堆） */
#ifdef RT_USING_MEMHEAP
/**
 * memory heap object interface
 * 内存堆对象接口
 */
/* 初始化 memheap */
rt_err_t rt_memheap_init(struct rt_memheap *memheap,
                         const char        *name,
                         void              *start_addr,
                         rt_size_t         size);
/* 脱离 memheap */
rt_err_t rt_memheap_detach(struct rt_memheap *heap);
/* 从 memheap 分配内存 */
void *rt_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
/* 重新分配 memheap 中的内存 */
void *rt_memheap_realloc(struct rt_memheap *heap, void *ptr, rt_size_t newsize);
/* 释放 memheap 中的内存 */
void rt_memheap_free(void *ptr);
/* 获取 memheap 的内存信息 */
void rt_memheap_info(struct rt_memheap *heap,
                     rt_size_t *total,
                     rt_size_t *used,
                     rt_size_t *max_used);
#endif /* RT_USING_MEMHEAP */

/* 如果将 MemHeap 作为系统主堆使用 */
#ifdef RT_USING_MEMHEAP_AS_HEAP
/**
 * memory heap as heap
 * 使用 memheap 作为系统堆
 */
/* memheap 方式的分配 */
void *_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
/* memheap 方式的释放 */
void _memheap_free(void *rmem);
/* memheap 方式的重分配 */
void *_memheap_realloc(struct rt_memheap *heap, void *rmem, rt_size_t newsize);
#endif

/* 如果开启了 SLAB 内存管理算法 */
#ifdef RT_USING_SLAB
/**
 * slab object interface
 * SLAB 对象接口
 */
/* 初始化 SLAB 分配器 */
rt_slab_t rt_slab_init(const char *name, void *begin_addr, rt_size_t size);
/* 脱离 SLAB 分配器 */
rt_err_t rt_slab_detach(rt_slab_t m);
/* 从 SLAB 分配连续的页面 */
void *rt_slab_page_alloc(rt_slab_t m, rt_size_t npages);
/* 释放连续的页面给 SLAB */
void rt_slab_page_free(rt_slab_t m, void *addr, rt_size_t npages);
/* 从 SLAB 分配内存 */
void *rt_slab_alloc(rt_slab_t m, rt_size_t size);
/* 重新分配 SLAB 中的内存 */
void *rt_slab_realloc(rt_slab_t m, void *ptr, rt_size_t size);
/* 释放 SLAB 中的内存 */
void rt_slab_free(rt_slab_t m, void *ptr);
#endif /* RT_USING_SLAB */

/**@}*/ /* 内存管理分组结束 */

/**
 * @addtogroup group_thread_comm
 * 线程间通信分组
 * @{
 */

/**
 * Suspend list - A basic building block for IPC primitives which interacts with
 *                scheduler directly. Its API is similar to a FIFO list.
 * 挂起列表 - IPC原语（如信号量等）与调度器直接交互的基础构件。其API类似于FIFO列表。
 *
 * Note: don't use in application codes directly
 * 注意：不要在应用程序代码中直接使用
 */
/* 打印挂起列表中的线程信息 */
void rt_susp_list_print(rt_list_t *list);
/* 恢复线程时保留线程错误码的标志定义 */
/* reserve thread error while resuming it */
#define RT_THREAD_RESUME_RES_THR_ERR (-1)
/* 从挂起列表中出队一个线程，并设置其错误码 */
struct rt_thread *rt_susp_list_dequeue(rt_list_t *susp_list, rt_err_t thread_error);
/* 唤醒挂起列表中的所有线程 */
rt_err_t rt_susp_list_resume_all(rt_list_t *susp_list, rt_err_t thread_error);
/* 在中断安全上下文中唤醒挂起列表中的所有线程，需传入自旋锁 */
rt_err_t rt_susp_list_resume_all_irq(rt_list_t *susp_list,
                                     rt_err_t thread_error,
                                     struct rt_spinlock *lock);

/* suspend and enqueue 挂起并加入队列 */
/* 将线程挂起并加入指定挂起列表 */
rt_err_t rt_thread_suspend_to_list(rt_thread_t thread, rt_list_t *susp_list, int ipc_flags, int suspend_flag);
/* 仅针对已挂起的线程，将其加入挂起队列，调用者必须持有调度器锁 */
/* only for a suspended thread, and caller must hold the scheduler lock */
rt_err_t rt_susp_list_enqueue(rt_list_t *susp_list, rt_thread_t thread, int ipc_flags);

/**
 * @addtogroup group_semaphore Semaphore
 * 信号量分组
 * @{
 */
/* 如果开启了信号量功能 */
#ifdef RT_USING_SEMAPHORE
/*
 * semaphore interface
 * 信号量接口
 */
/* 静态初始化信号量 */
rt_err_t rt_sem_init(rt_sem_t    sem,
                     const char *name,
                     rt_uint32_t value,
                     rt_uint8_t  flag);
/* 脱离静态信号量 */
rt_err_t rt_sem_detach(rt_sem_t sem);
/* 动态创建信号量 */
#ifdef RT_USING_HEAP
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag);
/* 删除动态信号量 */
rt_err_t rt_sem_delete(rt_sem_t sem);
#endif /* RT_USING_HEAP */

/* 获取信号量（可能永久等待） */
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t timeout);
/* 可中断方式获取信号量 */
rt_err_t rt_sem_take_interruptible(rt_sem_t sem, rt_int32_t timeout);
/* 可被杀死方式获取信号量 */
rt_err_t rt_sem_take_killable(rt_sem_t sem, rt_int32_t timeout);
/* 尝试获取信号量（不等待） */
rt_err_t rt_sem_trytake(rt_sem_t sem);
/* 释放信号量 */
rt_err_t rt_sem_release(rt_sem_t sem);
/* 信号量控制函数 */
rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg);
#endif /* RT_USING_SEMAPHORE */

/**@}*/ /* 信号量分组结束 */

/**
 * @addtogroup group_mutex Mutex
 * 互斥量分组
 * @{
 */
/* 如果开启了互斥量功能 */
#ifdef RT_USING_MUTEX
/*
 * mutex interface
 * 互斥量接口
 */
/* 静态初始化互斥量 */
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag);
/* 脱离静态互斥量 */
rt_err_t rt_mutex_detach(rt_mutex_t mutex);
/* 动态创建互斥量 */
#ifdef RT_USING_HEAP
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag);
/* 删除动态互斥量 */
rt_err_t rt_mutex_delete(rt_mutex_t mutex);
#endif /* RT_USING_HEAP */
/* 将指定线程从互斥量的等待队列中移除（放弃竞争） */
void rt_mutex_drop_thread(rt_mutex_t mutex, rt_thread_t thread);
/* 设置互斥量的优先级上限（优先级天花板协议） */
rt_uint8_t rt_mutex_setprioceiling(rt_mutex_t mutex, rt_uint8_t priority);
/* 获取互斥量的优先级上限 */
rt_uint8_t rt_mutex_getprioceiling(rt_mutex_t mutex);

/* 获取互斥量（可能永久等待） */
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t timeout);
/* 尝试获取互斥量（不等待） */
rt_err_t rt_mutex_trytake(rt_mutex_t mutex);
/* 可中断方式获取互斥量 */
rt_err_t rt_mutex_take_interruptible(rt_mutex_t mutex, rt_int32_t time);
/* 可被杀死方式获取互斥量 */
rt_err_t rt_mutex_take_killable(rt_mutex_t mutex, rt_int32_t time);
/* 释放互斥量 */
rt_err_t rt_mutex_release(rt_mutex_t mutex);
/* 互斥量控制函数 */
rt_err_t rt_mutex_control(rt_mutex_t mutex, int cmd, void *arg);

/* 内联函数：获取互斥量的当前持有者线程 */
rt_inline rt_thread_t rt_mutex_get_owner(rt_mutex_t mutex)
{
    return mutex->owner;
}
/* 内联函数：获取互斥量被持有的次数（递归锁计数） */
rt_inline rt_ubase_t rt_mutex_get_hold(rt_mutex_t mutex)
{
    return mutex->hold;
}

#endif /* RT_USING_MUTEX */

/**@}*/ /* 互斥量分组结束 */

/**
 * @addtogroup group_event Event
 * 事件集分组
 * @{
 */
/* 如果开启了事件集功能 */
#ifdef RT_USING_EVENT
/*
 * event interface
 * 事件集接口
 */
/* 静态初始化事件集 */
rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag);
/* 脱离静态事件集 */
rt_err_t rt_event_detach(rt_event_t event);
/* 动态创建事件集 */
#ifdef RT_USING_HEAP
rt_event_t rt_event_create(const char *name, rt_uint8_t flag);
/* 删除动态事件集 */
rt_err_t rt_event_delete(rt_event_t event);
#endif /* RT_USING_HEAP */

/* 发送事件（设置事件标志位） */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set);
/* 接收事件 */
rt_err_t rt_event_recv(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/* 可中断方式接收事件 */
rt_err_t rt_event_recv_interruptible(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/* 可被杀死方式接收事件 */
rt_err_t rt_event_recv_killable(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/* 事件集控制函数 */
rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg);
#endif /* RT_USING_EVENT */

/**@}*/ /* 事件集分组结束 */

/**
 * @addtogroup group_mailbox MailBox
 * 邮箱分组
 * @{
 */
/* 如果开启了邮箱功能 */
#ifdef RT_USING_MAILBOX
/*
 * mailbox interface
 * 邮箱接口
 */
/* 静态初始化邮箱 */
rt_err_t rt_mb_init(rt_mailbox_t mb,
                    const char  *name,
                    void        *msgpool,
                    rt_size_t    size,
                    rt_uint8_t   flag);
/* 脱离静态邮箱 */
rt_err_t rt_mb_detach(rt_mailbox_t mb);
/* 动态创建邮箱 */
#ifdef RT_USING_HEAP
rt_mailbox_t rt_mb_create(const char *name, rt_size_t size, rt_uint8_t flag);
/* 删除动态邮箱 */
rt_err_t rt_mb_delete(rt_mailbox_t mb);
#endif /* RT_USING_HEAP */

/* 发送邮件（不等待，满则失败） */
rt_err_t rt_mb_send(rt_mailbox_t mb, rt_ubase_t value);
/* 可中断方式发送邮件 */
rt_err_t rt_mb_send_interruptible(rt_mailbox_t mb, rt_ubase_t value);
/* 可被杀死方式发送邮件 */
rt_err_t rt_mb_send_killable(rt_mailbox_t mb, rt_ubase_t value);
/* 发送邮件（可等待指定时间） */
rt_err_t rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/* 可中断等待方式发送邮件 */
rt_err_t rt_mb_send_wait_interruptible(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/* 可被杀死等待方式发送邮件 */
rt_err_t rt_mb_send_wait_killable(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/* 发送紧急邮件（插队到队列最前面） */
rt_err_t rt_mb_urgent(rt_mailbox_t mb, rt_ubase_t value);
/* 接收邮件 */
rt_err_t rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/* 可中断方式接收邮件 */
rt_err_t rt_mb_recv_interruptible(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/* 可被杀死方式接收邮件 */
rt_err_t rt_mb_recv_killable(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/* 邮箱控制函数 */
rt_err_t rt_mb_control(rt_mailbox_t mb, int cmd, void *arg);
#endif /* RT_USING_MAILBOX */

/**@}*/ /* 邮箱分组结束 */

/**
 * @addtogroup group_messagequeue Message Queue
 * 消息队列分组
 * @{
 */
/* 如果开启了消息队列功能 */
#ifdef RT_USING_MESSAGEQUEUE

/* 消息队列中消息的头部结构 */
struct rt_mq_message
{
    struct rt_mq_message *next; /* 指向下一条消息 */
    rt_ssize_t length;         /* 消息长度 */
/* 如果开启了消息队列优先级特性 */
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
    rt_int32_t prio;          /* 消息优先级 */
#endif /* RT_USING_MESSAGEQUEUE_PRIORITY */
};

/* 宏：计算消息队列缓冲区的大小，考虑了内存对齐和消息头大小 */
#define RT_MQ_BUF_SIZE(msg_size, max_msgs) \
((RT_ALIGN((msg_size), RT_ALIGN_SIZE) + sizeof(struct rt_mq_message)) * (max_msgs))

/*
 * message queue interface
 * 消息队列接口
 */
/* 静态初始化消息队列 */
rt_err_t rt_mq_init(rt_mq_t     mq,
                    const char *name,
                    void       *msgpool,
                    rt_size_t   msg_size,
                    rt_size_t   pool_size,
                    rt_uint8_t  flag);
/* 脱离静态消息队列 */
rt_err_t rt_mq_detach(rt_mq_t mq);
/* 动态创建消息队列 */
#ifdef RT_USING_HEAP
rt_mq_t rt_mq_create(const char *name,
                     rt_size_t   msg_size,
                     rt_size_t   max_msgs,
                     rt_uint8_t  flag);
/* 删除动态消息队列 */
rt_err_t rt_mq_delete(rt_mq_t mq);
#endif /* RT_USING_HEAP */

/* 发送消息到队列（不等待） */
rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size);
/* 可中断方式发送消息 */
rt_err_t rt_mq_send_interruptible(rt_mq_t mq, const void *buffer, rt_size_t size);
/* 可被杀死方式发送消息 */
rt_err_t rt_mq_send_killable(rt_mq_t mq, const void *buffer, rt_size_t size);
/* 发送消息到队列（可等待指定时间） */
rt_err_t rt_mq_send_wait(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/* 可中断等待方式发送消息 */
rt_err_t rt_mq_send_wait_interruptible(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/* 可被杀死等待方式发送消息 */
rt_err_t rt_mq_send_wait_killable(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/* 发送紧急消息（插队到队列头部） */
rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size);
/* 从消息队列接收消息，返回接收到的长度 */
rt_ssize_t rt_mq_recv(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/* 可中断方式接收消息 */
rt_ssize_t rt_mq_recv_interruptible(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/* 可被杀死方式接收消息 */
rt_ssize_t rt_mq_recv_killable(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/* 消息队列控制函数 */
rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg);

/* 如果开启了带优先级的消息队列 */
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
/* 按优先级等待发送消息 */
rt_err_t rt_mq_send_wait_prio(rt_mq_t mq,
                              const void *buffer,
                              rt_size_t size,
                              rt_int32_t prio,
                              rt_int32_t timeout,
                              int suspend_flag);
/* 按优先级接收消息 */
rt_ssize_t rt_mq_recv_prio(rt_mq_t mq,
                           void *buffer,
                           rt_size_t size,
                           rt_int32_t *prio,
                           rt_int32_t timeout,
                           int suspend_flag);
#endif /* RT_USING_MESSAGEQUEUE_PRIORITY */
#endif /* RT_USING_MESSAGEQUEUE */

/**@}*/ /* 消息队列分组结束 */

/* defunct 僵尸线程/资源回收机制 */
/* 初始化僵尸线程回收机制 */
void rt_thread_defunct_init(void);
/* 将线程加入僵尸队列（等待回收） */
void rt_thread_defunct_enqueue(rt_thread_t thread);
/* 从僵尸队列中取出一个线程 */
rt_thread_t rt_thread_defunct_dequeue(void);
/* 执行僵尸线程的真正销毁操作 */
void rt_defunct_execute(void);

/*
 * spinlock
 * 自旋锁接口（通常用于多核SMP或中断与线程间的互斥）
 */
/* 前向声明自旋锁结构体 */
struct rt_spinlock;

/* 初始化自旋锁 */
void rt_spin_lock_init(struct rt_spinlock *lock);
/* 获取自旋锁（忙等待） */
void rt_spin_lock(struct rt_spinlock *lock);
/* 释放自旋锁 */
void rt_spin_unlock(struct rt_spinlock *lock);
/* 获取自旋锁并关闭本地CPU中断，返回中断状态 */
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock);
/* 恢复中断状态并释放自旋锁 */
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level);

/**@}*/ /* 线程间通信分组结束 */

/* 如果开启了设备驱动框架 */
#ifdef RT_USING_DEVICE
/**
 * @addtogroup group_device_driver
 * 设备驱动分组
 * @{
 */

/*
 * device (I/O) system interface
 * 设备 I/O 系统接口
 */
/* 根据名称查找设备 */
rt_device_t rt_device_find(const char *name);

/* 注册设备到系统 */
rt_err_t rt_device_register(rt_device_t dev,
                            const char *name,
                            rt_uint16_t flags);
/* 从系统中注销设备 */
rt_err_t rt_device_unregister(rt_device_t dev);

/* 动态创建设备对象 */
#ifdef RT_USING_HEAP
rt_device_t rt_device_create(int type, int attach_size);
/* 销毁动态创建的设备对象 */
void rt_device_destroy(rt_device_t device);
#endif /* RT_USING_HEAP */

/* 设置设备接收到数据的回调函数（指示有数据可读） */
rt_err_t
rt_device_set_rx_indicate(rt_device_t dev,
                          rt_err_t (*rx_ind)(rt_device_t dev, rt_size_t size));
/* 设置设备数据发送完成的回调函数 */
rt_err_t
rt_device_set_tx_complete(rt_device_t dev,
                          rt_err_t (*tx_done)(rt_device_t dev, void *buffer));

/* 初始化设备 */
rt_err_t  rt_device_init (rt_device_t dev);
/* 打开设备 */
rt_err_t  rt_device_open (rt_device_t dev, rt_uint16_t oflag);
/* 关闭设备 */
rt_err_t  rt_device_close(rt_device_t dev);
/* 读取设备数据 */
rt_ssize_t rt_device_read(rt_device_t dev,
                          rt_off_t    pos,
                          void       *buffer,
                          rt_size_t   size);
/* 向设备写入数据 */
rt_ssize_t rt_device_write(rt_device_t dev,
                          rt_off_t    pos,
                          const void *buffer,
                          rt_size_t   size);
/* 控制设备 */
rt_err_t  rt_device_control(rt_device_t dev, int cmd, void *arg);

/**@}*/ /* 设备驱动分组结束 */
#endif /* RT_USING_DEVICE */

/*
 * interrupt service
 * 中断服务接口
 */

/*
 * rt_interrupt_enter and rt_interrupt_leave only can be called by BSP
 * 进入和离开中断函数只能由 BSP (板级支持包) 调用
 */
/* 进入中断上下文（中断嵌套计数+1） */
void rt_interrupt_enter(void);
/* 离开中断上下文（中断嵌套计数-1） */
void rt_interrupt_leave(void);

/* 压入中断上下文环境 */
void rt_interrupt_context_push(rt_interrupt_context_t this_ctx);
/* 弹出中断上下文环境 */
void rt_interrupt_context_pop(void);
/* 获取当前中断上下文 */
void *rt_interrupt_context_get(void);

/**
 * CPU object
 * CPU 对象接口（多核相关）
 */
/* 获取当前执行代码的 CPU 对象 */
struct rt_cpu *rt_cpu_self(void);
/* 根据索引获取 CPU 对象 */
struct rt_cpu *rt_cpu_index(int index);

/* 如果开启了多核SMP支持 */
#ifdef RT_USING_SMP

/*
 * smp cpus lock service
 * SMP 多核 CPU 锁服务（全局锁住所有核的调度）
 */

/* 锁住所有 CPU 核心（禁止调度），返回锁状态 */
rt_base_t rt_cpus_lock(void);
/* 解锁所有 CPU 核心 */
void rt_cpus_unlock(rt_base_t level);
/* 恢复线程的 CPU 锁状态（用于线程切换） */
void rt_cpus_lock_status_restore(struct rt_thread *thread);

/* 如果开启了调试功能 */
#ifdef RT_USING_DEBUG
    /* 获取当前 CPU 的 ID（函数调用方式） */
    rt_base_t rt_cpu_get_id(void);
#else /* !RT_USING_DEBUG */
    /* 获取当前 CPU 的 ID（直接读取硬件寄存器，效率高） */
    #define rt_cpu_get_id rt_hw_cpu_id
#endif /* RT_USING_DEBUG */

#else /* !RT_USING_SMP 单核环境 */
/* 单核下 CPU ID 固定为 0 */
#define rt_cpu_get_id()  (0)

#endif /* RT_USING_SMP */

/*
 * the number of nested interrupts.
 * 获取当前中断嵌套的层数
 */
rt_uint8_t rt_interrupt_get_nest(void);

/* 中断相关的钩子函数 */
#ifdef RT_USING_HOOK
/* 设置进入中断时的钩子 */
void rt_interrupt_enter_sethook(void (*hook)(void));
/* 设置离开中断时的钩子 */
void rt_interrupt_leave_sethook(void (*hook)(void));
#endif /* RT_USING_HOOK */

/* 如果开启了组件自动初始化机制 */
#ifdef RT_USING_COMPONENTS_INIT
/* 组件初始化（在main线程中调用，初始化各种组件） */
void rt_components_init(void);
/* 板级组件初始化（在调度器启动前调用） */
void rt_components_board_init(void);
#endif /* RT_USING_COMPONENTS_INIT */

/**
 * @addtogroup group_kernel_service
 * 内核通用服务分组
 * @{
 */

/*
 * general kernel service
 * 通用内核服务
 */
/* 如果未使用控制台，将打印函数定义为空操作，节省空间 */
#ifndef RT_USING_CONSOLE
#define rt_kprintf(...)
#define rt_kputs(str)
#else
/* 格式化打印输出（类似 printf） */
int rt_kprintf(const char *fmt, ...);
/* 输出字符串 */
void rt_kputs(const char *str);
/* 如果开启了控制台输出控制 */
#ifdef RT_USING_CONSOLE_OUTPUT_CTL
/* 设置控制台是否使能输出 */
void rt_console_output_set_enabled(rt_bool_t enabled);
/* 获取控制台输出是否使能 */
rt_bool_t rt_console_output_get_enabled(void);
#else
/* 未开启控制台输出控制时，设置为空操作和默认返回真 */
#define rt_console_output_set_enabled(enabled) ((void)0)
#define rt_console_output_get_enabled()        (RT_TRUE)
#endif /* RT_USING_CONSOLE_OUTPUT_CTL */
#endif /* RT_USING_CONSOLE */

/* 打印当前线程的调用栈回溯信息 */
rt_err_t rt_backtrace(void);
/* 打印指定线程的调用栈回溯信息 */
rt_err_t rt_backtrace_thread(rt_thread_t thread);
/* 基于栈帧打印调用栈回溯信息 */
rt_err_t rt_backtrace_frame(rt_thread_t thread, struct rt_hw_backtrace_frame *frame);
/* 格式化打印回溯缓冲区中的数据 */
rt_err_t rt_backtrace_formatted_print(rt_ubase_t *buffer, long buflen);
/* 将回溯信息保存到指定的缓冲区中 */
rt_err_t rt_backtrace_to_buffer(rt_thread_t thread, struct rt_hw_backtrace_frame *frame,
                                long skip, rt_ubase_t *buffer, long buflen);

/* 如果同时开启了设备和控制台 */
#if defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE)
/* 设置控制台输出的设备 */
rt_device_t rt_console_set_device(const char *name);
/* 获取当前控制台使用的设备 */
rt_device_t rt_console_get_device(void);
/* 如果开启了线程安全的打印功能 */
#ifdef RT_USING_THREADSAFE_PRINTF
    /* 获取当前正在使用控制台的线程 */
    rt_thread_t rt_console_current_user(void);
#else
    /* 内联函数：未开启线程安全打印时，返回空指针 */
    rt_inline void *rt_console_current_user(void) { return RT_NULL; }
#endif /* RT_USING_THREADSAFE_PRINTF */
#endif /* defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE) */

/* 查找整数的最高有效位（最高位的1的索引，类似 __builtin_clz 的反操作） */
int __rt_fls(int val);
/* 查找整数的最低有效位（最低位的1的索引，类似 __builtin_ffs） */
int __rt_ffs(int value);
/* 查找 long 类型整数的最低有效位 */
unsigned long __rt_ffsl(unsigned long value);
/* 计算前导零的数量 */
unsigned long __rt_clz(unsigned long value);

/* 打印 RT-Thread 版本信息 */
void rt_show_version(void);

/* 如果开启了断言调试功能 */
#ifdef RT_DEBUGING_ASSERT
/* 声明断言钩子函数指针 */
extern void (*rt_assert_hook)(const char *ex, const char *func, rt_size_t line);
/* 设置断言发生时的钩子函数 */
void rt_assert_set_hook(void (*hook)(const char *ex, const char *func, rt_size_t line));
/* 默认的断言处理函数 */
void rt_assert_handler(const char *ex, const char *func, rt_size_t line);

/* 系统断言宏：如果表达式 EX 为假，则触发断言处理 */
#define RT_ASSERT(EX)                                                         \
if (!(EX))                                                                    \
{                                                                             \
    rt_assert_handler(#EX, __FUNCTION__, __LINE__);                           \
}
#else
/* 未开启断言时，仅用于消除编译器变量未使用的警告 */
#define RT_ASSERT(EX) {RT_UNUSED(EX);}
#endif /* RT_DEBUGING_ASSERT */

/* 如果开启了上下文调试检查 */
#ifdef RT_DEBUGING_CONTEXT
/* Macro to check current context 检查当前上下文的宏 */
/* 确保当前不在中断服务程序中执行 */
#define RT_DEBUG_NOT_IN_INTERRUPT                                             \
do                                                                            \
{                                                                             \
    if (rt_interrupt_get_nest() != 0)                                         \
    {                                                                         \
        rt_kprintf("Function[%s] shall not be used in ISR\n", __FUNCTION__);  \
        RT_ASSERT(0)                                                          \
    }                                                                         \
}                                                                             \
while (0)

/* "In thread context" means:
 * "处于线程上下文"意味着：
 *     1) the scheduler has been started 调度器已经启动
 *     2) not in interrupt context. 不在中断上下文中
 */
/* 确保当前处于线程上下文中 */
#define RT_DEBUG_IN_THREAD_CONTEXT                                            \
do                                                                            \
{                                                                             \
    if (rt_thread_self() == RT_NULL)                                          \
    {                                                                         \
        rt_kprintf("Function[%s] shall not be used before scheduler start\n", \
                   __FUNCTION__);                                             \
        RT_ASSERT(0)                                                          \
    }                                                                         \
    RT_DEBUG_NOT_IN_INTERRUPT;                                                \
}                                                                             \
while (0)

/* 如果是 SMP 多核系统 */
#if defined(RT_USING_SMP)
/**
 * @brief Check whether disabled interrupts make scheduler unavailable.
 * 检查禁用中断是否导致调度器不可用。
 *
 * In SMP builds, some kernel-internal lockless wait paths may disable local
 * interrupts while still using scheduler-related operations legally. Keep this
 * IRQ-disabled context assertion for UP builds only.
 * 在 SMP 构建中，一些内核内部的无锁等待路径可能会禁用本地中断，同时仍合法地使用与调度器相关的操作。
 * 因此仅在 UP（单核）构建中保留此 IRQ 禁用上下文断言。
 */
/* SMP 系统下，局部关中断不影响全局调度，所以返回假 */
#define RT_DEBUG_SCHEDULER_IRQ_DISABLED() (RT_FALSE)
#else
/**
 * @brief Check whether disabled interrupts make scheduler unavailable.
 * 检查禁用中断是否导致调度器不可用。
 *
 * In UP builds, globally disabled interrupts prevent normal scheduling and
 * timeout progress, so blocking scheduler paths must reject this context.
 * 在 UP（单核）构建中，全局禁用中断会阻止正常的调度和超时进展，
 * 因此阻塞调度路径必须拒绝此上下文。
 */
/* 单核系统下，关中断会导致调度器不可用，返回中断是否关闭的状态 */
#define RT_DEBUG_SCHEDULER_IRQ_DISABLED() rt_hw_interrupt_is_disabled()
#endif /* defined(RT_USING_SMP) */

/* "scheduler available" means: "调度器可用"意味着：
 *     1) the scheduler has been started. 调度器已启动
 *     2) not in interrupt context. 不在中断上下文
 *     3) scheduler is not locked. 调度器未被锁定（未进入临界区）
 *     4) interrupts are not disabled on UP. 单核下中断未关闭
 */
/* 检查调度器是否可用 */
#define RT_DEBUG_SCHEDULER_AVAILABLE(need_check)                              \
do                                                                            \
{                                                                             \
    if (need_check)                                                           \
    {                                                                         \
        if ((rt_critical_level() != 0) || RT_DEBUG_SCHEDULER_IRQ_DISABLED())  \
        {                                                                     \
            rt_kprintf("Function[%s]: scheduler is not available\n",          \
                    __FUNCTION__);                                            \
            RT_ASSERT(0)                                                      \
        }                                                                     \
        RT_DEBUG_IN_THREAD_CONTEXT;                                           \
    }                                                                         \
}                                                                             \
while (0)
#else
/* 未开启上下文调试检查时，宏定义为空操作 */
#define RT_DEBUG_NOT_IN_INTERRUPT
#define RT_DEBUG_IN_THREAD_CONTEXT
#define RT_DEBUG_SCHEDULER_AVAILABLE(need_check)
#endif /* RT_DEBUGING_CONTEXT */

/* 内联函数：判断当前是否处于线程上下文（调度器已启动且不在中断中） */
rt_inline rt_bool_t rt_in_thread_context(void)
{
    return rt_thread_self() != RT_NULL && rt_interrupt_get_nest() == 0;
}

/* is scheduler available 调度器是否可用 */
/* 内联函数：判断调度器是否可用（未加锁且处于线程上下文） */
rt_inline rt_bool_t rt_scheduler_is_available(void)
{
    return rt_critical_level() == 0 && rt_in_thread_context();
}

/* 如果是 SMP 多核系统 */
#ifdef RT_USING_SMP
/* is thread bond on core 线程是否绑定在特定核心上 */
/* 内联函数：判断线程是否绑核，如果未指定线程则检查当前线程 */
rt_inline rt_bool_t rt_sched_thread_is_binding(rt_thread_t thread)
{
    if (thread == RT_NULL)
    {
        thread = rt_thread_self();
    }
    /* 如果 bind_cpu 不等于 RT_CPUS_NR（即CPU总数），说明绑定了特定核心 */
    return !thread || RT_SCHED_CTX(thread).bind_cpu != RT_CPUS_NR;
}

#else
/* 单核系统下，线程始终在唯一的核上运行，返回真 */
#define rt_sched_thread_is_binding(thread) (RT_TRUE)
#endif

/**@}*/ /* 内核通用服务分组结束 */

/* 结束 C++ 的 extern "C" 块 */
#ifdef __cplusplus
}
#endif

/* 头文件保护宏结束 */
#endif /* __RT_THREAD_H__ */
