/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 * 
 * 版权声明：归 RT-Thread 开发团队所有，时间跨度从 2006 年至 2025 年。
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * 许可证标识：遵循 Apache 2.0 开源许可证。
 *
 * Change Logs:
 * 变更日志：记录了文件历次修改的时间、作者及修改内容摘要。
 * Date           Author       Notes
 * 2006-03-17     Bernard      the first version (首个版本)
 * 2006-04-28     Bernard      fix the scheduler algorthm (修复调度算法)
 * 2006-04-30     Bernard      add SCHEDULER_DEBUG (增加调度器调试)
 * 2006-05-27     Bernard      fix the scheduler algorthm for same priority
 *                             thread schedule (修复同优先级线程调度算法)
 * 2006-06-04     Bernard      rewrite the scheduler algorithm (重写调度算法)
 * 2006-08-03     Bernard      add hook support (增加钩子支持)
 * 2006-09-05     Bernard      add 32 priority level support (增加32级优先级支持)
 * 2006-09-24     Bernard      add rt_system_scheduler_start function (增加系统调度器启动函数)
 * 2009-09-16     Bernard      fix _rt_scheduler_stack_check (修复栈检查函数)
 * 2010-04-11     yi.qiu       add module feature (增加模块特性)
 * 2010-07-13     Bernard      fix the maximal number of rt_scheduler_lock_nest
 *                             issue found by kuronca (修复调度器锁嵌套最大值问题)
 * 2010-12-13     Bernard      add defunct list initialization even if not use heap.
 *                             (即使不使用堆也增加僵尸队列初始化)
 * 2011-05-10     Bernard      clean scheduler debug log. (清理调度器调试日志)
 * 2013-12-21     Grissiom     add rt_critical_level (增加获取临界区嵌套层数接口)
 * 2018-11-22     Jesven       remove the current task from ready queue
 *                             add per cpu ready queue
 *                             add _scheduler_get_highest_priority_thread to find highest priority task
 *                             rt_schedule_insert_thread won't insert current task to ready queue
 *                             in smp version, rt_hw_context_switch_interrupt maybe switch to
 *                             new task directly
 *                             (从就绪队列移除当前任务，增加每CPU就绪队列，增加查找最高优先级函数等SMP前向适配)
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to scheduler.c (将钩子函数移至此文件)
 * 2023-03-27     rose_man     Split into scheduler upc and scheduler_mp.c (拆分为单核和多核文件)
 * 2023-10-17     ChuShicheng  Modify the timing of clearing RT_THREAD_STAT_YIELD flag bits
 *                             (修改清除YIELD标志位的时机)
 * 2025-08-04     Pillar       Add rt_scheduler_critical_switch_flag (增加临界区调度挂起标志)
 * 2025-08-20     RyanCW       rt_scheduler_lock_nest use atomic operations (调度器锁嵌套使用原子操作)
 * 2025-09-20     wdfk_prog    fix scheduling exception caused by interrupt preemption in rt_schedule
 *                             (修复rt_schedule中中断抢占导致的调度异常)
 */

/* 定义宏，表示当前是 IPC/调度器源码层面，影响某些内部接口的可见性 */
#define __RT_IPC_SOURCE__
/* 包含 RT-Thread 系统核心头文件 */
#include <rtthread.h>
/* 包含 RT-Thread 硬件相关定义头文件 */
#include <rthw.h>

/* 定义调试标签，用于日志输出标识 */
#define DBG_TAG           "kernel.scheduler"
/* 定义调试级别为信息级别 */
#define DBG_LVL           DBG_INFO
/* 包含 RT-Thread 调试日志头文件 */
#include <rtdbg.h>

/* 线程优先级就绪表：每个优先级对应一个链表头，存放该优先级的就绪线程 */
rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
/* 优先级就绪位图：用于快速查找哪个优先级有就绪线程（适用于优先级<=32的情况） */
rt_uint32_t rt_thread_ready_priority_group;
/* 如果系统配置的最大优先级大于 32 */
#if RT_THREAD_PRIORITY_MAX > 32
/* Maximum priority level, 256 最大优先级256时的二级位图表 */
rt_uint8_t rt_thread_ready_table[32];
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

/* 调度器锁嵌套计数，使用原子变量，防止多核/中断并发访问导致的问题 */
static rt_atomic_t rt_scheduler_lock_nest;
/* 当前运行线程的优先级 */
rt_uint8_t rt_current_priority;

/* 临界区调度挂起标志：如果在锁调度期间发生了调度请求，置1，解锁后执行调度 */
static rt_int8_t rt_scheduler_critical_switch_flag;
/* 宏：判断是否有挂起的调度请求 */
#define IS_CRITICAL_SWITCH_PEND()  (rt_scheduler_critical_switch_flag == 1)
/* 宏：设置挂起调度请求标志 */
#define SET_CRITICAL_SWITCH_FLAG() (rt_scheduler_critical_switch_flag = 1)
/* 宏：清除挂起调度请求标志 */
#define CLR_CRITICAL_SWITCH_FLAG() (rt_scheduler_critical_switch_flag = 0)

