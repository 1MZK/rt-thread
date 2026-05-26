/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 * 
 * 版权声明：归 RT-Thread 开发团队所有。
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * 许可证标识：遵循 Apache 2.0 开源许可证。
 *
 * Change Logs:
 * 变更日志：记录了文件历次修改的时间、作者及修改内容摘要。
 * Date           Author       Notes
 * 2006-03-28     Bernard      first version (首次版本)
 * 2006-04-29     Bernard      implement thread timer (实现线程定时器)
 * 2006-04-30     Bernard      added THREAD_DEBUG (增加线程调试)
 * 2006-05-27     Bernard      fixed the rt_thread_yield bug (修复 yield 函数 bug)
 * 2006-06-03     Bernard      fixed the thread timer init bug (修复线程定时器初始化 bug)
 * 2006-08-10     Bernard      fixed the timer bug in thread_sleep (修复 sleep 中的定时器 bug)
 * 2006-09-03     Bernard      changed rt_timer_delete to rt_timer_detach (将 delete 改为 detach)
 * 2006-09-03     Bernard      implement rt_thread_detach (实现线程脱离)
 * 2008-02-16     Bernard      fixed the rt_thread_timeout bug (修复超时 bug)
 * 2010-03-21     Bernard      change the errno of rt_thread_delay/sleep to
 *                             RT_EOK. (将 delay/sleep 的返回错误码改为 RT_EOK)
 * 2010-11-10     Bernard      add cleanup callback function in thread exit. (增加线程退出时的清理回调)
 * 2011-09-01     Bernard      fixed rt_thread_exit issue when the current
 *                             thread preempted, which reported by Jiaxing Lee. (修复线程被抢占时的退出问题)
 * 2011-09-08     Bernard      fixed the scheduling issue in rt_thread_startup. (修复启动时的调度问题)
 * 2012-12-29     Bernard      fixed compiling warning. (修复编译警告)
 * 2016-08-09     ArdaFu       add thread suspend and resume hook. (增加挂起和恢复钩子)
 * 2017-04-10     armink       fixed the rt_thread_delete and rt_thread_detach
 *                             bug when thread has not startup. (修复线程未启动时删除/脱离的 bug)
 * 2018-11-22     Jesven       yield is same to rt_schedule
 *                             add support for tasks bound to cpu (yield 等同于调度，增加 CPU 绑核支持)
 * 2021-02-24     Meco Man     rearrange rt_thread_control() - schedule the thread when close it (重排控制函数，关闭时调度)
 * 2021-11-15     THEWON       Remove duplicate work between idle and _thread_exit (移除空闲线程与退出函数间的重复工作)
 * 2021-12-27     Meco Man     remove .init_priority (移除 init_priority 字段)
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to thread.c (将钩子函数移至此文件)
 * 2022-01-24     THEWON       let _thread_sleep return thread->error when using signal (使用信号时返回线程错误码)
 * 2022-10-15     Bernard      add nested mutex feature (增加嵌套互斥量特性)
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable (优化中断开关性能，改用自旋锁等)
 * 2023-12-10     xqyjlj       fix thread_exit/detach/delete
 *                             fix rt_thread_delay (修复退出/脱离/删除及延时逻辑)
 */

/* 包含 RT-Thread 硬件相关定义头文件 */
#include <rthw.h>
/* 包含 RT-Thread 系统的核心头文件 */
#include <rtthread.h>
/* 包含标准库空指针定义 */
#include <stddef.h>

/* 定义调试标签，用于日志输出标识 */
#define DBG_TAG           "kernel.thread"
/* 定义调试级别为信息级别 */
#define DBG_LVL           DBG_INFO
/* 包含 RT-Thread 调试日志头文件 */
#include <rtdbg.h>

/* 如果开启了钩子函数且使用函数指针方式实现 */
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
/* 定义线程挂起时的钩子函数指针 */
static void (*rt_thread_suspend_hook)(rt_thread_t thread);
/* 定义线程恢复时的钩子函数指针 */
static void (*rt_thread_resume_hook) (rt_thread_t thread);

/**
 * @brief   This function sets a hook function when the system suspend a thread.
 * 此函数设置系统挂起线程时的钩子函数。
 *
 * @note    The hook function must be simple and never be blocked or suspend.
 * 注意：钩子函数必须简短，绝对不能被阻塞或挂起。
 *
 * @param   hook is the specified hook function.
 * 参数 hook 是指定的钩子函数。
 */
void rt_thread_suspend_sethook(void (*hook)(rt_thread_t thread))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_thread_suspend_hook = hook;
}

/**
 * @brief   This function sets a hook function when the system resume a thread.
 * 此函数设置系统恢复线程时的钩子函数。
 *
 * @note    The hook function must be simple and never be blocked or suspend.
 * 注意：钩子函数必须简短，绝对不能被阻塞或挂起。
 *
 * @param   hook is the specified hook function.
 * 参数 hook 是指定的钩子函数。
 */
void rt_thread_resume_sethook(void (*hook)(rt_thread_t thread))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_thread_resume_hook = hook;
}

/* 定义线程初始化完成的钩子列表 */
RT_OBJECT_HOOKLIST_DEFINE(rt_thread_inited);
#endif /* defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR) */

/* 如果开启了互斥量功能 */
#ifdef RT_USING_MUTEX
/**
 * @brief 将线程从其关联的互斥量中脱离出来
 */
static void _thread_detach_from_mutex(rt_thread_t thread)
{
    /* 定义链表节点指针 */
    rt_list_t *node;
    /* 定义临时链表节点指针，用于安全遍历 */
    rt_list_t *tmp_list;
    /* 定义互斥量指针 */
    struct rt_mutex *mutex;
    /* 定义中断级别变量 */
    rt_base_t level;

    /* 获取自旋锁并关中断 */
    level = rt_spin_lock_irqsave(&thread->spinlock);

    /* check if thread is waiting on a mutex 检查线程是否正在等待某个互斥量 */
    if ((thread->pending_object) &&
        (rt_object_get_type(thread->pending_object) == RT_Object_Class_Mutex))
    {
        /* remove it from its waiting list 将其从该互斥量的等待队列中移除 */
        struct rt_mutex *mutex = (struct rt_mutex*)thread->pending_object;
        /* 调用互斥量内部函数丢弃该线程 */
        rt_mutex_drop_thread(mutex, thread);
        /* 清空线程的等待对象指针 */
        thread->pending_object = RT_NULL;
    }

    /* free taken mutex after detaching from waiting, so we don't lost mutex just got 
     * 从等待队列脱离后再释放已获取的互斥量，这样我们不会丢失刚拿到的互斥量 
     */
    /* 安全遍历线程已获取的互斥量链表 */
    rt_list_for_each_safe(node, tmp_list, &(thread->taken_object_list))
    {
        /* 通过链表节点获取互斥量结构体首地址 */
        mutex = rt_list_entry(node, struct rt_mutex, taken_list);
        /* 打印调试日志：线程退出时还持有互斥量 */
        LOG_D("Thread [%s] exits while holding mutex [%s].\n", thread->parent.name, mutex->parent.parent.name);
        /* recursively take 递归获取，将 hold 设为 1 以便直接释放 */
        mutex->hold = 1;
        /* 释放互斥量 */
        rt_mutex_release(mutex);
    }

    /* 释放自旋锁并恢复中断 */
    rt_spin_unlock_irqrestore(&thread->spinlock, level);
}

