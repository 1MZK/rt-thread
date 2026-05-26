/*
 * 版权所有 (c) 2006-2022, RT-Thread 开发团队
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更日志:
 * 日期           作者       备注
 * 2006-03-14     Bernard      第一版
 * 2006-04-25     Bernard      实现信号量
 * 2006-05-03     Bernard      增加 RT_IPC_DEBUG
 *                             将 IPC 等待时间的类型修改为 rt_int32_t
 * 2006-05-10     Bernard      修复获取信号量 bug 并增加 IPC 对象
 * 2006-05-12     Bernard      实现邮箱和消息队列
 * 2006-05-20     Bernard      实现互斥量
 * 2006-05-23     Bernard      实现快速事件
 * 2006-05-24     Bernard      实现事件
 * 2006-06-03     Bernard      修复线程定时器初始化 bug
 * 2006-06-05     Bernard      修复互斥量释放 bug
 * 2006-06-07     Bernard      修复消息队列发送 bug
 * 2006-08-04     Bernard      增加钩子支持
 * 2009-05-21     Yi.qiu       修复信号量释放 bug
 * 2009-07-18     Bernard      修复事件清除 bug
 * 2009-09-09     Bernard      移除快速事件并修复 ipc 释放 bug
 * 2009-10-10     Bernard      将信号量和互斥量的值改为无符号值
 * 2009-10-25     Bernard      如果重新计算的 delta tick 为负数，
 *                             将 mb/mq 接收超时更改为 0。
 * 2009-12-16     Bernard      修复当 IPC 标志为 RT_IPC_FLAG_PRIO 时的
 *                             rt_ipc_object_suspend 问题
 * 2010-01-20     mbbill       移除 rt_ipc_object_decrease 函数。
 * 2010-04-20     Bernard      在 mq 中将 memcpy 移到中断禁用之外
 * 2010-10-26     yi.qiu       在 rt_mp_delete 和 rt_mq_delete 中增加模块支持
 * 2010-11-10     Bernard      增加 IPC 复位命令的实现。
 * 2011-12-18     Bernard      在消息队列中增加更多参数检查
 * 2013-09-14     Grissiom     在 rt_event_recv 中增加选项检查
 * 2018-10-02     Bernard      为邮箱增加 64 位支持
 * 2019-09-16     tyx          为消息队列增加发送等待支持
 * 2020-07-29     Meco Man     修复接收没有挂起的事件时的 thread->event_set/event_info
 * 2020-10-11     Meco Man     增加值溢出检查代码
 * 2021-01-03     Meco Man     实现 rt_mb_urgent()
 * 2021-05-30     Meco Man     实现 rt_mutex_trytake()
 * 2022-01-07     Gabriel      将 __on_rt_xxxxx_hook 移动到 ipc.c
 * 2022-01-24     THEWON       让 rt_mutex_take 在使用信号时返回 thread->error
 * 2022-04-08     Stanley      修正描述
 * 2022-10-15     Bernard      增加嵌套互斥量特性
 * 2022-10-16     Bernard      在互斥量中增加优先级上限特性
 * 2023-04-16     Xin-zheqi    重新设计队列接收和发送函数，返回真实消息大小
 * 2023-09-15     xqyjlj       优化 rt_hw_interrupt_disable/enable 性能
 */

#include <rtthread.h>
#include <rthw.h>

#define DBG_TAG           "kernel.ipc" /* 调试标签 */
#define DBG_LVL           DBG_INFO    /* 调试级别 */
#include <rtdbg.h>

/* 获取消息字节数据的地址 */
#define GET_MESSAGEBYTE_ADDR(msg)               ((struct rt_mq_message *) msg + 1)
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
extern void (*rt_object_trytake_hook)(struct rt_object *object); /* 对象尝试获取钩子 */
extern void (*rt_object_take_hook)(struct rt_object *object);    /* 对象获取钩子 */
extern void (*rt_object_put_hook)(struct rt_object *object);     /* 对象放入钩子 */
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_thread_comm
 * @{
 */

/**
 * @brief    此函数将初始化一个 IPC 对象，例如信号量、互斥量、消息队列和邮箱。
 *
 * @note     执行此函数将完成 ipc 对象的挂起线程列表的初始化。
 *
 * @param    ipc 是指向 IPC 对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           当返回值为其他任何值时，表示初始化失败。
 *
 * @warning  此函数可以从所有 IPC 初始化和创建中调用。
 */
rt_inline rt_err_t _ipc_object_init(struct rt_ipc_object *ipc)
{
    /* 初始化 ipc 对象 */
    rt_list_init(&(ipc->suspend_thread));

    return RT_EOK;
}


/**
 * @brief   将线程从挂起列表中出队并将其设置为就绪态。这两个操作被
 *          视为一个原子操作，因此如果返回了一个线程，则它是
 *          被我们恢复的，而不是任何其他线程或异步事件。这非常有用
 *          如果消费者可能会被超时、信号等恢复，除了它的
 *          生产者之外。
 *
 * @param   susp_list 线程出队的列表。如果没有列表则为 RT_NULL。
 * @param   thread_error 恢复线程的线程错误码。
 *          此集合中的负值将被丢弃，线程错误
 *          将不会被改变。
 *
 * @return  struct rt_thread * 如果失败则为 RT_NULL，否则为恢复的线程
 */
struct rt_thread *rt_susp_list_dequeue(rt_list_t *susp_list, rt_err_t thread_error)
{
    rt_sched_lock_level_t slvl; /* 调度器锁级别 */
    rt_thread_t thread;         /* 线程指针 */
    rt_err_t error;             /* 错误码 */

    RT_SCHED_DEBUG_IS_UNLOCKED; /* 调试断言：调度器未锁 */
    RT_ASSERT(susp_list != RT_NULL); /* 断言：挂起列表不为空 */

    rt_sched_lock(&slvl); /* 锁定调度器 */
    if (!rt_list_isempty(susp_list)) /* 如果挂起列表不为空 */
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(susp_list->next); /* 获取列表中的下一个线程 */
        error = rt_sched_thread_ready(thread); /* 将线程设为就绪态 */

        if (error) /* 如果设为就绪态失败 */
        {
            LOG_D("%s [error:%d] failed to resume thread:%p from suspended list",
                  __func__, error, thread); /* 打印调试日志 */

            thread = RT_NULL; /* 返回空指针 */
        }
        else /* 如果设为就绪态成功 */
        {
            /* 线程错误不应该是一个负值 */
            if (thread_error >= 0)
            {
                /* 设置线程错误码以通知恢复的线程 */
                thread->error = thread_error;
            }
        }
    }
    else /* 如果挂起列表为空 */
    {
        thread = RT_NULL; /* 返回空指针 */
    }
    rt_sched_unlock(slvl); /* 解锁调度器 */

    LOG_D("resume thread:%s\n", thread->parent.name); /* 打印调试日志 */

    return thread; /* 返回线程指针 */
}


/**
 * @brief   此函数将恢复 IPC 对象列表中的所有挂起线程，
 *          包括 IPC 对象的挂起列表，以及邮箱的私有列表等。
 *
 * @note    此函数将恢复 IPC 对象列表中的所有线程。
 *          相比之下，rt_ipc_list_resume() 函数将恢复 IPC 对象列表中的一个挂起线程。
 *
 * @param   susp_list 是指向 IPC 对象的挂起线程列表的指针。
 * @param   thread_error 恢复线程的线程错误码。
 *          此集合中的负值将被丢弃，线程错误
 *          将不会被改变。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，函数执行成功。
 *           当返回值为其他任何值时，表示此操作失败。
 *
 */
rt_err_t rt_susp_list_resume_all(rt_list_t *susp_list, rt_err_t thread_error)
{
    struct rt_thread *thread; /* 线程指针 */

    RT_SCHED_DEBUG_IS_UNLOCKED; /* 调试断言：调度器未锁 */

    /* 唤醒所有挂起的线程 */
    thread = rt_susp_list_dequeue(susp_list, thread_error); /* 从列表中出队一个线程 */
    while (thread) /* 如果出队成功 */
    {
        /*
         * 恢复下一个线程
         * 在 rt_thread_resume 函数中，它将当前线程从
         * 挂起列表中移除
         */
        thread = rt_susp_list_dequeue(susp_list, thread_error); /* 继续出队下一个线程 */
    }

    return RT_EOK; /* 返回成功 */
}

/**
 * @brief   此函数将恢复 IPC 对象列表中的所有挂起线程，
 *          包括 IPC 对象的挂起列表，以及邮箱的私有列表等。
 *          操作期间传递并持有一个锁。
 *
 * @note    此函数将恢复 IPC 对象列表中的所有线程。
 *          相比之下，rt_ipc_list_resume() 函数将恢复 IPC 对象列表中的一个挂起线程。
 *
 * @param   susp_list 是指向 IPC 对象的挂起线程列表的指针。
 * @param   thread_error 恢复线程的线程错误码。
 *          此集合中的负值将被丢弃，线程错误
 *          将不会被改变。
 * @param   lock 操作 susp_list 时持有的锁
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，函数执行成功。
 *           当返回值为其他任何值时，表示此操作失败。
 *
 */
rt_err_t rt_susp_list_resume_all_irq(rt_list_t *susp_list,
                                     rt_err_t thread_error,
                                     struct rt_spinlock *lock)
{
    struct rt_thread *thread; /* 线程指针 */
    rt_base_t level; /* 中断级别 */

    RT_SCHED_DEBUG_IS_UNLOCKED; /* 调试断言：调度器未锁 */

    do
    {
        level = rt_spin_lock_irqsave(lock); /* 保存中断状态并锁自旋锁 */

        /*
         * 恢复下一个线程
         * 在 rt_thread_resume 函数中，它将当前线程从
         * 挂起列表中移除
         */
        thread = rt_susp_list_dequeue(susp_list, thread_error); /* 从列表中出队一个线程 */

        rt_spin_unlock_irqrestore(lock, level); /* 解自旋锁并恢复中断状态 */
    }
    while (thread); /* 如果出队成功，则继续循环 */

    return RT_EOK; /* 返回成功 */
}

/**
 * @brief   将线程添加到挂起列表
 *
 * @note    调用者必须持有调度器锁
 *
 * @param   susp_list 线程入队的列表
 * @param   thread 挂起的线程
 * @param   ipc_flags 挂起列表的模式
 * @return  成功返回 RT_EOK，否则失败
 */
rt_err_t rt_susp_list_enqueue(rt_list_t *susp_list, rt_thread_t thread, int ipc_flags)
{
    RT_SCHED_DEBUG_IS_LOCKED; /* 调试断言：调度器已锁 */

    switch (ipc_flags) /* 根据 IPC 标志分支 */
    {
    case RT_IPC_FLAG_FIFO: /* 先进先出模式 */
        rt_list_insert_before(susp_list, &RT_THREAD_LIST_NODE(thread)); /* 将线程插入列表尾部 */
        break; /* RT_IPC_FLAG_FIFO */

    case RT_IPC_FLAG_PRIO: /* 优先级模式 */
        {
            struct rt_list_node *n; /* 列表节点指针 */
            struct rt_thread *sthread; /* 比较用的线程指针 */

            /* 寻找合适的位置 */
            for (n = susp_list->next; n != susp_list; n = n->next) /* 遍历挂起列表 */
            {
                sthread = RT_THREAD_LIST_NODE_ENTRY(n); /* 获取节点对应的线程 */

                /* 找到了 */
                if (rt_sched_thread_get_curr_prio(thread) < rt_sched_thread_get_curr_prio(sthread)) /* 如果当前线程优先级更高 */
                {
                    /* 将此线程插入到 sthread 之前 */
                    rt_list_insert_before(&RT_THREAD_LIST_NODE(sthread), &RT_THREAD_LIST_NODE(thread));
                    break; /* 跳出循环 */
                }
            }

            /*
             * 没有找到合适的位置,
             * 追加到 suspend_thread 列表的末尾
             */
            if (n == susp_list)
                rt_list_insert_before(susp_list, &RT_THREAD_LIST_NODE(thread));
        }
        break;/* RT_IPC_FLAG_PRIO */

    default: /* 默认情况 */
        RT_ASSERT(0); /* 断言失败 */
        break;
    }

    return RT_EOK; /* 返回成功 */
}

/**
 * @brief   将挂起列表上的线程打印到系统控制台
 */
void rt_susp_list_print(rt_list_t *list)
{
#ifdef RT_USING_CONSOLE /* 如果使用控制台 */
    rt_sched_lock_level_t slvl; /* 调度器锁级别 */
    struct rt_thread *thread; /* 线程指针 */
    struct rt_list_node *node; /* 列表节点指针 */

    rt_sched_lock(&slvl); /* 锁定调度器 */

    for (node = list->next; node != list; node = node->next) /* 遍历列表 */
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(node); /* 获取节点对应的线程 */
        rt_kprintf("%.*s", RT_NAME_MAX, thread->parent.name); /* 打印线程名 */

        if (node->next != list) /* 如果不是最后一个节点 */
            rt_kprintf("/"); /* 打印分隔符 */
    }

    rt_sched_unlock(slvl); /* 解锁调度器 */
#else
    (void)list; /* 避免未使用变量警告 */
#endif
}


#ifdef RT_USING_SEMAPHORE /* 如果使用信号量 */
/**
 * @addtogroup group_semaphore Semaphore 信号量
 * @{
 */

/* 信号量对象初始化 */
static void _sem_object_init(rt_sem_t       sem,
                             rt_uint16_t    value,
                             rt_uint8_t     flag,
                             rt_uint16_t    max_value)
{
    /* 初始化 ipc 对象 */
    _ipc_object_init(&(sem->parent));

    sem->max_value = max_value; /* 设置最大值 */
    /* 设置初始值 */
    sem->value = value;

    /* 设置父对象 */
    sem->parent.parent.flag = flag; /* 设置标志 */
    rt_spin_lock_init(&(sem->spinlock)); /* 初始化自旋锁 */
}

/**
 * @brief    此函数将初始化一个静态信号量对象。
 *
 * @note     对于静态信号量对象，其内存空间是在编译时由编译器分配的，
 *           并且应放置在读写数据段或未初始化数据段上。
 *           相比之下，rt_sem_create() 函数会自动分配内存空间并初始化信号量。
 *
 * @see      rt_sem_create()
 *
 * @param    sem 是指向要初始化的信号量的指针。假定信号量的存储空间将
 *           在您的应用程序中分配。
 *
 * @param    name 是指向您希望赋予信号量的名称的指针。
 *
 * @param    value 是信号量的初始值。
 *           如果用于共享资源，应将值初始化为可用资源的数量。
 *           如果用于发出事件发生的信号，应将值初始化为 0。
 *
 * @param    flag 是信号量标志，它决定了当信号量不可用时多个线程等待的排队方式。
 *           信号量标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                         (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此信号量的所有线程
 *               将成为非实时线程。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示初始化失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_err_t rt_sem_init(rt_sem_t    sem,
                     const char *name,
                     rt_uint32_t value,
                     rt_uint8_t  flag)
{
    RT_ASSERT(sem != RT_NULL); /* 断言：信号量指针不为空 */
    RT_ASSERT(value < 0x10000U); /* 断言：值小于 0x10000U */
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    /* 初始化对象 */
    rt_object_init(&(sem->parent.parent), RT_Object_Class_Semaphore, name);

    _sem_object_init(sem, value, flag, RT_SEM_VALUE_MAX); /* 调用内部初始化函数 */

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_sem_init); /* 导出函数符号 */


/**
 * @brief    此函数将脱离一个静态信号量对象。
 *
 * @note     此函数用于脱离由 rt_sem_init() 函数初始化的静态信号量对象。
 *           相比之下，rt_sem_delete() 函数将删除一个信号量对象。
 *           当信号量成功脱离时，它将恢复信号量列表中的所有挂起线程。
 *
 * @see      rt_sem_delete()
 *
 * @param    sem 是指向要脱离的信号量对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示信号量脱离失败。
 *
 * @warning  此函数只能脱离由 rt_sem_init() 函数初始化的静态信号量。
 *           如果信号量是由 rt_sem_create() 函数创建的，您绝不能使用此函数脱离它,
 *           只能使用 rt_sem_delete() 函数完成删除。
 */
