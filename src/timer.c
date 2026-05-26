/*
 * 版权所有 (c) 2006-2024, RT-Thread 开发团队
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更日志:
 * 日期           作者       备注
 * 2006-03-12     Bernard      首个版本
 * 2006-04-29     Bernard      实现线程定时器
 * 2006-06-04     Bernard      实现 rt_timer_control
 * 2006-08-10     Bernard      修复周期定时器漏洞
 * 2006-09-03     Bernard      实现 rt_timer_detach
 * 2009-11-11     LiJin        添加软定时器
 * 2010-05-12     Bernard      修复定时器检查漏洞
 * 2010-11-02     Charlie      重新实现 tick 溢出问题
 * 2012-12-15     Bernard      修复软定时器中的下一次超时问题
 * 2014-07-12     Bernard      调用软定时器超时函数时不锁定调度器
 * 2021-08-15     supperthomas 添加注释
 * 2022-01-07     Gabriel      将 __on_rt_xxxxx_hook 移至 timer.c
 * 2022-04-19     Stanley      修正描述
 * 2023-09-15     xqyjlj       优化 rt_hw_interrupt_disable/enable 性能
 * 2024-01-25     Shell        添加 RT_TIMER_FLAG_THREAD_TIMER 以使定时器与调度器同步
 * 2024-05-01     wdfk-prog    合并 rt_timer_check 和 _soft_timer_check 函数
 */

/* 包含 RT-Thread 核心头文件 */
#include <rtthread.h>
/* 包含 RT-Thread 硬件相关头文件 */
#include <rthw.h>

/* 定义调试标签为 "kernel.timer" */
#define DBG_TAG           "kernel.timer"
/* 定义调试级别为 DBG_INFO */
#define DBG_LVL           DBG_INFO
/* 包含 RT-Thread 调试头文件 */
#include <rtdbg.h>

/* 如果没有定义全软定时器宏 RT_USING_TIMER_ALL_SOFT */
#ifndef RT_USING_TIMER_ALL_SOFT
/* 硬定时器列表 */
static rt_list_t _timer_list[RT_TIMER_SKIP_LIST_LEVEL];
/* 硬定时器自旋锁 */
static struct rt_spinlock _htimer_lock;
/* 结束 ifndef RT_USING_TIMER_ALL_SOFT */
#endif

/* 如果定义了使用软定时器宏 RT_USING_TIMER_SOFT */
#ifdef RT_USING_TIMER_SOFT

/* 如果没有定义软定时器线程栈大小 */
#ifndef RT_TIMER_THREAD_STACK_SIZE
/* 定义软定时器线程栈大小为 512 */
#define RT_TIMER_THREAD_STACK_SIZE     512
/* 结束 ifndef RT_TIMER_THREAD_STACK_SIZE */
#endif /* RT_TIMER_THREAD_STACK_SIZE */

/* 如果没有定义软定时器线程优先级 */
#ifndef RT_TIMER_THREAD_PRIO
/* 定义软定时器线程优先级为 0 */
#define RT_TIMER_THREAD_PRIO           0
/* 结束 ifndef RT_TIMER_THREAD_PRIO */
#endif /* RT_TIMER_THREAD_PRIO */

/* 软定时器列表 */
static rt_list_t _soft_timer_list[RT_TIMER_SKIP_LIST_LEVEL];
/* 软定时器自旋锁 */
static struct rt_spinlock _stimer_lock;
/* 软定时器线程控制块 */
static struct rt_thread _timer_thread;
/* 软定时器信号量 */
static struct rt_semaphore _soft_timer_sem;
/* 软定时器线程栈，按照 RT_ALIGN_SIZE 对齐 */
rt_align(RT_ALIGN_SIZE)
/* 定义软定时器线程栈数组 */
static rt_uint8_t _timer_thread_stack[RT_TIMER_THREAD_STACK_SIZE];
/* 结束 ifdef RT_USING_TIMER_SOFT */
#endif /* RT_USING_TIMER_SOFT */

/* 如果定义了使用钩子函数宏 RT_USING_HOOK 且定义了使用函数指针钩子宏 RT_HOOK_USING_FUNC_PTR */
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
/* 声明外部对象获取钩子函数指针 */
extern void (*rt_object_take_hook)(struct rt_object *object);
/* 声明外部对象放下钩子函数指针 */
extern void (*rt_object_put_hook)(struct rt_object *object);
/* 定义定时器进入钩子函数指针 */
static void (*rt_timer_enter_hook)(struct rt_timer *timer);
/* 定义定时器退出钩子函数指针 */
static void (*rt_timer_exit_hook)(struct rt_timer *timer);

/**
 * @addtogroup group_hook
 */

/**@{*/

/**
 * @brief 此函数将在定时器上设置一个钩子函数，
 *        该钩子函数将在进入定时器超时回调函数时被调用。
 *
 * @param hook 是定时器的函数指针
 */
void rt_timer_enter_sethook(void (*hook)(struct rt_timer *timer))
{
    /* 将传入的钩子函数指针赋值给全局的定时器进入钩子 */
    rt_timer_enter_hook = hook;
}

