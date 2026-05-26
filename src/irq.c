/*
 * 版权所有 (c) 2006-2022, RT-Thread 开发团队
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更日志:
 * 日期           作者       备注
 * 2006-02-24     Bernard    首个版本
 * 2006-05-03     Bernard    添加 IRQ_DEBUG
 * 2016-08-09     ArdaFu     添加中断进入和离开的钩子函数。
 * 2018-11-22     Jesven     rt_interrupt_get_nest 函数增加禁用中断功能
 * 2021-08-15     Supperthomas 修复注释
 * 2022-01-07     Gabriel    将 __on_rt_xxxxx_hook 移动到 irq.c 中
 * 2022-07-04     Yunjie     修复 RT_DEBUG_LOG
 * 2023-09-15     xqyjlj     性能优化 rt_hw_interrupt_disable/enable
 * 2024-01-05     Shell      修复 rt_interrupt_get_nest 中的数据竞争问题
 * 2024-01-03     Shell      支持中断上下文
 */

/* 包含硬件相关的头文件，提供底层硬件操作接口 */
#include <rthw.h>
/* 包含 RT-Thread 内核的核心头文件，提供内核对象、调度等接口 */
#include <rtthread.h>

/* 定义调试标签，用于标识该模块的调试信息来源 */
#define DBG_TAG           "kernel.irq"
/* 定义调试级别为 INFO（信息级），只输出信息及以上级别的日志 */
#define DBG_LVL           DBG_INFO
/* 包含 RT-Thread 调试系统头文件，实现 LOG_D 等日志宏 */
#include <rtdbg.h>

/* 如果定义了使用钩子函数(RT_USING_HOOK)且钩子使用函数指针(RT_HOOK_USING_FUNC_PTR) */
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)

/* 声明中断进入钩子函数指针，默认初始化为 NULL */
void (*rt_interrupt_enter_hook)(void);
/* 声明中断离开钩子函数指针，默认初始化为 NULL */
void (*rt_interrupt_leave_hook)(void);

/**
 * @ingroup group_hook
 *
 * @brief 该函数用于设置系统进入中断时调用的钩子函数
 *
 * @note 钩子函数必须简单，绝对不能被阻塞或挂起。
 *
 * @param hook 即将被调用的函数指针
 */
void rt_interrupt_enter_sethook(void (*hook)(void))
{
    /* 将传入的钩子函数指针赋值给全局的中断进入钩子变量 */
    rt_interrupt_enter_hook = hook;
}

/**
 * @ingroup group_hook
 *
 * @brief 该函数用于设置系统退出中断时调用的钩子函数
 *
 * @note 钩子函数必须简单，绝对不能被阻塞或挂起。
 *
 * @param hook 即将被调用的函数指针
 */
void rt_interrupt_leave_sethook(void (*hook)(void))
{
    /* 将传入的钩子函数指针赋值给全局的中断离开钩子变量 */
    rt_interrupt_leave_hook = hook;
}
#endif /* 结束条件编译 RT_USING_HOOK */

/**
 * @addtogroup group_kernel_core
 */

/**@{*/

/* 如果使用了多核处理器(SMP) */
#ifdef RT_USING_SMP
/* 定义中断嵌套计数宏，直接获取当前 CPU 核心的 irq_nest 成员 */
#define rt_interrupt_nest rt_cpu_self()->irq_nest
/* 如果是单核处理器 */
#else
/* 定义全局的易失性原子变量，记录中断嵌套层数，初始化为 0 */
volatile rt_atomic_t rt_interrupt_nest = 0;
#endif /* 结束条件编译 RT_USING_SMP */

/* 如果架构支持使用中断上下文列表 */
#ifdef ARCH_USING_IRQ_CTX_LIST
/**
 * @brief 将中断上下文压入当前 CPU 的上下文链表
 * @param this_ctx 当前中断上下文结构体指针
 */
void rt_interrupt_context_push(rt_interrupt_context_t this_ctx)
{
    /* 获取当前 CPU 核心的结构体指针 */
    struct rt_cpu *this_cpu = rt_cpu_self();
    /* 将当前中断上下文节点插入到当前 CPU 的中断上下文链表头部 */
    rt_slist_insert(&this_cpu->irq_ctx_head, &this_ctx->node);
}

/**
 * @brief 将中断上下文从当前 CPU 的上下文链表中弹出
 */
void rt_interrupt_context_pop(void)
{
    /* 获取当前 CPU 核心的结构体指针 */
    struct rt_cpu *this_cpu = rt_cpu_self();
    /* 从当前 CPU 的中断上下文链表头部弹出一个节点 */
    rt_slist_pop(&this_cpu->irq_ctx_head);
}