rt_err_t rt_sem_detach(rt_sem_t sem)
{
    rt_base_t level; /* 中断级别 */

    /* 参数检查 */
    RT_ASSERT(sem != RT_NULL); /* 断言：信号量指针不为空 */
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore); /* 断言：对象类型为信号量 */
    RT_ASSERT(rt_object_is_systemobject(&sem->parent.parent)); /* 断言：是系统对象(静态对象) */

    level = rt_spin_lock_irqsave(&(sem->spinlock)); /* 保存中断状态并锁自旋锁 */
    /* 唤醒所有挂起的线程 */
    rt_susp_list_resume_all(&(sem->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 脱离信号量对象 */
    rt_object_detach(&(sem->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_sem_detach); /* 导出函数符号 */

#ifdef RT_USING_HEAP /* 如果使用堆 */
/**
 * @brief    创建一个信号量对象。
 *
 * @note     对于信号量对象，其内存空间是自动分配的。
 *           相比之下，rt_sem_init() 函数将初始化一个静态信号量对象。
 *
 * @see      rt_sem_init()
 *
 * @param    name 是指向您希望赋予信号量的名称的指针。
 *
 * @param    value 是信号量的初始值。
 *           如果用于共享资源，应将值初始化为可用资源的数量。
 *           如果用于发出事件发生的信号，应将值初始化为 0。
 *
 * @param    flag 是信号量标志，它决定了当信号量不可用时多个线程等待的排队方式。
 *           信号量标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                         (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此信号量的所有线程
 *               将成为非实时线程。
 *
 * @return   返回指向信号量对象的指针。当返回值为 RT_NULL 时，表示创建失败。
 *
 * @warning  此函数不能在中断上下文中调用。您可以使用宏 RT_DEBUG_NOT_IN_INTERRUPT 来检查。
 */
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag)
{
    rt_sem_t sem; /* 信号量指针 */

    RT_ASSERT(value < 0x10000U); /* 断言：值小于 0x10000U */
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    /* 分配对象 */
    sem = (rt_sem_t)rt_object_allocate(RT_Object_Class_Semaphore, name);
    if (sem == RT_NULL) /* 如果分配失败 */
        return sem; /* 返回空指针 */

    _sem_object_init(sem, value, flag, RT_SEM_VALUE_MAX); /* 调用内部初始化函数 */

    return sem; /* 返回信号量指针 */
}
RTM_EXPORT(rt_sem_create); /* 导出函数符号 */


/**
 * @brief    此函数将删除一个信号量对象并释放内存空间。
 *
 * @note     此函数用于删除由 rt_sem_create() 函数创建的信号量对象。
 *           相比之下，rt_sem_detach() 函数将脱离一个静态信号量对象。
 *           当信号量成功删除时，它将恢复信号量列表中的所有挂起线程。
 *
 * @see      rt_sem_detach()
 *
 * @param    sem 是指向要删除的信号量对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示信号量脱离失败。
 *
 * @warning  此函数只能删除由 rt_sem_create() 函数初始化的信号量。
 *           如果信号量是由 rt_sem_init() 函数初始化的，您绝不能使用此函数删除它,
 *           只能使用 rt_sem_detach() 函数完成脱离。
 */
rt_err_t rt_sem_delete(rt_sem_t sem)
{
    rt_ubase_t level; /* 中断级别 */

    /* 参数检查 */
    RT_ASSERT(sem != RT_NULL); /* 断言：信号量指针不为空 */
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore); /* 断言：对象类型为信号量 */
    RT_ASSERT(rt_object_is_systemobject(&sem->parent.parent) == RT_FALSE); /* 断言：不是系统对象(动态对象) */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    level = rt_spin_lock_irqsave(&(sem->spinlock)); /* 保存中断状态并锁自旋锁 */
    /* 唤醒所有挂起的线程 */
    rt_susp_list_resume_all(&(sem->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 删除信号量对象 */
    rt_object_delete(&(sem->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_sem_delete); /* 导出函数符号 */
#endif /* RT_USING_HEAP */


/**
 * @brief    此函数将获取一个信号量，如果信号量不可用，线程将等待
 *           该信号量直到指定的时间。
 *
 * @note     当调用此函数时，sem->value 的计数值将减 1 直到它等于 0。
 *           当 sem->value 为 0 时，表示信号量不可用。此时，它将挂起
 *           准备获取信号量的线程。
 *           相反，rt_sem_release() 函数每次会将 sem->value 的计数值加 1。
 *
 * @see      rt_sem_trytake()
 *
 * @param    sem 是指向信号量对象的指针。
 *
 * @param    timeout 是超时时间（单位：OS tick）。如果信号量不可用，线程将等待
 *           该信号量直到由此参数指定的时间量。
 *
 *           注意:
 *           如果使用宏 RT_WAITING_FOREVER 设置此参数，意味着当
 *           队列中消息不可用时，线程将永远等待。
 *           如果使用宏 RT_WAITING_NO 设置此参数，意味着此
 *           函数是非阻塞的，将立即返回。
 *
 * @return   返回操作状态。只有当返回值为 RT_EOK 时，操作才成功。
 *           如果返回值为其他任何值，则表示获取信号量失败。
 *
 * @warning  此函数只能在线程上下文中调用。绝不能在中断上下文中调用。
 */
static rt_err_t _rt_sem_take(rt_sem_t sem, rt_int32_t timeout, int suspend_flag)
{
    rt_base_t level; /* 中断级别 */
    struct rt_thread *thread; /* 线程指针 */
    rt_err_t ret; /* 返回值 */

    /* 参数检查 */
    RT_ASSERT(sem != RT_NULL); /* 断言：信号量指针不为空 */
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore); /* 断言：对象类型为信号量 */

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(sem->parent.parent))); /* 调用尝试获取钩子 */

    /* 当前上下文检查 */
    RT_DEBUG_SCHEDULER_AVAILABLE(1); /* 调试断言：调度器可用 */

    level = rt_spin_lock_irqsave(&(sem->spinlock)); /* 保存中断状态并锁自旋锁 */

    LOG_D("thread %s take sem:%s, which value is: %d",
          rt_thread_self()->parent.name,
          sem->parent.parent.name,
          sem->value); /* 打印调试日志 */

    if (sem->value > 0) /* 如果信号量可用 */
    {
        /* 信号量可用 */
        sem->value --; /* 信号量值减 1 */
        rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */
    }
    else /* 如果信号量不可用 */
    {
        /* 不等待，返回超时 */
        if (timeout == 0) /* 如果超时时间为 0 */
        {
            rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */
            return -RT_ETIMEOUT; /* 返回超时错误 */
        }
        else /* 如果需要等待 */
        {
            /* 信号量不可用，压入挂起列表 */
            /* 获取当前线程 */
            thread = rt_thread_self();

            /* 重置线程错误码 */
            thread->error = RT_EINTR;

            LOG_D("sem take: suspend thread - %s", thread->parent.name); /* 打印调试日志 */

            /* 挂起线程 */
            ret = rt_thread_suspend_to_list(thread, &(sem->parent.suspend_thread),
                                            sem->parent.parent.flag, suspend_flag);
            if (ret != RT_EOK) /* 如果挂起失败 */
            {
                rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */
                return ret; /* 返回错误码 */
            }

            /* 有等待时间，启动线程定时器 */
            if (timeout > 0) /* 如果超时时间大于 0 */
            {
                rt_tick_t timeout_tick = timeout; /* 超时 tick 数 */
                LOG_D("set thread:%s to timer list", thread->parent.name); /* 打印调试日志 */

                /* 重置线程定时器的超时并启动它 */
                rt_timer_control(&(thread->thread_timer),
                                 RT_TIMER_CTRL_SET_TIME,
                                 &timeout_tick);
                rt_timer_start(&(thread->thread_timer));
            }

            /* 使能中断 */
            rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */

            /* 执行调度 */
            rt_schedule();

            if (thread->error != RT_EOK) /* 如果线程错误码不为 RT_EOK */
            {
                return thread->error > 0 ? -thread->error : thread->error; /* 返回错误码 */
            }
        }
    }

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(sem->parent.parent))); /* 调用获取钩子 */

    return RT_EOK; /* 返回成功 */
}

/* 获取信号量(不可中断) */
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_sem_take); /* 导出函数符号 */

/* 获取信号量(可被中断打断) */
rt_err_t rt_sem_take_interruptible(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_sem_take_interruptible); /* 导出函数符号 */

/* 获取信号量(可被致命信号打断) */
rt_err_t rt_sem_take_killable(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_KILLABLE);
}
RTM_EXPORT(rt_sem_take_killable); /* 导出函数符号 */

/**
 * @brief    此函数将尝试获取一个信号量，如果信号量不可用，线程立即返回。
 *
 * @note     此函数与 rt_sem_take() 函数非常相似，当信号量不可用时,
 *           rt_sem_trytake() 函数将立即返回而不等待超时。
 *           换句话说，rt_sem_trytake(sem) 与 rt_sem_take(sem, 0) 效果相同。
 *
 * @see      rt_sem_take()
 *
 * @param    sem 是指向信号量对象的指针。
 *
 * @return   返回操作状态。只有当返回值为 RT_EOK 时，操作才成功。
 *           如果返回值为其他任何值，则表示获取信号量失败。
 */
rt_err_t rt_sem_trytake(rt_sem_t sem)
{
    return rt_sem_take(sem, RT_WAITING_NO); /* 不等待获取信号量 */
}
RTM_EXPORT(rt_sem_trytake); /* 导出函数符号 */


/**
 * @brief    此函数将释放一个信号量。如果有线程挂起在该信号量上，它将被恢复。
 *
 * @note     如果有线程挂起在此信号量上，此信号量对象列表中的第一个线程
 *           将被恢复，并执行线程调度 (rt_schedule)。
 *           如果没有线程挂起在此信号量上，此信号量的计数值 sem->value 将增加 1。
 *
 * @param    sem 是指向信号量对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示释放信号量失败。
 */
rt_err_t rt_sem_release(rt_sem_t sem)
{
    rt_base_t level; /* 中断级别 */
    rt_bool_t need_schedule; /* 是否需要调度 */

    /* 参数检查 */
    RT_ASSERT(sem != RT_NULL); /* 断言：信号量指针不为空 */
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore); /* 断言：对象类型为信号量 */

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(sem->parent.parent))); /* 调用放入钩子 */

    need_schedule = RT_FALSE; /* 初始化为不需要调度 */

    level = rt_spin_lock_irqsave(&(sem->spinlock)); /* 保存中断状态并锁自旋锁 */

    LOG_D("thread %s releases sem:%s, which value is: %d",
          rt_thread_self()->parent.name,
          sem->parent.parent.name,
          sem->value); /* 打印调试日志 */

    if (!rt_list_isempty(&sem->parent.suspend_thread)) /* 如果挂起列表不为空 */
    {
        /* 恢复挂起的线程 */
        rt_susp_list_dequeue(&(sem->parent.suspend_thread), RT_EOK);
        need_schedule = RT_TRUE; /* 需要调度 */
    }
    else /* 如果没有挂起的线程 */
    {
        if(sem->value < sem->max_value) /* 如果信号量值未达到最大值 */
        {
            sem->value ++; /* 增加值 */
        }
        else /* 如果信号量值已达到最大值 */
        {
            rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */
            return -RT_EFULL; /* 值溢出 */
        }
    }

    rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 恢复了一个线程，重新调度 */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_sem_release); /* 导出函数符号 */


/**
 * @brief    此函数将设置信号量对象的一些额外属性。
 *
 * @note     目前此函数仅支持 RT_IPC_CMD_RESET 命令来重置信号量。
 *
 * @param    sem 是指向信号量对象的指针。
 *
 * @param    cmd 是用于配置信号量某些属性的命令字。
 *
 * @param    arg 是执行命令的函数的参数。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示此函数执行失败。
 */
rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg)
{
    rt_base_t level; /* 中断级别 */

    /* 参数检查 */
    RT_ASSERT(sem != RT_NULL); /* 断言：信号量指针不为空 */
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore); /* 断言：对象类型为信号量 */

    if (cmd == RT_IPC_CMD_RESET) /* 如果是复位命令 */
    {
        rt_ubase_t value; /* 值 */

        /* 获取值 */
        value = (rt_uintptr_t)arg;
        level = rt_spin_lock_irqsave(&(sem->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 恢复所有等待的线程 */
        rt_susp_list_resume_all(&sem->parent.suspend_thread, RT_ERROR);

        /* 设置新值 */
        sem->value = (rt_uint16_t)value;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */
        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }
    else if (cmd == RT_IPC_CMD_SET_VLIMIT) /* 如果是设置值上限命令 */
    {
        rt_ubase_t max_value; /* 最大值 */
        rt_bool_t need_schedule = RT_FALSE; /* 是否需要调度 */

        max_value = (rt_uint16_t)((rt_uintptr_t)arg); /* 获取最大值 */
        if (max_value > RT_SEM_VALUE_MAX || max_value < 1) /* 如果最大值不合法 */
        {
            return -RT_EINVAL; /* 返回无效参数错误 */
        }

        level = rt_spin_lock_irqsave(&(sem->spinlock)); /* 保存中断状态并锁自旋锁 */
        if (max_value < sem->value) /* 如果新最大值小于当前值 */
        {
            if (!rt_list_isempty(&sem->parent.suspend_thread)) /* 如果有挂起的线程 */
            {
                /* 恢复所有等待的线程 */
                rt_susp_list_resume_all(&sem->parent.suspend_thread, RT_ERROR);
                need_schedule = RT_TRUE; /* 需要调度 */
            }
        }
        /* 设置新值 */
        sem->max_value = max_value;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level); /* 解自旋锁并恢复中断状态 */

        if (need_schedule) /* 如果需要调度 */
        {
            rt_schedule(); /* 重新调度 */
        }

        return RT_EOK; /* 返回成功 */
    }

    return -RT_ERROR; /* 返回错误 */
}
RTM_EXPORT(rt_sem_control); /* 导出函数符号 */

/**@}*/
#endif /* RT_USING_SEMAPHORE */

#ifdef RT_USING_MUTEX /* 如果使用互斥量 */
/* 遍历每个挂起的线程以更新挂起线程中的最高优先级 */
rt_inline rt_uint8_t _mutex_update_priority(struct rt_mutex *mutex)
{
    struct rt_thread *thread; /* 线程指针 */

    if (!rt_list_isempty(&mutex->parent.suspend_thread)) /* 如果挂起列表不为空 */
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next); /* 获取第一个挂起的线程 */
        mutex->priority = rt_sched_thread_get_curr_prio(thread); /* 更新互斥量的优先级为该线程的优先级 */
    }
    else /* 如果挂起列表为空 */
    {
        mutex->priority = 0xff; /* 设置互斥量优先级为最低 */
    }

    return mutex->priority; /* 返回互斥量优先级 */
}

/* 获取其获取的对象及其初始优先级内的最高优先级 */
rt_inline rt_uint8_t _thread_get_mutex_priority(struct rt_thread* thread)
{
    rt_list_t *node = RT_NULL; /* 列表节点指针 */
    struct rt_mutex *mutex = RT_NULL; /* 互斥量指针 */
    rt_uint8_t priority = rt_sched_thread_get_init_prio(thread); /* 获取线程的初始优先级 */

    rt_list_for_each(node, &(thread->taken_object_list)) /* 遍历线程获取的对象列表 */
    {
        mutex = rt_list_entry(node, struct rt_mutex, taken_list); /* 获取互斥量 */
        rt_uint8_t mutex_prio = mutex->priority; /* 获取互斥量优先级 */
        /* 优先级至少为优先级上限 */
        mutex_prio = mutex_prio < mutex->ceiling_priority ? mutex_prio : mutex->ceiling_priority;

        if (priority > mutex_prio) /* 如果当前优先级比互斥量优先级低 */
        {
            priority = mutex_prio; /* 更新优先级为更高的互斥量优先级 */
        }
    }

    return priority; /* 返回最高优先级 */
}

/* 更新目标线程的优先级以及挂起它的线程(如果有) */
rt_inline void _thread_update_priority(struct rt_thread *thread, rt_uint8_t priority, int suspend_flag)
{
    rt_err_t ret = -RT_ERROR; /* 返回值 */
    struct rt_object* pending_obj = RT_NULL; /* 挂起对象指针 */

    LOG_D("thread:%s priority -> %d", thread->parent.name, priority); /* 打印调试日志 */

    /* 改变线程的优先级 */
    ret = rt_sched_thread_change_priority(thread, priority);

    while ((ret == RT_EOK) && rt_sched_thread_is_suspended(thread)) /* 如果改变成功且线程处于挂起状态 */
    {
        /* 是否改变获取的互斥量的优先级 */
        pending_obj = thread->pending_object; /* 获取线程正在等待的对象 */

        if (pending_obj && rt_object_get_type(pending_obj) == RT_Object_Class_Mutex) /* 如果等待的是互斥量 */
        {
            rt_uint8_t mutex_priority = 0xff; /* 互斥量优先级 */
            struct rt_mutex* pending_mutex = (struct rt_mutex *)pending_obj; /* 获取互斥量指针 */

            /* 重新插入线程到挂起线程列表以重新排序优先级列表 */
            rt_list_remove(&RT_THREAD_LIST_NODE(thread));

            ret = rt_susp_list_enqueue(
                &(pending_mutex->parent.suspend_thread), thread,
                pending_mutex->parent.parent.flag); /* 将线程入队 */
            if (ret == RT_EOK) /* 如果入队成功 */
            {
                /* 更新优先级 */
                _mutex_update_priority(pending_mutex);
                /* 改变互斥量拥有者线程的优先级 */
                LOG_D("mutex: %s priority -> %d", pending_mutex->parent.parent.name,
                        pending_mutex->priority); /* 打印调试日志 */

                mutex_priority = _thread_get_mutex_priority(pending_mutex->owner); /* 获取互斥量拥有者的最高优先级 */
                if (mutex_priority != rt_sched_thread_get_curr_prio(pending_mutex->owner)) /* 如果优先级需要改变 */
                {
                    thread = pending_mutex->owner; /* 更新线程为互斥量拥有者 */

                    ret = rt_sched_thread_change_priority(thread, mutex_priority); /* 改变优先级 */
                }
                else /* 如果优先级不需要改变 */
                {
                    ret = -RT_ERROR; /* 设置返回值以退出循环 */
                }
            }
        }
        else /* 如果等待的不是互斥量 */
        {
            ret = -RT_ERROR; /* 设置返回值以退出循环 */
        }
    }
}