/* 如果开启了钩子函数且使用函数指针方式实现 */
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
/* 定义线程切换钩子（记录从哪个线程切换到哪个线程） */
static void (*rt_scheduler_hook)(struct rt_thread *from, struct rt_thread *to);
/* 定义线程上下文切换钩子（记录切换到哪个线程） */
static void (*rt_scheduler_switch_hook)(struct rt_thread *tid);

/**
 * @addtogroup group_hook
 * 钩子函数分组
 */

/**@{*/

/**
 * @brief This function will set a hook function, which will be invoked when thread
 *        switch happens.
 * 此函数设置一个钩子函数，当发生线程切换时将被调用。
 *
 * @param hook is the hook function.
 * 参数 hook 是钩子函数。
 */
void rt_scheduler_sethook(void (*hook)(struct rt_thread *from, struct rt_thread *to))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_scheduler_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when context
 *        switch happens.
 * 此函数设置一个钩子函数，当发生上下文切换时将被调用。
 *
 * @param hook is the hook function.
 * 参数 hook 是钩子函数。
 */
void rt_scheduler_switch_sethook(void (*hook)(struct rt_thread *tid))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_scheduler_switch_hook = hook;
}

/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_thread_management
 * 线程管理分组
 *
 * @cond
 *
 * @{
 */

/**
 * @brief 从就绪队列中获取最高优先级的线程
 */
static struct rt_thread* _scheduler_get_highest_priority_thread(rt_ubase_t *highest_prio)
{
    /* 定义最高优先级线程指针 */
    struct rt_thread *highest_priority_thread;
    /* 定义最高就绪优先级变量 */
    rt_ubase_t highest_ready_priority;

/* 如果优先级数量大于 32，使用二级位图查找 */
#if RT_THREAD_PRIORITY_MAX > 32
    /* 定义组号变量 */
    rt_ubase_t number;

    /* 通过查找前导零指令(FFS)找到就绪组中的最低位(即最高优先级组) */
    number = __rt_ffs(rt_thread_ready_priority_group) - 1;
    /* 在对应的组中再次使用 FFS 找到具体的最高优先级 */
    highest_ready_priority = (number << 3) + __rt_ffs(rt_thread_ready_table[number]) - 1;
/* 如果优先级数量 <= 32，直接使用一级位图查找 */
#else
    /* 直接在位图中找最低位，得到最高就绪优先级 */
    highest_ready_priority = __rt_ffs(rt_thread_ready_priority_group) - 1;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* get highest ready priority thread 获取最高就绪优先级线程 */
    /* 从对应优先级的链表中取出第一个线程控制块 */
    highest_priority_thread = RT_THREAD_LIST_NODE_ENTRY(rt_thread_priority_table[highest_ready_priority].next);

    /* 通过输出参数返回最高优先级数值 */
    *highest_prio = highest_ready_priority;

    /* 返回最高优先级线程指针 */
    return highest_priority_thread;
}

/**
 * @brief Lock the scheduler and save the interrupt level
 * 锁定调度器并保存中断状态
 *
 * @param plvl Pointer to store the interrupt level before locking
 * 参数 plvl 指向用于保存锁定前中断状态的指针
 *
 * @return rt_err_t
 *   - RT_EOK on success 成功返回 RT_EOK
 *   - -RT_EINVAL if plvl is NULL 如果 plvl 为空返回 -RT_EINVAL
 *
 * @details This function:
 *   - Disables interrupts to prevent preemption
 *   - Saves the previous interrupt level in plvl
 *   - Must be paired with rt_sched_unlock() to restore interrupts
 * 此函数：
 *   - 关闭中断以防止抢占
 *   - 在 plvl 中保存之前的中断状态
 *   - 必须与 rt_sched_unlock() 配对使用以恢复中断
 *
 * @note The lock is implemented by disabling interrupts
 *       Caller must ensure plvl is valid
 * 注意：锁通过禁用中断实现，调用者必须确保 plvl 有效
 */
rt_err_t rt_sched_lock(rt_sched_lock_level_t *plvl)
{
    /* 定义中断状态变量 */
    rt_base_t level;
    /* 如果指针无效 */
    if (!plvl)
        /* 返回无效参数错误 */
        return -RT_EINVAL;

    /* 关闭硬件中断并保存当前中断状态 */
    level = rt_hw_interrupt_disable();
    /* 将中断状态保存到调用者提供的变量中 */
    *plvl = level;

    /* 返回成功 */
    return RT_EOK;
}

/**
 * @brief Unlock the scheduler and restore the interrupt level
 * 解锁调度器并恢复中断状态
 *
 * @param level The interrupt level to restore (previously saved by rt_sched_lock)
 * 参数 level 是要恢复的中断状态（由 rt_sched_lock 保存）
 * @return rt_err_t Always returns RT_EOK 总是返回 RT_EOK
 *
 * @details This function:
 *   - Restores the interrupt level that was saved when locking the scheduler
 *   - Must be called to match each rt_sched_lock() call
 * 此函数：
 *   - 恢复锁定调度器时保存的中断状态
 *   - 必须与每个 rt_sched_lock() 调用匹配调用
 *
 * @note Must be called with the same interrupt level that was saved by rt_sched_lock()
 *       Should not be called without a corresponding rt_sched_lock() first
 * 注意：必须使用 rt_sched_lock 保存的相同中断状态调用
 *       没有先调用 rt_sched_lock() 则不应调用此函数
 */
rt_err_t rt_sched_unlock(rt_sched_lock_level_t level)
{
    /* 恢复硬件中断到传入的状态 */
    rt_hw_interrupt_enable(level);

    /* 返回成功 */
    return RT_EOK;
}

/**
 * @brief Unlock scheduler and trigger a reschedule if needed
 * 解锁调度器并在需要时触发重新调度
 *
 * @param level The interrupt level to restore (previously saved by rt_sched_lock)
 * 参数 level 是要恢复的中断状态
 * @return rt_err_t Always returns RT_EOK 总是返回 RT_EOK
 *
 * @details This function:
 *   - Restores the interrupt level that was saved when locking the scheduler
 *   - Triggers a reschedule if the scheduler is available (rt_thread_self() != NULL)
 *   - Combines the functionality of rt_sched_unlock() and rt_schedule()
 * 此函数：
 *   - 恢复锁定调度器时保存的中断状态
 *   - 如果调度器可用，触发重新调度
 *   - 结合了 rt_sched_unlock() 和 rt_schedule() 的功能
 */
rt_err_t rt_sched_unlock_n_resched(rt_sched_lock_level_t level)
{
    /* 如果当前线程存在（说明调度器已启动） */
    if (rt_thread_self())
    {
        /* if scheduler is available 如果调度器可用，执行调度 */
        rt_schedule();
    }
    /* 恢复中断状态 */
    rt_hw_interrupt_enable(level);

    /* 返回成功 */
    return RT_EOK;
}

/**
 * @brief Initialize the system scheduler for single-core systems
 * 初始化单核系统的调度器
 *
 * @details This function performs the following initialization tasks:
 *   - Resets the scheduler lock nest counter to 0
 *   - Initializes the priority table for all priority levels
 *   - Clears the ready priority group bitmap
 *   - For systems with >32 priority levels, initializes the ready table
 * 此函数执行以下初始化任务：
 *   - 重置调度器锁嵌套计数为0
 *   - 初始化所有优先级的优先级表（链表）
 *   - 清除就绪优先级位图
 *   - 对于 >32 优先级的系统，初始化就绪表
 *
 * @note This function must be called before any thread scheduling can occur.
 *       It prepares the scheduler data structures for single-core operation
 * 注意：在任何线程调度发生之前必须调用此函数。
 *       它为单核操作准备调度器数据结构。
 */
void rt_system_scheduler_init(void)
{
    /* 定义偏移量变量 */
    rt_base_t offset;
    /* 初始化调度器锁嵌套计数为 0 */
    rt_scheduler_lock_nest = 0;

    /* 打印调试信息：启动调度器，最大优先级值 */
    LOG_D("start scheduler: max priority 0x%02x",
          RT_THREAD_PRIORITY_MAX);

    /* 遍历所有优先级 */
    for (offset = 0; offset < RT_THREAD_PRIORITY_MAX; offset ++)
    {
        /* 初始化每个优先级对应的就绪链表 */
        rt_list_init(&rt_thread_priority_table[offset]);
    }

    /* initialize ready priority group 初始化就绪优先级位图为 0 */
    rt_thread_ready_priority_group = 0;

/* 如果优先级大于 32 */
#if RT_THREAD_PRIORITY_MAX > 32
    /* initialize ready table 初始化二级就绪位图表为 0 */
    rt_memset(rt_thread_ready_table, 0, sizeof(rt_thread_ready_table));
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
}

/**
 * @brief Start the system scheduler and switch to the highest priority thread
 * 启动系统调度器并切换到最高优先级线程
 *
 * @details This function:
 *   - Gets the highest priority ready thread using _scheduler_get_highest_priority_thread()
 *   - Sets it as the current thread for the CPU
 *   - Removes the thread from ready queue and sets its status to RUNNING
 *   - Performs a context switch to the selected thread using rt_hw_context_switch_to()
 * 此函数：
 *   - 获取最高优先级就绪线程
 *   - 将其设置为当前 CPU 的当前线程
 *   - 从就绪队列移除该线程并将其状态设为 RUNNING
 *   - 使用 rt_hw_context_switch_to() 执行上下文切换到选定的线程
 *
 * @note This function does not return as it switches to the first thread to run.
 *       Must be called after rt_system_scheduler_init().
 *       The selected thread will begin execution immediately
 * 注意：此函数不会返回，因为它切换到第一个运行的线程。
 *       必须在 rt_system_scheduler_init() 之后调用。
 *       选定的线程将立即开始执行
 */
void rt_system_scheduler_start(void)
{
    /* 定义目标线程指针 */
    struct rt_thread *to_thread;
    /* 定义最高就绪优先级变量 */
    rt_ubase_t highest_ready_priority;

    /* 获取最高优先级就绪线程 */
    to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);

    /* 将该线程设置为当前 CPU 的当前线程 */
    rt_cpu_self()->current_thread = to_thread;

    /* flush critical switch flag 清除临界区调度挂起标志 */
    CLR_CRITICAL_SWITCH_FLAG();

    /* 将目标线程从就绪队列中移除（因为马上要运行它） */
    rt_sched_remove_thread(to_thread);
    /* 设置目标线程的状态为运行态 */
    RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING;

    /* switch to new thread 切换到新线程 */

    /* 调用硬件上下文切换函数，永不返回 */
    rt_hw_context_switch_to((rt_uintptr_t)&to_thread->sp);

    /* never come back 永远不会执行到这里 */
}