/* 如果未开启互斥量功能 */
#else

/* 空函数，避免编译错误 */
static void _thread_detach_from_mutex(rt_thread_t thread) {}
#endif

/**
 * @brief 线程退出处理函数
 */
static void _thread_exit(void)
{
    /* 定义线程结构体指针 */
    struct rt_thread *thread;
    /* 定义临界区级别变量 */
    rt_base_t critical_level;

    /* get current thread 获取当前运行的线程 */
    thread = rt_thread_self();

    /* 进入临界区，关闭调度 */
    critical_level = rt_enter_critical();

    /* 关闭线程，将其从调度器中移除 */
    rt_thread_close(thread);

    /* 将线程从其关联的互斥量中脱离 */
    _thread_detach_from_mutex(thread);

    /* insert to defunct thread list 将线程插入到僵尸（待回收）线程队列 */
    rt_thread_defunct_enqueue(thread);

    /* 安全退出临界区，恢复之前的调度状态 */
    rt_exit_critical_safe(critical_level);

    /* switch to next task 触发调度，切换到下一个任务 */
    rt_schedule();
}

/**
 * @brief   This function is the timeout function for thread, normally which is invoked
 *          when thread is timeout to wait some resource.
 * 此函数是线程的超时处理函数，通常在线程等待资源超时时被调用。
 *
 * @param   parameter is the parameter of thread timeout function
 * 参数 parameter 是线程超时函数的参数（即线程控制块指针）
 */
static void _thread_timeout(void *parameter)
{
    /* 定义线程指针 */
    struct rt_thread *thread;
    /* 定义调度器锁级别变量 */
    rt_sched_lock_level_t slvl;

    /* 将参数强制转换为线程结构体指针 */
    thread = (struct rt_thread *)parameter;

    /* parameter check 参数检查 */
    /* 断言线程指针不为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 断言对象类型为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 获取调度器锁 */
    rt_sched_lock(&slvl);

    /**
     * resume of the thread and stop of the thread timer should be an atomic
     * operation. So we don't expected that thread had resumed.
     * 线程的恢复和线程定时器的停止应该是一个原子操作。
     * 因此我们不期望线程已经被恢复了。
     */
    /* 断言线程当前处于挂起状态 */
    RT_ASSERT(rt_sched_thread_is_suspended(thread));

    /* set error number 设置错误码为超时 */
    thread->error = -RT_ETIMEOUT;

    /* remove from suspend list 将线程从挂起链表中移除 */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));
    /* insert to schedule ready list 将线程插入到调度就绪链表 */
    rt_sched_insert_thread(thread);
    /* do schedule and release the scheduler lock 执行调度并释放调度器锁 */
    rt_sched_unlock_n_resched(slvl);
}

/**
 * @brief 线程底层初始化函数
 */
static rt_err_t _thread_init(struct rt_thread *thread,
                             const char       *name,
                             void (*entry)(void *parameter),
                             void             *parameter,
                             void             *stack_start,
                             rt_uint32_t       stack_size,
                             rt_uint8_t        priority,
                             rt_uint32_t       tick)
{
    /* 标记 name 参数未使用，避免编译警告 */
    RT_UNUSED(name);

    /* 初始化线程调度上下文（优先级、时间片等） */
    rt_sched_thread_init_ctx(thread, tick, priority);

/* 如果开启了内存保护 */
#ifdef RT_USING_MEM_PROTECTION
    /* 初始化内存区域指针为空 */
    thread->mem_regions = RT_NULL;
#endif

/* 如果开启了智能进程（LWP） */
#ifdef RT_USING_SMART
    /* 初始化唤醒处理函数为空 */
    thread->wakeup_handle.func = RT_NULL;
#endif

    /* 设置线程入口函数 */
    thread->entry = (void *)entry;
    /* 设置线程入口参数 */
    thread->parameter = parameter;

    /* stack init 栈初始化 */
    /* 设置栈起始地址 */
    thread->stack_addr = stack_start;
    /* 设置栈大小 */
    thread->stack_size = stack_size;

    /* init thread stack 初始化线程栈，填充 '#' 字符，用于后续栈溢出检测 */
    rt_memset(thread->stack_addr, '#', thread->stack_size);
/* 如果开启了硬件栈保护 */
#ifdef RT_USING_HW_STACK_GUARD
    /* 初始化硬件栈保护机制 */
    rt_hw_stack_guard_init(thread);
#endif
/* 如果 CPU 栈是向上增长的 */
#ifdef ARCH_CPU_STACK_GROWS_UPWARD
    /* 栈指针指向栈顶（高地址），并调用硬件栈初始化函数构建上下文 */
    thread->sp = (void *)rt_hw_stack_init(thread->entry, thread->parameter,
                                          (void *)((char *)thread->stack_addr),
                                          (void *)_thread_exit);
/* 如果 CPU 栈是向下增长的（默认） */
#else
    /* 栈指针指向栈底（低地址），并调用硬件栈初始化函数构建上下文 */
    thread->sp = (void *)rt_hw_stack_init(thread->entry, thread->parameter,
                                          (rt_uint8_t *)((char *)thread->stack_addr + thread->stack_size - sizeof(rt_ubase_t)),
                                          (void *)_thread_exit);
#endif /* ARCH_CPU_STACK_GROWS_UPWARD */

/* 如果开启了互斥量 */
#ifdef RT_USING_MUTEX
    /* 初始化线程持有的互斥量链表 */
    rt_list_init(&thread->taken_object_list);
    /* 初始化线程正在等待的对象为空 */
    thread->pending_object = RT_NULL;
#endif

/* 如果开启了事件集 */
#ifdef RT_USING_EVENT
    /* 初始化事件接收集为 0 */
    thread->event_set = 0;
    /* 初始化事件接收信息为 0 */
    thread->event_info = 0;
#endif /* RT_USING_EVENT */

    /* error and flags 错误码和标志 */
    /* 初始化错误码为 OK */
    thread->error = RT_EOK;

    /* lock init 锁初始化 */
/* 如果开启了多核 SMP */
#ifdef RT_USING_SMP
    /* 原子操作初始化 CPU 锁嵌套计数为 0 */
    rt_atomic_store(&thread->cpus_lock_nest, 0);
#endif

    /* initialize cleanup function and user data 初始化清理函数和用户数据 */
    /* 清理函数置 0 */
    thread->cleanup   = 0;
    /* 用户数据置 0 */
    thread->user_data = 0;

    /* initialize thread timer 初始化线程内置定时器 */
    rt_timer_init(&(thread->thread_timer),
                  thread->parent.name,
                  _thread_timeout,
                  thread,
                  0,
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_THREAD_TIMER);

    /* initialize signal 初始化信号 */
#ifdef RT_USING_SIGNALS
    /* 信号掩码清 0 */
    thread->sig_mask    = 0x00;
    /* 待处理信号清 0 */
    thread->sig_pending = 0x00;

/* 如果不是 SMP 架构 */
#ifndef RT_USING_SMP
    /* 信号返回指针置空 */
    thread->sig_ret     = RT_NULL;
#endif /* RT_USING_SMP */
    /* 信号向量表置空 */
    thread->sig_vectors = RT_NULL;
    /* 信号信息链表置空 */
    thread->si_list     = RT_NULL;
#endif /* RT_USING_SIGNALS */

/* 如果开启了智能进程 */
#ifdef RT_USING_SMART
    /* 线程 ID 引用计数清 0 */
    thread->tid_ref_count = 0;
    /* 所属进程指针置空 */
    thread->lwp = RT_NULL;
    /* 资源回收器指针置空 */
    thread->susp_recycler = RT_NULL;
    /* 健壮互斥量链表指针置空 */
    thread->robust_list = RT_NULL;
    /* 初始化兄弟链表（进程内的线程链表） */
    rt_list_init(&(thread->sibling));

    /* lwp thread-signal init 进程线程信号初始化 */
    /* 信号掩码清零 */
    rt_memset(&thread->signal.sigset_mask, 0, sizeof(lwp_sigset_t));
    /* 信号挂起集清零 */
    rt_memset(&thread->signal.sig_queue.sigset_pending, 0, sizeof(lwp_sigset_t));
    /* 信号信息链表初始化 */
    rt_list_init(&thread->signal.sig_queue.siginfo_list);

    /* 用户上下文清零 */
    rt_memset(&thread->user_ctx, 0, sizeof thread->user_ctx);

    /* initialize user_time and system_time 初始化用户态和内核态运行时间 */
    thread->user_time = 0;
    thread->system_time = 0;
#endif

/* 如果开启了 CPU 使用率追踪 */
#ifdef RT_USING_CPU_USAGE_TRACER
    /* 用户时间清 0 */
    thread->user_time = 0;
    /* 系统时间清 0 */
    thread->system_time = 0;
    /* 上次总时间清 0 */
    thread->total_time_prev = 0;
    /* CPU 使用率清 0 */
    thread->cpu_usage = 0;
#endif /* RT_USING_CPU_USAGE_TRACER */

/* 如果开启了 POSIX 线程 */
#ifdef RT_USING_PTHREADS
    /* pthread 私有数据置空 */
    thread->pthread_data = RT_NULL;
#endif /* RT_USING_PTHREADS */

/* 如果开启了动态模块 */
#ifdef RT_USING_MODULE
    /* 线程所属模块 ID 置 0 */
    thread->parent.module_id = 0;
#endif /* RT_USING_MODULE */

    /* 初始化线程的自旋锁 */
    rt_spin_lock_init(&thread->spinlock);

    /* 调用线程初始化完成的钩子函数列表 */
    RT_OBJECT_HOOKLIST_CALL(rt_thread_inited, (thread));

    /* 返回成功 */
    return RT_EOK;
}