/* 检查并更新优先级 */
static rt_bool_t _check_and_update_prio(rt_thread_t thread, rt_mutex_t mutex)
{
    RT_SCHED_DEBUG_IS_LOCKED; /* 调试断言：调度器已锁 */
    rt_bool_t do_sched = RT_FALSE; /* 是否需要调度 */

    if ((mutex->ceiling_priority != 0xFF) || (rt_sched_thread_get_curr_prio(thread) == mutex->priority)) /* 如果互斥量设置了优先级上限或线程优先级与互斥量优先级相同 */
    {
        rt_uint8_t priority = 0xff; /* 优先级 */

        /* 获取线程获取列表中的最高优先级 */
        priority = _thread_get_mutex_priority(thread);

        rt_sched_thread_change_priority(thread, priority); /* 改变线程优先级 */

        /**
         * 通知一个挂起的重新调度。由于调度器已锁定，我们此时
         * 不会真正进行重新调度
         */
        do_sched = RT_TRUE; /* 需要调度 */
    }
    return do_sched; /* 返回是否需要调度 */
}

/* 互斥量删除或脱离前的处理 */
static void _mutex_before_delete_detach(rt_mutex_t mutex)
{
    rt_sched_lock_level_t slvl; /* 调度器锁级别 */
    rt_bool_t need_schedule = RT_FALSE; /* 是否需要调度 */

    rt_spin_lock(&(mutex->spinlock)); /* 锁自旋锁 */
    /* 唤醒所有挂起的线程 */
    rt_susp_list_resume_all(&(mutex->parent.suspend_thread), RT_ERROR);

    rt_sched_lock(&slvl); /* 锁定调度器 */

    /* 从线程的获取列表中移除互斥量 */
    rt_list_remove(&mutex->taken_list);

    /* 是否改变线程优先级 */
    if (mutex->owner) /* 如果互斥量有拥有者 */
    {
        need_schedule = _check_and_update_prio(mutex->owner, mutex); /* 检查并更新优先级 */
    }

    if (need_schedule) /* 如果需要调度 */
    {
        rt_sched_unlock_n_resched(slvl); /* 解锁调度器并重新调度 */
    }
    else /* 如果不需要调度 */
    {
        rt_sched_unlock(slvl); /* 解锁调度器 */
    }

    /* 解锁并在需要时进行必要的重新调度 */
    rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */
}

/**
 * @addtogroup group_mutex Mutex 互斥量
 * @{
 */

/**
 * @brief    初始化一个静态互斥量对象。
 *
 * @note     对于静态互斥量对象，其内存空间是在编译时由编译器分配的，
 *           并且应放置在读写数据段或未初始化数据段上。
 *           相比之下，rt_mutex_create() 函数会自动分配内存空间并初始化互斥量。
 *
 * @see      rt_mutex_create()
 *
 * @param    mutex 是指向要初始化的互斥量的指针。假定互斥量的存储空间将
 *           在您的应用程序中分配。
 *
 * @param    name 是指向赋予互斥量的名称的指针。
 *
 * @param    flag 是互斥量标志，它决定了当互斥量不可用时多个线程等待的排队方式。
 *           注意：此参数已被废弃。它可以是 RT_IPC_FLAG_PRIO、RT_IPC_FLAG_FIFO 或 RT_NULL。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示初始化失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag)
{
    /* flag 参数已被废弃 */
    RT_UNUSED(flag);

    /* 参数检查 */
    RT_ASSERT(mutex != RT_NULL); /* 断言：互斥量指针不为空 */

    /* 初始化对象 */
    rt_object_init(&(mutex->parent.parent), RT_Object_Class_Mutex, name);

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(mutex->parent));

    mutex->owner    = RT_NULL; /* 初始化拥有者为空 */
    mutex->priority = 0xFF; /* 初始化优先级为最低 */
    mutex->hold     = 0; /* 初始化持有次数为 0 */
    mutex->ceiling_priority = 0xFF; /* 初始化优先级上限为 0xFF */
    rt_list_init(&(mutex->taken_list)); /* 初始化获取列表 */

    /* flag 只能是 RT_IPC_FLAG_PRIO。RT_IPC_FLAG_FIFO 无法解决无界优先级反转问题 */
    mutex->parent.parent.flag = RT_IPC_FLAG_PRIO;
    rt_spin_lock_init(&(mutex->spinlock)); /* 初始化自旋锁 */

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mutex_init); /* 导出函数符号 */


/**
 * @brief    此函数将脱离一个静态互斥量对象。
 *
 * @note     此函数用于脱离由 rt_mutex_init() 函数初始化的静态互斥量对象。
 *           相比之下，rt_mutex_delete() 函数将删除一个互斥量对象。
 *           当互斥量成功脱离时，它将恢复互斥量列表中的所有挂起线程。
 *
 * @see      rt_mutex_delete()
 *
 * @param    mutex 是指向要脱离的互斥量对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示互斥量脱离失败。
 *
 * @warning  此函数只能脱离由 rt_mutex_init() 函数初始化的静态互斥量。
 *           如果互斥量是由 rt_mutex_create() 函数创建的，您绝不能使用此函数脱离它,
 *           只能使用 rt_mutex_delete() 函数完成删除。
 */
rt_err_t rt_mutex_detach(rt_mutex_t mutex)
{
    /* 参数检查 */
    RT_ASSERT(mutex != RT_NULL); /* 断言：互斥量指针不为空 */
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex); /* 断言：对象类型为互斥量 */
    RT_ASSERT(rt_object_is_systemobject(&mutex->parent.parent)); /* 断言：是系统对象 */

    _mutex_before_delete_detach(mutex); /* 执行删除/脱离前的处理 */

    /* 脱离互斥量对象 */
    rt_object_detach(&(mutex->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mutex_detach); /* 导出函数符号 */

/* 从互斥量的挂起列表中丢弃一个线程 */

/**
 * @brief 从互斥量的挂起列表中丢弃一个线程
 *
 * @param mutex 是指向互斥量对象的指针。
 * @param thread 是应该从互斥量中丢弃的线程。
 */
void rt_mutex_drop_thread(rt_mutex_t mutex, rt_thread_t thread)
{
    rt_uint8_t priority; /* 优先级 */
    rt_bool_t need_update = RT_FALSE; /* 是否需要更新优先级 */
    rt_sched_lock_level_t slvl; /* 调度器锁级别 */

    /* 参数检查 */
    RT_DEBUG_IN_THREAD_CONTEXT; /* 调试断言：在线程上下文中 */
    RT_ASSERT(mutex != RT_NULL); /* 断言：互斥量指针不为空 */
    RT_ASSERT(thread != RT_NULL); /* 断言：线程指针不为空 */

    rt_spin_lock(&(mutex->spinlock)); /* 锁自旋锁 */

    RT_ASSERT(thread->pending_object == &mutex->parent.parent); /* 断言：线程等待的对象是该互斥量 */

    rt_sched_lock(&slvl); /* 锁定调度器 */

    /* 从挂起列表中脱离 */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));

    /**
     * 应该改变互斥量拥有者线程的优先级
     * 注意：当前线程从互斥量挂起列表中脱离后，有
     *       可能互斥量拥有者已经释放了互斥量。这
     *       意味着 mutex->owner 在此时可能为空。如果发生了这种情况,
     *       拥有者已经重置了其优先级。所以跳过是可以的
     */
    if (mutex->owner && rt_sched_thread_get_curr_prio(mutex->owner) ==
                            rt_sched_thread_get_curr_prio(thread)) /* 如果拥有者优先级与当前线程优先级相同 */
    {
        need_update = RT_TRUE; /* 需要更新优先级 */
    }

    /* 更新互斥量的优先级 */
    if (!rt_list_isempty(&mutex->parent.suspend_thread)) /* 如果挂起列表不为空 */
    {
        /* 列表中有更多挂起的线程 */
        struct rt_thread *th; /* 线程指针 */

        th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next); /* 获取第一个挂起的线程 */
        /* 更新互斥量的优先级 */
        mutex->priority = rt_sched_thread_get_curr_prio(th);
    }
    else /* 如果挂起列表为空 */
    {
        /* 设置互斥量优先级为最大优先级(最低) */
        mutex->priority = 0xff;
    }

    /* 尝试改变互斥量拥有者线程的优先级 */
    if (need_update) /* 如果需要更新 */
    {
        /* 获取线程中互斥量的最大优先级 */
        priority = _thread_get_mutex_priority(mutex->owner);
        if (priority != rt_sched_thread_get_curr_prio(mutex->owner)) /* 如果优先级需要改变 */
        {
            _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE); /* 更新优先级 */
        }
    }

    rt_sched_unlock(slvl); /* 解锁调度器 */
    rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */
}


/**
 * @brief 设置互斥量的优先级上限属性。
 *
 * @param mutex 是指向互斥量对象的指针。
 * @param priority 是应该设置给互斥量的优先级。
 *
 * @return 返回旧的优先级上限
 */
rt_uint8_t rt_mutex_setprioceiling(rt_mutex_t mutex, rt_uint8_t priority)
{
    rt_uint8_t ret_priority = 0xFF; /* 返回的优先级 */
    rt_uint8_t highest_prio; /* 最高优先级 */
    rt_sched_lock_level_t slvl; /* 调度器锁级别 */

    RT_DEBUG_IN_THREAD_CONTEXT; /* 调试断言：在线程上下文中 */

    if ((mutex) && (priority < RT_THREAD_PRIORITY_MAX)) /* 如果互斥量存在且优先级合法 */
    {
        /* 如果对一个互斥量发生多次更新，这里是临界区 */
        rt_spin_lock(&(mutex->spinlock)); /* 锁自旋锁 */
        ret_priority = mutex->ceiling_priority; /* 保存旧的优先级上限 */
        mutex->ceiling_priority = priority; /* 设置新的优先级上限 */
        if (mutex->owner) /* 如果互斥量有拥有者 */
        {
            rt_sched_lock(&slvl); /* 锁定调度器 */
            highest_prio = _thread_get_mutex_priority(mutex->owner); /* 获取最高优先级 */
            if (highest_prio != rt_sched_thread_get_curr_prio(mutex->owner)) /* 如果需要更新优先级 */
            {
                _thread_update_priority(mutex->owner, highest_prio, RT_UNINTERRUPTIBLE); /* 更新优先级 */
            }
            rt_sched_unlock(slvl); /* 解锁调度器 */
        }
        rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */
    }
    else /* 如果参数不合法 */
    {
        rt_set_errno(-RT_EINVAL); /* 设置错误码 */
    }

    return ret_priority; /* 返回旧的优先级上限 */
}
RTM_EXPORT(rt_mutex_setprioceiling); /* 导出函数符号 */


/**
 * @brief 获取互斥量的优先级上限属性。
 *
 * @param mutex 是指向互斥量对象的指针。
 *
 * @return 返回互斥量的当前优先级上限。
 */
rt_uint8_t rt_mutex_getprioceiling(rt_mutex_t mutex)
{
    rt_uint8_t prio = 0xFF; /* 优先级 */

    /* 参数检查 */
    RT_DEBUG_IN_THREAD_CONTEXT; /* 调试断言：在线程上下文中 */
    RT_ASSERT(mutex != RT_NULL); /* 断言：互斥量指针不为空 */

    if (mutex) /* 如果互斥量存在 */
    {
        rt_spin_lock(&(mutex->spinlock)); /* 锁自旋锁 */
        prio = mutex->ceiling_priority; /* 获取优先级上限 */
        rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */
    }

    return prio; /* 返回优先级上限 */
}
RTM_EXPORT(rt_mutex_getprioceiling); /* 导出函数符号 */


#ifdef RT_USING_HEAP /* 如果使用堆 */
/**
 * @brief    此函数将创建一个互斥量对象。
 *
 * @note     对于互斥量对象，其内存空间是自动分配的。
 *           相比之下，rt_mutex_init() 函数将初始化一个静态互斥量对象。
 *
 * @see      rt_mutex_init()
 *
 * @param    name 是指向赋予互斥量的名称的指针。
 *
 * @param    flag 是互斥量标志，它决定了当互斥量不可用时多个线程等待的排队方式。
 *           注意：此参数已被废弃。它可以是 RT_IPC_FLAG_PRIO、RT_IPC_FLAG_FIFO 或 RT_NULL。
 *
 * @return   返回指向互斥量对象的指针。当返回值为 RT_NULL 时，表示创建失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag)
{
    struct rt_mutex *mutex; /* 互斥量指针 */

    /* flag 参数已被废弃 */
    RT_UNUSED(flag);

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    /* 分配对象 */
    mutex = (rt_mutex_t)rt_object_allocate(RT_Object_Class_Mutex, name);
    if (mutex == RT_NULL) /* 如果分配失败 */
        return mutex; /* 返回空指针 */

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(mutex->parent));

    mutex->owner    = RT_NULL; /* 初始化拥有者为空 */
    mutex->priority = 0xFF; /* 初始化优先级为最低 */
    mutex->hold     = 0; /* 初始化持有次数为 0 */
    mutex->ceiling_priority = 0xFF; /* 初始化优先级上限为 0xFF */
    rt_list_init(&(mutex->taken_list)); /* 初始化获取列表 */

    /* flag 只能是 RT_IPC_FLAG_PRIO。RT_IPC_FLAG_FIFO 无法解决无界优先级反转问题 */
    mutex->parent.parent.flag = RT_IPC_FLAG_PRIO;
    rt_spin_lock_init(&(mutex->spinlock)); /* 初始化自旋锁 */

    return mutex; /* 返回互斥量指针 */
}
RTM_EXPORT(rt_mutex_create); /* 导出函数符号 */


/**
 * @brief    此函数将删除一个互斥量对象并释放此内存空间。
 *
 * @note     此函数用于删除由 rt_mutex_create() 函数创建的互斥量对象。
 *           相比之下，rt_mutex_detach() 函数将脱离一个静态互斥量对象。
 *           当互斥量成功删除时，它将恢复互斥量列表中的所有挂起线程。
 *
 * @see      rt_mutex_detach()
 *
 * @param    mutex 是指向要删除的互斥量对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示互斥量脱离失败。
 *
 * @warning  此函数只能删除由 rt_mutex_create() 函数初始化的互斥量。
 *           如果互斥量是由 rt_mutex_init() 函数初始化的，您绝不能使用此函数删除它,
 *           只能使用 rt_mutex_detach() 函数完成脱离。
 */
