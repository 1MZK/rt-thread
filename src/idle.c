/*
 * 版权所有 (c) 2006-2022, RT-Thread 开发团队
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更日志:
 * 日期           作者       备注
 * 2006-03-23     Bernard    首个版本
 * 2010-11-10     Bernard    在线程退出时添加清理回调函数。
 * 2012-12-29     Bernard    修复编译警告。
 * 2013-12-21     Grissiom   让 rt_thread_idle_excute 循环直到没有
 *                           死亡的线程。
 * 2016-08-09     ArdaFu     添加获取空闲线程句柄的方法。
 * 2018-02-07     Bernard    锁定调度器以保护 tid->cleanup。
 * 2018-07-14     armink     添加空闲钩子列表
 * 2018-11-22     Jesven     添加每 CPU 的空闲任务
 *                           合并主 CPU 和从 CPU 的代码
 * 2021-11-15     THEWON     移除空闲线程与 _thread_exit 之间的重复工作
 * 2023-09-15     xqyjlj     性能优化 rt_hw_interrupt_disable/enable
 * 2023-11-07     xqyjlj     修复线程退出问题
 * 2023-12-10     xqyjlj     添加 _hook_spinlock
 */

/* 包含硬件相关的头文件，提供底层硬件操作接口 */
#include <rthw.h>
/* 包含 RT-Thread 内核的核心头文件，提供内核对象、调度等接口 */
#include <rtthread.h>

/* 如果定义了使用动态加载模块功能 */
#ifdef RT_USING_MODULE
/* 包含动态模块头文件 */
#include <dlmodule.h>
#endif /* 结束条件编译 RT_USING_MODULE */

/* 如果定义了使用钩子函数功能 */
#ifdef RT_USING_HOOK
/* 如果没有定义使用空闲钩子功能，则强制定义它，因为系统钩子通常需要在空闲时运行 */
#ifndef RT_USING_IDLE_HOOK
#define RT_USING_IDLE_HOOK
#endif /* 结束条件编译 RT_USING_IDLE_HOOK */
#endif /* 结束条件编译 RT_USING_HOOK */

/* 如果没有自定义空闲线程栈大小 */
#ifndef IDLE_THREAD_STACK_SIZE
/* 如果使用了空闲钩子或者使用了堆内存管理，栈空间需要稍大一些 */
#if defined (RT_USING_IDLE_HOOK) || defined(RT_USING_HEAP)
/* 定义空闲线程栈大小为 256 字节 */
#define IDLE_THREAD_STACK_SIZE  256
/* 否则，不需要太多栈空间 */
#else
/* 定义空闲线程栈大小为 128 字节 */
#define IDLE_THREAD_STACK_SIZE  128
#endif /* 结束条件编译 (RT_USING_IDLE_HOOK) || defined(RT_USING_HEAP) */
#endif /* 结束条件编译 IDLE_THREAD_STACK_SIZE */

/* 定义 CPU 数量宏，方便后续代码使用 */
#define _CPUS_NR                RT_CPUS_NR

/* 定义空闲线程控制块数组，每个 CPU 都有一个对应的空闲线程 */
static struct rt_thread idle_thread[_CPUS_NR];
/* 按系统要求的对齐方式（RT_ALIGN_SIZE）进行对齐 */
rt_align(RT_ALIGN_SIZE)
/* 定义空闲线程的栈空间数组，二维数组，为每个 CPU 分配独立的栈 */
static rt_uint8_t idle_thread_stack[_CPUS_NR][IDLE_THREAD_STACK_SIZE];

/* 如果定义了使用空闲钩子功能 */
#ifdef RT_USING_IDLE_HOOK
/* 如果没有定义空闲钩子列表的大小 */
#ifndef RT_IDLE_HOOK_LIST_SIZE
/* 定义空闲钩子列表最大容量为 4 个 */
#define RT_IDLE_HOOK_LIST_SIZE  4
#endif /* 结束条件编译 RT_IDLE_HOOK_LIST_SIZE */

/* 定义空闲钩子函数指针数组，用于保存注册的钩子函数 */
static void (*idle_hook_list[RT_IDLE_HOOK_LIST_SIZE])(void);
/* 定义一个自旋锁，用于保护空闲钩子列表的并发访问（多核/中断安全） */
static struct rt_spinlock _hook_spinlock;

/**
 * @brief 设置空闲线程的钩子函数
 * @param hook 钩子函数的指针
 * @return 成功返回 RT_EOK，列表已满返回 -RT_EFULL
 */