/**
 * @addtogroup group_thread_management
 * 线程管理分组
 * @{
 */

/**
 * @brief   This function will initialize a thread. It's used to initialize a
 *          static thread object.
 * 此函数将初始化一个线程，用于初始化静态线程对象。
 *
 * @param   thread Thread handle. Thread handle is provided by the user and
 *                 points to the corresponding thread control block memory address.
 * 参数 thread 是线程句柄，由用户提供，指向对应的线程控制块内存地址。
 *
 * @param   name Name of the thread (shall be unique); the maximum length of the
 *               thread name is specified by the `RT_NAME_MAX` macro defined in
 *               `rtconfig.h`, and the extra part is automatically truncated.
 * 参数 name 是线程名称（应唯一），最大长度由 RT_NAME_MAX 决定，超出部分自动截断。
 *
 * @param   entry Entry function of thread.
 * 参数 entry 是线程入口函数。
 *
 * @param   parameter Parameter of thread entry function.
 * 参数 parameter 是线程入口函数的参数。
 *
 * @param   stack_start Start address of thread stack.
 * 参数 stack_start 是线程栈的起始地址。
 *
 * @param   stack_size Size of thread stack in bytes. Stack space address
 *                     alignment is required in most systems (for example,
 *                     alignment to 4-byte addresses in the ARM architecture).
 * 参数 stack_size 是线程栈大小（字节）。大多数系统需要栈地址对齐。
 *
 * @param   priority Priority of thread. The priority range is based on the
 *                   system configuration (macro definition `RT_THREAD_PRIORITY_MAX`
 *                   in `rtconfig.h`). If 256 levels of priority are supported,
 *                   the range is from 0 to 255. The smaller the value, the
 *                   higher the priority, and 0 is the highest priority.
 * 参数 priority 是线程优先级。数值越小优先级越高，0 为最高优先级。
 *
 * @param   tick Time slice if there are same priority thread. The unit of the
 *               time slice (tick) is the tick of the operating system. When
 *               there are threads with the same priority in the system, this
 *               parameter specifies the maximum length of time of a thread for
 *               one schedule. At the end of this time slice run, the scheduler
 *               automatically selects the next ready state of the same priority
 *               thread to run.
 * 参数 tick 是时间片。相同优先级线程轮流运行的时间长度。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。RT_EOK 表示成功，其他表示失败。
 */
rt_err_t rt_thread_init(struct rt_thread *thread,
                        const char       *name,
                        void (*entry)(void *parameter),
                        void             *parameter,
                        void             *stack_start,
                        rt_uint32_t       stack_size,
                        rt_uint8_t        priority,
                        rt_uint32_t       tick)
{
    /* parameter check 参数检查 */
    /* 线程句柄不能为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 栈起始地址不能为空 */
    RT_ASSERT(stack_start != RT_NULL);
    /* 时间片不能为 0 */
    RT_ASSERT(tick != 0);

    /* clean memory data of thread 清空线程控制块的内存数据 */
    rt_memset(thread, 0x0, sizeof(struct rt_thread));

    /* initialize thread object 初始化内核对象，将其加入对象容器 */
    rt_object_init((rt_object_t)thread, RT_Object_Class_Thread, name);

    /* 调用内部初始化函数完成核心属性初始化 */
    return _thread_init(thread,
                        name,
                        entry,
                        parameter,
                        stack_start,
                        stack_size,
                        priority,
                        tick);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_init);

/**
 * @brief   This function will return self thread object.
 * 此函数返回当前运行的线程对象。
 *
 * @return  The self thread object. If returns `RT_NULL`, it means that the
 *          scheduler has not started yet.
 * 返回当前线程对象。如果返回 RT_NULL，说明调度器还未启动。
 */
rt_thread_t rt_thread_self(void)
{
/* 如果是单核系统 */
#ifndef RT_USING_SMP
    /* 直接返回当前 CPU 的当前线程指针 */
    return rt_cpu_self()->current_thread;

/* 如果使用硬件获取当前线程机制 */
#elif defined (ARCH_USING_HW_THREAD_SELF)
    /* 调用硬件接口获取当前线程 */
    return rt_hw_thread_self();

/* 如果是多核且使用软件获取机制 */
#else /* !ARCH_USING_HW_THREAD_SELF */
    /* 定义线程指针 */
    rt_thread_t self;
    /* 定义中断锁变量 */
    rt_base_t lock;

    /* 关闭本地 CPU 中断，防止在读取期间被切换 */
    lock = rt_hw_local_irq_disable();
    /* 获取当前 CPU 的当前线程指针 */
    self = rt_cpu_self()->current_thread;
    /* 恢复本地 CPU 中断 */
    rt_hw_local_irq_enable(lock);

    /* 返回线程指针 */
    return self;
#endif /* ARCH_USING_HW_THREAD_SELF */
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_self);