rt_err_t rt_mutex_delete(rt_mutex_t mutex)
{
    /* 参数检查 */
    RT_ASSERT(mutex != RT_NULL); /* 断言：互斥量指针不为空 */
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex); /* 断言：对象类型为互斥量 */
    RT_ASSERT(rt_object_is_systemobject(&mutex->parent.parent) == RT_FALSE); /* 断言：不是系统对象 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    _mutex_before_delete_detach(mutex); /* 执行删除/脱离前的处理 */

    /* 删除互斥量对象 */
    rt_object_delete(&(mutex->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mutex_delete); /* 导出函数符号 */
#endif /* RT_USING_HEAP */


/**
 * @brief    此函数将获取一个互斥量，如果互斥量不可用，线程将等待
 *           该互斥量直到指定的时间。
 *
 * @note     当调用此函数时，mutex->value 的计数值将减 1 直到它等于 0。
 *           当 mutex->value 为 0 时，表示互斥量不可用。此时，它将挂起
 *           准备获取互斥量的线程。
 *           相反，rt_mutex_release() 函数每次会将 mutex->value 的计数值加 1。
 *
 * @see      rt_mutex_trytake()
 *
 * @param    mutex 是指向互斥量对象的指针。
 *
 * @param    timeout 是超时时间（单位：OS tick）。如果互斥量不可用，线程将等待
 *           该互斥量直到由此参数指定的时间量。
 *           注意：通常，我们将此参数设置为 RT_WAITING_FOREVER，这意味着当互斥量不可用时,
 *           线程将永远等待。
 *
 * @return   返回操作状态。只有当返回值为 RT_EOK 时，操作才成功。
 *           如果返回值为其他任何值，则表示获取互斥量失败。
 *
 * @warning  此函数只能在线程上下文中调用。绝不能在中断上下文中调用。
 */
static rt_err_t _rt_mutex_take(rt_mutex_t mutex, rt_int32_t timeout, int suspend_flag)
{
    struct rt_thread *thread; /* 线程指针 */
    rt_err_t ret; /* 返回值 */

    /* 即使 time = 0 也不能在中断中使用此函数 */
    /* 当前上下文检查 */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE); /* 调试断言：调度器可用 */

    /* 参数检查 */
    RT_ASSERT(mutex != RT_NULL); /* 断言：互斥量指针不为空 */
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex); /* 断言：对象类型为互斥量 */

    /* 获取当前线程 */
    thread = rt_thread_self();

    rt_spin_lock(&(mutex->spinlock)); /* 锁自旋锁 */

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mutex->parent.parent))); /* 调用尝试获取钩子 */

    LOG_D("mutex_take: current thread %s, hold: %d",
          thread->parent.name, mutex->hold); /* 打印调试日志 */

    /* 重置线程错误码 */
    thread->error = RT_EOK;

    if (mutex->owner == thread) /* 如果当前线程已经是互斥量的拥有者(嵌套获取) */
    {
        if (mutex->hold < RT_MUTEX_HOLD_MAX) /* 如果持有次数未达到上限 */
        {
            /* 是同一个线程 */
            mutex->hold ++; /* 持有次数加 1 */
        }
        else /* 如果持有次数达到上限 */
        {
            rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */
            return -RT_EFULL; /* 值溢出 */
        }
    }
    else /* 如果当前线程不是互斥量的拥有者 */
    {
        /* 互斥量是否有拥有者线程 */
        if (mutex->owner == RT_NULL) /* 如果互斥量没有拥有者 */
        {
            /* 设置互斥量拥有者和原始优先级 */
            mutex->owner    = thread;
            mutex->priority = 0xff;
            mutex->hold     = 1;

            if (mutex->ceiling_priority != 0xFF) /* 如果设置了优先级上限 */
            {
                /* 将线程的优先级设置为优先级上限 */
                if (mutex->ceiling_priority < rt_sched_thread_get_curr_prio(mutex->owner)) /* 如果上限优先级更高 */
                    _thread_update_priority(mutex->owner, mutex->ceiling_priority, suspend_flag); /* 更新线程优先级 */
            }

            /* 将互斥量插入到线程的获取对象列表中 */
            rt_list_insert_after(&thread->taken_object_list, &mutex->taken_list);
        }
        else /* 如果互斥量已经有拥有者 */
        {
            /* 不等待，返回超时 */
            if (timeout == 0) /* 如果超时时间为 0 */
            {
                /* 设置错误为超时 */
                thread->error = RT_ETIMEOUT;

                rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */
                return -RT_ETIMEOUT; /* 返回超时错误 */
            }
            else /* 如果需要等待 */
            {
                rt_sched_lock_level_t slvl; /* 调度器锁级别 */
                rt_uint8_t priority; /* 优先级 */

                /* 互斥量不可用，压入挂起列表 */
                LOG_D("mutex_take: suspend thread: %s",
                      thread->parent.name); /* 打印调试日志 */

                /* 挂起当前线程 */
                ret = rt_thread_suspend_to_list(thread, &(mutex->parent.suspend_thread),
                                                mutex->parent.parent.flag, suspend_flag);
                if (ret != RT_EOK) /* 如果挂起失败 */
                {
                    rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */
                    return ret; /* 返回错误码 */
                }

                /* 在线程中设置挂起对象为该互斥量 */
                thread->pending_object = &(mutex->parent.parent);

                rt_sched_lock(&slvl); /* 锁定调度器 */

                priority = rt_sched_thread_get_curr_prio(thread); /* 获取当前线程优先级 */

                /* 更新互斥量的优先级级别 */
                if (priority < mutex->priority) /* 如果当前线程优先级更高 */
                {
                    mutex->priority = priority; /* 更新互斥量优先级 */
                    if (mutex->priority < rt_sched_thread_get_curr_prio(mutex->owner)) /* 如果互斥量优先级比拥有者优先级高 */
                    {
                        _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE); /* 更新拥有者线程优先级 */ /* TODO */
                    }
                }

                rt_sched_unlock(slvl); /* 解锁调度器 */

                /* 有等待时间，启动线程定时器 */
                if (timeout > 0) /* 如果超时时间大于 0 */
                {
                    rt_tick_t timeout_tick = timeout; /* 超时 tick 数 */
                    LOG_D("mutex_take: start the timer of thread:%s",
                          thread->parent.name); /* 打印调试日志 */

                    /* 重置线程定时器的超时并启动它 */
                    rt_timer_control(&(thread->thread_timer),
                                     RT_TIMER_CTRL_SET_TIME,
                                     &timeout_tick);
                    rt_timer_start(&(thread->thread_timer));
                }

                rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */

                /* 执行调度 */
                rt_schedule();

                rt_spin_lock(&(mutex->spinlock)); /* 锁自旋锁 */

                if (mutex->owner == thread) /* 如果当前线程获取到了互斥量 */
                {
                    /**
                     * 成功获取互斥量
                     * 注意：断言以避免意外的恢复
                     */
                    RT_ASSERT(thread->error == RT_EOK); /* 断言：线程错误码为 RT_EOK */
                }
                else /* 如果当前线程没有获取到互斥量，并且已从挂起列表脱离 */
                {
                    /* 互斥量未被获取，且线程已从挂起列表脱离 */

                    rt_bool_t need_update = RT_FALSE; /* 是否需要更新优先级 */
                    RT_ASSERT(mutex->owner != thread); /* 断言：拥有者不是当前线程 */

                    /* 在调用其他 API 之前先获取值 */
                    ret = thread->error; /* 获取线程错误码 */

                    /* 意外恢复 */
                    if (ret == RT_EOK) /* 如果错误码为 RT_EOK */
                    {
                        ret = -RT_EINTR; /* 设置为中断错误 */
                    }

                    rt_sched_lock(&slvl); /* 锁定调度器 */

                    /**
                     * 应该改变互斥量拥有者线程的优先级
                     * 注意：当前线程从互斥量挂起列表中脱离后，有
                     *       可能互斥量拥有者已经释放了互斥量。这
                     *       意味着 mutex->owner 在此时可能为空。如果发生了这种情况,
                     *       拥有者已经重置了其优先级。所以跳过是可以的
                     */
                    if (mutex->owner && rt_sched_thread_get_curr_prio(mutex->owner) == rt_sched_thread_get_curr_prio(thread)) /* 如果拥有者优先级与当前线程优先级相同 */
                        need_update = RT_TRUE; /* 需要更新优先级 */

                    /* 更新互斥量的优先级 */
                    if (!rt_list_isempty(&mutex->parent.suspend_thread)) /* 如果挂起列表不为空 */
                    {
                        /* 列表中有更多挂起的线程 */
                        struct rt_thread *th; /* 线程指针 */

                        th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next); /* 获取第一个挂起的线程 */
                        /* 更新互斥量的优先级 */
                        mutex->priority = rt_sched_thread_get_curr_prio(th);
                    }
                    else /* 如果挂起列表为空 */
                    {
                        /* 设置互斥量优先级为最大优先级(最低) */
                        mutex->priority = 0xff;
                    }

                    /* 尝试改变互斥量拥有者线程的优先级 */
                    if (need_update) /* 如果需要更新 */
                    {
                        /* 获取线程中互斥量的最大优先级 */
                        priority = _thread_get_mutex_priority(mutex->owner);
                        if (priority != rt_sched_thread_get_curr_prio(mutex->owner)) /* 如果优先级需要改变 */
                        {
                            _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE); /* 更新优先级 */
                        }
                    }

                    rt_sched_unlock(slvl); /* 解锁调度器 */

                    rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */

                    /* 退出前清除挂起对象 */
                    thread->pending_object = RT_NULL;

                    /* 将线程错误码修正为负值并返回 */
                    return ret > 0 ? -ret : ret;
                }
            }
        }
    }

    rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mutex->parent.parent))); /* 调用获取钩子 */

    return RT_EOK; /* 返回成功 */
}

/* 获取互斥量(不可中断) */
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mutex_take); /* 导出函数符号 */

/* 获取互斥量(可被中断打断) */
rt_err_t rt_mutex_take_interruptible(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mutex_take_interruptible); /* 导出函数符号 */

/* 获取互斥量(可被致命信号打断) */
rt_err_t rt_mutex_take_killable(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_KILLABLE);
}
RTM_EXPORT(rt_mutex_take_killable); /* 导出函数符号 */

/**
 * @brief    此函数将尝试获取一个互斥量，如果互斥量不可用，线程立即返回。
 *
 * @note     此函数与 rt_mutex_take() 函数非常相似，不同之处在于
 *           当互斥量不可用时，rt_mutex_trytake() 将立即返回而不等待超时。
 *           换句话说，rt_mutex_trytake(mutex) 与 rt_mutex_take(mutex, 0) 效果相同。
 *
 * @see      rt_mutex_take()
 *
 * @param    mutex 是指向互斥量对象的指针。
 *
 * @return   返回操作状态。只有当返回值为 RT_EOK 时，操作才成功。
 *           如果返回值为其他任何值，则表示获取互斥量失败。
 */
rt_err_t rt_mutex_trytake(rt_mutex_t mutex)
{
    return rt_mutex_take(mutex, RT_WAITING_NO); /* 不等待获取互斥量 */
}
RTM_EXPORT(rt_mutex_trytake); /* 导出函数符号 */


/**
 * @brief    此函数将释放一个互斥量。如果有线程挂起在该互斥量上，该线程将被恢复。
 *
 * @note     如果有线程挂起在此互斥量上，此互斥量对象列表中的第一个线程
 *           将被恢复，并执行线程调度 (rt_schedule)。
 *           如果没有线程挂起在此互斥量上，此互斥量的计数值 mutex->value 将增加 1。
 *
 * @param    mutex 是指向互斥量对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示释放互斥量失败。
 */
rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
    rt_sched_lock_level_t slvl; /* 调度器锁级别 */
    struct rt_thread *thread; /* 线程指针 */
    rt_bool_t need_schedule; /* 是否需要调度 */

    /* 参数检查 */
    RT_ASSERT(mutex != RT_NULL); /* 断言：互斥量指针不为空 */
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex); /* 断言：对象类型为互斥量 */

    need_schedule = RT_FALSE; /* 初始化为不需要调度 */

    /* 只有线程能释放互斥量，因为我们需要测试所有权 */
    RT_DEBUG_IN_THREAD_CONTEXT; /* 调试断言：在线程上下文中 */

    /* 获取当前线程 */
    thread = rt_thread_self();

    rt_spin_lock(&(mutex->spinlock)); /* 锁自旋锁 */

    LOG_D("mutex_release:current thread %s, hold: %d",
          thread->parent.name, mutex->hold); /* 打印调试日志 */

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mutex->parent.parent))); /* 调用放入钩子 */

    /* 互斥量只能被拥有者释放 */
    if (thread != mutex->owner) /* 如果当前线程不是拥有者 */
    {
        thread->error = -RT_ERROR; /* 设置错误码 */
        rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */

        return -RT_ERROR; /* 返回错误 */
    }

    /* 减少持有次数 */
    mutex->hold --;
    /* 如果没有持有了 */
    if (mutex->hold == 0) /* 如果持有次数为 0 */
    {
        rt_sched_lock(&slvl); /* 锁定调度器 */

        /* 从线程的获取列表中移除互斥量 */
        rt_list_remove(&mutex->taken_list);

        /* 是否改变线程优先级 */
        need_schedule = _check_and_update_prio(thread, mutex); /* 检查并更新优先级 */

        /* 唤醒挂起的线程 */
        if (!rt_list_isempty(&mutex->parent.suspend_thread)) /* 如果挂起列表不为空 */
        {
            struct rt_thread *next_thread; /* 下一个线程指针 */
            do
            {
                /* 获取第一个挂起的线程 */
                next_thread = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);

                RT_ASSERT(rt_sched_thread_is_suspended(next_thread)); /* 断言：线程处于挂起状态 */

                /* 从互斥量的挂起列表中移除该线程 */
                rt_list_remove(&RT_THREAD_LIST_NODE(next_thread));

                /* 恢复线程到就绪队列 */
                if (rt_sched_thread_ready(next_thread) != RT_EOK) /* 如果恢复失败 */
                {
                    /**
                     * 在我们尝试时，超时定时器已触发。所以我们跳过
                     * 此线程并再试一次。
                     */
                    next_thread = RT_NULL; /* 设置为空以继续循环 */
                }
            } while (!next_thread && !rt_list_isempty(&mutex->parent.suspend_thread)); /* 如果没有获取到线程且列表不为空 */

            if (next_thread) /* 如果获取到了下一个线程 */
            {
                LOG_D("mutex_release: resume thread: %s",
                    next_thread->parent.name); /* 打印调试日志 */

                /* 设置新拥有者并将互斥量放入线程的获取列表 */
                mutex->owner = next_thread;
                mutex->hold  = 1;
                rt_list_insert_after(&next_thread->taken_object_list, &mutex->taken_list);

                /* 清除挂起对象 */
                next_thread->pending_object = RT_NULL;

                /* 更新互斥量优先级 */
                if (!rt_list_isempty(&(mutex->parent.suspend_thread))) /* 如果挂起列表不为空 */
                {
                    struct rt_thread *th; /* 线程指针 */

                    th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next); /* 获取第一个挂起的线程 */
                    mutex->priority = rt_sched_thread_get_curr_prio(th); /* 更新优先级 */
                }
                else /* 如果挂起列表为空 */
                {
                    mutex->priority = 0xff; /* 设置优先级为最低 */
                }

                need_schedule = RT_TRUE; /* 需要调度 */
            }
            else /* 如果没有等待的线程被唤醒，清除拥有者 */
            {
                /* 没有等待的线程被唤醒，清除拥有者 */
                mutex->owner = RT_NULL;
                mutex->priority = 0xff;
            }

            rt_sched_unlock(slvl); /* 解锁调度器 */
        }
        else /* 如果挂起列表为空 */
        {
            rt_sched_unlock(slvl); /* 解锁调度器 */

            /* 清除拥有者 */
            mutex->owner    = RT_NULL;
            mutex->priority = 0xff;
        }
    }

    rt_spin_unlock(&(mutex->spinlock)); /* 解自旋锁 */

    /* 执行调度 */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mutex_release); /* 导出函数符号 */


/**
 * @brief    此函数将设置互斥量对象的一些额外属性。
 *
 * @note     目前此函数未实现控制功能。
 *
 * @param    mutex 是指向互斥量对象的指针。
 *
 * @param    cmd 是用于配置互斥量某些属性的命令字。
 *
 * @param    arg 是执行命令的函数的参数。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示此函数执行失败。
 */
rt_err_t rt_mutex_control(rt_mutex_t mutex, int cmd, void *arg)
{
    RT_UNUSED(mutex); /* 避免未使用变量警告 */
    RT_UNUSED(cmd); /* 避免未使用变量警告 */
    RT_UNUSED(arg); /* 避免未使用变量警告 */

    return -RT_EINVAL; /* 返回无效参数错误 */
}
RTM_EXPORT(rt_mutex_control); /* 导出函数符号 */

/**@}*/
#endif /* RT_USING_MUTEX */

#ifdef RT_USING_EVENT /* 如果使用事件 */
/**
 * @addtogroup group_event Event 事件
 * @{
 */

/**
 * @brief    此函数将初始化一个静态事件对象。
 *
 * @note     对于静态事件对象，其内存空间是在编译时由编译器分配的，
 *           并且应放置在读写数据段或未初始化数据段上。
 *           相比之下，rt_event_create() 函数会自动分配内存空间并初始化事件。
 *
 * @see      rt_event_create()
 *
 * @param    event 是指向要初始化的事件的指针。假定事件的存储空间将
 *           在您的应用程序中分配。
 *
 * @param    name 是指向赋予事件的名称的指针。
 *
 * @param    flag 是事件标志，它决定了当事件不可用时多个线程等待的排队方式。
 *           事件标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                         (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此事件的所有线程
 *               将成为非实时线程。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示初始化失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag)
{
    /* 参数检查 */
    RT_ASSERT(event != RT_NULL); /* 断言：事件指针不为空 */
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    /* 初始化对象 */
    rt_object_init(&(event->parent.parent), RT_Object_Class_Event, name);

    /* 设置父对象标志 */
    event->parent.parent.flag = flag;

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(event->parent));

    /* 初始化事件 */
    event->set = 0; /* 事件集初始化为 0 */
    rt_spin_lock_init(&(event->spinlock)); /* 初始化自旋锁 */

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_event_init); /* 导出函数符号 */


/**
 * @brief    此函数将脱离一个静态事件对象。
 *
 * @note     此函数用于脱离由 rt_event_init() 函数初始化的静态事件对象。
 *           相比之下，rt_event_delete() 函数将删除一个事件对象。
 *           当事件成功脱离时，它将恢复事件列表中的所有挂起线程。
 *
 * @see      rt_event_delete()
 *
 * @param    event 是指向要脱离的事件对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示事件脱离失败。
 *
 * @warning  此函数只能脱离由 rt_event_init() 函数初始化的静态事件。
 *           如果事件是由 rt_event_create() 函数创建的，您绝不能使用此函数脱离它,
 *           只能使用 rt_event_delete() 函数完成删除。
 */