/**
 * @brief 此函数将设置一个钩子函数，
 *        该钩子函数将在退出定时器超时回调函数时被调用。
 *
 * @param hook 是定时器的函数指针
 */
void rt_timer_exit_sethook(void (*hook)(struct rt_timer *timer))
{
    /* 将传入的钩子函数指针赋值给全局的定时器退出钩子 */
    rt_timer_exit_hook = hook;
}

/**@}*/
/* 结束 ifdef RT_USING_HOOK */
#endif /* RT_USING_HOOK */

/* 内联函数：获取定时器对应的自旋锁指针 */
rt_inline struct rt_spinlock* _timerlock_idx(struct rt_timer *timer)
{
    /* 如果定义了全软定时器宏 */
#ifdef RT_USING_TIMER_ALL_SOFT
    /* 直接返回软定时器自旋锁地址 */
    return &_stimer_lock;
/* 否则 */
#else
    /* 如果定义了使用软定时器宏 */
#ifdef RT_USING_TIMER_SOFT
    /* 如果定时器标志位包含软定时器标志 */
    if (timer->parent.flag & RT_TIMER_FLAG_SOFT_TIMER)
    {
        /* 返回软定时器自旋锁地址 */
        return &_stimer_lock;
    }
    /* 否则 */
    else
/* 结束 ifdef RT_USING_TIMER_SOFT */
#endif /* RT_USING_TIMER_SOFT */
    {
        /* 返回硬定时器自旋锁地址 */
        return &_htimer_lock;
    }
/* 结束 ifdef RT_USING_TIMER_ALL_SOFT */
#endif
}

/**
 * @brief [内部] 定时器的初始化函数
 *
 *        rt_timer_init 的内部调用函数
 *
 * @see rt_timer_init
 *
 * @param timer 是定时器对象
 *
 * @param timeout 是超时函数
 *
 * @param parameter 是超时函数的参数
 *
 * @param time 是定时器的 tick 数
 *
 * @param flag 是定时器的标志
 */
static void _timer_init(rt_timer_t timer,
                        void (*timeout)(void *parameter),
                        void      *parameter,
                        rt_tick_t  time,
                        rt_uint8_t flag)
{
    /* 定义循环变量 i */
    int i;

    /* 如果定义了全软定时器宏 */
#ifdef RT_USING_TIMER_ALL_SOFT
    /* 标志位追加软定时器标志 */
    flag               |= RT_TIMER_FLAG_SOFT_TIMER;
/* 结束 ifdef RT_USING_TIMER_ALL_SOFT */
#endif

    /* 设置标志 */
    timer->parent.flag  = flag;

    /* 设置为未激活状态 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    /* 设置超时回调函数 */
    timer->timeout_func = timeout;
    /* 设置超时回调函数的参数 */
    timer->parameter    = parameter;

    /* 初始化超时 tick 为 0 */
    timer->timeout_tick = 0;
    /* 设置初始 tick 时间 */
    timer->init_tick    = time;

    /* 初始化定时器列表 */
    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        /* 初始化定时器每一层的链表节点 */
        rt_list_init(&(timer->row[i]));
    }
}

/**
 * @brief  查找下一个空的定时器 tick
 *
 * @param timer_list 是时间列表数组
 *
 * @param timeout_tick 是下一个定时器的 tick 指针
 *
 * @return  返回操作状态。如果返回值是 RT_EOK，函数执行成功。
 *          如果返回值是其他任何值，说明此操作失败。
 */
static rt_err_t _timer_list_next_timeout(rt_list_t timer_list[], rt_tick_t *timeout_tick)
{
    /* 定义定时器结构体指针 */
    struct rt_timer *timer;

    /* 如果定时器列表最高层不为空 */
    if (!rt_list_isempty(&timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1]))
    {
        /* 获取定时器列表最高层的下一个节点对应的定时器结构体 */
        timer = rt_list_entry(timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1].next,
                              struct rt_timer, row[RT_TIMER_SKIP_LIST_LEVEL - 1]);
        /* 将该定时器的超时 tick 赋值给传出参数 */
        *timeout_tick = timer->timeout_tick;
        /* 返回成功 */
        return RT_EOK;
    }
    /* 否则返回错误 */
    return -RT_ERROR;
}

/**
 * @brief 移除定时器
 *
 * @param timer 是定时器的指针
 */
rt_inline void _timer_remove(rt_timer_t timer)
{
    /* 定义循环变量 i */
    int i;

    /* 遍历跳表的所有层 */
    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        /* 从对应层的链表中移除该定时器节点 */
        rt_list_remove(&timer->row[i]);
    }
}

/* 如果调试级别为 DBG_LOG */
#if (DBG_LVL == DBG_LOG)
/**
 * @brief 定时器的层数计数
 *
 * @param timer 是定时器的头指针
 *
 * @return 定时器的层数
 */
static int _timer_count_height(struct rt_timer *timer)
{
    /* 定义变量 i 和计数器 cnt 初始化为 0 */
    int i, cnt = 0;

    /* 遍历跳表的所有层 */
    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        /* 如果该层链表不为空 */
        if (!rt_list_isempty(&timer->row[i]))
            /* 计数器加 1 */
            cnt++;
    }
    /* 返回计数 */
    return cnt;
}
/**
 * @brief 打印所有定时器信息
 *
 * @param timer_heads 是定时器的头指针数组
 */