rt_err_t rt_thread_idle_sethook(void (*hook)(void))
{
    /* 定义循环变量 */
    rt_size_t i;
    /* 定义返回值，默认设置为列表已满的错误码 */
    rt_err_t ret = -RT_EFULL;
    /* 定义中断状态变量，用于保存关中断前的状态 */
    rt_base_t level;

    /* 加自旋锁并关闭本地 CPU 中断，保存中断状态到 level */
    level = rt_spin_lock_irqsave(&_hook_spinlock);

    /* 遍历空闲钩子列表 */
    for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
    {
        /* 如果找到空位（该位置尚未注册钩子函数） */
        if (idle_hook_list[i] == RT_NULL)
        {
            /* 将钩子函数注册到该位置 */
            idle_hook_list[i] = hook;
            /* 设置返回值为成功 */
            ret = RT_EOK;
            /* 跳出循环，无需继续查找 */
            break;
        }
    }

    /* 解锁自旋锁并恢复之前的中断状态 */
    rt_spin_unlock_irqrestore(&_hook_spinlock, level);

    /* 返回操作结果 */
    return ret;
}

/**
 * @addtogroup group_thread_management
 * @{
 */

/**
 * @brief 删除钩子列表上的空闲钩子。
 *
 * @param hook 指定的钩子函数。
 *
 * @return `RT_EOK`: 删除成功。
 *         `-RT_ENOSYS`: 未找到该钩子。
 */
rt_err_t rt_thread_idle_delhook(void (*hook)(void))
{
    /* 定义循环变量 */
    rt_size_t i;
    /* 定义返回值，默认设置为未找到的错误码 */
    rt_err_t ret = -RT_ENOSYS;
    /* 定义中断状态变量 */
    rt_base_t level;

    /* 加自旋锁并关闭本地 CPU 中断，保存中断状态 */
    level = rt_spin_lock_irqsave(&_hook_spinlock);

    /* 遍历空闲钩子列表 */
    for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
    {
        /* 如果找到了要删除的钩子函数 */
        if (idle_hook_list[i] == hook)
        {
            /* 将该位置置空，相当于删除 */
            idle_hook_list[i] = RT_NULL;
            /* 设置返回值为成功 */
            ret = RT_EOK;
            /* 跳出循环 */
            break;
        }
    }

    /* 解锁自旋锁并恢复之前的中断状态 */
    rt_spin_unlock_irqrestore(&_hook_spinlock, level);

    /* 返回操作结果 */
    return ret;
}

#endif /* 结束条件编译 RT_USING_IDLE_HOOK */

/**
 * @brief 空闲线程入口函数
 * @param parameter 线程入口参数（未使用）
 */
static void idle_thread_entry(void *parameter)
{
    /* 声明未使用参数，避免编译器警告 */
    RT_UNUSED(parameter);
    
/* 如果使用了多核调度(SMP) */
#ifdef RT_USING_SMP
    /* 如果当前 CPU 核心不是主核心（ID 不为 0） */
    if (rt_cpu_get_id() != 0)
    {
        /* 从核心进入死循环 */
        while (1)
        {
            /* 执行从核心特定的空闲任务，通常包含 WFE 等省电指令 */
            rt_hw_secondary_cpu_idle_exec();
        }
    }
#endif /* 结束条件编译 RT_USING_SMP */

    /* 主核心或单核系统进入死循环 */
    while (1)
    {
/* 如果使用了空闲钩子功能 */
#ifdef RT_USING_IDLE_HOOK
        /* 定义循环变量 */
        rt_size_t i;
        /* 定义钩子函数指针变量 */
        void (*idle_hook)(void);

        /* 遍历空闲钩子列表 */
        for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
        {
            /* 从列表中取出钩子函数指针 */
            idle_hook = idle_hook_list[i];
            /* 如果钩子函数不为空 */
            if (idle_hook != RT_NULL)
            {
                /* 执行该钩子函数 */
                idle_hook();
            }
        }
#endif /* 结束条件编译 RT_USING_IDLE_HOOK */

/* 如果未使用 SMP 且未使用 RT-Thread Smart 微内核 */
#if !defined(RT_USING_SMP) && !defined(RT_USING_SMART)
    /* 执行资源回收，清理已经结束的线程（僵尸线程） */
    rt_defunct_execute();
#endif

/* 如果使用了电源管理功能 */
#ifdef RT_USING_PM
    /* 声明电源管理器函数（由于是在循环内部，用外部声明） */
    void rt_system_power_manager(void);
    /* 调用系统电源管理器，进入低功耗模式或调整系统时钟等 */
    rt_system_power_manager();
#endif /* 结束条件编译 RT_USING_PM */
    }
}

/**
 * @brief 该函数将初始化空闲线程，然后启动它。
 *
 * @note 此函数必须在系统初始化时调用。
 */