rt_err_t rt_event_detach(rt_event_t event)
{
    rt_base_t level; /* 中断级别 */

    /* 参数检查 */
    RT_ASSERT(event != RT_NULL); /* 断言：事件指针不为空 */
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event); /* 断言：对象类型为事件 */
    RT_ASSERT(rt_object_is_systemobject(&event->parent.parent)); /* 断言：是系统对象 */

    level = rt_spin_lock_irqsave(&(event->spinlock)); /* 保存中断状态并锁自旋锁 */
    /* 恢复所有挂起的线程 */
    rt_susp_list_resume_all(&(event->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 脱离事件对象 */
    rt_object_detach(&(event->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_event_detach); /* 导出函数符号 */

#ifdef RT_USING_HEAP /* 如果使用堆 */
/**
 * @brief    创建一个事件对象。
 *
 * @note     对于事件对象，其内存空间是自动分配的。
 *           相比之下，rt_event_init() 函数将初始化一个静态事件对象。
 *
 * @see      rt_event_init()
 *
 * @param    name 是指向赋予事件的名称的指针。
 *
 * @param    flag 是事件标志，它决定了当事件不可用时多个线程等待的排队方式。
 *           事件标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                         (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此事件的所有线程
 *               将成为非实时线程。
 *
 * @return   返回指向事件对象的指针。当返回值为 RT_NULL 时，表示创建失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_event_t rt_event_create(const char *name, rt_uint8_t flag)
{
    rt_event_t event; /* 事件指针 */

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    /* 分配对象 */
    event = (rt_event_t)rt_object_allocate(RT_Object_Class_Event, name);
    if (event == RT_NULL) /* 如果分配失败 */
        return event; /* 返回空指针 */

    /* 设置父对象 */
    event->parent.parent.flag = flag;

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(event->parent));

    /* 初始化事件 */
    event->set = 0; /* 事件集初始化为 0 */
    rt_spin_lock_init(&(event->spinlock)); /* 初始化自旋锁 */

    return event; /* 返回事件指针 */
}
RTM_EXPORT(rt_event_create); /* 导出函数符号 */


/**
 * @brief    此函数将删除一个事件对象并释放内存空间。
 *
 * @note     此函数用于删除由 rt_event_create() 函数创建的事件对象。
 *           相比之下，rt_event_detach() 函数将脱离一个静态事件对象。
 *           当事件成功删除时，它将恢复事件列表中的所有挂起线程。
 *
 * @see      rt_event_detach()
 *
 * @param    event 是指向要删除的事件对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示事件脱离失败。
 *
 * @warning  此函数只能删除由 rt_event_create() 函数初始化的事件。
 *           如果事件是由 rt_event_init() 函数初始化的，您绝不能使用此函数删除它,
 *           只能使用 rt_event_detach() 函数完成脱离。
 */
rt_err_t rt_event_delete(rt_event_t event)
{
    /* 参数检查 */
    RT_ASSERT(event != RT_NULL); /* 断言：事件指针不为空 */
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event); /* 断言：对象类型为事件 */
    RT_ASSERT(rt_object_is_systemobject(&event->parent.parent) == RT_FALSE); /* 断言：不是系统对象 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    rt_spin_lock(&(event->spinlock)); /* 锁自旋锁 */
    /* 恢复所有挂起的线程 */
    rt_susp_list_resume_all(&(event->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock(&(event->spinlock)); /* 解自旋锁 */

    /* 删除事件对象 */
    rt_object_delete(&(event->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_event_delete); /* 导出函数符号 */
#endif /* RT_USING_HEAP */


/**
 * @brief    此函数将发送一个事件到事件对象。
 *           如果有线程挂起在该事件上，该线程将被恢复。
 *
 * @note     当使用此函数时，您需要使用参数 来 指定事件对象的事件标志，
 *           然后函数将遍历挂起在事件对象上的线程列表。
 *           如果有线程挂起在该事件上，并且线程的 event_info 与当前事件对象的事件标志匹配，
 *           该线程将被恢复。
 *
 * @param    event 是指向要发送的事件对象的指针。
 *
 * @param    set 是您将为此事件标志设置的标志。
 *           您可以设置一个事件标志，也可以通过 OR 逻辑运算设置多个标志。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示事件脱离失败。
 */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set)
{
    struct rt_list_node *n; /* 列表节点指针 */
    struct rt_thread *thread; /* 线程指针 */
    rt_sched_lock_level_t slvl; /* 调度器锁级别 */
    rt_base_t level; /* 中断级别 */
    rt_base_t status; /* 状态 */
    rt_bool_t need_schedule; /* 是否需要调度 */
    rt_uint32_t need_clear_set = 0; /* 需要清除的事件集 */

    /* 参数检查 */
    RT_ASSERT(event != RT_NULL); /* 断言：事件指针不为空 */
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event); /* 断言：对象类型为事件 */

    if (set == 0) /* 如果设置的事件集为 0 */
        return -RT_ERROR; /* 返回错误 */

    need_schedule = RT_FALSE; /* 初始化为不需要调度 */

    level = rt_spin_lock_irqsave(&(event->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 设置事件 */
    event->set |= set;

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(event->parent.parent))); /* 调用放入钩子 */

    rt_sched_lock(&slvl); /* 锁定调度器 */
    if (!rt_list_isempty(&event->parent.suspend_thread)) /* 如果挂起列表不为空 */
    {
        /* 搜索线程列表以恢复线程 */
        n = event->parent.suspend_thread.next; /* 获取第一个节点 */
        while (n != &(event->parent.suspend_thread)) /* 遍历挂起列表 */
        {
            /* 获取线程 */
            thread = RT_THREAD_LIST_NODE_ENTRY(n);

            status = -RT_ERROR; /* 初始化状态为错误 */
            if (thread->event_info & RT_EVENT_FLAG_AND) /* 如果是 AND 事件 */
            {
                if ((thread->event_set & event->set) == thread->event_set) /* 如果事件集匹配 */
                {
                    /* 接收到了 AND 事件 */
                    status = RT_EOK; /* 设置状态为成功 */
                }
            }
            else if (thread->event_info & RT_EVENT_FLAG_OR) /* 如果是 OR 事件 */
            {
                if (thread->event_set & event->set) /* 如果事件集有交集 */
                {
                    /* 保存接收到的事件集 */
                    thread->event_set = thread->event_set & event->set;

                    /* 接收到了 OR 事件 */
                    status = RT_EOK; /* 设置状态为成功 */
                }
            }
            else /* 其他情况 */
            {
                rt_sched_unlock(slvl); /* 解锁调度器 */
                rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */

                return -RT_EINVAL; /* 返回无效参数错误 */
            }

            /* 将节点移动到下一个 */
            n = n->next;

            /* 条件满足，恢复线程 */
            if (status == RT_EOK) /* 如果状态为成功 */
            {
                /* 清除事件 */
                if (thread->event_info & RT_EVENT_FLAG_CLEAR) /* 如果需要清除事件标志 */
                    need_clear_set |= thread->event_set; /* 添加到需要清除的集合中 */

                /* 恢复线程，并且线程列表脱离 */
                rt_sched_thread_ready(thread); /* 将线程设为就绪态 */
                thread->error = RT_EOK; /* 设置错误码为成功 */

                /* 需要进行调度 */
                need_schedule = RT_TRUE; /* 需要调度 */
            }
        }
        if (need_clear_set) /* 如果有需要清除的事件集 */
        {
            event->set &= ~need_clear_set; /* 清除事件集 */
        }
    }

    rt_sched_unlock(slvl); /* 解锁调度器 */
    rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 执行调度 */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_event_send); /* 导出函数符号 */


/**
 * @brief  此函数将从事件对象接收一个事件。如果事件不可用，线程将等待
 *         该事件直到指定的时间。
 *
 * @note   如果有线程挂起在此信号量上，此信号量对象列表中的第一个线程
 *         将被恢复，并执行线程调度 (rt_schedule)。
 *         如果没有线程挂起在此信号量上，此信号量的计数值 sem->value 将增加 1。
 *
 * @param    event 是指向要接收的事件对象的指针。
 *
 * @param    set 是您将为此事件标志设置的标志。
 *           您可以设置一个事件标志，也可以通过 OR 逻辑运算设置多个标志。
 *
 * @param    option 是此接收事件的选项，它指示接收事件是如何操作的。
 *           选项可以是以下一个或多个值，当选择多个值时，使用逻辑 OR 运算。
 *           (注意：RT_EVENT_FLAG_OR 和 RT_EVENT_FLAG_AND 只能选择一个)：
 *
 *
 *               RT_EVENT_FLAG_OR           线程选择使用逻辑 OR 接收事件。
 *
 *               RT_EVENT_FLAG_AND          线程选择使用逻辑 AND 接收事件。
 *
 *               RT_EVENT_FLAG_CLEAR        当线程接收到相应事件时，函数
 *                                          决定是否清除事件标志。
 *
 * @param    timeout 是超时时间（单位：OS tick）。
 *
 * @param    recved 是指向接收到的事件的指针。如果您不关心此值，可以使用 RT_NULL 设置。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示信号量释放失败。
 */
static rt_err_t _rt_event_recv(rt_event_t   event,
                               rt_uint32_t  set,
                               rt_uint8_t   option,
                               rt_int32_t   timeout,
                               rt_uint32_t *recved,
                               int suspend_flag)
{
    struct rt_thread *thread; /* 线程指针 */
    rt_base_t level; /* 中断级别 */
    rt_base_t status; /* 状态 */
    rt_err_t ret; /* 返回值 */

    /* 参数检查 */
    RT_ASSERT(event != RT_NULL); /* 断言：事件指针不为空 */
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event); /* 断言：对象类型为事件 */

    /* 当前上下文检查 */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE); /* 调试断言：调度器可用 */

    if (set == 0) /* 如果设置的事件集为 0 */
        return -RT_ERROR; /* 返回错误 */

    /* 初始化状态 */
    status = -RT_ERROR; /* 初始化状态为错误 */
    /* 获取当前线程 */
    thread = rt_thread_self();
    /* 重置线程错误码 */
    thread->error = -RT_EINTR;

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(event->parent.parent))); /* 调用尝试获取钩子 */

    level = rt_spin_lock_irqsave(&(event->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 检查事件集 */
    if (option & RT_EVENT_FLAG_AND) /* 如果是 AND 选项 */
    {
        if ((event->set & set) == set) /* 如果事件集匹配 */
            status = RT_EOK; /* 设置状态为成功 */
    }
    else if (option & RT_EVENT_FLAG_OR) /* 如果是 OR 选项 */
    {
        if (event->set & set) /* 如果事件集有交集 */
            status = RT_EOK; /* 设置状态为成功 */
    }
    else /* 其他情况 */
    {
        /* 应该设置 RT_EVENT_FLAG_AND 或 RT_EVENT_FLAG_OR */
        RT_ASSERT(0); /* 断言失败 */
    }

    if (status == RT_EOK) /* 如果状态为成功(事件已发生) */
    {
        thread->error = RT_EOK; /* 设置线程错误码为成功 */

        /* 设置接收到的事件 */
        if (recved)
            *recved = (event->set & set);

        /* 填充线程事件信息 */
        thread->event_set = (event->set & set);
        thread->event_info = option;

        /* 接收到事件 */
        if (option & RT_EVENT_FLAG_CLEAR) /* 如果需要清除事件标志 */
            event->set &= ~set; /* 清除事件集 */
    }
    else if (timeout == 0) /* 如果不等待 */
    {
        /* 不等待 */
        thread->error = -RT_ETIMEOUT; /* 设置线程错误码为超时 */

        rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */

        return -RT_ETIMEOUT; /* 返回超时错误 */
    }
    else /* 如果需要等待 */
    {
        /* 填充线程事件信息 */
        thread->event_set  = set;
        thread->event_info = option;

        /* 将线程放入挂起线程列表 */
        ret = rt_thread_suspend_to_list(thread, &(event->parent.suspend_thread),
                                        event->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK) /* 如果挂起失败 */
        {
            rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */
            return ret; /* 返回错误码 */
        }

        /* 如果有等待超时，激活线程定时器 */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            rt_tick_t timeout_tick = timeout; /* 超时 tick 数 */
            /* 重置线程定时器的超时并启动它 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */

        /* 执行调度 */
        rt_schedule();

        if (thread->error != RT_EOK) /* 如果线程错误码不为成功 */
        {
            /* 返回错误 */
            return thread->error; /* 返回错误码 */
        }

        /* 接收到一个事件，禁用中断以保护 */
        level = rt_spin_lock_irqsave(&(event->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 设置接收到的事件 */
        if (recved)
            *recved = thread->event_set;
    }

    rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(event->parent.parent))); /* 调用获取钩子 */

    return thread->error; /* 返回线程错误码 */
}

/* 接收事件(不可中断) */
rt_err_t rt_event_recv(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_event_recv); /* 导出函数符号 */

/* 接收事件(可被中断打断) */
rt_err_t rt_event_recv_interruptible(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_event_recv_interruptible); /* 导出函数符号 */

/* 接收事件(可被致命信号打断) */
rt_err_t rt_event_recv_killable(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_KILLABLE);
}
RTM_EXPORT(rt_event_recv_killable); /* 导出函数符号 */
/**
 * @brief    此函数将设置事件对象的一些额外属性。
 *
 * @note     目前此函数仅支持 RT_IPC_CMD_RESET 命令来重置事件。
 *
 * @param    event 是指向事件对象的指针。
 *
 * @param    cmd 是用于配置事件某些属性的命令字。
 *
 * @param    arg 是执行命令的函数的参数。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示此函数执行失败。
 */
rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg)
{
    rt_base_t level; /* 中断级别 */

    RT_UNUSED(arg); /* 避免未使用变量警告 */

    /* 参数检查 */
    RT_ASSERT(event != RT_NULL); /* 断言：事件指针不为空 */
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event); /* 断言：对象类型为事件 */

    if (cmd == RT_IPC_CMD_RESET) /* 如果是复位命令 */
    {
        level = rt_spin_lock_irqsave(&(event->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 恢复所有等待的线程 */
        rt_susp_list_resume_all(&event->parent.suspend_thread, RT_ERROR);

        /* 初始化事件集 */
        event->set = 0;

        rt_spin_unlock_irqrestore(&(event->spinlock), level); /* 解自旋锁并恢复中断状态 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }

    return -RT_ERROR; /* 返回错误 */
}
RTM_EXPORT(rt_event_control); /* 导出函数符号 */

/**@}*/
#endif /* RT_USING_EVENT */

#ifdef RT_USING_MAILBOX /* 如果使用邮箱 */
/**
 * @addtogroup group_mailbox MailBox 邮箱
 * @{
 */

/**
 * @brief    初始化一个静态邮箱对象。
 *
 * @note     对于静态邮箱对象，其内存空间是在编译时由编译器分配的，
 *           并且应放置在读写数据段或未初始化数据段上。
 *           相比之下，rt_mb_create() 函数会自动分配内存空间并初始化邮箱。
 *
 * @see      rt_mb_create()
 *
 * @param    mb 是指向要初始化的邮箱的指针。
 *           假定邮箱的存储空间将在您的应用程序中分配。
 *
 * @param    name 是指向赋予邮箱的名称的指针。
 *
 * @param    msgpool 保存接收邮件的缓冲区的起始地址。
 *
 * @param    size 是邮箱中邮件的最大数量。
 *           例如，当邮箱缓冲区容量为 N 时，size 为 N/4。
 *
 * @param    flag 是邮箱标志，它决定了当邮箱不可用时多个线程等待的排队方式。
 *           邮箱标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                       (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此邮箱的所有线程
 *               将成为非实时线程。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示初始化失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_err_t rt_mb_init(rt_mailbox_t mb,
                    const char  *name,
                    void        *msgpool,
                    rt_size_t    size,
                    rt_uint8_t   flag)
{
    RT_ASSERT(mb != RT_NULL); /* 断言：邮箱指针不为空 */
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    /* 初始化对象 */
    rt_object_init(&(mb->parent.parent), RT_Object_Class_MailBox, name);

    /* 设置父对象标志 */
    mb->parent.parent.flag = flag;

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(mb->parent));

    /* 初始化邮箱 */
    mb->msg_pool   = (rt_ubase_t *)msgpool; /* 设置消息池地址 */
    mb->size       = (rt_uint16_t)size; /* 设置邮箱大小 */
    mb->entry      = 0; /* 初始化邮件数为 0 */
    mb->in_offset  = 0; /* 初始化入偏移为 0 */
    mb->out_offset = 0; /* 初始化出偏移为 0 */

    /* 初始化发送者挂起线程的附加列表 */
    rt_list_init(&(mb->suspend_sender_thread));
    rt_spin_lock_init(&(mb->spinlock)); /* 初始化自旋锁 */

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mb_init); /* 导出函数符号 */


/**
 * @brief    此函数将脱离一个静态邮箱对象。
 *
 * @note     此函数用于脱离由 rt_mb_init() 函数初始化的静态邮箱对象。
 *           相比之下，rt_mb_delete() 函数将删除一个邮箱对象。
 *           当邮箱成功脱离时，它将恢复邮箱列表中的所有挂起线程。
 *
 * @see      rt_mb_delete()
 *
 * @param    mb 是指向要脱离的邮箱对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示邮箱脱离失败。
 *
 * @warning  此函数只能脱离由 rt_mb_init() 函数初始化的静态邮箱。
 *           如果邮箱是由 rt_mb_create() 函数创建的，您绝不能使用此函数脱离它,
 *           只能使用 rt_mb_delete() 函数完成删除。
 */