/**
 * @brief   This function will start a thread and put it to system ready queue.
 * 此函数启动一个线程并将其放入系统就绪队列。
 *
 * @param   thread Handle of the thread to be started.
 * 参数 thread 是待启动的线程句柄。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_startup(rt_thread_t thread)
{
    /* parameter check 参数检查 */
    /* 线程句柄不能为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 线程状态必须为初始化状态 (RT_THREAD_INIT) */
    RT_ASSERT((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_INIT);
    /* 对象类型必须为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 打印调试信息：启动线程名和优先级 */
    LOG_D("startup a thread:%s with priority:%d",
          thread->parent.name, RT_SCHED_PRIV(thread).current_priority);

    /* calculate priority attribute and reset thread stat to suspend 
     * 计算优先级属性并重置线程状态为挂起状态 
     */
    rt_sched_thread_startup(thread);

    /* resume and do a schedule if scheduler is available 恢复线程，如果调度器可用则执行调度 */
    rt_thread_resume(thread);

    /* 返回成功 */
    return RT_EOK;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_startup);

/**
 * @brief   This function will close a thread. The thread object will be removed from
 *          thread queue and detached/deleted from the system object management.
 *          It's different from rt_thread_delete or rt_thread_detach that this will not enqueue
 *          the closing thread to cleanup queue.
 * 此函数关闭线程。线程将从队列移除并从对象管理器脱离/删除。
 * 与 delete/detach 不同，它不会将线程加入僵尸清理队列。
 *
 * @param   thread is the thread to be closed.
 * 参数 thread 是待关闭的线程。
 *
 * @return  Return the operation status. If the return value is RT_EOK, the function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_close(rt_thread_t thread)
{
    /* 定义调度器锁级别 */
    rt_sched_lock_level_t slvl;
    /* 定义线程状态变量 */
    rt_uint8_t thread_status;

    /* forbid scheduling on current core if closing current thread 
     * 如果关闭的是当前线程，禁止在当前核心上调度 
     */
    RT_ASSERT(thread != rt_thread_self() || rt_critical_level());

    /* before checking status of scheduler 在检查调度器状态前获取锁 */
    rt_sched_lock(&slvl);

    /* check if thread is already closed 检查线程是否已经被关闭 */
    thread_status = rt_sched_thread_get_stat(thread);
    /* 如果线程未关闭 */
    if (thread_status != RT_THREAD_CLOSE)
    {
        /* 如果线程不是初始化状态 */
        if (thread_status != RT_THREAD_INIT)
        {
            /* remove from schedule 从调度器就绪/挂起队列中移除 */
            rt_sched_remove_thread(thread);
        }

        /* release thread timer 脱离线程内置定时器 */
        rt_timer_detach(&(thread->thread_timer));

        /* change stat 改变线程状态为关闭态 */
        rt_sched_thread_close(thread);
    }

    /* scheduler works are done 调度器相关工作完成，释放锁 */
    rt_sched_unlock(slvl);

    /* 返回成功 */
    return RT_EOK;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_close);

/* 声明内部脱离函数 */
static rt_err_t _thread_detach(rt_thread_t thread);

/**
 * @brief   This function will detach a thread. The thread object will be removed from
 *          thread queue and detached/deleted from the system object management.
 * 此函数脱离一个静态线程。线程将从队列移除并从对象管理器脱离。
 *
 * @param   thread Handle of the thread to be deleted. The thread must be
 *                 initialized by `rt_thread_init()`.
 * 参数 thread 是待脱离的线程句柄，必须是由 rt_thread_init 初始化的。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_detach(rt_thread_t thread)
{
    /* parameter check 参数检查 */
    /* 线程句柄不能为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 对象类型必须为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);
    /* 必须是静态对象（系统对象） */
    RT_ASSERT(rt_object_is_systemobject((rt_object_t)thread));

    /* 调用内部脱离函数 */
    return _thread_detach(thread);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_detach);

/**
 * @brief 线程脱离底层实现
 */
static rt_err_t _thread_detach(rt_thread_t thread)
{
    /* 定义错误码 */
    rt_err_t error;
    /* 定义临界区级别 */
    rt_base_t critical_level;

    /**
     * forbid scheduling on current core before returning since current thread
     * may be detached from scheduler.
     * 返回前禁止在当前核心调度，因为当前线程可能从调度器脱离了。
     */
    /* 进入临界区 */
    critical_level = rt_enter_critical();

    /* 关闭线程 */
    error = rt_thread_close(thread);

    /* 将线程从其关联的互斥量中脱离 */
    _thread_detach_from_mutex(thread);

    /* insert to defunct thread list 插入到僵尸（待回收）线程队列 */
    rt_thread_defunct_enqueue(thread);

    /* 安全退出临界区 */
    rt_exit_critical_safe(critical_level);
    /* 返回错误码 */
    return error;
}

/* 如果开启了堆内存管理（动态分配） */
#ifdef RT_USING_HEAP
/**
 * @brief   This function will create a thread object and allocate thread object memory.
 *          and stack.
 * 此函数创建一个线程对象，并分配线程控制块和栈的内存。
 *
 * @param   name The name of the thread (shall be unique.); the maximum length of
 *               the thread name is specified by macro `RT_NAME_MAX` in `rtconfig.h`,
 *               and the extra part is automatically truncated.
 * 参数 name 是线程名称。
 *
 * @param   entry Entry function of thread.
 * 参数 entry 是线程入口函数。
 *
 * @param   parameter Parameter of thread entry function.
 * 参数 parameter 是入口函数参数。
 *
 * @param   stack_size Size of thread stack in bytes.
 * 参数 stack_size 是栈大小（字节）。
 *
 * @param   priority Priority of thread. The priority range is based on the
 *                   system configuration (macro definition `RT_THREAD_PRIORITY_MAX`
 *                   in rtconfig.h). If 256-level priority is supported, then
 *                   the range is from 0 to 255. The smaller the value, the
 *                   higher the priority, and 0 is the highest priority.
 * 参数 priority 是优先级。
 *
 * @param   tick Time slice if there are same priority thread. The unit of the
 *               time slice (tick) is the tick of the operating system. When
 *               there are threads with the same priority in the system, this
 *               parameter specifies the maximum length of time of a thread for
 *               one schedule. At the end of this time slice run, the scheduler
 *               automatically selects the next ready state of the same priority
 *               thread to run.
 * 参数 tick 是时间片。
 *
 * @return  If the return value is a `rt_thread` structure pointer, the function is successfully executed.
 *          If the return value is `RT_NULL`, it means this operation failed.
 * 成功返回线程指针，失败返回 RT_NULL。
 */