void rt_timer_dump(rt_list_t timer_heads[])
{
    /* 定义链表指针 */
    rt_list_t *list;

    /* 遍历定时器列表最高层 */
    for (list = timer_heads[RT_TIMER_SKIP_LIST_LEVEL - 1].next;
         list != &timer_heads[RT_TIMER_SKIP_LIST_LEVEL - 1];
         list = list->next)
    {
        /* 获取链表节点对应的定时器结构体 */
        struct rt_timer *timer = rt_list_entry(list,
                                               struct rt_timer,
                                               row[RT_TIMER_SKIP_LIST_LEVEL - 1]);
        /* 打印该定时器的层数 */
        rt_kprintf("%d", _timer_count_height(timer));
    }
    /* 打印换行 */
    rt_kprintf("\n");
}
/* 结束 if (DBG_LVL == DBG_LOG) */
#endif /* (DBG_LVL == DBG_LOG) */

/**
 * @addtogroup group_clock-management
 */

/**@{*/

/**
 * @brief 此函数将初始化一个定时器
 *        通常此函数用于初始化一个静态定时器对象。
 *
 * @param timer 是定时器的指针
 *
 * @param name 是指向定时器名称的指针
 *
 * @param timeout 是定时器的回调函数
 *
 * @param parameter 是回调函数的参数
 *
 * @param time 是定时器的超时 tick 数
 *
 *             注意：最大超时 tick 不应超过 (RT_TICK_MAX/2 - 1)。
 *
 * @param flag 是定时器的标志
 *
 */
void rt_timer_init(rt_timer_t  timer,
                   const char *name,
                   void (*timeout)(void *parameter),
                   void       *parameter,
                   rt_tick_t   time,
                   rt_uint8_t  flag)
{
    /* 参数检查：定时器指针不为空 */
    RT_ASSERT(timer != RT_NULL);
    /* 参数检查：超时回调函数指针不为空 */
    RT_ASSERT(timeout != RT_NULL);
    /* 参数检查：超时时间小于最大 tick 数的一半 */
    RT_ASSERT(time < RT_TICK_MAX / 2);

    /* 定时器对象初始化 */
    rt_object_init(&(timer->parent), RT_Object_Class_Timer, name);

    /* 调用内部初始化函数 */
    _timer_init(timer, timeout, parameter, time, flag);
}
/* 导出 rt_timer_init 函数符号 */
RTM_EXPORT(rt_timer_init);

/**
 * @brief 此函数将从定时器管理中脱离一个定时器。
 *
 * @param timer 是要被脱离的定时器
 *
 * @return 脱离操作的状态
 */
rt_err_t rt_timer_detach(rt_timer_t timer)
{
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义自旋锁指针 */
    struct rt_spinlock *spinlock;

    /* 参数检查：定时器指针不为空 */
    RT_ASSERT(timer != RT_NULL);
    /* 参数检查：对象类型为定时器 */
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);
    /* 参数检查：对象为系统对象（静态对象） */
    RT_ASSERT(rt_object_is_systemobject(&timer->parent));

    /* 获取该定时器对应的自旋锁 */
    spinlock = _timerlock_idx(timer);
    /* 保存中断状态并上自旋锁 */
    level = rt_spin_lock_irqsave(spinlock);

    /* 从链表中移除定时器 */
    _timer_remove(timer);
    /* 停止定时器，清除激活标志 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(spinlock, level);
    /* 脱离定时器对象 */
    rt_object_detach(&(timer->parent));

    /* 返回成功 */
    return RT_EOK;
}
/* 导出 rt_timer_detach 函数符号 */
RTM_EXPORT(rt_timer_detach);

/* 如果定义了使用堆内存宏 RT_USING_HEAP */
#ifdef RT_USING_HEAP
/**
 * @brief 此函数将创建一个定时器
 *
 * @param name 是定时器的名称
 *
 * @param timeout 是超时函数
 *
 * @param parameter 是超时函数的参数
 *
 * @param time 是定时器的超时 tick 数
 *
 *        注意：最大超时 tick 不应超过 (RT_TICK_MAX/2 - 1)。
 *
 * @param flag 是定时器的标志。如果设置了以下一个或多个标志，定时器将根据所选标志值调用超时函数。
 *
 *          RT_TIMER_FLAG_ONE_SHOT          单次定时
 *          RT_TIMER_FLAG_PERIODIC          周期定时
 *
 *          RT_TIMER_FLAG_HARD_TIMER        硬件定时器
 *          RT_TIMER_FLAG_SOFT_TIMER        软件定时器
 *          RT_TIMER_FLAG_THREAD_TIMER      线程定时器
 *
 *        注意：
 *        你可以使用 "|" 逻辑运算符传递多个值。默认情况下，系统将使用 RT_TIME_FLAG_HARD_TIMER。
 *
 * @return 创建的定时器对象
 */