rt_err_t rt_mb_detach(rt_mailbox_t mb)
{
    rt_base_t level; /* 中断级别 */

    /* 参数检查 */
    RT_ASSERT(mb != RT_NULL); /* 断言：邮箱指针不为空 */
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox); /* 断言：对象类型为邮箱 */
    RT_ASSERT(rt_object_is_systemobject(&mb->parent.parent)); /* 断言：是系统对象 */

    level = rt_spin_lock_irqsave(&(mb->spinlock)); /* 保存中断状态并锁自旋锁 */
    /* 恢复所有挂起的线程 */
    rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);
    /* 同时恢复邮箱私有的挂起发送线程 */
    rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 脱离邮箱对象 */
    rt_object_detach(&(mb->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mb_detach); /* 导出函数符号 */

#ifdef RT_USING_HEAP /* 如果使用堆 */
/**
 * @brief  创建一个邮箱对象。
 *
 * @note   对于邮箱对象，其内存空间是自动分配的。
 *         相比之下，rt_mb_init() 函数将初始化一个静态邮箱对象。
 *
 * @see    rt_mb_init()
 *
 * @param  name 是指向赋予邮箱的名称的指针。
 *
 * @param    size 是邮箱中邮件的最大数量。
 *           例如，当邮箱缓冲区容量为 N 时，size 为 N/4。
 *
 * @param    flag 是邮箱标志，它决定了当邮箱不可用时多个线程等待的排队方式。
 *           邮箱标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                         (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此邮箱的所有线程
 *               将成为非实时线程。
 *
 * @return   返回指向邮箱对象的指针。当返回值为 RT_NULL 时，表示创建失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_mailbox_t rt_mb_create(const char *name, rt_size_t size, rt_uint8_t flag)
{
    rt_mailbox_t mb; /* 邮箱指针 */

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    /* 分配对象 */
    mb = (rt_mailbox_t)rt_object_allocate(RT_Object_Class_MailBox, name);
    if (mb == RT_NULL) /* 如果分配失败 */
        return mb; /* 返回空指针 */

    /* 设置父对象 */
    mb->parent.parent.flag = flag;

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(mb->parent));

    /* 初始化邮箱 */
    mb->size     = (rt_uint16_t)size; /* 设置邮箱大小 */
    mb->msg_pool = (rt_ubase_t *)RT_KERNEL_MALLOC(mb->size * sizeof(rt_ubase_t)); /* 分配消息池内存 */
    if (mb->msg_pool == RT_NULL) /* 如果分配失败 */
    {
        /* 删除邮箱对象 */
        rt_object_delete(&(mb->parent.parent));

        return RT_NULL; /* 返回空指针 */
    }
    mb->entry      = 0; /* 初始化邮件数为 0 */
    mb->in_offset  = 0; /* 初始化入偏移为 0 */
    mb->out_offset = 0; /* 初始化出偏移为 0 */

    /* 初始化发送者挂起线程的附加列表 */
    rt_list_init(&(mb->suspend_sender_thread));
    rt_spin_lock_init(&(mb->spinlock)); /* 初始化自旋锁 */

    return mb; /* 返回邮箱指针 */
}
RTM_EXPORT(rt_mb_create); /* 导出函数符号 */


/**
 * @brief    此函数将删除一个邮箱对象并释放内存空间。
 *
 * @note     此函数用于删除由 rt_mb_create() 函数创建的邮箱对象。
 *           相比之下，rt_mb_detach() 函数将脱离一个静态邮箱对象。
 *           当邮箱成功删除时，它将恢复邮箱列表中的所有挂起线程。
 *
 * @see      rt_mb_detach()
 *
 * @param    mb 是指向要删除的邮箱对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示邮箱脱离失败。
 *
 * @warning  此函数只能删除由 rt_mb_create() 函数创建的邮箱。
 *           如果邮箱是由 rt_mb_init() 函数初始化的，您绝不能使用此函数删除它,
 *           只能使用 rt_mb_detach() 函数完成脱离。
 */
rt_err_t rt_mb_delete(rt_mailbox_t mb)
{
    /* 参数检查 */
    RT_ASSERT(mb != RT_NULL); /* 断言：邮箱指针不为空 */
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox); /* 断言：对象类型为邮箱 */
    RT_ASSERT(rt_object_is_systemobject(&mb->parent.parent) == RT_FALSE); /* 断言：不是系统对象 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */
    rt_spin_lock(&(mb->spinlock)); /* 锁自旋锁 */

    /* 恢复所有挂起的线程 */
    rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);

    /* 同时恢复邮箱私有的挂起发送线程 */
    rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);

    rt_spin_unlock(&(mb->spinlock)); /* 解自旋锁 */

    /* 释放邮箱池 */
    RT_KERNEL_FREE(mb->msg_pool);

    /* 删除邮箱对象 */
    rt_object_delete(&(mb->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mb_delete); /* 导出函数符号 */
#endif /* RT_USING_HEAP */


/**
 * @brief    此函数将发送一封邮件到邮箱对象。如果有线程挂起在邮箱上，
 *           该线程将被恢复。
 *
 * @note     当使用此函数发送邮件时，如果邮箱已满，当前线程将
 *           等待超时。如果达到设置的超时时间仍然没有可用空间，
 *           发送线程将被恢复并返回错误码。
 *           相比之下，当邮箱已满时，rt_mb_send() 函数将立即返回错误码而不等待。
 *
 * @see      rt_mb_send()
 *
 * @param    mb 是指向要发送的邮箱对象的指针。
 *
 * @param    value 是您想要发送的邮件内容的值。
 *
 * @param    timeout 是超时时间（单位：OS tick）。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示邮箱脱离失败。
 *
 * @warning  此函数可以在中断上下文和线程上下文中调用。
 */
static rt_err_t _rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout,
                         int suspend_flag)
{
    struct rt_thread *thread; /* 线程指针 */
    rt_base_t level; /* 中断级别 */
    rt_uint32_t tick_delta; /* delta tick */
    rt_err_t ret; /* 返回值 */

    /* 参数检查 */
    RT_ASSERT(mb != RT_NULL); /* 断言：邮箱指针不为空 */
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox); /* 断言：对象类型为邮箱 */

    /* 当前上下文检查 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0); /* 调试断言：调度器可用 */

    /* 初始化 delta tick */
    tick_delta = 0;
    /* 获取当前线程 */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mb->parent.parent))); /* 调用放入钩子 */

    /* 禁用中断 */
    level = rt_spin_lock_irqsave(&(mb->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 对于非阻塞调用 */
    if (mb->entry == mb->size && timeout == 0) /* 如果邮箱已满且不等待 */
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */
        return -RT_EFULL; /* 返回满错误 */
    }

    /* 邮箱已满 */
    while (mb->entry == mb->size) /* 如果邮箱已满 */
    {
        /* 重置线程中的错误码 */
        thread->error = -RT_EINTR;

        /* 不等待，返回超时 */
        if (timeout == 0) /* 如果不等待 */
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

            return -RT_EFULL; /* 返回满错误 */
        }

        /* 挂起当前线程 */
        ret = rt_thread_suspend_to_list(thread, &(mb->suspend_sender_thread),
                                        mb->parent.parent.flag, suspend_flag);

        if (ret != RT_EOK) /* 如果挂起失败 */
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */
            return ret; /* 返回错误码 */
        }

        /* 有等待时间，启动线程定时器 */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            rt_tick_t timeout_tick = timeout; /* 超时 tick 数 */
            /* 获取定时器的起始 tick */
            tick_delta = rt_tick_get();

            LOG_D("mb_send_wait: start timer of thread:%s",
                  thread->parent.name); /* 打印调试日志 */

            /* 重置线程定时器的超时并启动它 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }
        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

        /* 重新调度 */
        rt_schedule();

        /* 从挂起状态恢复 */
        if (thread->error != RT_EOK) /* 如果线程错误码不为成功 */
        {
            /* 返回错误 */
            return thread->error; /* 返回错误码 */
        }

        level = rt_spin_lock_irqsave(&(mb->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 如果不是永远等待，则重新计算超时 tick */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            tick_delta = rt_tick_get() - tick_delta; /* 计算经过的 tick */
            timeout -= tick_delta; /* 减少超时时间 */
            if (timeout < 0) /* 如果超时时间小于 0 */
                timeout = 0; /* 设置为 0 */
        }
    }

    /* 设置指针 */
    mb->msg_pool[mb->in_offset] = value;
    /* 增加输入偏移 */
    ++ mb->in_offset;
    if (mb->in_offset >= mb->size) /* 如果入偏移超过大小 */
        mb->in_offset = 0; /* 循环 */

    if(mb->entry < RT_MB_ENTRY_MAX) /* 如果邮件数未达到上限 */
    {
        /* 增加邮件条目 */
        mb->entry ++;
    }
    else /* 如果邮件数达到上限 */
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */
        return -RT_EFULL; /* 值溢出 */
    }

    /* 恢复挂起的线程 */
    if (!rt_list_isempty(&mb->parent.suspend_thread)) /* 如果有挂起的接收线程 */
    {
        rt_susp_list_dequeue(&(mb->parent.suspend_thread), RT_EOK); /* 唤醒一个接收线程 */

        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

    return RT_EOK; /* 返回成功 */
}

/* 发送邮件等待(不可中断) */
rt_err_t rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_send_wait); /* 导出函数符号 */

/* 发送邮件等待(可被中断打断) */
rt_err_t rt_mb_send_wait_interruptible(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_send_wait_interruptible); /* 导出函数符号 */

/* 发送邮件等待(可被致命信号打断) */
rt_err_t rt_mb_send_wait_killable(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mb_send_wait_killable); /* 导出函数符号 */
/**
 * @brief    此函数将发送一封邮件到邮箱对象。如果有线程挂起在邮箱上，
 *           该线程将被恢复。
 *
 * @note     当使用此函数发送邮件时，如果邮箱已满，此函数将立即返回错误码而不等待。
 *           相比之下，rt_mb_send_wait() 函数设置了超时等待邮件发送。
 *
 * @see      rt_mb_send_wait()
 *
 * @param    mb 是指向要发送的邮箱对象的指针。
 *
 * @param    value 是您想要发送的邮件内容的值。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示邮箱脱离失败。
 */
rt_err_t rt_mb_send(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait(mb, value, 0); /* 不等待发送邮件 */
}
RTM_EXPORT(rt_mb_send); /* 导出函数符号 */

/* 发送邮件(可被中断打断) */
rt_err_t rt_mb_send_interruptible(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait_interruptible(mb, value, 0); /* 不等待发送邮件 */
}
RTM_EXPORT(rt_mb_send_interruptible); /* 导出函数符号 */

/* 发送邮件(可被致命信号打断) */
rt_err_t rt_mb_send_killable(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait_killable(mb, value, 0); /* 不等待发送邮件 */
}
RTM_EXPORT(rt_mb_send_killable); /* 导出函数符号 */

/**
 * @brief    此函数将发送一封紧急邮件到邮箱对象。
 *
 * @note     此函数与 rt_mb_send() 函数几乎相同。唯一的区别是
 *           当发送紧急邮件时，邮件将被放置在邮件队列的头部，以便
 *           接收者可以首先接收到紧急邮件。
 *
 * @see      rt_mb_send()
 *
 * @param    mb 是指向要发送的邮箱对象的指针。
 *
 * @param    value 是您想要发送的邮件内容。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示邮箱脱离失败。
 */
rt_err_t rt_mb_urgent(rt_mailbox_t mb, rt_ubase_t value)
{
    rt_base_t level; /* 中断级别 */

    /* 参数检查 */
    RT_ASSERT(mb != RT_NULL); /* 断言：邮箱指针不为空 */
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox); /* 断言：对象类型为邮箱 */

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mb->parent.parent))); /* 调用放入钩子 */

    level = rt_spin_lock_irqsave(&(mb->spinlock)); /* 保存中断状态并锁自旋锁 */

    if (mb->entry == mb->size) /* 如果邮箱已满 */
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */
        return -RT_EFULL; /* 返回满错误 */
    }

    /* 倒回到前一个位置 */
    if (mb->out_offset > 0) /* 如果出偏移大于 0 */
    {
        mb->out_offset --; /* 出偏移减 1 */
    }
    else /* 如果出偏移为 0 */
    {
        mb->out_offset = mb->size - 1; /* 出偏移设置为大小减 1(循环) */
    }

    /* 设置指针 */
    mb->msg_pool[mb->out_offset] = value;

    /* 增加邮件条目 */
    mb->entry ++;

    /* 恢复挂起的线程 */
    if (!rt_list_isempty(&mb->parent.suspend_thread)) /* 如果有挂起的接收线程 */
    {
        rt_susp_list_dequeue(&(mb->parent.suspend_thread), RT_EOK); /* 唤醒一个接收线程 */

        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mb_urgent); /* 导出函数符号 */


/**
 * @brief    此函数将从邮箱对象接收一封邮件，如果邮箱对象中没有邮件，
 *           线程将等待指定的时间。
 *
 * @note     只有当邮箱中有邮件时，接收线程才能立即获取邮件并
 *           返回 RT_EOK，否则接收线程将被挂起直到设置的超时时间。如果
 *           在指定时间内仍未接收到邮件，将返回 -RT_ETIMEOUT。
 *
 * @param    mb 是指向要接收的邮箱对象的指针。
 *
 * @param    value 是您将为此邮箱标志设置的标志。
 *           您可以设置一个邮箱标志，也可以通过 OR 逻辑运算设置多个标志。
 *
 * @param    timeout 是超时时间（单位：OS tick）。如果队列中邮箱对象不可用，
 *           线程将等待队列中的对象直到由此参数指定的时间量。
 *
 *           注意:
 *           如果使用宏 RT_WAITING_FOREVER 设置此参数，意味着当
 *           队列中邮箱对象不可用时，线程将永远等待。
 *           如果使用宏 RT_WAITING_NO 设置此参数，意味着此
 *           函数是非阻塞的，将立即返回。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示邮箱释放失败。
 */
static rt_err_t _rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout, int suspend_flag)
{
    struct rt_thread *thread; /* 线程指针 */
    rt_base_t level; /* 中断级别 */
    rt_uint32_t tick_delta; /* delta tick */
    rt_err_t ret; /* 返回值 */

    /* 参数检查 */
    RT_ASSERT(mb != RT_NULL); /* 断言：邮箱指针不为空 */
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox); /* 断言：对象类型为邮箱 */

    /* 当前上下文检查 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0); /* 调试断言：调度器可用 */

    /* 初始化 delta tick */
    tick_delta = 0;
    /* 获取当前线程 */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mb->parent.parent))); /* 调用尝试获取钩子 */

    level = rt_spin_lock_irqsave(&(mb->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 对于非阻塞调用 */
    if (mb->entry == 0 && timeout == 0) /* 如果邮箱为空且不等待 */
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

        return -RT_ETIMEOUT; /* 返回超时错误 */
    }

    /* 邮箱为空 */
    while (mb->entry == 0) /* 如果邮箱为空 */
    {
        /* 重置线程中的错误码 */
        thread->error = -RT_EINTR;

        /* 不等待，返回超时 */
        if (timeout == 0) /* 如果不等待 */
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

            thread->error = -RT_ETIMEOUT; /* 设置线程错误码为超时 */

            return -RT_ETIMEOUT; /* 返回超时错误 */
        }

        /* 挂起当前线程 */
        ret = rt_thread_suspend_to_list(thread, &(mb->parent.suspend_thread),
                                        mb->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK) /* 如果挂起失败 */
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */
            return ret; /* 返回错误码 */
        }

        /* 有等待时间，启动线程定时器 */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            rt_tick_t timeout_tick = timeout; /* 超时 tick 数 */
            /* 获取定时器的起始 tick */
            tick_delta = rt_tick_get();

            LOG_D("mb_recv: start timer of thread:%s",
                  thread->parent.name); /* 打印调试日志 */

            /* 重置线程定时器的超时并启动它 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

        /* 重新调度 */
        rt_schedule();

        /* 从挂起状态恢复 */
        if (thread->error != RT_EOK) /* 如果线程错误码不为成功 */
        {
            /* 返回错误 */
            return thread->error; /* 返回错误码 */
        }
        level = rt_spin_lock_irqsave(&(mb->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 如果不是永远等待，则重新计算超时 tick */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            tick_delta = rt_tick_get() - tick_delta; /* 计算经过的 tick */
            timeout -= tick_delta; /* 减少超时时间 */
            if (timeout < 0) /* 如果超时时间小于 0 */
                timeout = 0; /* 设置为 0 */
        }
    }

    /* 填充指针 */
    *value = mb->msg_pool[mb->out_offset];

    /* 增加输出偏移 */
    ++ mb->out_offset;
    if (mb->out_offset >= mb->size) /* 如果出偏移超过大小 */
        mb->out_offset = 0; /* 循环 */

    /* 减少邮件条目 */
    if(mb->entry > 0) /* 如果邮件数大于 0 */
    {
        mb->entry --; /* 邮件数减 1 */
    }

    /* 恢复挂起的线程 */
    if (!rt_list_isempty(&(mb->suspend_sender_thread))) /* 如果有挂起的发送线程 */
    {
        rt_susp_list_dequeue(&(mb->suspend_sender_thread), RT_EOK); /* 唤醒一个发送线程 */

        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

        RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mb->parent.parent))); /* 调用获取钩子 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mb->parent.parent))); /* 调用获取钩子 */

    return RT_EOK; /* 返回成功 */
}

/* 接收邮件(不可中断) */
rt_err_t rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_recv); /* 导出函数符号 */

/* 接收邮件(可被中断打断) */
rt_err_t rt_mb_recv_interruptible(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_recv_interruptible); /* 导出函数符号 */

/* 接收邮件(可被致命信号打断) */
rt_err_t rt_mb_recv_killable(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mb_recv_killable); /* 导出函数符号 */

/**
 * @brief    此函数将设置邮箱对象的一些额外属性。
 *
 * @note     目前此函数仅支持 RT_IPC_CMD_RESET 命令来重置邮箱。
 *
 * @param    mb 是指向邮箱对象的指针。
 *
 * @param    cmd 是用于配置邮箱某些属性的命令。
 *
 * @param    arg 是执行命令的函数的参数。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示此函数执行失败。
 */