/**
 * @brief 获取当前 CPU 的中断上下文
 * @return 返回中断上下文的指针
 */
void *rt_interrupt_context_get(void)
{
    /* 获取当前 CPU 核心的结构体指针 */
    struct rt_cpu *this_cpu = rt_cpu_self();
    /* 获取链表第一个条目，并将其转换为中断上下文结构体，最后返回其中的 context 成员 */
    return rt_slist_first_entry(&this_cpu->irq_ctx_head, struct rt_interrupt_context, node)->context;
}
#endif /* 结束条件编译 ARCH_USING_IRQ_CTX_LIST */

/**
 * @brief 该函数由 BSP (板级支持包) 调用，当进入中断服务例程时触发
 *
 * @note 请不要在应用程序中调用此例程
 *
 * @see rt_interrupt_leave
 */
/* rt_weak 声明该函数为弱符号，如果用户在其他地方重定义了该函数，则链接时使用用户的版本 */
rt_weak void rt_interrupt_enter(void)
{
    /* 原子操作：中断嵌套层数加 1，保证多核/中断情况下的数据安全 */
    rt_atomic_add(&(rt_interrupt_nest), 1);
    /* 调用中断进入钩子函数（如果已配置并使能） */
    RT_OBJECT_HOOK_CALL(rt_interrupt_enter_hook,());
    /* 输出调试级别的日志：中断已到来，以及当前的中断嵌套层数 */
    LOG_D("irq has come..., irq current nest:%d",
          (rt_int32_t)rt_atomic_load(&(rt_interrupt_nest)));
}
/* 将该函数导出到内核符号表，方便模块调用 */
RTM_EXPORT(rt_interrupt_enter);


/**
 * @brief 该函数由 BSP (板级支持包) 调用，当离开中断服务例程时触发
 *
 * @note 请不要在应用程序中调用此例程
 *
 * @see rt_interrupt_enter
 */
/* rt_weak 声明该函数为弱符号 */
rt_weak void rt_interrupt_leave(void)
{
    /* 输出调试级别的日志：中断即将离开，以及当前的中断嵌套层数 */
    LOG_D("irq is going to leave, irq current nest:%d",
                 (rt_int32_t)rt_atomic_load(&(rt_interrupt_nest)));
    /* 调用中断离开钩子函数（如果已配置并使能） */
    RT_OBJECT_HOOK_CALL(rt_interrupt_leave_hook,());
    /* 原子操作：中断嵌套层数减 1，保证多核/中断情况下的数据安全 */
    rt_atomic_sub(&(rt_interrupt_nest), 1);

}
/* 将该函数导出到内核符号表 */
RTM_EXPORT(rt_interrupt_leave);


/**
 * @brief 该函数用于获取当前的中断嵌套层数
 *
 * 用户应用程序可以调用此函数来判断当前上下文是否处于中断上下文。
 *
 * @return 返回嵌套中断的数量
 */
/* rt_weak 声明该函数为弱符号 */
rt_weak rt_uint8_t rt_interrupt_get_nest(void)
{
    /* 定义返回值变量 */
    rt_uint8_t ret;
    /* 定义中断状态保存变量 */
    rt_base_t level;

    /* 禁用本地 CPU 中断，并保存之前的中断状态到 level */
    level = rt_hw_local_irq_disable();
    /* 原子读取当前的中断嵌套层数到 ret 变量 */
    ret = rt_atomic_load(&rt_interrupt_nest);
    /* 恢复之前保存的中断状态（开/关中断） */
    rt_hw_local_irq_enable(level);
    /* 返回获取到的中断嵌套层数 */
    return ret;
}
/* 将该函数导出到内核符号表 */
RTM_EXPORT(rt_interrupt_get_nest);

/* 导出底层硬件中断禁用函数到内核符号表 */
RTM_EXPORT(rt_hw_interrupt_disable);
/* 导出底层硬件中断使能函数到内核符号表 */
RTM_EXPORT(rt_hw_interrupt_enable);

/**
 * @brief 该函数用于判断当前 CPU 中断是否处于禁用状态
 * @return 返回 RT_TRUE 表示中断被禁用，RT_FALSE 表示中断未被禁用
 */
/* rt_weak 声明该函数为弱符号，默认实现返回 RT_FALSE，需由具体架构代码覆盖实现 */
rt_weak rt_bool_t rt_hw_interrupt_is_disabled(void)
{
    /* 默认返回假，具体架构需要重写此函数以反映真实的中断禁用状态 */
    return RT_FALSE;
}
/* 将该函数导出到内核符号表 */
RTM_EXPORT(rt_hw_interrupt_is_disabled);
/**@}*/