rt_thread_t rt_thread_create(const char *name,
                             void (*entry)(void *parameter),
                             void       *parameter,
                             rt_uint32_t stack_size,
                             rt_uint8_t  priority,
                             rt_uint32_t tick)
{
    /* parameter check 参数检查，时间片不能为 0 */
    RT_ASSERT(tick != 0);

    /* 定义线程指针 */
    struct rt_thread *thread;
    /* 定义栈起始地址指针 */
    void *stack_start;

    /* 动态分配线程对象内存 */
    thread = (struct rt_thread *)rt_object_allocate(RT_Object_Class_Thread,
                                                    name);
    /* 如果分配失败 */
    if (thread == RT_NULL)
        /* 返回空指针 */
        return RT_NULL;

    /* 动态分配线程栈内存 */
    stack_start = (void *)RT_KERNEL_MALLOC(stack_size);
    /* 如果栈分配失败 */
    if (stack_start == RT_NULL)
    {
        /* allocate stack failure 需要释放之前分配的线程对象内存 */
        rt_object_delete((rt_object_t)thread);

        /* 返回空指针 */
        return RT_NULL;
    }

    /* 调用内部初始化函数完成核心属性初始化 */
    _thread_init(thread,
                 name,
                 entry,
                 parameter,
                 stack_start,
                 stack_size,
                 priority,
                 tick);

    /* 返回线程指针 */
    return thread;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_create);

/**
 * @brief   This function will delete a thread. The thread object will be removed from
 *          thread queue and deleted from system object management in the idle thread.
 * 此函数删除动态线程。线程将从队列移除，并在空闲线程中释放内存。
 *
 * @param   thread Handle of the thread to be deleted.
 * 参数 thread 是待删除的线程句柄。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_delete(rt_thread_t thread)
{
    /* parameter check 参数检查 */
    /* 线程句柄不能为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 对象类型必须为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);
    /* 必须不是静态对象（即动态创建的对象） */
    RT_ASSERT(rt_object_is_systemobject((rt_object_t)thread) == RT_FALSE);

    /* 调用内部脱离函数（加入僵尸队列等待空闲线程回收内存） */
    return _thread_detach(thread);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief   This function will let current thread yield processor, and scheduler will
 *          choose the highest thread to run. After yield processor, the current thread
 *          is still in READY state.
 * 此函数让当前线程让出处理器，调度器将选择最高优先级线程运行。让出后当前线程仍为就绪态。
 *
 * @return  Return the operation status. If the return value is RT_EOK, the function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_yield(void)
{
    /* 定义调度器锁级别 */
    rt_sched_lock_level_t slvl;
    /* 获取调度器锁 */
    rt_sched_lock(&slvl);

    /* 将当前线程移到同优先级就绪队列的末尾 */
    rt_sched_thread_yield(rt_thread_self());

    /* 解锁调度器并执行重新调度 */
    rt_sched_unlock_n_resched(slvl);

    /* 返回成功 */
    return RT_EOK;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_yield);

/**
 * @brief   This function will let current thread sleep for some ticks. Change current thread state to suspend,
 *          when the thread timer reaches the tick value, scheduler will awaken this thread.
 * 此函数让当前线程休眠指定 tick 数。将线程设为挂起态，定时器到达后唤醒。
 *
 * @param   tick is the sleep ticks.
 * 参数 tick 是休眠的 tick 数。
 *
 * @return  Return the operation status. If the return value is RT_EOK, the function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
static rt_err_t _thread_sleep(rt_tick_t tick)
{
    /* 定义线程指针 */
    struct rt_thread *thread;
    /* 定义临界区级别 */
    rt_base_t critical_level;
    /* 定义错误码 */
    int err;

    /* 如果 tick 为 0，无效参数 */
    if (tick == 0)
    {
        /* 返回无效参数错误 */
        return -RT_EINVAL;
    }

    /* set to current thread 获取当前线程 */
    thread = rt_thread_self();
    /* 断言线程不为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 断言对象类型为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* current context checking 当前上下文检查：确保调度器可用 */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE);

    /* reset thread error 重置线程错误码为 OK */
    thread->error = RT_EOK;

    /* lock scheduler since current thread may be suspended 锁定调度器，因为当前线程可能会被挂起 */
    critical_level = rt_enter_critical();

    /* suspend thread 挂起线程，设置为可中断挂起状态 (RT_INTERRUPTIBLE) */
    err = rt_thread_suspend_with_flag(thread, RT_INTERRUPTIBLE);

    /* reset the timeout of thread timer and start it 如果挂起成功 */
    if (err == RT_EOK)
    {
        /* 设置线程定时器的超时时间 */
        rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &tick);
        /* 启动线程定时器 */
        rt_timer_start(&(thread->thread_timer));

        /* 设置错误码为被中断（如果在休眠期间被信号等唤醒） */
        thread->error = -RT_EINTR;

        /* notify a pending rescheduling 通知请求重新调度 */
        rt_schedule();

        /* exit critical and do a rescheduling 退出临界区并执行调度（此处唤醒后才会继续往下执行） */
        rt_exit_critical_safe(critical_level);

        /* clear error number of this thread to RT_EOK 如果是因为超时唤醒的，将错误码清为 OK */
        if (thread->error == -RT_ETIMEOUT)
            thread->error = RT_EOK;
    }
    /* 如果挂起失败 */
    else
    {
        /* 退出临界区 */
        rt_exit_critical_safe(critical_level);
    }

    /* 返回错误码 */
    return err;
}

/**
 * @brief   This function will let current thread delay for some ticks.
 * 此函数让当前线程延时指定 tick 数。
 *
 * @param   tick The delay ticks, in units of 1 OS Tick.
 * 参数 tick 是延时的 tick 数。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_delay(rt_tick_t tick)
{
    /* 直接调用内部休眠函数 */
    return _thread_sleep(tick);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_delay);

/**
 * @brief   This function will let current thread delay until (*tick + inc_tick).
 * 此函数让当前线程延时到绝对时间点 (*tick + inc_tick)。
 *
 * @param   tick is the tick of last wakeup.
 * 参数 tick 是上次唤醒时的 tick 指针。
 *
 * @param   inc_tick is the increment tick.
 * 参数 inc_tick 是增量 tick。
 *
 * @return  Return the operation status. If the return value is RT_EOK, the function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_delay_until(rt_tick_t *tick, rt_tick_t inc_tick)
{
    /* 定义线程指针 */
    struct rt_thread *thread;
    /* 定义当前 tick 变量 */
    rt_tick_t cur_tick;
    /* 定义临界区级别 */
    rt_base_t critical_level;

    /* 断言 tick 指针不为空 */
    RT_ASSERT(tick != RT_NULL);

    /* set to current thread 获取当前线程 */
    thread = rt_thread_self();
    /* 断言线程不为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 断言对象类型为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* reset thread error 重置线程错误码 */
    thread->error = RT_EOK;

    /* disable interrupt 进入临界区 */
    critical_level = rt_enter_critical();

    /* 获取当前系统 tick */
    cur_tick = rt_tick_get();
    /* 如果当前 tick 距离上次唤醒的时间差小于增量 tick（还没到目标时间） */
    if (cur_tick - *tick < inc_tick)
    {
        /* 计算剩余需要休眠的 tick */
        rt_tick_t left_tick;

        /* 计算下一次唤醒的绝对 tick */
        *tick += inc_tick;
        /* 计算还需要休眠的 tick 数 */
        left_tick = *tick - cur_tick;

        /* suspend thread 挂起线程，设置为不可中断挂起状态 (RT_UNINTERRUPTIBLE) */
        rt_thread_suspend_with_flag(thread, RT_UNINTERRUPTIBLE);

        /* reset the timeout of thread timer and start it 设置定时器并启动 */
        rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &left_tick);
        rt_timer_start(&(thread->thread_timer));

        /* 退出临界区 */
        rt_exit_critical_safe(critical_level);

        /* 触发调度 */
        rt_schedule();

        /* clear error number of this thread to RT_EOK 如果是超时唤醒，清除错误码 */
        if (thread->error == -RT_ETIMEOUT)
        {
            thread->error = RT_EOK;
        }
    }
    /* 如果已经超过了目标时间 */
    else
    {
        /* 更新上次唤醒时间戳为当前时间 */
        *tick = cur_tick;
        /* 退出临界区 */
        rt_exit_critical_safe(critical_level);
    }

    /* 返回线程错误码 */
    return thread->error;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_delay_until);