rt_timer_t rt_timer_create(const char *name,
                           void (*timeout)(void *parameter),
                           void       *parameter,
                           rt_tick_t   time,
                           rt_uint8_t  flag)
{
    /* 定义定时器结构体指针 */
    struct rt_timer *timer;

    /* 参数检查：超时回调函数指针不为空 */
    RT_ASSERT(timeout != RT_NULL);
    /* 参数检查：超时时间小于最大 tick 数的一半 */
    RT_ASSERT(time < RT_TICK_MAX / 2);

    /* 分配一个定时器对象 */
    timer = (struct rt_timer *)rt_object_allocate(RT_Object_Class_Timer, name);
    /* 如果分配失败 */
    if (timer == RT_NULL)
    {
        /* 返回空指针 */
        return RT_NULL;
    }

    /* 调用内部初始化函数 */
    _timer_init(timer, timeout, parameter, time, flag);

    /* 返回创建的定时器指针 */
    return timer;
}
/* 导出 rt_timer_create 函数符号 */
RTM_EXPORT(rt_timer_create);

/**
 * @brief 此函数将删除一个定时器并释放定时器内存
 *
 * @param timer 是要被删除的定时器
 *
 * @return 操作状态，RT_EOK 表示成功；-RT_ERROR 表示错误
 */
rt_err_t rt_timer_delete(rt_timer_t timer)
{
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义自旋锁指针 */
    struct rt_spinlock *spinlock;

    /* 参数检查：定时器指针不为空 */
    RT_ASSERT(timer != RT_NULL);
    /* 参数检查：对象类型为定时器 */
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);
    /* 参数检查：对象不是系统对象（动态对象） */
    RT_ASSERT(rt_object_is_systemobject(&timer->parent) == RT_FALSE);

    /* 获取该定时器对应的自旋锁 */
    spinlock = _timerlock_idx(timer);

    /* 保存中断状态并上自旋锁 */
    level = rt_spin_lock_irqsave(spinlock);

    /* 从链表中移除定时器 */
    _timer_remove(timer);
    /* 停止定时器，清除激活标志 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(spinlock, level);
    /* 删除定时器对象并释放内存 */
    rt_object_delete(&(timer->parent));

    /* 返回成功 */
    return RT_EOK;
}
/* 导出 rt_timer_delete 函数符号 */
RTM_EXPORT(rt_timer_delete);
/* 结束 ifdef RT_USING_HEAP */
#endif /* RT_USING_HEAP */

/**
 * @brief 此函数将启动定时器
 *
 * @param timer_list 定时器列表
 * @param timer 是要启动的定时器
 *
 * @return 操作状态，RT_EOK 表示成功，-RT_ERROR 表示错误
 */