/**
 * @brief Perform thread scheduling once. Select the highest priority thread and switch to it.
 * 执行一次线程调度。选择最高优先级线程并切换过去。
 *
 * @details This function:
 *   - Disables interrupts to prevent preemption during scheduling
 *   - Checks if scheduler is enabled (lock_nest == 0)
 *   - Gets the highest priority ready thread
 *   - Determines if current thread should continue running or be preempted
 *   - Performs context switch if needed:
 *     * From current thread to new thread (normal case)
 *     * Handles special cases like interrupt context switches
 *   - Manages thread states (READY/RUNNING) and priority queues
 *   - Handles thread yield flags and signal processing
 * 此函数：
 *   - 关闭中断以防调度期间被抢占
 *   - 检查调度器是否启用 (lock_nest == 0)
 *   - 获取最高优先级就绪线程
 *   - 决定当前线程是继续运行还是被抢占
 *   - 如需则执行上下文切换
 *   - 管理线程状态和优先级队列
 *   - 处理线程让出标志和信号处理
 */
void rt_schedule(void)
{
    /* 定义中断状态变量 */
    rt_base_t level;
    /* 定义目标线程指针 */
    struct rt_thread *to_thread;
    /* 定义源线程指针 */
    struct rt_thread *from_thread;
    /* 定义当前线程指针 */
    struct rt_thread *curr_thread;
    /* 定义中断嵌套层数变量 */
    rt_base_t interrupt_nest;

    /* disable interrupt 关闭中断 */
    level = rt_hw_interrupt_disable();

    /* using local variable to avoid unnecessary function call 
     * 使用局部变量保存当前线程，避免重复函数调用开销 
     */
    curr_thread = rt_thread_self();

    /* check the scheduler is enabled or not 检查调度器是否启用（是否加锁） */
    if (rt_scheduler_lock_nest == 0)
    {
        /* 定义最高就绪优先级变量 */
        rt_ubase_t highest_ready_priority;

        /* 如果就绪优先级位图不为0，说明有就绪线程 */
        if (rt_thread_ready_priority_group != 0)
        {
            /* need_insert_from_thread: need to insert from_thread to ready queue 
             * 标志位：是否需要将当前线程重新插入就绪队列 
             */
            int need_insert_from_thread = 0;

            /* 获取最高优先级的就绪线程 */
            to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);

            /* 如果当前线程状态为运行态 */
            if ((RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_RUNNING)
            {
                /* 如果当前线程优先级高于最高就绪优先级，当前线程继续运行 */
                if (RT_SCHED_PRIV(curr_thread).current_priority < highest_ready_priority)
                {
                    /* 目标线程依然是当前线程 */
                    to_thread = curr_thread;
                }
                /* 如果优先级相同，且当前线程没有执行让出(yield)操作，当前线程继续运行 */
                else if (RT_SCHED_PRIV(curr_thread).current_priority == highest_ready_priority
                         && (RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_YIELD_MASK) == 0)
                {
                    /* 目标线程依然是当前线程 */
                    to_thread = curr_thread;
                }
                /* 否则，当前线程被抢占，需要将其加入就绪队列 */
                else
                {
                    /* 设置需要插入当前线程到就绪队列的标志 */
                    need_insert_from_thread = 1;
                }
            }

            /* 如果选中的目标线程不是当前线程，则需要切换 */
            if (to_thread != curr_thread)
            {
                /* if the destination thread is not the same as current thread 
                 * 如果目标线程与当前线程不同 
                 */
                /* 更新全局当前优先级变量 */
                rt_current_priority = (rt_uint8_t)highest_ready_priority;
                /* 保存当前线程为源线程 */
                from_thread                   = curr_thread;
                /* 更新 CPU 的当前线程为目标线程 */
                rt_cpu_self()->current_thread = to_thread;

                /* 调用调度器钩子函数 */
                RT_OBJECT_HOOK_CALL(rt_scheduler_hook, (from_thread, to_thread));

                /* 如果需要将当前线程重新放入就绪队列 */
                if (need_insert_from_thread)
                {
                    /* 将源线程插入就绪队列 */
                    rt_sched_insert_thread(from_thread);
                }

                /* 如果源线程带有让出标志 */
                if ((RT_SCHED_CTX(from_thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
                {
                    /* 清除让出标志 */
                    RT_SCHED_CTX(from_thread).stat &= ~RT_THREAD_STAT_YIELD_MASK;
                }

                /* 从就绪队列中移除目标线程（因为它要运行了） */
                rt_sched_remove_thread(to_thread);
                /* 设置目标线程状态为运行态，并保留其他非掩码状态位 */
                RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(to_thread).stat & ~RT_THREAD_STAT_MASK);

                /* switch to new thread 切换到新线程 */
                /* 获取当前中断嵌套层数 */
                interrupt_nest = rt_interrupt_get_nest();
                /* 打印调度调试信息 */
                LOG_D("[%d]switch to priority#%d "
                         "thread:%.*s(sp:0x%08x), "
                         "from thread:%.*s(sp: 0x%08x)",
                         interrupt_nest, highest_ready_priority,
                         RT_NAME_MAX, to_thread->parent.name, to_thread->sp,
                         RT_NAME_MAX, from_thread->parent.name, from_thread->sp);

                /* 检查目标线程的栈是否溢出 */
                RT_SCHEDULER_STACK_CHECK(to_thread);

                /* 如果不在中断上下文中 */
                if (interrupt_nest == 0)
                {
                    /* 声明信号处理函数（在C文件内部使用） */
                    extern void rt_thread_handle_sig(rt_bool_t clean_state);

                    /* 调用线程切换钩子函数 */
                    RT_OBJECT_HOOK_CALL(rt_scheduler_switch_hook, (from_thread));

                    /* 执行硬件上下文切换（从 from_thread 切换到 to_thread） */
                    rt_hw_context_switch((rt_uintptr_t)&from_thread->sp,
                            (rt_uintptr_t)&to_thread->sp);

                    /* enable interrupt 恢复中断（切换回来后执行） */
                    rt_hw_interrupt_enable(level);

/* 如果开启了信号功能 */
#ifdef RT_USING_SIGNALS
                    /* check stat of thread for signal 检查线程是否有信号待处理 */
                    level = rt_hw_interrupt_disable();
                    /* 如果当前线程有挂起的信号标志 */
                    if (RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_SIGNAL_PENDING)
                    {
                        /* 声明信号处理函数 */
                        extern void rt_thread_handle_sig(rt_bool_t clean_state);

                        /* 清除信号挂起标志 */
                        RT_SCHED_CTX(curr_thread).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;

                        /* 恢复中断 */
                        rt_hw_interrupt_enable(level);

                        /* check signal status 执行信号处理 */
                        rt_thread_handle_sig(RT_TRUE);
                    }
                    /* 如果没有挂起信号 */
                    else
                    {
                        /* 恢复中断 */
                        rt_hw_interrupt_enable(level);
                    }
#endif /* RT_USING_SIGNALS */
                    /* 跳转到退出 */
                    goto __exit;
                }
                /* 如果在中断上下文中 */
                else
                {
                    /* 打印调试信息：在中断中切换 */
                    LOG_D("switch in interrupt");

                    /* 执行中断中的上下文切换（延迟到中断退出时真正切换） */
                    rt_hw_context_switch_interrupt((rt_uintptr_t)&from_thread->sp,
                            (rt_uintptr_t)&to_thread->sp, from_thread, to_thread);
                }
            }
            /* 如果目标线程就是当前线程（无需切换） */
            else
            {
                /* 从就绪队列移除当前线程 */
                rt_sched_remove_thread(curr_thread);
                /* 重置当前线程状态为 RUNNING */
                RT_SCHED_CTX(curr_thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(curr_thread).stat & ~RT_THREAD_STAT_MASK);
            }
        }
    }
    /* 如果调度器被锁 */
    else
    {
        /* 设置临界区挂起调度标志，等解锁后再调度 */
        SET_CRITICAL_SWITCH_FLAG();
    }

    /* enable interrupt 恢复中断状态 */
    rt_hw_interrupt_enable(level);

/* 退出标签 */
__exit:
    /* 返回 */
    return;
}

/**
 * @brief Initialize thread scheduling attributes for startup
 * 初始化线程启动时的调度属性
 *
 * @param thread The thread to be initialized
 * 参数 thread 是待初始化的线程
 *
 * @details This function:
 *   - For systems with >32 priority levels:
 *     * Sets the thread's priority group number (5 bits)
 *     * Creates number mask for the priority group
 *     * Creates high mask for the specific priority (3 bits)
 *   - For systems with <=32 priority levels:
 *     * Creates a simple number mask for the priority
 *   - Sets thread state to SUSPEND to prepare for later activation
 * 此函数：
 *   - 对于 >32 优先级系统：计算组号、组掩码、组内偏移掩码
 *   - 对于 <=32 优先级系统：计算简单的优先级掩码
 *   - 将线程状态设为挂起态，以便后续激活
 *
 * @note This function must be called before a thread can be scheduled.
 *       It prepares the thread's priority-related data structures.
 *       Normally, there isn't anyone racing with us so this operation is lockless
 * 注意：线程调度前必须调用。准备优先级相关数据结构。
 *       通常没有并发竞争，所以无锁操作。
 */
void rt_sched_thread_startup(struct rt_thread *thread)
{
/* 如果优先级大于 32 */
#if RT_THREAD_PRIORITY_MAX > 32
    /* 计算优先级所在组号 (高5位) */
    RT_SCHED_PRIV(thread).number = RT_SCHED_PRIV(thread).current_priority >> 3;            /* 5bit */
    /* 计算组号对应的位掩码 */
    RT_SCHED_PRIV(thread).number_mask = 1L << RT_SCHED_PRIV(thread).number;
    /* 计算在组内的偏移对应的位掩码 (低3位) */
    RT_SCHED_PRIV(thread).high_mask = 1L << (RT_SCHED_PRIV(thread).current_priority & 0x07);  /* 3bit */
/* 如果优先级 <= 32 */
#else
    /* 计算优先级对应的位掩码 */
    RT_SCHED_PRIV(thread).number_mask = 1L << RT_SCHED_PRIV(thread).current_priority;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* change thread stat, so we can resume it 改变线程状态为挂起态，这样我们才能恢复它 */
    RT_SCHED_CTX(thread).stat = RT_THREAD_SUSPEND;
}

/**
 * @brief Initialize thread's scheduling private data
 * 初始化线程的调度私有数据
 *
 * @param thread Pointer to the thread control block
 * 参数 thread 是线程控制块指针
 * @param tick Initial time slice value for the thread
 * 参数 tick 是线程的初始时间片
 * @param priority Initial priority of the thread
 * 参数 priority 是线程的初始优先级
 *
 * @details This function:
 *   - Initializes the thread's list node
 *   - Sets initial and current priority (must be < RT_THREAD_PRIORITY_MAX)
 *   - Initializes priority masks (number_mask, number, high_mask for >32 priorities)
 *   - Sets initial and remaining time slice ticks
 * 此函数：
 *   - 初始化链表节点
 *   - 设置初始和当前优先级
 *   - 初始化优先级掩码
 *   - 设置初始和剩余时间片
 */
void rt_sched_thread_init_priv(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority)
{
    /* 初始化线程在就绪队列中的链表节点 */
    rt_list_init(&RT_THREAD_LIST_NODE(thread));

    /* priority init 优先级初始化 */
    /* 断言优先级合法 */
    RT_ASSERT(priority < RT_THREAD_PRIORITY_MAX);
    /* 保存初始优先级 */
    RT_SCHED_PRIV(thread).init_priority    = priority;
    /* 设置当前优先级 */
    RT_SCHED_PRIV(thread).current_priority = priority;

    /* don't add to scheduler queue as init thread 初始化时不加入调度队列，掩码设为0 */
    RT_SCHED_PRIV(thread).number_mask = 0;
/* 如果优先级大于 32 */
#if RT_THREAD_PRIORITY_MAX > 32
    /* 组号和组内掩码初始化为 0 */
    RT_SCHED_PRIV(thread).number = 0;
    RT_SCHED_PRIV(thread).high_mask = 0;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* tick init 时间片初始化 */
    /* 保存初始时间片 */
    RT_SCHED_PRIV(thread).init_tick = tick;
    /* 设置剩余时间片 */
    RT_SCHED_PRIV(thread).remaining_tick = tick;
}

/**
 * @brief This function will insert a thread to the system ready queue. The state of
 *        thread will be set as READY and the thread will be removed from suspend queue.
 * 此函数将线程插入系统就绪队列。状态设为就绪，并从挂起队列移除。
 *
 * @param thread is the thread to be inserted.
 * 参数 thread 是待插入的线程。
 *
 * @note  Please do not invoke this function in user application.
 * 注意：请不要在用户应用中调用此函数。
 */
void rt_sched_insert_thread(struct rt_thread *thread)
{
    /* 定义中断状态变量 */
    rt_base_t level;

    /* 断言线程指针不为空 */
    RT_ASSERT(thread != RT_NULL);

    /* disable interrupt 关闭中断 */
    level = rt_hw_interrupt_disable();

    /* it's current thread, it should be RUNNING thread 如果是当前线程，其状态应该是 RUNNING */
    if (thread == rt_current_thread)
    {
        /* 设置状态为运行态 */
        RT_SCHED_CTX(thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
        /* 跳转到退出 */
        goto __exit;
    }

    /* READY thread, insert to ready queue 就绪线程，插入就绪队列 */
    /* 设置线程状态为就绪态 */
    RT_SCHED_CTX(thread).stat = RT_THREAD_READY | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
    /* there is no time slices left(YIELD), inserting thread before ready list 
     * 如果没有剩余时间片(主动让出)，将线程插入就绪链表的前面（同等优先级下晚点调度） 
     */
    if((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
    {
        /* 插入到对应优先级链表的前面 */
        rt_list_insert_before(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                              &RT_THREAD_LIST_NODE(thread));
    }
    /* there are some time slices left, inserting thread after ready list to schedule it firstly at next time
     * 如果还有剩余时间片(被抢占)，将线程插入就绪链表的后面（同等优先级下优先调度）
     */
    else
    {
        /* 插入到对应优先级链表的后面 */
        rt_list_insert_after(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                              &RT_THREAD_LIST_NODE(thread));
    }

    /* 打印调试信息 */
    LOG_D("insert thread[%.*s], the priority: %d",
          RT_NAME_MAX, thread->parent.name, RT_SCHED_PRIV(rt_current_thread).current_priority);

    /* set priority mask 设置优先级位图掩码 */
#if RT_THREAD_PRIORITY_MAX > 32
    /* 在二级位图表中置位对应位 */
    rt_thread_ready_table[RT_SCHED_PRIV(thread).number] |= RT_SCHED_PRIV(thread).high_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    /* 在一级位图表中置位对应位 */
    rt_thread_ready_priority_group |= RT_SCHED_PRIV(thread).number_mask;

/* 退出标签 */
__exit:
    /* enable interrupt 恢复中断 */
    rt_hw_interrupt_enable(level);
}

/**
 * @brief This function will remove a thread from system ready queue.
 * 此函数将线程从系统就绪队列中移除。
 *
 * @param thread is the thread to be removed.
 * 参数 thread 是待移除的线程。
 *
 * @note  Please do not invoke this function in user application.
 * 注意：请不要在用户应用中调用此函数。
 */
void rt_sched_remove_thread(struct rt_thread *thread)
{
    /* 定义中断状态变量 */
    rt_base_t level;

    /* 断言线程指针不为空 */
    RT_ASSERT(thread != RT_NULL);

    /* disable interrupt 关闭中断 */
    level = rt_hw_interrupt_disable();

    /* 打印调试信息 */
    LOG_D("remove thread[%.*s], the priority: %d",
          RT_NAME_MAX, thread->parent.name,
          RT_SCHED_PRIV(rt_current_thread).current_priority);

    /* remove thread from ready list 将线程从就绪链表中移除 */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));
    /* 如果该优先级对应的链表已经为空 */
    if (rt_list_isempty(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority])))
    {
/* 如果优先级大于 32 */
#if RT_THREAD_PRIORITY_MAX > 32
        /* 清除二级位图表中对应的位 */
        rt_thread_ready_table[RT_SCHED_PRIV(thread).number] &= ~RT_SCHED_PRIV(thread).high_mask;
        /* 如果该组中所有优先级都没有就绪线程了 */
        if (rt_thread_ready_table[RT_SCHED_PRIV(thread).number] == 0)
        {
            /* 清除一级位图表中对应的组位 */
            rt_thread_ready_priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
        }
/* 如果优先级 <= 32 */
#else
        /* 直接清除一级位图表中对应的位 */
        rt_thread_ready_priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    }

    /* enable interrupt 恢复中断 */
    rt_hw_interrupt_enable(level);
}

/* 如果开启了临界区调试功能 */
#ifdef RT_DEBUGING_CRITICAL

/* 定义临界区错误发生标志 */
static volatile int _critical_error_occurred = 0;

/**
 * @brief Safely exit critical section with level checking
 * 安全退出临界区并带级别检查
 *
 * @param critical_level The expected critical level to match current lock nest
 * 参数 critical_level 是期望匹配当前锁嵌套的临界区级别
 *
 * @details This function:
 *   - Disables interrupts to prevent preemption during check
 *   - Verifies the provided critical_level matches current rt_scheduler_lock_nest
 *   - If mismatch detected (debug mode only):
 *     * Sets error flag
 *     * Prints debug information including backtrace
 *     * Enters infinite loop to halt system
 *   - Always calls rt_exit_critical() to perform actual exit
 * 此函数：
 *   - 关闭中断以防检查期间被抢占
 *   - 验证提供的 critical_level 是否与当前 rt_scheduler_lock_nest 匹配
 *   - 如果不匹配：
 *     * 设置错误标志
 *     * 打印调试信息和回溯栈
 *     * 进入死循环停机
 *   - 总是调用 rt_exit_critical() 执行实际退出
 *
 * @note This is a debug version that adds safety checks for critical section exit.
 * 注意：这是增加安全检查的调试版本。
 */
void rt_exit_critical_safe(rt_base_t critical_level)
{
    /* 定义中断状态变量 */
    rt_base_t level;
    /* disable interrupt 关闭中断 */
    level = rt_hw_interrupt_disable();

    /* 如果之前没发生过错误 */
    if (!_critical_error_occurred)
    {
        /* 如果传入的级别与当前锁嵌套数不匹配 */
        if (critical_level != rt_scheduler_lock_nest)
        {
            /* 定义死循环变量 */
            int dummy = 1;
            /* 标记错误已发生 */
            _critical_error_occurred = 1;

            /* 打印错误信息：不兼容的临界区级别 */
            rt_kprintf("%s: un-compatible critical level\n" \
                       "\tCurrent %d\n\tCaller %d\n",
                       __func__, rt_scheduler_lock_nest,
                       critical_level);
            /* 打印调用栈 */
            rt_backtrace();

            /* 进入死循环，死机等待调试 */
            while (dummy) ;
        }
    }
    /* 恢复中断 */
    rt_hw_interrupt_enable(level);

    /* 调用正常的退出临界区函数 */
    rt_exit_critical();
}

/* 如果未开启临界区调试功能 */
#else /* !RT_DEBUGING_CRITICAL */

/**
 * @brief Safely exit critical section (non-debug version)
 *        If the scheduling function is called before exiting, it will be scheduled in this function.
 * 安全退出临界区（非调试版本）
 *        如果退出前调用了调度函数，将在此函数中执行调度。
 *
 * @param critical_level The expected critical level (unused in non-debug build)
 * 参数 critical_level 是期望的临界区级别（在非调试构建中未使用）
 *
 * @details This is the non-debug version that simply calls rt_exit_critical().
 *          The critical_level parameter is ignored in this implementation.
 * 这是非调试版本，简单调用 rt_exit_critical()。
 *          critical_level 参数在此实现中被忽略。
 */
void rt_exit_critical_safe(rt_base_t critical_level)
{
    /* 直接调用退出临界区函数 */
    rt_exit_critical();
}

#endif/* RT_DEBUGING_CRITICAL */
/* 导出函数符号 */
RTM_EXPORT(rt_exit_critical_safe);

/**
 * @brief Enter critical section and lock the scheduler
 * 进入临界区并锁定调度器
 *
 * @return rt_base_t The current critical level (nesting count)
 * 返回当前的临界区级别（嵌套计数）
 *
 * @details This function:
 *   - Disables interrupts to prevent preemption
 *   - Increments the scheduler lock nesting count
 *   - Returns the new nesting count as critical level
 *   - Re-enables interrupts while maintaining the lock
 * 此函数：
 *   - 禁用中断以防抢占
 *   - 增加调度器锁嵌套计数
 *   - 返回新的嵌套计数作为临界区级别
 *   - 重新启用中断同时保持锁
 *
 * @note The nesting count can go up to RT_UINT16_MAX.
 *       Must be paired with rt_exit_critical().
 *       Interrupts are only disabled during the lock operation.
 * 注意：嵌套计数最大可达 RT_UINT16_MAX。
 *       必须与 rt_exit_critical() 配对。
 *       中断只在锁操作期间禁用。
 */
rt_base_t rt_enter_critical(void)
{
    /* 定义临界区级别变量 */
    rt_base_t critical_level;

    /* 原子操作：将调度器锁嵌套计数加1，并返回加1后的结果 */
    critical_level = rt_atomic_add(&rt_scheduler_lock_nest, 1) + 1;
    /* 返回当前嵌套计数 */
    return critical_level;
}
/* 导出函数符号 */
RTM_EXPORT(rt_enter_critical);

/**
 * @brief Exit critical section and unlock scheduler
 *        If the scheduling function is called before exiting, it will be scheduled in this function.
 * 退出临界区并解锁调度器
 *        如果退出前调用了调度函数，将在此函数中执行调度。
 *
 * @details This function:
 *   - Decrements the scheduler lock nesting count
 *   - If nesting count reaches zero:
 *     * Resets the nesting count
 *     * Re-enables interrupts
 *     * Triggers a scheduler run if current thread exists
 *   - If nesting count still positive:
 *     * Just re-enables interrupts while maintaining lock
 * 此函数：
 *   - 递减调度器锁嵌套计数
 *   - 如果计数归零：
 *     * 重置计数
 *     * 恢复中断
 *     * 如果有挂起调度请求则触发调度
 *   - 如果计数仍为正：
 *     * 仅恢复中断并保持锁
 *
 * @note Must be paired with rt_enter_critical().
 *       Interrupts are only disabled during the lock operation.
 *       Scheduling only occurs when fully unlocked (nest=0)
 * 注意：必须与 rt_enter_critical() 配对。
 *       仅当完全解锁(nest=0)时才发生调度
 */
void rt_exit_critical(void)
{
    /* 定义中断状态变量 */
    rt_base_t level;

    /* disable interrupt 关闭中断 */
    level = rt_hw_interrupt_disable();

    /* 调度器锁嵌套计数递减 */
    rt_scheduler_lock_nest --;
    /* 如果嵌套计数小于等于0，说明完全解锁 */
    if (rt_scheduler_lock_nest <= 0)
    {
        /* 重置嵌套计数为0，防止溢出或异常 */
        rt_scheduler_lock_nest = 0;
        /* enable interrupt 恢复中断 */
        rt_hw_interrupt_enable(level);

        /* 如果有挂起的调度请求 */
        if (IS_CRITICAL_SWITCH_PEND())
        {
            /* 清除挂起标志 */
            CLR_CRITICAL_SWITCH_FLAG();
            /* if scheduler is started and needs to be scheduled, do a schedule 
             * 如果调度器已启动且需要调度，执行一次调度 
             */
            rt_schedule();
        }
    }
    /* 如果嵌套计数仍大于0 */
    else
    {
        /* enable interrupt 恢复中断 */
        rt_hw_interrupt_enable(level);
    }
}
/* 导出函数符号 */
RTM_EXPORT(rt_exit_critical);

/**
 * @brief Get the scheduler lock level.
 * 获取调度器锁的级别（嵌套层数）。
 *
 * @return the level of the scheduler lock. 0 means unlocked.
 * 返回调度器锁的级别。0 表示未锁定。
 */
rt_uint16_t rt_critical_level(void)
{
    /* 原子读取调度器锁嵌套计数并强制转换为 16 位无符号整数返回 */
    return (rt_uint16_t)rt_atomic_load(&rt_scheduler_lock_nest);
}
/* 导出函数符号 */
RTM_EXPORT(rt_critical_level);

/**
 * @brief 绑定线程到指定 CPU（单核版本，直接返回无效参数错误）
 */
rt_err_t rt_sched_thread_bind_cpu(struct rt_thread *thread, int cpu)
{
    /* 单核系统不支持绑核操作，返回无效参数错误 */
    return -RT_EINVAL;
}

/**
 * @} group_thread_management
 *
 * @endcond
 */