/**
 * @brief   This function will let current thread delay for some milliseconds.
 * 此函数让当前线程延时指定的毫秒数。
 *
 * @param   ms The delay time in units of 1ms.
 * 参数 ms 是延时的毫秒数。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_mdelay(rt_int32_t ms)
{
    /* 定义 tick 变量 */
    rt_tick_t tick;

    /* 将毫秒数转换为 tick 数 */
    tick = rt_tick_from_millisecond(ms);

    /* 调用内部休眠函数 */
    return _thread_sleep(tick);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_mdelay);

/* 如果开启了多核 SMP，此处为空占位 */
#ifdef RT_USING_SMP
#endif

/**
 * @brief   This function will control thread behaviors according to control command.
 * 此函数根据控制命令控制线程行为。
 *
 * @param   thread Handle of the thread to be controlled.
 * 参数 thread 是待控制的线程句柄。
 *
 * @param   cmd Control command, which includes.
 * 参数 cmd 是控制命令，包括：
 *              - `RT_THREAD_CTRL_CHANGE_PRIORITY` for changing priority level of thread. 改变优先级
 *              - `RT_THREAD_CTRL_STARTUP` for starting a thread, equivalent to
 *                the `rt_thread_startup()` function call. 启动线程
 *              - `RT_THREAD_CTRL_CLOSE` for closing a thread, equivalent to the
 *                `rt_thread_delete()` function call. 关闭线程
 *              - `RT_THREAD_CTRL_BIND_CPU` for bind the thread to a CPU. 绑定 CPU
 *              - `RT_THREAD_CTRL_RESET_PRIORITY` for reset priority level of thread. 重置优先级
 *
 * @param   arg Argument of control command.
 * 参数 arg 是控制命令的参数。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_control(rt_thread_t thread, int cmd, void *arg)
{
    /* parameter check 参数检查 */
    /* 线程句柄不能为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 对象类型必须为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 根据命令分支 */
    switch (cmd)
    {
        /* 改变优先级 */
        case RT_THREAD_CTRL_CHANGE_PRIORITY:
        {
            /* 定义错误码 */
            rt_err_t error;
            /* 定义调度锁级别 */
            rt_sched_lock_level_t slvl;
            /* 获取调度锁 */
            rt_sched_lock(&slvl);
            /* 调用调度器内部函数改变优先级 */
            error = rt_sched_thread_change_priority(thread, *(rt_uint8_t *)arg);
            /* 释放调度锁 */
            rt_sched_unlock(slvl);
            /* 返回结果 */
            return error;
        }

        /* 重置优先级 */
        case RT_THREAD_CTRL_RESET_PRIORITY:
        {
            /* 定义错误码 */
            rt_err_t error;
            /* 定义调度锁级别 */
            rt_sched_lock_level_t slvl;
            /* 获取调度锁 */
            rt_sched_lock(&slvl);
            /* 调用调度器内部函数重置优先级 */
            error = rt_sched_thread_reset_priority(thread, *(rt_uint8_t *)arg);
            /* 释放调度锁 */
            rt_sched_unlock(slvl);
            /* 返回结果 */
            return error;
        }

        /* 启动线程 */
        case RT_THREAD_CTRL_STARTUP:
        {
            /* 直接调用启动函数 */
            return rt_thread_startup(thread);
        }

        /* 关闭线程 */
        case RT_THREAD_CTRL_CLOSE:
        {
            /* 定义返回错误码 */
            rt_err_t rt_err = -RT_EINVAL;

            /* 如果是静态线程对象 */
            if (rt_object_is_systemobject((rt_object_t)thread) == RT_TRUE)
            {
                /* 调用脱离函数 */
                rt_err = rt_thread_detach(thread);
            }
    /* 如果支持动态内存 */
    #ifdef RT_USING_HEAP
            /* 如果是动态线程对象 */
            else
            {
                /* 调用删除函数 */
                rt_err = rt_thread_delete(thread);
            }
    #endif /* RT_USING_HEAP */
            /* 触发调度，因为关闭线程后可能需要切换 */
            rt_schedule();
            /* 返回结果 */
            return rt_err;
        }

        /* 绑定 CPU */
        case RT_THREAD_CTRL_BIND_CPU:
        {
            /* 定义 CPU 编号变量 */
            rt_uint8_t cpu;

            /* 将参数转为 CPU 编号 */
            cpu = (rt_uint8_t)(rt_size_t)arg;
            /* 调用调度器内部函数绑定 CPU */
            return rt_sched_thread_bind_cpu(thread, cpu);
        }

    /* 默认分支 */
    default:
        /* 跳出 switch */
        break;
    }

    /* 返回成功 */
    return RT_EOK;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_control);

/* 如果开启了智能进程 */
#ifdef RT_USING_SMART
/* 包含 LWP 信号头文件 */
#include <lwp_signal.h>
#endif

/* Convert suspend_flag to corresponding thread suspend state value 
 * 将挂起标志转换为对应的线程挂起状态值 
 */
static rt_uint8_t _thread_get_suspend_state(int suspend_flag)
{
    /* 根据挂起标志判断 */
    switch (suspend_flag)
    {
    /* 可中断挂起 */
    case RT_INTERRUPTIBLE:
        /* 返回可中断挂起状态 */
        return RT_THREAD_SUSPEND_INTERRUPTIBLE;
    /* 可杀死挂起 */
    case RT_KILLABLE:
        /* 返回可杀死挂起状态 */
        return RT_THREAD_SUSPEND_KILLABLE;
    /* 不可中断挂起或默认 */
    case RT_UNINTERRUPTIBLE:
    default:
        /* 返回不可中断挂起状态 */
        return RT_THREAD_SUSPEND_UNINTERRUPTIBLE;
    }
}

/**
 * @brief 设置线程的挂起状态
 */
static void _thread_set_suspend_state(struct rt_thread *thread, int suspend_flag)
{
    /* 定义状态变量 */
    rt_uint8_t stat;

    /* 断言线程不为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 获取挂起状态值 */
    stat = _thread_get_suspend_state(suspend_flag);
    /* 保留状态中的非掩码位（如阻塞标志等），更新挂起状态位 */
    RT_SCHED_CTX(thread).stat = stat | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
}