static rt_err_t _timer_start(rt_list_t *timer_list, rt_timer_t timer)
{
    /* 定义行层级变量 */
    unsigned int row_lvl;
    /* 定义行头指针数组 */
    rt_list_t *row_head[RT_TIMER_SKIP_LIST_LEVEL];
    /* 定义测试编号变量 */
    unsigned int tst_nr;
    /* 定义静态随机编号变量 */
    static unsigned int random_nr;

    /* 首先将定时器从列表中移除 */
    _timer_remove(timer);
    /* 改变定时器状态，清除激活标志 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    /* 调用对象获取钩子函数 */
    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(timer->parent)));

    /* 计算定时器的超时 tick = 当前 tick + 初始 tick */
    timer->timeout_tick = rt_tick_get() + timer->init_tick;

    /* 第 0 层的行头指向定时器列表第 0 层地址 */
    row_head[0]  = &timer_list[0];
    /* 遍历跳表的每一层 */
    for (row_lvl = 0; row_lvl < RT_TIMER_SKIP_LIST_LEVEL; row_lvl++)
    {
        /* 遍历当前层的链表，直到到达链表末尾 */
        for (; row_head[row_lvl] != timer_list[row_lvl].prev;
             row_head[row_lvl]  = row_head[row_lvl]->next)
        {
            /* 定义临时定时器结构体指针 */
            struct rt_timer *t;
            /* 获取当前节点的下一个节点指针 */
            rt_list_t *p = row_head[row_lvl]->next;

            /* 修正入口指针，获取下一个节点对应的定时器结构体 */
            t = rt_list_entry(p, struct rt_timer, row[row_lvl]);

            /* 如果我们有两个超时时间相同的定时器，
             * 优先调用早插入的定时器。
             * 因此将新定时器插入到相同超时时间定时器列表的末尾。
             */
            if ((t->timeout_tick - timer->timeout_tick) == 0)
            {
                /* 超时时间相同，继续往后找 */
                continue;
            }
            /* 如果差值小于最大 tick 数的一半，说明 t 的超时时间比当前定时器晚 */
            else if ((t->timeout_tick - timer->timeout_tick) < RT_TICK_MAX / 2)
            {
                /* 找到插入位置，跳出内层循环 */
                break;
            }
        }
        /* 如果不是最后一层 */
        if (row_lvl != RT_TIMER_SKIP_LIST_LEVEL - 1)
            /* 上一层行头指向当前层行头的下一个节点（跨层连接） */
            row_head[row_lvl + 1] = row_head[row_lvl] + 1;
    }

    /* 有趣的是，这个超级简单的定时器插入计数器在均匀分布列表高度方面
     * 工作得非常非常好。所谓 "非常非常好"，我的意思是它很容易击败
     * timer->timeout_tick 的随机性（实际上，timeout_tick 并不随机且容易被攻击）。 */
    /* 随机数递增 */
    random_nr++;
    /* 将随机数赋给测试编号 */
    tst_nr = random_nr;

    /* 将定时器插入到跳表最高层的对应位置 */
    rt_list_insert_after(row_head[RT_TIMER_SKIP_LIST_LEVEL - 1],
                         &(timer->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
    /* 从第 2 层开始遍历到最高层，决定是否在低层插入 */
    for (row_lvl = 2; row_lvl <= RT_TIMER_SKIP_LIST_LEVEL; row_lvl++)
    {
        /* 如果测试编号的掩码为 0 */
        if (!(tst_nr & RT_TIMER_SKIP_LIST_MASK))
            /* 在对应的低层插入定时器节点 */
            rt_list_insert_after(row_head[RT_TIMER_SKIP_LIST_LEVEL - row_lvl],
                                 &(timer->row[RT_TIMER_SKIP_LIST_LEVEL - row_lvl]));
        /* 否则 */
        else
            /* 跳出循环，不再在更低层插入 */
            break;
        /* 移位我们测试过的位。对于 1 位和 2 位工作良好。 */
        tst_nr >>= (RT_TIMER_SKIP_LIST_MASK + 1) >> 1;
    }

    /* 设置定时器为激活状态 */
    timer->parent.flag |= RT_TIMER_FLAG_ACTIVATED;

    /* 返回成功 */
    return RT_EOK;
}

/**
 * @brief 此函数将检查定时器列表，如果发生超时事件，
 *        将调用相应的超时函数。
 *
 * @param timer_list 要检查的定时器列表。
 * @param lock 定时器列表的自旋锁。
 */
static void _timer_check(rt_list_t *timer_list, struct rt_spinlock *lock)
{
    /* 定义定时器结构体指针 */
    struct rt_timer *t;
    /* 定义当前 tick 变量 */
    rt_tick_t current_tick;
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义临时链表用于存放超时的定时器 */
    rt_list_t list;

    /* 保存中断状态并上自旋锁 */
    level = rt_spin_lock_irqsave(lock);

    /* 获取当前 tick */
    current_tick = rt_tick_get();

    /* 初始化临时链表 */
    rt_list_init(&list);

    /* 当定时器列表最高层不为空时循环 */
    while (!rt_list_isempty(&timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1]))
    {
        /* 获取定时器列表最高层的第一个定时器 */
        t = rt_list_entry(timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1].next,
                          struct rt_timer, row[RT_TIMER_SKIP_LIST_LEVEL - 1]);

        /* 重新获取当前 tick */
        current_tick = rt_tick_get();

        /*
         * 假设新的 tick 小于最大 tick 持续时间的一半。
         */
        /* 如果当前 tick 与定时器超时 tick 的差值小于最大 tick 的一半，说明已超时 */
        if ((current_tick - t->timeout_tick) < RT_TICK_MAX / 2)
        {
            /* 调用定时器进入钩子函数 */
            RT_OBJECT_HOOK_CALL(rt_timer_enter_hook, (t));

            /* 首先从定时器列表中移除定时器 */
            _timer_remove(t);
            /* 如果不是周期定时器 */
            if (!(t->parent.flag & RT_TIMER_FLAG_PERIODIC))
            {
                /* 清除激活标志 */
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
            }

            /* 将定时器添加到临时列表 */
            rt_list_insert_after(&list, &(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));

            /* 解锁自旋锁并恢复中断状态，以便在回调中响应中断 */
            rt_spin_unlock_irqrestore(lock, level);

            /* 调用超时函数 */
            t->timeout_func(t->parameter);

            /* 调用定时器退出钩子函数 */
            RT_OBJECT_HOOK_CALL(rt_timer_exit_hook, (t));

            /* 再次保存中断状态并上自旋锁 */
            level = rt_spin_lock_irqsave(lock);

            /* 检查定时器对象是否被脱离或再次启动（在回调中可能被操作） */
            if (rt_list_isempty(&list))
            {
                /* 如果临时列表为空，继续下一次循环 */
                continue;
            }
            /* 从临时列表中移除该定时器节点 */
            rt_list_remove(&(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
            /* 如果是周期定时器且仍处于激活状态 */
            if ((t->parent.flag & RT_TIMER_FLAG_PERIODIC) &&
                (t->parent.flag & RT_TIMER_FLAG_ACTIVATED))
            {
                /* 重新启动它 */
                /* 先清除激活标志，因为 _timer_start 里面会根据这个标志做处理 */
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
                /* 调用内部启动函数重新插入定时器列表 */
                _timer_start(timer_list, t);
            }
        }
        /* 否则（未超时），跳出循环 */
        else break;
    }
    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(lock, level);
}

/**
 * @brief 此函数将启动定时器
 *
 * @param timer 是要启动的定时器
 *
 * @return 操作状态，RT_EOK 表示成功，-RT_ERROR 表示错误
 */
rt_err_t rt_timer_start(rt_timer_t timer)
{
    /* 定义调度器锁级别变量 */
    rt_sched_lock_level_t slvl;
    /* 是否为线程定时器标志，初始化为 0 */
    int is_thread_timer = 0;
    /* 定义自旋锁指针 */
    struct rt_spinlock *spinlock;
    /* 定义定时器列表指针 */
    rt_list_t *timer_list;
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义返回错误码 */
    rt_err_t err;

    /* 参数检查：定时器指针不为空 */
    RT_ASSERT(timer != RT_NULL);
    /* 参数检查：对象类型为定时器 */
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    /* 如果定义了全软定时器宏 */
#ifdef RT_USING_TIMER_ALL_SOFT
    /* 定时器列表指向软定时器列表 */
    timer_list = _soft_timer_list;
    /* 自旋锁指向软定时器自旋锁 */
    spinlock = &_stimer_lock;
/* 否则 */
#else
    /* 如果定义了使用软定时器宏 */
#ifdef RT_USING_TIMER_SOFT
    /* 如果定时器标志位包含软定时器标志 */
    if (timer->parent.flag & RT_TIMER_FLAG_SOFT_TIMER)
    {
        /* 定时器列表指向软定时器列表 */
        timer_list = _soft_timer_list;
        /* 自旋锁指向软定时器自旋锁 */
        spinlock = &_stimer_lock;
    }
    /* 否则 */
    else
/* 结束 ifdef RT_USING_TIMER_SOFT */
#endif /* RT_USING_TIMER_SOFT */
    {
        /* 定时器列表指向硬定时器列表 */
        timer_list = _timer_list;
        /* 自旋锁指向硬定时器自旋锁 */
        spinlock = &_htimer_lock;
    }
/* 结束 ifdef RT_USING_TIMER_ALL_SOFT */
#endif

    /* 如果定时器标志位包含线程定时器标志 */
    if (timer->parent.flag & RT_TIMER_FLAG_THREAD_TIMER)
    {
        /* 定义线程结构体指针 */
        rt_thread_t thread;
        /* 标记为线程定时器 */
        is_thread_timer = 1;
        /* 锁定调度器 */
        rt_sched_lock(&slvl);

        /* 通过 timer 成员反推出所在的线程控制块 */
        thread = rt_container_of(timer, struct rt_thread, thread_timer);
        /* 断言检查：确保对象类型为线程 */
        RT_ASSERT(rt_object_get_type(&thread->parent) == RT_Object_Class_Thread);
        /* 启动线程定时器（通知调度器） */
        rt_sched_thread_timer_start(thread);
    }

    /* 保存中断状态并上自旋锁 */
    level = rt_spin_lock_irqsave(spinlock);

    /* 调用内部启动函数 */
    err = _timer_start(timer_list, timer);

    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(spinlock, level);

    /* 如果是线程定时器 */
    if (is_thread_timer)
    {
        /* 解锁调度器 */
        rt_sched_unlock(slvl);
    }

    /* 返回错误码 */
    return err;
}
/* 导出 rt_timer_start 函数符号 */
RTM_EXPORT(rt_timer_start);

/**
 * @brief 此函数将停止定时器
 *
 * @param timer 是要停止的定时器
 *
 * @return 操作状态，RT_EOK 表示成功，-RT_ERROR 表示错误
 */
rt_err_t rt_timer_stop(rt_timer_t timer)
{
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义自旋锁指针 */
    struct rt_spinlock *spinlock;

    /* 定时器检查：指针不为空 */
    RT_ASSERT(timer != RT_NULL);
    /* 参数检查：对象类型为定时器 */
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    /* 获取该定时器对应的自旋锁 */
    spinlock = _timerlock_idx(timer);

    /* 保存中断状态并上自旋锁 */
    level = rt_spin_lock_irqsave(spinlock);

    /* 如果定时器未处于激活状态 */
    if (!(timer->parent.flag & RT_TIMER_FLAG_ACTIVATED))
    {
        /* 解锁自旋锁并恢复中断状态 */
        rt_spin_unlock_irqrestore(spinlock, level);
        /* 返回错误状态 */
        return -RT_ERROR;
    }
    /* 调用对象放下钩子函数 */
    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(timer->parent)));

    /* 从链表中移除定时器 */
    _timer_remove(timer);
    /* 改变状态，清除激活标志 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(spinlock, level);

    /* 返回成功 */
    return RT_EOK;
}
/* 导出 rt_timer_stop 函数符号 */
RTM_EXPORT(rt_timer_stop);