void rt_thread_idle_init(void)
{
    /* 定义循环变量 */
    rt_ubase_t i;
/* 如果允许线程名称长度大于 0 */
#if RT_NAME_MAX > 0
    /* 定义用于存放线程名称的字符数组 */
    char idle_thread_name[RT_NAME_MAX];
#endif /* 结束条件编译 RT_NAME_MAX > 0 */

/* 如果使用了空闲钩子功能 */
#ifdef RT_USING_IDLE_HOOK
    /* 初始化保护钩子列表的自旋锁 */
    rt_spin_lock_init(&_hook_spinlock);
#endif

    /* 遍历所有 CPU 核心 */
    for (i = 0; i < _CPUS_NR; i++)
    {
/* 如果允许线程名称长度大于 0 */
#if RT_NAME_MAX > 0
        /* 格式化生成空闲线程的名称，如 "tidle0", "tidle1" */
        rt_snprintf(idle_thread_name, RT_NAME_MAX, "tidle%d", i);
#endif /* 结束条件编译 RT_NAME_MAX > 0 */
        /* 静态初始化空闲线程控制块 */
        rt_thread_init(&idle_thread[i],
/* 如果允许线程名称长度大于 0，使用动态生成的名称 */
#if RT_NAME_MAX > 0
                idle_thread_name,
/* 否则使用默认名称 */
#else
                "tidle",
#endif /* 结束条件编译 RT_NAME_MAX > 0 */
                idle_thread_entry,                 /* 线程入口函数 */
                RT_NULL,                          /* 线程入口参数 */
                &idle_thread_stack[i][0],          /* 线程栈起始地址 */
                sizeof(idle_thread_stack[i]),      /* 线程栈大小 */
                RT_THREAD_PRIORITY_MAX - 1,        /* 线程优先级（最低优先级） */
                32);                               /* 线程时间片大小 */
/* 如果使用了多核调度(SMP) */
#ifdef RT_USING_SMP
        /* 将该空闲线程绑定到对应的 CPU 核心上运行 */
        rt_thread_control(&idle_thread[i], RT_THREAD_CTRL_BIND_CPU, (void*)i);
#endif /* 结束条件编译 RT_USING_SMP */

        /* 更新当前 CPU 核心结构体中的空闲线程指针 */
        rt_cpu_index(i)->idle_thread = &idle_thread[i];

        /* 启动该空闲线程，将其加入就绪队列 */
        rt_thread_startup(&idle_thread[i]);
    }
}

/**
 * @brief 该函数获取当前 CPU 空闲线程的句柄。
 * @return 返回空闲线程的控制块指针
 */
rt_thread_t rt_thread_idle_gethandler(void)
{
    /* 获取当前 CPU 核心的 ID 号 */
    int id = rt_cpu_get_id();

    /* 根据核心 ID 返回对应的空闲线程控制块指针，并强制转换为 rt_thread_t 类型 */
    return (rt_thread_t)(&idle_thread[id]);
}

/**
 * @brief 检查指定的线程是否为系统空闲线程之一。
 *
 * @details
 * RT-Thread 为每个 CPU 创建了一个空闲线程。这些空闲线程是特殊的
 * 归调度器所有的线程，当没有其他就绪线程存在时，它们作为后备可运行线程。
 *
 * 此辅助函数主要用于在可能阻塞或挂起线程的代码路径中进行防御性检查，
 * 因为空闲线程绝对不能进入阻塞或挂起状态。挂起空闲线程可能导致系统没有就绪
 * 线程，从而破坏调度。
 *
 * @param thread 要测试的线程。
 *
 * @return 如果 @p thread 是任何 CPU 的空闲线程，则返回 RT_TRUE；否则返回 RT_FALSE。
 *
 * @note
 * - 在 SMP（多核）配置中，每个 CPU 都有一个空闲线程，因此此函数会
 *   检查所有的空闲线程对象。
 * - 传入 RT_NULL 将返回 RT_FALSE。
 */
rt_bool_t rt_thread_is_idle_thread(rt_thread_t thread)
{
    /* 定义循环变量 */
    rt_ubase_t i;

    /* 检查传入的线程指针是否不为空 */
    if (thread != RT_NULL)
    {
        /* 遍历所有 CPU 核心 */
        for (i = 0; i < _CPUS_NR; i++)
        {
            /* 如果传入的线程指针等于某个 CPU 的空闲线程指针 */
            if (thread == &idle_thread[i])
                /* 匹配成功，说明是空闲线程，返回 RT_TRUE */
                return RT_TRUE;
        }
    }

    /* 如果传入指针为空，或者遍历完未找到匹配的空闲线程，返回 RT_FALSE */
    return RT_FALSE;
}

/** @} group_thread_management */