/**
 * @brief   This function will suspend the specified thread and change it to suspend state.
 * 此函数挂起指定线程并将其加入挂起队列。
 *
 * @note    This function ONLY can suspend current thread itself.
 *              rt_thread_suspend(rt_thread_self());
 *          Do not use the rt_thread_suspend to suspend other threads. You have no way of knowing what code a
 *          thread is executing when you suspend it. If you suspend a thread while sharing a resouce with
 *          other threads and occupying this resouce, starvation can occur very easily.
 * 注意：通常只应挂起当前线程自身。不要轻易挂起其他线程，可能导致死锁或资源饥饿。
 *
 * @param   thread the thread to be suspended.
 * 参数 thread 是待挂起的线程。
 * @param   susp_list the list thread enqueued to. RT_NULL if no list.
 * 参数 susp_list 是挂起队列链表头。如果无队列为 RT_NULL。
 * @param   ipc_flags is a flag for the thread object to be suspended. It determines how the thread is suspended.
 *          The flag can be ONE of the following values:
 *              RT_IPC_FLAG_PRIO          The pending threads will queue in order of priority.
 *              RT_IPC_FLAG_FIFO          The pending threads will queue in the first-in-first-out method
 * 参数 ipc_flags 是 IPC 标志（PRIO 或 FIFO），决定在队列中的排列方式。
 * @param   suspend_flag status flag of the thread to be suspended.
 * 参数 suspend_flag 是挂起状态标志（可中断/不可中断/可杀死）。
 *
 * @return  Return the operation status. If the return value is RT_EOK, the function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_suspend_to_list(rt_thread_t thread, rt_list_t *susp_list, int ipc_flags, int suspend_flag)
{
    /* 定义状态变量 */
    rt_base_t stat;
    /* 定义调度器锁级别 */
    rt_sched_lock_level_t slvl;

    /* parameter check 参数检查 */
    /* 线程句柄不能为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 对象类型必须为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);
    /* 不允许挂起空闲线程 */
    RT_ASSERT(!rt_thread_is_idle_thread(thread));

    /* 打印调试日志 */
    LOG_D("thread suspend: %s", thread->parent.name);

    /* 获取调度器锁 */
    rt_sched_lock(&slvl);

    /* 获取线程当前状态 */
    stat = rt_sched_thread_get_stat(thread);
    /* 如果线程已经在挂起态（阻塞态） */
    if (stat & RT_THREAD_SUSPEND_MASK)
    {
        /* 如果线程的定时器已经启动 */
        if (RT_SCHED_CTX(thread).sched_flag_ttmr_set == 1)
        {
            /* The new suspend operation will halt the tick timer. 
             * 新的挂起操作将停止 tick 定时器 
             */
            LOG_D("Thread [%s]'s timer has been halted.\n", thread->parent.name);
            /* 停止线程定时器 */
            rt_sched_thread_timer_stop(thread);
        }
        /* Upgrade suspend state if new state is stricter 
         * 如果新的挂起状态更严格，则升级挂起状态 
         */
        if (stat < _thread_get_suspend_state(suspend_flag))
        {
            /* 更新为更严格的挂起状态 */
            _thread_set_suspend_state(thread, suspend_flag);
        }
        /* 释放调度器锁 */
        rt_sched_unlock(slvl);
        /* Already suspended, just set the status to success. 已经挂起，直接返回成功 */
        return RT_EOK;
    }
    /* 如果线程不是就绪态也不是运行态 */
    else if ((stat != RT_THREAD_READY) && (stat != RT_THREAD_RUNNING))
    {
        /* 打印警告：线程状态异常 */
        LOG_W("thread suspend: thread disorder, 0x%02x", RT_SCHED_CTX(thread).stat);
        /* 释放调度器锁 */
        rt_sched_unlock(slvl);
        /* 返回错误 */
        return -RT_ERROR;
    }

    /* 如果线程正在运行 */
    if (stat == RT_THREAD_RUNNING)
    {
        /* not suspend running status thread on other core 
         * 不能挂起其他核心上正在运行的线程 
         */
        RT_ASSERT(thread == rt_thread_self());
    }

/* 如果开启了智能进程 */
#ifdef RT_USING_SMART
    /* 如果线程属于某个用户进程 */
    if (thread->lwp)
    {
        /* 释放调度器锁，因为接下来可能要处理信号，可能会休眠 */
        rt_sched_unlock(slvl);

        /* check pending signals for thread before suspend 挂起前检查是否有待处理的信号 */
        if (lwp_thread_signal_suspend_check(thread, suspend_flag) == 0)
        {
            /* not to suspend 如果有信号需处理，不挂起，返回被中断错误 */
            return -RT_EINTR;
        }

        /* 重新获取调度器锁 */
        rt_sched_lock(&slvl);
        /* 如果之前线程是就绪态 */
        if (stat == RT_THREAD_READY)
        {
            /* 重新获取线程状态 */
            stat = rt_sched_thread_get_stat(thread);

            /* 如果状态不再是就绪态 */
            if (stat != RT_THREAD_READY)
            {
                /* status updated while we check for signal 在检查信号期间状态更新，返回错误 */
                rt_sched_unlock(slvl);
                return -RT_ERROR;
            }
        }
    }
#endif

    /* change thread stat 改变线程状态，从调度器就绪队列移除 */
    rt_sched_remove_thread(thread);
    /* 设置新的挂起状态 */
    _thread_set_suspend_state(thread, suspend_flag);

    /* 如果提供了挂起队列 */
    if (susp_list)
    {
        /**
         * enqueue thread on the push list before leaving critical region of
         * scheduler, so we won't miss notification of async events.
         * 在离开调度器临界区前将线程入队，这样就不会错过异步事件的通知。
         */
        rt_susp_list_enqueue(susp_list, thread, ipc_flags);
    }

    /* stop thread timer anyway 无论如何停止线程定时器 */
    rt_sched_thread_timer_stop(thread);

    /* 释放调度器锁 */
    rt_sched_unlock(slvl);

    /* 调用线程挂起钩子函数 */
    RT_OBJECT_HOOK_CALL(rt_thread_suspend_hook, (thread));
    /* 返回成功 */
    return RT_EOK;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_suspend_to_list);

/**
 * @brief   This function will suspend the specified thread and change it to suspend state.
 * 此函数挂起指定线程（不加入任何 IPC 挂起队列）。
 *
 * @note    This function ONLY can suspend current thread itself.
 *          Do not use the rt_thread_suspend to suspend other threads. You have no way of knowing what code a
 *          thread is executing when you suspend it. If you suspend a thread while sharing a resouce with
 *          other threads and occupying this resouce, starvation can occur very easily.
 * 注意：通常只挂起自身。
 *
 * @param   thread the thread to be suspended.
 * 参数 thread 是待挂起的线程。
 * @param   suspend_flag status flag of the thread to be suspended.
 * 参数 suspend_flag 是挂起状态标志。
 *
 * @return  Return the operation status. If the return value is RT_EOK, the function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_suspend_with_flag(rt_thread_t thread, int suspend_flag)
{
    /* 调用挂起到队列的函数，队列传入 RT_NULL */
    return rt_thread_suspend_to_list(thread, RT_NULL, 0, suspend_flag);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_suspend_with_flag);