/**
 * @brief 此函数将获取或设置定时器的一些选项
 *
 * @param timer 是要获取或设置的定时器
 * @param cmd 是控制命令
 * @param arg 是参数
 *
 * @return 控制操作的状态
 */
rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg)
{
    /* 定义自旋锁指针 */
    struct rt_spinlock *spinlock;
    /* 定义中断级别变量 */
    rt_base_t level;

    /* 参数检查：定时器指针不为空 */
    RT_ASSERT(timer != RT_NULL);
    /* 参数检查：对象类型为定时器 */
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    /* 获取该定时器对应的自旋锁 */
    spinlock = _timerlock_idx(timer);

    /* 保存中断状态并上自旋锁 */
    level = rt_spin_lock_irqsave(spinlock);
    /* 根据命令分支处理 */
    switch (cmd)
    {
    /* 获取定时器的初始超时 tick */
    case RT_TIMER_CTRL_GET_TIME:
        /* 将初始 tick 赋值给 arg 指向的变量 */
        *(rt_tick_t *)arg = timer->init_tick;
        /* 跳出 switch */
        break;

    /* 设置定时器的初始超时 tick */
    case RT_TIMER_CTRL_SET_TIME:
        /* 断言检查：设置的超时时间小于最大 tick 数的一半 */
        RT_ASSERT((*(rt_tick_t *)arg) < RT_TICK_MAX / 2);
        /* 如果定时器处于激活状态 */
        if (timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
        {
            /* 先从链表中移除定时器 */
            _timer_remove(timer);
            /* 清除激活标志 */
            timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
        }
        /* 更新初始 tick */
        timer->init_tick = *(rt_tick_t *)arg;
        /* 跳出 switch */
        break;

    /* 设置定时器为单次模式 */
    case RT_TIMER_CTRL_SET_ONESHOT:
        /* 清除周期定时标志 */
        timer->parent.flag &= ~RT_TIMER_FLAG_PERIODIC;
        /* 跳出 switch */
        break;

    /* 设置定时器为周期模式 */
    case RT_TIMER_CTRL_SET_PERIODIC:
        /* 添加周期定时标志 */
        timer->parent.flag |= RT_TIMER_FLAG_PERIODIC;
        /* 跳出 switch */
        break;

    /* 获取定时器状态 */
    case RT_TIMER_CTRL_GET_STATE:
        /* 如果定时器处于激活状态 */
        if(timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
        {
            /* 定时器已启动并运行 */
            *(rt_uint32_t *)arg = RT_TIMER_FLAG_ACTIVATED;
        }
        /* 否则 */
        else
        {
            /* 定时器已停止 */
            *(rt_uint32_t *)arg = RT_TIMER_FLAG_DEACTIVATED;
        }
        /* 跳出 switch */
        break;

    /* 获取定时器的剩余超时 tick */
    case RT_TIMER_CTRL_GET_REMAIN_TIME:
        /* 将超时 tick 赋值给 arg 指向的变量 */
        *(rt_tick_t *)arg =  timer->timeout_tick;
        /* 跳出 switch */
        break;
    /* 获取定时器的超时回调函数 */
    case RT_TIMER_CTRL_GET_FUNC:
        /* 将超时函数指针赋值给 arg 指向的变量 */
        *(void **)arg = (void *)timer->timeout_func;
        /* 跳出 switch */
        break;

    /* 设置定时器的超时回调函数 */
    case RT_TIMER_CTRL_SET_FUNC:
        /* 将 arg 作为超时函数指针设置给定时器 */
        timer->timeout_func = (void (*)(void*))arg;
        /* 跳出 switch */
        break;

    /* 获取定时器超时回调函数的参数 */
    case RT_TIMER_CTRL_GET_PARM:
        /* 将参数赋值给 arg 指向的变量 */
        *(void **)arg = timer->parameter;
        /* 跳出 switch */
        break;

    /* 设置定时器超时回调函数的参数 */
    case RT_TIMER_CTRL_SET_PARM:
        /* 将 arg 设置为定时器的参数 */
        timer->parameter = arg;
        /* 跳出 switch */
        break;

    /* 默认分支 */
    default:
        /* 跳出 switch */
        break;
    }
    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(spinlock, level);

    /* 返回成功 */
    return RT_EOK;
}
/* 导出 rt_timer_control 函数符号 */
RTM_EXPORT(rt_timer_control);

/**
 * @brief 此函数将检查定时器列表，如果发生超时事件，
 *        将调用相应的超时函数。
 *
 * @note 此函数应在操作系统定时器中断中被调用。
 */
void rt_timer_check(void)
{
    /* 断言检查：确保在中断中调用此函数 */
    RT_ASSERT(rt_interrupt_get_nest() > 0);

    /* 如果定义了 SMP 宏 */
#ifdef RT_USING_SMP
    /* 仅在核心 0 上运行 */
    if (rt_cpu_get_id() != 0)
    {
        /* 如果不是核心 0，直接返回 */
        return;
    }
/* 结束 ifdef RT_USING_SMP */
#endif

    /* 如果定义了使用软定时器宏 */
#ifdef RT_USING_TIMER_SOFT
    /* 定义返回错误码，初始化为 RT_ERROR */
    rt_err_t ret = RT_ERROR;
    /* 定义下一次超时 tick 变量 */
    rt_tick_t next_timeout;

    /* 获取软定时器的下一次超时 tick */
    ret = _timer_list_next_timeout(_soft_timer_list, &next_timeout);
    /* 如果获取成功且下一次超时时间小于等于当前 tick（已超时） */
    if ((ret == RT_EOK) && (next_timeout <= rt_tick_get()))
    {
        /* 释放软定时器信号量，唤醒软定时器线程 */
        rt_sem_release(&_soft_timer_sem);
    }
/* 结束 ifdef RT_USING_TIMER_SOFT */
#endif
    /* 如果没有定义全软定时器宏 */
#ifndef RT_USING_TIMER_ALL_SOFT
    /* 检查硬定时器列表 */
    _timer_check(_timer_list, &_htimer_lock);
/* 结束 ifndef RT_USING_TIMER_ALL_SOFT */
#endif
}

/**
 * @brief 此函数将返回系统中下一个超时的 tick。
 *
 * @return 系统中下一个超时的 tick
 */
rt_tick_t rt_timer_next_timeout_tick(void)
{
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义硬定时器和软定时器的下一次超时 tick，初始化为最大值 */
    rt_tick_t htimer_next_timeout = RT_TICK_MAX, stimer_next_timeout = RT_TICK_MAX;

    /* 如果没有定义全软定时器宏 */
#ifndef RT_USING_TIMER_ALL_SOFT
    /* 保存中断状态并上硬定时器自旋锁 */
    level = rt_spin_lock_irqsave(&_htimer_lock);
    /* 获取硬定时器的下一次超时 tick */
    _timer_list_next_timeout(_timer_list, &htimer_next_timeout);
    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(&_htimer_lock, level);
/* 结束 ifndef RT_USING_TIMER_ALL_SOFT */
#endif

    /* 如果定义了使用软定时器宏 */
#ifdef RT_USING_TIMER_SOFT
    /* 保存中断状态并上软定时器自旋锁 */
    level = rt_spin_lock_irqsave(&_stimer_lock);
    /* 获取软定时器的下一次超时 tick */
    _timer_list_next_timeout(_soft_timer_list, &stimer_next_timeout);
    /* 解锁自旋锁并恢复中断状态 */
    rt_spin_unlock_irqrestore(&_stimer_lock, level);
/* 结束 ifdef RT_USING_TIMER_SOFT */
#endif

    /* 返回硬定时器和软定时器中较早的超时时间 */
    return htimer_next_timeout < stimer_next_timeout ? htimer_next_timeout : stimer_next_timeout;
}

/* 如果定义了使用软定时器宏 */
#ifdef RT_USING_TIMER_SOFT
/**
 * @brief 系统定时器线程入口
 *
 * @param parameter 是线程的参数
 */
static void _timer_thread_entry(void *parameter)
{
    /* 忽略未使用的参数警告 */
    RT_UNUSED(parameter);

    /* 无限循环 */
    while (1)
    {
        /* 检查软件定时器 */
        _timer_check(_soft_timer_list, &_stimer_lock);
        /* 获取软定时器信号量，永久等待 */
        rt_sem_take(&_soft_timer_sem, RT_WAITING_FOREVER);
    }
}
/* 结束 ifdef RT_USING_TIMER_SOFT */
#endif /* RT_USING_TIMER_SOFT */

/**
 * @ingroup group_system_init
 *
 * @brief 此函数将初始化系统定时器
 */
void rt_system_timer_init(void)
{
    /* 如果没有定义全软定时器宏 */
#ifndef RT_USING_TIMER_ALL_SOFT
    /* 定义循环变量 i */
    rt_size_t i;

    /* 遍历硬定时器列表数组 */
    for (i = 0; i < sizeof(_timer_list) / sizeof(_timer_list[0]); i++)
    {
        /* 初始化硬定时器列表的每一个链表 */
        rt_list_init(_timer_list + i);
    }

    /* 初始化硬定时器自旋锁 */
    rt_spin_lock_init(&_htimer_lock);
/* 结束 ifndef RT_USING_TIMER_ALL_SOFT */
#endif
}

/**
 * @ingroup group_system_init
 *
 * @brief 此函数将初始化系统定时器线程
 */
void rt_system_timer_thread_init(void)
{
    /* 如果定义了使用软定时器宏 */
#ifdef RT_USING_TIMER_SOFT
    /* 定义循环变量 i */
    int i;

    /* 遍历软定时器列表数组 */
    for (i = 0;
         i < sizeof(_soft_timer_list) / sizeof(_soft_timer_list[0]);
         i++)
    {
        /* 初始化软定时器列表的每一个链表 */
        rt_list_init(_soft_timer_list + i);
    }
    /* 初始化软定时器自旋锁 */
    rt_spin_lock_init(&_stimer_lock);
    /* 初始化软定时器信号量 */
    rt_sem_init(&_soft_timer_sem, "stimer", 0, RT_IPC_FLAG_PRIO);
    /* 设置信号量的最大值限制为 1 */
    rt_sem_control(&_soft_timer_sem, RT_IPC_CMD_SET_VLIMIT, (void*)1);
    /* 启动软件定时器线程 */
    rt_thread_init(&_timer_thread,
                   "timer",
                   _timer_thread_entry,
                   RT_NULL,
                   &_timer_thread_stack[0],
                   sizeof(_timer_thread_stack),
                   RT_TIMER_THREAD_PRIO,
                   10);

    /* 启动该线程 */
    rt_thread_startup(&_timer_thread);
/* 结束 ifdef RT_USING_TIMER_SOFT */
#endif /* RT_USING_TIMER_SOFT */
}

/**@}*/