rt_err_t rt_mb_control(rt_mailbox_t mb, int cmd, void *arg)
{
    rt_base_t level; /* 中断级别 */

    RT_UNUSED(arg); /* 避免未使用变量警告 */

    /* 参数检查 */
    RT_ASSERT(mb != RT_NULL); /* 断言：邮箱指针不为空 */
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox); /* 断言：对象类型为邮箱 */

    if (cmd == RT_IPC_CMD_RESET) /* 如果是复位命令 */
    {
        level = rt_spin_lock_irqsave(&(mb->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 恢复所有等待的线程 */
        rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);
        /* 同时恢复邮箱私有的挂起发送线程 */
        rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);

        /* 重新初始化邮箱 */
        mb->entry      = 0; /* 邮件数为 0 */
        mb->in_offset  = 0; /* 入偏移为 0 */
        mb->out_offset = 0; /* 出偏移为 0 */

        rt_spin_unlock_irqrestore(&(mb->spinlock), level); /* 解自旋锁并恢复中断状态 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }

    return -RT_ERROR; /* 返回错误 */
}
RTM_EXPORT(rt_mb_control); /* 导出函数符号 */

/**@}*/
#endif /* RT_USING_MAILBOX */

#ifdef RT_USING_MESSAGEQUEUE /* 如果使用消息队列 */
/**
 * @addtogroup group_messagequeue Message Queue 消息队列
 * @{
 */

/**
 * @brief    初始化一个静态消息队列对象。
 *
 * @note     对于静态消息队列对象，其内存空间是在编译时由编译器分配的，
 *           并且应放置在读写数据段或未初始化数据段上。
 *           相比之下，rt_mq_create() 函数会自动分配内存空间并初始化消息队列。
 *
 * @see      rt_mq_create()
 *
 * @param    mq 是指向要初始化的消息队列的指针。假定消息队列的存储空间将
 *           在您的应用程序中分配。
 *
 * @param    name 是指向赋予消息队列的名称的指针。
 *
 * @param    msgpool 是指向您预先为消息队列分配的内存空间起始地址的指针。
 *           换句话说，msgpool 是指向起始地址的消息队列缓冲区的指针。
 *
 * @param    msg_size 是消息队列中消息的最大长度（单位：字节）。
 *
 * @param    pool_size 是预先为消息队列分配的内存空间的大小。
 *
 * @param    flag 是消息队列标志，它决定了当消息队列不可用时多个线程等待的排队方式。
 *           消息队列标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                         (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此消息队列的所有线程
 *               将成为非实时线程。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示初始化失败。
 *
 * @warning  此函数只能从线程中调用。
 */
rt_err_t rt_mq_init(rt_mq_t     mq,
                    const char *name,
                    void       *msgpool,
                    rt_size_t   msg_size,
                    rt_size_t   pool_size,
                    rt_uint8_t  flag)
{
    struct rt_mq_message *head; /* 消息头指针 */
    rt_base_t temp; /* 临时变量 */
    register rt_size_t msg_align_size; /* 消息对齐大小 */

    /* 参数检查 */
    RT_ASSERT(mq != RT_NULL); /* 断言：消息队列指针不为空 */
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    /* 初始化对象 */
    rt_object_init(&(mq->parent.parent), RT_Object_Class_MessageQueue, name);

    /* 设置父对象标志 */
    mq->parent.parent.flag = flag;

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(mq->parent));

    /* 设置消息池 */
    mq->msg_pool = msgpool;

    /* 获取正确的消息大小 */
    msg_align_size = RT_ALIGN(msg_size, RT_ALIGN_SIZE); /* 对齐消息大小 */
    mq->msg_size = msg_size; /* 设置消息大小 */
    mq->max_msgs = pool_size / (msg_align_size + sizeof(struct rt_mq_message)); /* 计算最大消息数 */

    if (0 == mq->max_msgs) /* 如果最大消息数为 0 */
    {
        return -RT_EINVAL; /* 返回无效参数错误 */
    }

    /* 初始化消息列表 */
    mq->msg_queue_head = RT_NULL; /* 消息队列头为空 */
    mq->msg_queue_tail = RT_NULL; /* 消息队列尾为空 */

    /* 初始化消息空闲列表 */
    mq->msg_queue_free = RT_NULL; /* 空闲列表为空 */
    for (temp = 0; temp < mq->max_msgs; temp ++) /* 遍历所有消息 */
    {
        head = (struct rt_mq_message *)((rt_uint8_t *)mq->msg_pool +
                                        temp * (msg_align_size + sizeof(struct rt_mq_message))); /* 获取消息头 */
        head->next = (struct rt_mq_message *)mq->msg_queue_free; /* 链接到空闲列表 */
        mq->msg_queue_free = head; /* 更新空闲列表头 */
    }

    /* 初始条目为零 */
    mq->entry = 0; /* 初始化消息条目为 0 */

    /* 初始化发送者挂起线程的附加列表 */
    rt_list_init(&(mq->suspend_sender_thread));
    rt_spin_lock_init(&(mq->spinlock)); /* 初始化自旋锁 */

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mq_init); /* 导出函数符号 */


/**
 * @brief    此函数将脱离一个静态消息队列对象。
 *
 * @note     此函数用于脱离由 rt_mq_init() 函数初始化的静态消息队列对象。
 *           相比之下，rt_mq_delete() 函数将删除一个消息队列对象。
 *           当消息队列成功脱离时，它将恢复消息队列列表中的所有挂起线程。
 *
 * @see      rt_mq_delete()
 *
 * @param    mq 是指向要脱离的消息队列对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，初始化成功。
 *           如果返回值为其他任何值，则表示消息队列脱离失败。
 *
 * @warning  此函数只能脱离由 rt_mq_init() 函数初始化的静态消息队列。
 *           如果消息队列是由 rt_mq_create() 函数创建的，您绝不能使用此函数脱离它,
 *           只能使用 rt_mq_delete() 函数完成删除。
 */
rt_err_t rt_mq_detach(rt_mq_t mq)
{
    rt_base_t level; /* 中断级别 */

    /* 参数检查 */
    RT_ASSERT(mq != RT_NULL); /* 断言：消息队列指针不为空 */
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue); /* 断言：对象类型为消息队列 */
    RT_ASSERT(rt_object_is_systemobject(&mq->parent.parent)); /* 断言：是系统对象 */

    level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */
    /* 恢复所有挂起的线程 */
    rt_susp_list_resume_all(&mq->parent.suspend_thread, RT_ERROR);
    /* 同时恢复消息队列私有的挂起发送线程 */
    rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 脱离消息队列对象 */
    rt_object_detach(&(mq->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mq_detach); /* 导出函数符号 */

#ifdef RT_USING_HEAP /* 如果使用堆 */
/**
 * @brief    创建一个消息队列对象。
 *
 * @note     对于消息队列对象，其内存空间是自动分配的。
 *           相比之下，rt_mq_init() 函数将初始化一个静态消息队列对象。
 *
 * @see      rt_mq_init()
 *
 * @param    name 是指向赋予消息队列的名称的指针。
 *
 * @param    msg_size 是消息队列中消息的最大长度（单位：字节）。
 *
 * @param    max_msgs 是消息队列中消息的最大数量。
 *
 * @param    flag 是消息队列标志，它决定了当消息队列不可用时多个线程等待的排队方式。
 *           消息队列标志可以是以下值之一：
 *
 *               RT_IPC_FLAG_PRIO          挂起的线程将按优先级顺序排队。
 *
 *               RT_IPC_FLAG_FIFO          挂起的线程将按先进先出方式排队
 *                                         (也称为先到先得 (FCFS) 调度策略)。
 *
 *               注意：RT_IPC_FLAG_FIFO 是非实时调度模式。强烈建议
 *               使用 RT_IPC_FLAG_PRIO 以确保线程是实时的，除非您的应用程序关心
 *               先进先出原则，并且您清楚地了解涉及此消息队列的所有线程
 *               将成为非实时线程。
 *
 * @return   返回指向消息队列对象的指针。当返回值为 RT_NULL 时，表示创建失败。
 *
 * @warning  此函数不能在中断上下文中调用。您可以使用宏 RT_DEBUG_NOT_IN_INTERRUPT 来检查。
 */
rt_mq_t rt_mq_create(const char *name,
                     rt_size_t   msg_size,
                     rt_size_t   max_msgs,
                     rt_uint8_t  flag)
{
    struct rt_messagequeue *mq; /* 消息队列指针 */
    struct rt_mq_message *head; /* 消息头指针 */
    rt_base_t temp; /* 临时变量 */
    register rt_size_t msg_align_size; /* 消息对齐大小 */

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO)); /* 断言：标志有效 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    /* 分配对象 */
    mq = (rt_mq_t)rt_object_allocate(RT_Object_Class_MessageQueue, name);
    if (mq == RT_NULL) /* 如果分配失败 */
        return mq; /* 返回空指针 */

    /* 设置父对象 */
    mq->parent.parent.flag = flag;

    /* 初始化 ipc 对象 */
    _ipc_object_init(&(mq->parent));

    /* 初始化消息队列 */

    /* 获取正确的消息大小 */
    msg_align_size = RT_ALIGN(msg_size, RT_ALIGN_SIZE); /* 对齐消息大小 */
    mq->msg_size = msg_size; /* 设置消息大小 */
    mq->max_msgs = max_msgs; /* 设置最大消息数 */

    /* 分配消息池 */
    mq->msg_pool = RT_KERNEL_MALLOC((msg_align_size + sizeof(struct rt_mq_message)) * mq->max_msgs); /* 分配内存 */
    if (mq->msg_pool == RT_NULL) /* 如果分配失败 */
    {
        rt_object_delete(&(mq->parent.parent)); /* 删除对象 */

        return RT_NULL; /* 返回空指针 */
    }

    /* 初始化消息列表 */
    mq->msg_queue_head = RT_NULL; /* 消息队列头为空 */
    mq->msg_queue_tail = RT_NULL; /* 消息队列尾为空 */

    /* 初始化消息空闲列表 */
    mq->msg_queue_free = RT_NULL; /* 空闲列表为空 */
    for (temp = 0; temp < mq->max_msgs; temp ++) /* 遍历所有消息 */
    {
        head = (struct rt_mq_message *)((rt_uint8_t *)mq->msg_pool +
                                        temp * (msg_align_size + sizeof(struct rt_mq_message))); /* 获取消息头 */
        head->next = (struct rt_mq_message *)mq->msg_queue_free; /* 链接到空闲列表 */
        mq->msg_queue_free = head; /* 更新空闲列表头 */
    }

    /* 初始条目为零 */
    mq->entry = 0; /* 初始化消息条目为 0 */

    /* 初始化发送者挂起线程的附加列表 */
    rt_list_init(&(mq->suspend_sender_thread));
    rt_spin_lock_init(&(mq->spinlock)); /* 初始化自旋锁 */

    return mq; /* 返回消息队列指针 */
}
RTM_EXPORT(rt_mq_create); /* 导出函数符号 */


/**
 * @brief    此函数将删除一个消息队列对象并释放内存。
 *
 * @note     此函数用于删除由 rt_mq_create() 函数创建的消息队列对象。
 *           相比之下，rt_mq_detach() 函数将脱离一个静态消息队列对象。
 *           当消息队列成功删除时，它将恢复消息队列列表中的所有挂起线程。
 *
 * @see      rt_mq_detach()
 *
 * @param    mq 是指向要删除的消息队列对象的指针。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示消息队列脱离失败。
 *
 * @warning  此函数只能删除由 rt_mq_create() 函数初始化的消息队列。
 *           如果消息队列是由 rt_mq_init() 函数初始化的，您绝不能使用此函数删除它,
 *           只能使用 rt_mq_detach() 函数完成脱离。
 *           例如，rt_mq_create() 函数不能在中断上下文中调用。
 */
rt_err_t rt_mq_delete(rt_mq_t mq)
{
    /* 参数检查 */
    RT_ASSERT(mq != RT_NULL); /* 断言：消息队列指针不为空 */
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue); /* 断言：对象类型为消息队列 */
    RT_ASSERT(rt_object_is_systemobject(&mq->parent.parent) == RT_FALSE); /* 断言：不是系统对象 */

    RT_DEBUG_NOT_IN_INTERRUPT; /* 调试断言：不在中断中 */

    rt_spin_lock(&(mq->spinlock)); /* 锁自旋锁 */
    /* 恢复所有挂起的线程 */
    rt_susp_list_resume_all(&(mq->parent.suspend_thread), RT_ERROR);
    /* 同时恢复消息队列私有的挂起发送线程 */
    rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);

    rt_spin_unlock(&(mq->spinlock)); /* 解自旋锁 */

    /* 释放消息队列池 */
    RT_KERNEL_FREE(mq->msg_pool);

    /* 删除消息队列对象 */
    rt_object_delete(&(mq->parent.parent));

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mq_delete); /* 导出函数符号 */
#endif /* RT_USING_HEAP */

/**
 * @brief    此函数将发送一条消息到消息队列对象。如果
 *           有线程挂起在消息队列上，该线程将被恢复。
 *
 * @note     当使用此函数发送消息时，如果消息队列已满，当前线程将
 *           等待超时。如果达到超时且仍然没有可用空间，发送
 *           线程将被恢复并返回错误码。相比之下，_rt_mq_send_wait() 函数
 *           在消息队列已满时将立即返回错误码而不等待。
 *
 * @see      _rt_mq_send_wait()
 *
 * @param    mq 是指向要发送的消息队列对象的指针。
 *
 * @param    buffer 是消息的内容。
 *
 * @param    size 是消息的长度（单位：字节）。
 *
 * @param    prio 是消息优先级，值越大优先级越高
 *
 * @param    timeout 是超时时间（单位：OS tick）。
 *
 * @param    suspend_flag 要挂起的线程的状态标志。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。如果返回值为其他任何值，
 *           则表示消息队列脱离失败。
 *
 * @warning  此函数可以在中断上下文和线程上下文中调用。
 */
static rt_err_t _rt_mq_send_wait(rt_mq_t mq,
                                 const void *buffer,
                                 rt_size_t size,
                                 rt_int32_t prio,
                                 rt_int32_t timeout,
                                 int suspend_flag)
{
    rt_base_t level; /* 中断级别 */
    struct rt_mq_message *msg; /* 消息指针 */
    rt_uint32_t tick_delta; /* delta tick */
    struct rt_thread *thread; /* 线程指针 */
    rt_err_t ret; /* 返回值 */

    RT_UNUSED(prio); /* 避免未使用变量警告 */

    /* 参数检查 */
    RT_ASSERT(mq != RT_NULL); /* 断言：消息队列指针不为空 */
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue); /* 断言：对象类型为消息队列 */
    RT_ASSERT(buffer != RT_NULL); /* 断言：缓冲区指针不为空 */
    RT_ASSERT(size != 0); /* 断言：大小不为 0 */

    /* 当前上下文检查 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0); /* 调试断言：调度器可用 */

    /* 大于一个消息大小 */
    if (size > mq->msg_size) /* 如果消息大小超过最大消息大小 */
        return -RT_ERROR; /* 返回错误 */

    /* 初始化 delta tick */
    tick_delta = 0;
    /* 获取当前线程 */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mq->parent.parent))); /* 调用放入钩子 */

    level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 获取一个空闲列表，必须有一个空项 */
    msg = (struct rt_mq_message *)mq->msg_queue_free;
    /* 对于非阻塞调用 */
    if (msg == RT_NULL && timeout == 0) /* 如果没有空闲消息且不等待 */
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        return -RT_EFULL; /* 返回满错误 */
    }

    /* 消息队列已满 */
    while ((msg = (struct rt_mq_message *)mq->msg_queue_free) == RT_NULL) /* 如果没有空闲消息 */
    {
        /* 重置线程中的错误码 */
        thread->error = -RT_EINTR;

        /* 不等待，返回超时 */
        if (timeout == 0) /* 如果不等待 */
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

            return -RT_EFULL; /* 返回满错误 */
        }

        /* 挂起当前线程 */
        ret = rt_thread_suspend_to_list(thread, &(mq->suspend_sender_thread),
                                        mq->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK) /* 如果挂起失败 */
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */
            return ret; /* 返回错误码 */
        }

        /* 有等待时间，启动线程定时器 */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            rt_tick_t timeout_tick = timeout; /* 超时 tick 数 */
            /* 获取定时器的起始 tick */
            tick_delta = rt_tick_get();

            LOG_D("mq_send_wait: start timer of thread:%s",
                  thread->parent.name); /* 打印调试日志 */

            /* 重置线程定时器的超时并启动它 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        /* 重新调度 */
        rt_schedule();

        /* 从挂起状态恢复 */
        if (thread->error != RT_EOK) /* 如果线程错误码不为成功 */
        {
            /* 返回错误 */
            return thread->error; /* 返回错误码 */
        }
        level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 如果不是永远等待，则重新计算超时 tick */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            tick_delta = rt_tick_get() - tick_delta; /* 计算经过的 tick */
            timeout -= tick_delta; /* 减少超时时间 */
            if (timeout < 0) /* 如果超时时间小于 0 */
                timeout = 0; /* 设置为 0 */
        }
    }

    /* 移动空闲列表指针 */
    mq->msg_queue_free = msg->next;

    rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* msg 是列表的新尾部，next 应该为 NULL */
    msg->next = RT_NULL;

    /* 添加长度 */
    ((struct rt_mq_message *)msg)->length = size;
    /* 复制缓冲区 */
    rt_memcpy(GET_MESSAGEBYTE_ADDR(msg), buffer, size);

    /* 禁用中断 */
    level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY /* 如果使用消息队列优先级 */
    msg->prio = prio; /* 设置消息优先级 */
    if (mq->msg_queue_head == RT_NULL) /* 如果消息队列头为空 */
        mq->msg_queue_head = msg; /* 设置消息队列头 */

    struct rt_mq_message *node, *prev_node = RT_NULL; /* 节点指针和前驱节点指针 */
    for (node = mq->msg_queue_head; node != RT_NULL; node = node->next) /* 遍历消息队列 */
    {
        if (node->prio < msg->prio) /* 如果当前节点优先级小于消息优先级 */
        {
            if (prev_node == RT_NULL) /* 如果没有前驱节点 */
                mq->msg_queue_head = msg; /* 设置消息队列头 */
            else /* 如果有前驱节点 */
                prev_node->next = msg; /* 插入到前驱节点后 */
            msg->next = node; /* 设置消息的下一个节点 */
            break; /* 跳出循环 */
        }
        if (node->next == RT_NULL) /* 如果到达队列尾部 */
        {
            if (node != msg) /* 如果当前节点不是消息本身 */
                node->next = msg; /* 插入到尾部 */
            mq->msg_queue_tail = msg; /* 更新消息队列尾 */
            break; /* 跳出循环 */
        }
        prev_node = node; /* 更新前驱节点 */
    }