/**
 * @brief   This function will suspend the specified thread and change it to suspend state.
 * 此函数挂起指定线程（默认为不可中断挂起）。
 *
 * @note    This function can suspend both the current thread itself and other threads.
 *          Please use this API with extreme caution when suspending other threads.
 * 注意：此 API 可挂起其他线程，但必须极其谨慎。
 *
 * @warning Suspending other threads arbitrarily can lead to:
 *          - Deadlock situations
 *          - Resource starvation
 *          - System instability
 *          - Unpredictable behavior
 * 警告：随意挂起其他线程可能导致死锁、饥饿、系统不稳定等。
 *
 * @param   thread Handle of the thread to be suspended. Can be the current thread
 *                 (rt_thread_self()) or any other thread.
 * 参数 thread 是待挂起的线程句柄。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_suspend(rt_thread_t thread)
{
    /* 调用带标志的挂起函数，默认为不可中断 (RT_UNINTERRUPTIBLE) */
    return rt_thread_suspend_with_flag(thread, RT_UNINTERRUPTIBLE);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_suspend);

/**
 * @brief   This function will resume a thread and put it to system ready queue.
 * 此函数恢复一个线程并将其放入系统就绪队列。
 *
 * @param   thread Handle of the thread to be resumed.
 * 参数 thread 是待恢复的线程句柄。
 *
 * @return  Return the operation status. If the return value is `RT_EOK`, the
 *          function is successfully executed.
 *          If the return value is any other values, it means this operation failed.
 * 返回操作状态。
 */
rt_err_t rt_thread_resume(rt_thread_t thread)
{
    /* 定义调度器锁级别 */
    rt_sched_lock_level_t slvl;
    /* 定义错误码 */
    rt_err_t error;

    /* parameter check 参数检查 */
    /* 线程句柄不能为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 对象类型必须为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 打印调试日志 */
    LOG_D("thread resume: %s", thread->parent.name);

    /* 获取调度器锁 */
    rt_sched_lock(&slvl);

    /* 将线程设为就绪态（从挂起队列移到就绪队列） */
    error = rt_sched_thread_ready(thread);

    /* 如果设置成功 */
    if (!error)
    {
        /* 解锁调度器并执行重新调度 */
        error = rt_sched_unlock_n_resched(slvl);

        /**
         * RT_ESCHEDLOCKED indicates that the current thread is in a critical section,
         * rather than 'thread' can't be resumed. Therefore, we can ignore this error.
         * RT_ESCHEDLOCKED 表示当前线程处于临界区，而不是 'thread' 不能被恢复。
         * 因此，我们可以忽略这个错误。
         */
        if (error == -RT_ESCHEDLOCKED)
        {
            /* 忽略调度器被锁错误，返回成功 */
            error = RT_EOK;
        }
    }
    /* 如果设置失败 */
    else
    {
        /* 释放调度器锁 */
        rt_sched_unlock(slvl);
    }

    /* 调用线程恢复钩子函数 */
    RT_OBJECT_HOOK_CALL(rt_thread_resume_hook, (thread));

    /* 返回错误码 */
    return error;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_resume);

/* 如果开启了智能进程 */
#ifdef RT_USING_SMART
/**
 * This function will wakeup a thread with customized operation.
 * 此函数使用自定义操作唤醒线程。
 *
 * @param thread the thread to be resumed
 * 参数 thread 是待唤醒的线程
 *
 * @return the operation status, RT_EOK on OK, -RT_ERROR on error
 * 返回操作状态
 */
rt_err_t rt_thread_wakeup(rt_thread_t thread)
{
    /* 定义调度器锁级别 */
    rt_sched_lock_level_t slvl;
    /* 定义返回错误码 */
    rt_err_t ret;
    /* 定义唤醒处理函数指针 */
    rt_wakeup_func_t func = RT_NULL;

    /* 断言线程不为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 断言对象类型为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 获取调度器锁 */
    rt_sched_lock(&slvl);
    /* 取出线程的唤醒处理函数 */
    func = thread->wakeup_handle.func;
    /* 清空线程的唤醒处理函数 */
    thread->wakeup_handle.func = RT_NULL;
    /* 释放调度器锁 */
    rt_sched_unlock(slvl);

    /* 如果存在自定义唤醒函数 */
    if (func)
    {
        /* 调用自定义唤醒函数 */
        ret = func(thread->wakeup_handle.user_data, thread);
    }
    /* 如果没有自定义唤醒函数 */
    else
    {
        /* 默认调用普通的恢复函数 */
        ret = rt_thread_resume(thread);
    }
    /* 返回结果 */
    return ret;
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_wakeup);

/**
 * @brief 设置线程的自定义唤醒回调函数
 */
void rt_thread_wakeup_set(struct rt_thread *thread, rt_wakeup_func_t func, void* user_data)
{
    /* 定义调度器锁级别 */
    rt_sched_lock_level_t slvl;

    /* 断言线程不为空 */
    RT_ASSERT(thread != RT_NULL);
    /* 断言对象类型为线程 */
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 获取调度器锁 */
    rt_sched_lock(&slvl);
    /* 设置唤醒回调函数 */
    thread->wakeup_handle.func = func;
    /* 设置唤醒回调的用户数据 */
    thread->wakeup_handle.user_data = user_data;
    /* 释放调度器锁 */
    rt_sched_unlock(slvl);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_wakeup_set);
#endif

/**
 * @brief   This function will find the specified thread.
 * 此函数查找指定名称的线程。
 *
 * @note    Please don't invoke this function in interrupt status.
 * 注意：请不要在中断中调用此函数。
 *
 * @param   name is the name of thread finding.
 * 参数 name 是待查找的线程名称。
 *
 * @return  If the return value is a rt_thread structure pointer, the function is successfully executed.
 *          If the return value is RT_NULL, it means this operation failed.
 * 返回找到的线程指针，未找到返回 RT_NULL。
 */
rt_thread_t rt_thread_find(char *name)
{
    /* 调用通用对象查找函数，指定类型为线程 */
    return (rt_thread_t)rt_object_find(name, RT_Object_Class_Thread);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_find);

/**
 * @brief   This function will return the name of the specified thread
 * 此函数获取指定线程的名称
 *
 * @note    Please don't invoke this function in interrupt status
 * 注意：请不要在中断中调用此函数
 *
 * @param   thread the thread to retrieve thread name
 * 参数 thread 是待获取名称的线程
 * @param   name buffer to store the thread name string
 * 参数 name 是用于存储名称的缓冲区
 * @param   name_size maximum size of the buffer to store thread name
 * 参数 name_size 是缓冲区最大容量
 *
 * @return  If the return value is RT_EOK, the function is successfully executed
 *          If the return value is -RT_EINVAL, it means this operation failed
 * 返回操作状态
 */
rt_err_t rt_thread_get_name(rt_thread_t thread, char *name, rt_uint8_t name_size)
{
    /* 如果线程句柄为空，返回无效参数错误；否则调用通用对象获取名称函数 */
    return (thread == RT_NULL) ? -RT_EINVAL : rt_object_get_name(&thread->parent, name, name_size);
}
/* 导出函数符号 */
RTM_EXPORT(rt_thread_get_name);

/** @} group_thread_management 线程管理分组结束 */