#else /* 如果不使用消息队列优先级 */
    /* 将消息链接到消息队列 */
    if (mq->msg_queue_tail != RT_NULL) /* 如果尾部存在 */
    {
        /* 如果尾部存在, */
        ((struct rt_mq_message *)mq->msg_queue_tail)->next = msg;
    }

    /* 设置新尾部 */
    mq->msg_queue_tail = msg;
    /* 如果头部为空，设置头部 */
    if (mq->msg_queue_head == RT_NULL)
        mq->msg_queue_head = msg;
#endif

    if(mq->entry < RT_MQ_ENTRY_MAX) /* 如果消息条目未达到上限 */
    {
        /* 增加消息条目 */
        mq->entry ++;
    }
    else /* 如果消息条目达到上限 */
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */
        return -RT_EFULL; /* 值溢出 */
    }

    /* 恢复挂起的线程 */
    if (!rt_list_isempty(&mq->parent.suspend_thread)) /* 如果有挂起的接收线程 */
    {
        rt_susp_list_dequeue(&(mq->parent.suspend_thread), RT_EOK); /* 唤醒一个接收线程 */

        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }
    rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

    return RT_EOK; /* 返回成功 */
}

/* 发送消息等待(不可中断) */
rt_err_t rt_mq_send_wait(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_send_wait); /* 导出函数符号 */

/* 发送消息等待(可被中断打断) */
rt_err_t rt_mq_send_wait_interruptible(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_send_wait_interruptible); /* 导出函数符号 */

/* 发送消息等待(可被致命信号打断) */
rt_err_t rt_mq_send_wait_killable(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mq_send_wait_killable); /* 导出函数符号 */
/**
 * @brief    此函数将发送一条消息到消息队列对象。
 *           如果有线程挂起在消息队列上，该线程将被恢复。
 *
 * @note     当使用此函数发送消息时，如果消息队列已满，当前线程将等待超时。
 *           相比之下，当消息队列已满时，rt_mq_send_wait() 函数将立即返回错误码而不等待。
 *
 * @see      rt_mq_send_wait()
 *
 * @param    mq 是指向要发送的消息队列对象的指针。
 *
 * @param    buffer 是消息的内容。
 *
 * @param    size 是消息的长度（单位：字节）。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示消息队列脱离失败。
 *
 * @warning  此函数可以在中断上下文和线程上下文中调用。
 */
rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait(mq, buffer, size, 0); /* 不等待发送消息 */
}
RTM_EXPORT(rt_mq_send); /* 导出函数符号 */

/* 发送消息(可被中断打断) */
rt_err_t rt_mq_send_interruptible(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait_interruptible(mq, buffer, size, 0); /* 不等待发送消息 */
}
RTM_EXPORT(rt_mq_send_interruptible); /* 导出函数符号 */

/* 发送消息(可被致命信号打断) */
rt_err_t rt_mq_send_killable(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait_killable(mq, buffer, size, 0); /* 不等待发送消息 */
}
RTM_EXPORT(rt_mq_send_killable); /* 导出函数符号 */
/**
 * @brief    此函数将发送一条紧急消息到消息队列对象。
 *
 * @note     此函数与 rt_mq_send() 函数几乎相同。唯一的区别是
 *           当发送紧急消息时，消息被放置在消息队列的头部，以便
 *           接收者可以首先接收到紧急消息。
 *
 * @see      rt_mq_send()
 *
 * @param    mq 是指向要发送的消息队列对象的指针。
 *
 * @param    buffer 是消息的内容。
 *
 * @param    size 是消息的长度（单位：字节）。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示邮箱脱离失败。
 */
rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    rt_base_t level; /* 中断级别 */
    struct rt_mq_message *msg; /* 消息指针 */

    /* 参数检查 */
    RT_ASSERT(mq != RT_NULL); /* 断言：消息队列指针不为空 */
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue); /* 断言：对象类型为消息队列 */
    RT_ASSERT(buffer != RT_NULL); /* 断言：缓冲区指针不为空 */
    RT_ASSERT(size != 0); /* 断言：大小不为 0 */

    /* 大于一个消息大小 */
    if (size > mq->msg_size) /* 如果消息大小超过最大消息大小 */
        return -RT_ERROR; /* 返回错误 */

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mq->parent.parent))); /* 调用放入钩子 */

    level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 获取一个空闲列表，必须有一个空项 */
    msg = (struct rt_mq_message *)mq->msg_queue_free;
    /* 消息队列已满 */
    if (msg == RT_NULL) /* 如果没有空闲消息 */
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        return -RT_EFULL; /* 返回满错误 */
    }
    /* 移动空闲列表指针 */
    mq->msg_queue_free = msg->next;

    rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 添加长度 */
    ((struct rt_mq_message *)msg)->length = size;
    /* 复制缓冲区 */
    rt_memcpy(GET_MESSAGEBYTE_ADDR(msg), buffer, size);

    level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 将消息链接到消息队列的开头 */
    msg->next = (struct rt_mq_message *)mq->msg_queue_head;
    mq->msg_queue_head = msg;

    /* 如果没有尾部 */
    if (mq->msg_queue_tail == RT_NULL)
        mq->msg_queue_tail = msg;

    if(mq->entry < RT_MQ_ENTRY_MAX) /* 如果消息条目未达到上限 */
    {
        /* 增加消息条目 */
        mq->entry ++;
    }
    else /* 如果消息条目达到上限 */
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */
        return -RT_EFULL; /* 值溢出 */
    }

    /* 恢复挂起的线程 */
    if (!rt_list_isempty(&mq->parent.suspend_thread)) /* 如果有挂起的接收线程 */
    {
        rt_susp_list_dequeue(&(mq->parent.suspend_thread), RT_EOK); /* 唤醒一个接收线程 */

        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

    return RT_EOK; /* 返回成功 */
}
RTM_EXPORT(rt_mq_urgent); /* 导出函数符号 */

/**
 * @brief    此函数将从消息队列对象接收一条消息，
 *           如果消息队列对象中没有消息，线程将等待指定的时间。
 *
 * @note     只有当邮箱中有邮件时，接收线程才能立即获取邮件并返回 RT_EOK，
 *           否则接收线程将被挂起直到超时。
 *           如果在指定时间内未接收到邮件，将返回 -RT_ETIMEOUT。
 *
 * @param    mq 是指向要接收的消息队列对象的指针。
 *
 * @param    buffer 是消息的内容。
 *
 * @param    prio 是消息优先级，值越大优先级越高
 *
 * @param    size 是消息的长度（单位：字节）。
 *
 * @param    timeout 是超时时间（单位：OS tick）。如果消息不可用，线程将等待
 *           队列中的消息直到由此参数指定的时间量。
 *
 * @param    suspend_flag 要挂起的线程的状态标志。
 *
 *           注意:
 *           如果使用宏 RT_WAITING_FOREVER 设置此参数，意味着当
 *           队列中消息不可用时，线程将永远等待。
 *           如果使用宏 RT_WAITING_NO 设置此参数，意味着此
 *           函数是非阻塞的，将立即返回。
 *
 * @return   返回消息的实际长度。当返回值大于零时，操作成功。
 *           如果返回值为其他任何值，则表示邮箱释放失败。
 */
static rt_ssize_t _rt_mq_recv(rt_mq_t mq,
                              void *buffer,
                              rt_size_t size,
                              rt_int32_t *prio,
                              rt_int32_t timeout,
                              int suspend_flag)
{
    struct rt_thread *thread; /* 线程指针 */
    rt_base_t level; /* 中断级别 */
    struct rt_mq_message *msg; /* 消息指针 */
    rt_uint32_t tick_delta; /* delta tick */
    rt_err_t ret; /* 返回值 */
    rt_size_t len; /* 消息长度 */

    RT_UNUSED(prio); /* 避免未使用变量警告 */

    /* 参数检查 */
    RT_ASSERT(mq != RT_NULL); /* 断言：消息队列指针不为空 */
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue); /* 断言：对象类型为消息队列 */
    RT_ASSERT(buffer != RT_NULL); /* 断言：缓冲区指针不为空 */
    RT_ASSERT(size != 0); /* 断言：大小不为 0 */

    /* 当前上下文检查 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0); /* 调试断言：调度器可用 */

    /* 初始化 delta tick */
    tick_delta = 0;
    /* 获取当前线程 */
    thread = rt_thread_self();
    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mq->parent.parent))); /* 调用尝试获取钩子 */

    level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */

    /* 对于非阻塞调用 */
    if (mq->entry == 0 && timeout == 0) /* 如果消息队列为空且不等待 */
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        return -RT_ETIMEOUT; /* 返回超时错误 */
    }

    /* 消息队列为空 */
    while (mq->entry == 0) /* 如果消息队列为空 */
    {
        /* 重置线程中的错误码 */
        thread->error = -RT_EINTR;

        /* 不等待，返回超时 */
        if (timeout == 0) /* 如果不等待 */
        {
            /* 使能中断 */
            rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

            thread->error = -RT_ETIMEOUT; /* 设置线程错误码为超时 */

            return -RT_ETIMEOUT; /* 返回超时错误 */
        }

        /* 挂起当前线程 */
        ret = rt_thread_suspend_to_list(thread, &(mq->parent.suspend_thread),
                                        mq->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK) /* 如果挂起失败 */
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */
            return ret; /* 返回错误码 */
        }

        /* 有等待时间，启动线程定时器 */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            rt_tick_t timeout_tick = timeout; /* 超时 tick 数 */
            /* 获取定时器的起始 tick */
            tick_delta = rt_tick_get();

            LOG_D("set thread:%s to timer list",
                  thread->parent.name); /* 打印调试日志 */

            /* 重置线程定时器的超时并启动它 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        /* 重新调度 */
        rt_schedule();

        /* 接收消息 */
        if (thread->error != RT_EOK) /* 如果线程错误码不为成功 */
        {
            /* 返回错误 */
            return thread->error; /* 返回错误码 */
        }

        level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 如果不是永远等待，则重新计算超时 tick */
        if (timeout > 0) /* 如果超时时间大于 0 */
        {
            tick_delta = rt_tick_get() - tick_delta; /* 计算经过的 tick */
            timeout -= tick_delta; /* 减少超时时间 */
            if (timeout < 0) /* 如果超时时间小于 0 */
                timeout = 0; /* 设置为 0 */
        }
    }

    /* 从队列获取消息 */
    msg = (struct rt_mq_message *)mq->msg_queue_head;

    /* 移动消息队列头 */
    mq->msg_queue_head = msg->next;
    /* 到达队列尾部，设置为 NULL */
    if (mq->msg_queue_tail == msg)
        mq->msg_queue_tail = RT_NULL;

    /* 减少消息条目 */
    if(mq->entry > 0) /* 如果消息条目大于 0 */
    {
        mq->entry --; /* 消息条目减 1 */
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

    /* 获取真实消息长度 */
    len = ((struct rt_mq_message *)msg)->length;

    if (len > size) /* 如果消息长度大于缓冲区大小 */
        len = size; /* 截断消息长度 */
    /* 复制消息 */
    rt_memcpy(buffer, GET_MESSAGEBYTE_ADDR(msg), len);

#ifdef RT_USING_MESSAGEQUEUE_PRIORITY /* 如果使用消息队列优先级 */
    if (prio != RT_NULL) /* 如果优先级指针不为空 */
        *prio = msg->prio; /* 获取消息优先级 */
#endif
    level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */
    /* 将消息放入空闲列表 */
    msg->next = (struct rt_mq_message *)mq->msg_queue_free;
    mq->msg_queue_free = msg;

    /* 恢复挂起的线程 */
    if (!rt_list_isempty(&(mq->suspend_sender_thread))) /* 如果有挂起的发送线程 */
    {
        rt_susp_list_dequeue(&(mq->suspend_sender_thread), RT_EOK); /* 唤醒一个发送线程 */

        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mq->parent.parent))); /* 调用获取钩子 */

        rt_schedule(); /* 重新调度 */

        return len; /* 返回消息长度 */
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mq->parent.parent))); /* 调用获取钩子 */

    return len; /* 返回消息长度 */
}

/* 接收消息(不可中断) */
rt_ssize_t rt_mq_recv(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, 0, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_recv); /* 导出函数符号 */

/* 接收消息(可被中断打断) */
rt_ssize_t rt_mq_recv_interruptible(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, 0, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_recv_interruptible); /* 导出函数符号 */

/* 接收消息(可被致命信号打断) */
rt_ssize_t rt_mq_recv_killable(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, 0, timeout, RT_KILLABLE);
}
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY /* 如果使用消息队列优先级 */
/* 带优先级的发送消息等待 */
rt_err_t rt_mq_send_wait_prio(rt_mq_t mq,
                              const void *buffer,
                              rt_size_t size,
                              rt_int32_t prio,
                              rt_int32_t timeout,
                              int suspend_flag)
{
    return _rt_mq_send_wait(mq, buffer, size, prio, timeout, suspend_flag);
}
/* 带优先级的接收消息 */
rt_ssize_t rt_mq_recv_prio(rt_mq_t mq,
                           void *buffer,
                           rt_size_t size,
                           rt_int32_t *prio,
                           rt_int32_t timeout,
                           int suspend_flag)
{
    return _rt_mq_recv(mq, buffer, size, prio, timeout, suspend_flag);
}
#endif
RTM_EXPORT(rt_mq_recv_killable); /* 导出函数符号 */
/**
 * @brief    此函数将设置消息队列对象的一些额外属性。
 *
 * @note     目前此函数仅支持 RT_IPC_CMD_RESET 命令来重置消息队列。
 *
 * @param    mq 是指向消息队列对象的指针。
 *
 * @param    cmd 是用于配置消息队列某些属性的命令。
 *
 * @param    arg 是执行命令的函数的参数。
 *
 * @return   返回操作状态。当返回值为 RT_EOK 时，操作成功。
 *           如果返回值为其他任何值，则表示此函数执行失败。
 */
rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg)
{
    rt_base_t level; /* 中断级别 */
    struct rt_mq_message *msg; /* 消息指针 */

    RT_UNUSED(arg); /* 避免未使用变量警告 */

    /* 参数检查 */
    RT_ASSERT(mq != RT_NULL); /* 断言：消息队列指针不为空 */
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue); /* 断言：对象类型为消息队列 */

    if (cmd == RT_IPC_CMD_RESET) /* 如果是复位命令 */
    {
        level = rt_spin_lock_irqsave(&(mq->spinlock)); /* 保存中断状态并锁自旋锁 */

        /* 恢复所有等待的线程 */
        rt_susp_list_resume_all(&mq->parent.suspend_thread, RT_ERROR);
        /* 同时恢复消息队列私有的挂起发送线程 */
        rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);

        /* 释放队列中的所有消息 */
        while (mq->msg_queue_head != RT_NULL) /* 如果消息队列头不为空 */
        {
            /* 从队列获取消息 */
            msg = (struct rt_mq_message *)mq->msg_queue_head;

            /* 移动消息队列头 */
            mq->msg_queue_head = msg->next;
            /* 到达队列尾部，设置为 NULL */
            if (mq->msg_queue_tail == msg)
                mq->msg_queue_tail = RT_NULL;

            /* 将消息放入空闲列表 */
            msg->next = (struct rt_mq_message *)mq->msg_queue_free;
            mq->msg_queue_free = msg;
        }

        /* 清除条目 */
        mq->entry = 0;

        rt_spin_unlock_irqrestore(&(mq->spinlock), level); /* 解自旋锁并恢复中断状态 */

        rt_schedule(); /* 重新调度 */

        return RT_EOK; /* 返回成功 */
    }

    return -RT_ERROR; /* 返回错误 */
}
RTM_EXPORT(rt_mq_control); /* 导出函数符号 */

/**@}*/
#endif /* RT_USING_MESSAGEQUEUE */
/**@}*/
