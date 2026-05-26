/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
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
 * 2006-03-14     Bernard      the first version (首次版本)
 * 2006-04-21     Bernard      change the scheduler lock to interrupt lock (将调度锁改为中断锁)
 * 2006-05-18     Bernard      fix the object init bug (修复对象初始化bug)
 * 2006-08-03     Bernard      add hook support (增加钩子支持)
 * 2007-01-28     Bernard      rename RT_OBJECT_Class_Static to RT_Object_Class_Static (重命名静态对象宏)
 * 2010-10-26     yi.qiu       add module support in rt_object_allocate and rt_object_free (在分配和释放中增加模块支持)
 * 2017-12-10     Bernard      Add object_info enum. (增加 object_info 枚举)
 * 2018-01-25     Bernard      Fix the object find issue when enable MODULE. (修复开启模块时的对象查找问题)
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to object.c (将钩子函数移至 object.c)
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable (优化中断开关性能，改用自旋锁)
 * 2023-11-17     xqyjlj       add process group and session support (增加进程组和会话支持)
 */

/* 包含 RT-Thread 系统的核心头文件 */
#include <rtthread.h>
/* 包含 RT-Thread 硬件相关定义头文件 */
#include <rthw.h>

/* 如果开启了动态模块（应用模块）功能 */
#ifdef RT_USING_MODULE
/* 包含动态模块头文件 */
#include <dlmodule.h>
#endif /* RT_USING_MODULE */

/* 如果开启了智能操作系统（进程/内存隔离等特性） */
#ifdef RT_USING_SMART
/* 包含轻量级进程头文件 */
#include <lwp.h>
#endif

/* 定义调试标签，用于日志输出标识 */
#define DBG_TAG           "kernel.obj"
/* 定义调试级别为错误级别，即只输出错误日志 */
#define DBG_LVL           DBG_ERROR
/* 包含 RT-Thread 调试日志头文件 */
#include <rtdbg.h>

/* 自定义对象结构体定义，继承自基础内核对象 */
struct rt_custom_object
{
    struct rt_object parent;     /* 继承的父对象，必须放在结构体首位以实现多态 */
    rt_err_t (*destroy)(void *); /* 自定义对象的销毁回调函数指针 */
    void *data;                  /* 自定义对象的私有数据指针 */
};

/*
 * define object_info for the number of _object_container items.
 * 定义 object_info 枚举，用于表示 _object_container 数组的索引（即对象容器的数量和类型）
 */
enum rt_object_info_type
{
    RT_Object_Info_Thread = 0,                         /**< The object is a thread. 线程对象索引为0 */
#ifdef RT_USING_SEMAPHORE
    RT_Object_Info_Semaphore,                          /**< The object is a semaphore. 信号量对象索引 */
#endif
#ifdef RT_USING_MUTEX
    RT_Object_Info_Mutex,                              /**< The object is a mutex. 互斥量对象索引 */
#endif
#ifdef RT_USING_EVENT
    RT_Object_Info_Event,                              /**< The object is a event. 事件集对象索引 */
#endif
#ifdef RT_USING_MAILBOX
    RT_Object_Info_MailBox,                            /**< The object is a mail box. 邮箱对象索引 */
#endif
#ifdef RT_USING_MESSAGEQUEUE
    RT_Object_Info_MessageQueue,                       /**< The object is a message queue. 消息队列对象索引 */
#endif
#ifdef RT_USING_MEMHEAP
    RT_Object_Info_MemHeap,                            /**< The object is a memory heap 内存堆对象索引 */
#endif
#ifdef RT_USING_MEMPOOL
    RT_Object_Info_MemPool,                            /**< The object is a memory pool. 内存池对象索引 */
#endif
#ifdef RT_USING_DEVICE
    RT_Object_Info_Device,                             /**< The object is a device 设备对象索引 */
#endif
    RT_Object_Info_Timer,                              /**< The object is a timer. 定时器对象索引 */
#ifdef RT_USING_MODULE
    RT_Object_Info_Module,                             /**< The object is a module. 动态模块对象索引 */
#endif
#ifdef RT_USING_HEAP
    RT_Object_Info_Memory,                             /**< The object is a memory. 堆内存对象索引 */
#endif
#ifdef RT_USING_SMART
    RT_Object_Info_Channel,                            /**< The object is a IPC channel 进程间通信通道对象索引 */
    RT_Object_Info_ProcessGroup,                       /**< The object is a process group 进程组对象索引 */
    RT_Object_Info_Session,                            /**< The object is a session 会话对象索引 */
#endif
#ifdef RT_USING_HEAP
    RT_Object_Info_Custom,                             /**< The object is a custom object 自定义对象索引 */
#endif
    RT_Object_Info_Unknown,                            /**< The object is unknown. 未知类型，同时也代表了对象容器的总数量 */
};

/* 宏：用于静态初始化对象容器中的双向链表，将前后指针都指向自己（空链表） */
#define _OBJ_CONTAINER_LIST_INIT(c)     \
    {&(_object_container[c].object_list), &(_object_container[c].object_list)}

/* 静态定义并初始化对象容器数组，每种内核对象类型对应一个容器 */
static struct rt_object_information _object_container[RT_Object_Info_Unknown] =
{
    /* initialize object container - thread 初始化线程对象容器 */
    {RT_Object_Class_Thread, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Thread), sizeof(struct rt_thread), RT_SPINLOCK_INIT},
#ifdef RT_USING_SEMAPHORE
    /* initialize object container - semaphore 初始化信号量对象容器 */
    {RT_Object_Class_Semaphore, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Semaphore), sizeof(struct rt_semaphore), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MUTEX
    /* initialize object container - mutex 初始化互斥量对象容器 */
    {RT_Object_Class_Mutex, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Mutex), sizeof(struct rt_mutex), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_EVENT
    /* initialize object container - event 初始化事件集对象容器 */
    {RT_Object_Class_Event, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Event), sizeof(struct rt_event), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MAILBOX
    /* initialize object container - mailbox 初始化邮箱对象容器 */
    {RT_Object_Class_MailBox, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MailBox), sizeof(struct rt_mailbox), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MESSAGEQUEUE
    /* initialize object container - message queue 初始化消息队列对象容器 */
    {RT_Object_Class_MessageQueue, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MessageQueue), sizeof(struct rt_messagequeue), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MEMHEAP
    /* initialize object container - memory heap 初始化内存堆对象容器 */
    {RT_Object_Class_MemHeap, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MemHeap), sizeof(struct rt_memheap), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_MEMPOOL
    /* initialize object container - memory pool 初始化内存池对象容器 */
    {RT_Object_Class_MemPool, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_MemPool), sizeof(struct rt_mempool), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_DEVICE
    /* initialize object container - device 初始化设备对象容器 */
    {RT_Object_Class_Device, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Device), sizeof(struct rt_device), RT_SPINLOCK_INIT},
#endif
    /* initialize object container - timer 初始化定时器对象容器 */
    {RT_Object_Class_Timer, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Timer), sizeof(struct rt_timer), RT_SPINLOCK_INIT},
#ifdef RT_USING_MODULE
    /* initialize object container - module 初始化动态模块对象容器 */
    {RT_Object_Class_Module, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Module), sizeof(struct rt_dlmodule), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_HEAP
    /* initialize object container - small memory 初始化堆内存对象容器 */
    {RT_Object_Class_Memory, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Memory), sizeof(struct rt_memory), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_SMART
    /* initialize object container - module 初始化通道、进程组、会话对象容器 */
    {RT_Object_Class_Channel, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Channel), sizeof(struct rt_channel), RT_SPINLOCK_INIT},
    {RT_Object_Class_ProcessGroup, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_ProcessGroup), sizeof(struct rt_processgroup), RT_SPINLOCK_INIT},
    {RT_Object_Class_Session, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Session), sizeof(struct rt_session), RT_SPINLOCK_INIT},
#endif
#ifdef RT_USING_HEAP
    /* 初始化自定义对象容器 */
    {RT_Object_Class_Custom, _OBJ_CONTAINER_LIST_INIT(RT_Object_Info_Custom), sizeof(struct rt_custom_object), RT_SPINLOCK_INIT},
#endif
};

/* 如果开启了钩子函数且使用函数指针方式实现 */
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
/* 定义对象附加(初始化)时的钩子函数指针 */
static void (*rt_object_attach_hook)(struct rt_object *object);
/* 定义对象脱离时的钩子函数指针 */
static void (*rt_object_detach_hook)(struct rt_object *object);
/* 定义对象尝试获取时的钩子函数指针 */
void (*rt_object_trytake_hook)(struct rt_object *object);
/* 定义对象成功获取时的钩子函数指针 */
void (*rt_object_take_hook)(struct rt_object *object);
/* 定义对象释放时的钩子函数指针 */
void (*rt_object_put_hook)(struct rt_object *object);

/**
 * @addtogroup group_hook
 * 钩子函数分组
 * @{
 */

/**
 * @brief This function will set a hook function, which will be invoked when object
 *        attaches to kernel object system.
 * 此函数设置一个钩子函数，当对象附加到内核对象系统时将被调用。
 *
 * @param hook is the hook function.
 * 参数 hook 是钩子函数指针。
 */
void rt_object_attach_sethook(void (*hook)(struct rt_object *object))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_object_attach_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when object
 *        detaches from kernel object system.
 * 此函数设置一个钩子函数，当对象从内核对象系统脱离时将被调用。
 *
 * @param hook is the hook function
 * 参数 hook 是钩子函数指针
 */
void rt_object_detach_sethook(void (*hook)(struct rt_object *object))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_object_detach_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when object
 *        is taken from kernel object system.
 * 此函数设置一个钩子函数，当对象正准备从内核对象系统获取时将被调用。
 *
 *        The object is taken means: 对象被获取意味着：
 *            semaphore - semaphore is taken by thread 信号量被线程获取
 *            mutex - mutex is taken by thread 互斥量被线程获取
 *            event - event is received by thread 事件被线程接收
 *            mailbox - mail is received by thread 邮件被线程接收
 *            message queue - message is received by thread 消息被线程接收
 *
 * @param hook is the hook function.
 * 参数 hook 是钩子函数指针。
 */
void rt_object_trytake_sethook(void (*hook)(struct rt_object *object))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_object_trytake_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when object
 *        have been taken from kernel object system.
 * 此函数设置一个钩子函数，当对象已经成功从内核对象系统获取时将被调用。
 *
 *        The object have been taken means: 对象已经被获取意味着：
 *            semaphore - semaphore have been taken by thread 信号量已被线程获取
 *            mutex - mutex have been taken by thread 互斥量已被线程获取
 *            event - event have been received by thread 事件已被线程接收
 *            mailbox - mail have been received by thread 邮件已被线程接收
 *            message queue - message have been received by thread 消息已被线程接收
 *            timer - timer is started 定时器已启动
 *
 * @param hook the hook function.
 * 参数 hook 是钩子函数指针。
 */
void rt_object_take_sethook(void (*hook)(struct rt_object *object))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_object_take_hook = hook;
}

/**
 * @brief This function will set a hook function, which will be invoked when object
 *        is put to kernel object system.
 * 此函数设置一个钩子函数，当对象被放回（释放）到内核对象系统时将被调用。
 *
 * @param hook is the hook function
 * 参数 hook 是钩子函数指针
 */
void rt_object_put_sethook(void (*hook)(struct rt_object *object))
{
    /* 将传入的钩子函数指针赋值给全局变量 */
    rt_object_put_hook = hook;
}

/** @} group_hook 钩子函数分组结束 */
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_object_management
 * 对象管理分组
 * @{
 */

/**
 * @brief This function will return the specified type of object information.
 * 此函数返回指定类型的对象信息结构体指针。
 *
 * @param type is the type of object, which can be
 *             RT_Object_Class_Thread/Semaphore/Mutex... etc
 * 参数 type 是对象类型枚举，如线程、信号量、互斥量等
 *
 * @return the object type information or RT_NULL
 * 返回对象类型信息结构体指针，如果找不到则返回 RT_NULL
 */
struct rt_object_information *
rt_object_get_information(enum rt_object_class_type type)
{
    /* 定义循环索引变量 */
    int index;

    /* 剔除类型中的静态标志位，获取纯粹的对象类型 */
    type = (enum rt_object_class_type)(type & ~RT_Object_Class_Static);

    /* 遍历对象容器数组 */
    for (index = 0; index < RT_Object_Info_Unknown; index ++)
        /* 如果找到类型匹配的容器，返回该容器的指针 */
        if (_object_container[index].type == type) return &_object_container[index];

    /* 未找到匹配类型，返回空指针 */
    return RT_NULL;
}
/* 将该函数导出给内核模块系统使用 */
RTM_EXPORT(rt_object_get_information);

/**
 * @brief This function will return the length of object list in object container.
 * 此函数返回对象容器中对象链表的长度（即该类型对象的数量）。
 *
 * @param type is the type of object, which can be
 *             RT_Object_Class_Thread/Semaphore/Mutex... etc
 * 参数 type 是对象类型枚举
 *
 * @return the length of object list
 * 返回对象链表的长度
 */
int rt_object_get_length(enum rt_object_class_type type)
{
    /* 初始化计数器为0 */
    int count = 0;
    /* 定义中断级别变量，用于保存中断状态 */
    rt_base_t level;
    /* 定义链表节点指针，用于遍历 */
    struct rt_list_node *node = RT_NULL;
    /* 定义对象信息结构体指针 */
    struct rt_object_information *information = RT_NULL;

    /* 获取指定类型的对象信息 */
    information = rt_object_get_information((enum rt_object_class_type)type);
    /* 如果信息为空，说明类型无效，直接返回0 */
    if (information == RT_NULL) return 0;

    /* 获取自旋锁并关闭中断，保存之前的中断状态到 level */
    level = rt_spin_lock_irqsave(&(information->spinlock));
    /* 遍历对象链表 */
    rt_list_for_each(node, &(information->object_list))
    {
        /* 每遍历到一个节点，计数器加1 */
        count ++;
    }
    /* 释放自旋锁并恢复之前的中断状态 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    /* 返回对象数量 */
    return count;
}
/* 导出函数符号 */
RTM_EXPORT(rt_object_get_length);

/**
 * @brief This function will copy the object pointer of the specified type,
 *        with the maximum size specified by maxlen.
 * 此函数将指定类型的对象指针拷贝到给定的数组中，最大拷贝数量由 maxlen 指定。
 *
 * @param type is the type of object, which can be
 *             RT_Object_Class_Thread/Semaphore/Mutex... etc
 * 参数 type 是对象类型枚举
 *
 * @param pointers is the pointer will be saved to.
 * 参数 pointers 是用于保存对象指针的数组
 *
 * @param maxlen is the maximum number of pointers can be saved.
 * 参数 maxlen 是数组能保存的最大指针数量
 *
 * @return the copied number of object pointers.
 * 返回实际拷贝的对象指针数量
 */
int rt_object_get_pointers(enum rt_object_class_type type, rt_object_t *pointers, int maxlen)
{
    /* 初始化索引（也是计数器）为0 */
    int index = 0;
    /* 定义中断级别变量 */
    rt_base_t level;

    /* 定义对象结构体指针 */
    struct rt_object *object;
    /* 定义链表节点指针 */
    struct rt_list_node *node = RT_NULL;
    /* 定义对象信息结构体指针 */
    struct rt_object_information *information = RT_NULL;

    /* 如果最大长度小于等于0，直接返回0 */
    if (maxlen <= 0) return 0;

    /* 获取指定类型的对象信息 */
    information = rt_object_get_information(type);
    /* 如果信息为空，返回0 */
    if (information == RT_NULL) return 0;

    /* 获取自旋锁并关闭中断 */
    level = rt_spin_lock_irqsave(&(information->spinlock));
    /* retrieve pointer of object 遍历对象链表以获取对象指针 */
    rt_list_for_each(node, &(information->object_list))
    {
        /* 通过链表节点获取所在的对象结构体首地址 */
        object = rt_list_entry(node, struct rt_object, list);

        /* 将对象指针存入 pointers 数组 */
        pointers[index] = object;
        /* 索引递增 */
        index ++;

        /* 如果已达到最大存储数量，跳出循环 */
        if (index >= maxlen) break;
    }
    /* 释放自旋锁并恢复中断 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    /* 返回实际拷贝的对象指针数量 */
    return index;
}
/* 导出函数符号 */
RTM_EXPORT(rt_object_get_pointers);

/**
 * @brief This function will initialize an object and add it to object system
 *        management.
 * 此函数初始化一个静态对象并将其添加到对象系统管理中。
 *
 * @param object The specified object to be initialized.
 *               The object pointer that needs to be initialized must point to
 *               a specific object memory block, not a null pointer or a wild pointer.
 * 参数 object 是待初始化的对象指针。必须指向有效的内存块，不能是空指针或野指针。
 *
 * @param type The object type. The type of the object must be a enumeration
 *             type listed in rt_object_class_type, RT_Object_Class_Static
 *             excluded. (For static objects, or objects initialized with the
 *             rt_object_init interface, the system identifies it as an
 *             RT_Object_Class_Static type)
 * 参数 type 是对象类型。不能包含 RT_Object_Class_Static 标志（系统会自动添加）。
 *
 * @param name Name of the object. In system, the object's name must be unique.
 *             Each object can be set to a name, and the maximum length for the
 *             name is specified by RT_NAME_MAX. The system does not care if it
 *             uses '\0' as a terminal symbol.
 * 参数 name 是对象名称。在系统中建议唯一，最大长度由 RT_NAME_MAX 决定。
 */
void rt_object_init(struct rt_object         *object,
                    enum rt_object_class_type type,
                    const char               *name)
{
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义对象名称长度变量 */
    rt_size_t obj_name_len;
/* 如果开启了调试断言 */
#ifdef RT_DEBUGING_ASSERT
    /* 定义链表节点指针，用于遍历检查 */
    struct rt_list_node *node = RT_NULL;
#endif /* RT_DEBUGING_ASSERT */
    /* 定义对象信息结构体指针 */
    struct rt_object_information *information;
/* 如果开启了动态模块功能 */
#ifdef RT_USING_MODULE
    /* 获取当前运行的模块指针 */
    struct rt_dlmodule *module = dlmodule_self();
#endif /* RT_USING_MODULE */

    /* get object information 获取对象类型对应的信息结构 */
    information = rt_object_get_information(type);
    /* 断言信息结构不为空，确保类型有效 */
    RT_ASSERT(information != RT_NULL);

/* 如果开启了调试断言 */
#ifdef RT_DEBUGING_ASSERT
    /* check object type to avoid re-initialization 检查对象类型以避免重复初始化 */

    /* enter critical 进入临界区，获取自旋锁并关中断 */
    level = rt_spin_lock_irqsave(&(information->spinlock));
    /* try to find object 遍历链表尝试查找是否已经存在该对象 */
    for (node  = information->object_list.next;
            node != &(information->object_list);
            node  = node->next)
    {
        struct rt_object *obj;

        /* 获取当前节点对应的对象结构体 */
        obj = rt_list_entry(node, struct rt_object, list);
        /* 断言当前对象不等于要初始化的对象，防止重复初始化 */
        RT_ASSERT(obj != object);
    }
    /* leave critical 离开临界区，释放自旋锁并恢复中断 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);
#endif /* RT_DEBUGING_ASSERT */

    /* initialize object's parameters 初始化对象的参数 */
    /* set object type to static 设置对象类型为静态（加上静态标志位） */
    object->type = type | RT_Object_Class_Static;

/* 如果配置了对象名称数组长度大于0 */
#if RT_NAME_MAX > 0
    /* 如果传入的名称不为空 */
    if (name)
    {
        /* 获取名称长度 */
        obj_name_len = rt_strlen(name);
        /* 如果名称长度超过限制 */
        if(obj_name_len > RT_NAME_MAX - 1)
        {
            /* 输出错误日志提示名称超限 */
            LOG_E("Object name %s exceeds RT_NAME_MAX=%d, consider increasing RT_NAME_MAX.", name, RT_NAME_MAX);
        }
        /* 截断拷贝名称，最多拷贝 RT_NAME_MAX - 1 个字符 */
        rt_strncpy(object->name, name, RT_NAME_MAX - 1);
        /* 确保最后一个字符是字符串结束符 '\0' */
        object->name[RT_NAME_MAX - 1] = '\0';
    }
    /* 如果传入名称为空 */
    else
    {
        /* 将对象名称第一个字节置为结束符 */
        object->name[0] = '\0';
    }
#else
    /* 如果未配置名称数组长度，则直接将指针赋值（不安全，通常不使用） */
    object->name = name;
#endif

    /* 调用对象附加钩子函数（如果已配置） */
    RT_OBJECT_HOOK_CALL(rt_object_attach_hook, (object));

    /* 获取自旋锁并关中断，进入临界区 */
    level = rt_spin_lock_irqsave(&(information->spinlock));

/* 如果开启了动态模块功能 */
#ifdef RT_USING_MODULE
    /* 如果当前处于某个模块中 */
    if (module)
    {
        /* 将对象插入到模块的对象链表中 */
        rt_list_insert_after(&(module->object_list), &(object->list));
        /* 设置对象所属的模块 ID */
        object->module_id = (void *)module;
    }
    /* 如果不在模块中 */
    else
#endif /* RT_USING_MODULE */
    {
        /* insert object into information object list 将对象插入到全局对象容器的链表中 */
        rt_list_insert_after(&(information->object_list), &(object->list));
    }
    /* 释放自旋锁并恢复中断，离开临界区 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);
}

/**
 * @brief This function will detach a static object from object system,
 *        and the memory of static object is not freed.
 * 此函数将一个静态对象从对象系统中脱离，但不释放静态对象的内存。
 *
 * @param object the specified object to be detached.
 * 参数 object 是待脱离的对象指针。
 */
void rt_object_detach(rt_object_t object)
{
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义对象信息结构体指针 */
    struct rt_object_information *information;

    /* object check 对象指针检查，确保不为空 */
    RT_ASSERT(object != RT_NULL);

    /* 调用对象脱离钩子函数（如果已配置） */
    RT_OBJECT_HOOK_CALL(rt_object_detach_hook, (object));

    /* 根据对象类型获取对应的信息结构 */
    information = rt_object_get_information((enum rt_object_class_type)object->type);
    /* 断言信息结构不为空 */
    RT_ASSERT(information != RT_NULL);

    /* 获取自旋锁并关中断 */
    level = rt_spin_lock_irqsave(&(information->spinlock));
    /* remove from old list 将对象从所在的链表中移除 */
    rt_list_remove(&(object->list));
    /* 释放自旋锁并恢复中断 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    /* 将对象类型设置为 Null，表示已脱离系统 */
    object->type = RT_Object_Class_Null;
}

/* 如果开启了堆内存管理（动态内存分配） */
#ifdef RT_USING_HEAP
/**
 * @brief This function will allocate an object from object system.
 * 此函数将从对象系统中分配一个动态对象。
 *
 * @param type Type of object. The type of the allocated object can only be of
 *             type rt_object_class_type other than RT_Object_Class_Static.
 *             In addition, the type of object allocated through this interface
 *             is dynamic, not static.
 * 参数 type 是对象类型。不能是 RT_Object_Class_Static，通过此接口分配的是动态对象。
 *
 * @param name Name of the object. In system, the object's name must be unique.
 *             Each object can be set to a name, and the maximum length for the
 *             name is specified by RT_NAME_MAX. The system does not care if it
 *             uses '\0' as a terminal symbol.
 * 参数 name 是对象名称。
 *
 * @return object handle allocated successfully, or RT_NULL if no memory can be allocated.
 * 返回成功分配的对象句柄，如果内存不足则返回 RT_NULL。
 */
rt_object_t rt_object_allocate(enum rt_object_class_type type, const char *name)
{
    /* 定义对象指针 */
    struct rt_object *object;
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义对象名称长度变量 */
    rt_size_t obj_name_len;
    /* 定义对象信息结构体指针 */
    struct rt_object_information *information;
/* 如果开启了动态模块功能 */
#ifdef RT_USING_MODULE
    /* 获取当前运行的模块指针 */
    struct rt_dlmodule *module = dlmodule_self();
#endif /* RT_USING_MODULE */

    /* 调试断言：确保不在中断上下文中调用此函数（因为涉及内存分配可能阻塞） */
    RT_DEBUG_NOT_IN_INTERRUPT;

    /* get object information 获取对象类型对应的信息结构 */
    information = rt_object_get_information(type);
    /* 断言信息结构不为空 */
    RT_ASSERT(information != RT_NULL);

    /* 根据对象的大小，从堆中分配内存 */
    object = (struct rt_object *)RT_KERNEL_MALLOC(information->object_size);
    /* 如果分配失败 */
    if (object == RT_NULL)
    {
        /* no memory can be allocated 返回空指针 */
        return RT_NULL;
    }

    /* clean memory data of object 将分配到的对象内存清零 */
    rt_memset(object, 0x0, information->object_size);

    /* initialize object's parameters 初始化对象的参数 */

    /* set object type 设置对象类型（动态对象没有 RT_Object_Class_Static 标志） */
    object->type = type;

    /* set object flag 设置对象标志为0 */
    object->flag = 0;

/* 如果配置了对象名称数组长度大于0 */
#if RT_NAME_MAX > 0
    /* 如果传入的名称不为空 */
    if (name)
    {
        /* 获取名称长度 */
        obj_name_len = rt_strlen(name);
        /* 如果名称长度超过限制 */
        if(obj_name_len > RT_NAME_MAX - 1)
        {
            /* 输出错误日志提示名称超限 */
            LOG_E("Object name %s exceeds RT_NAME_MAX=%d, consider increasing RT_NAME_MAX.", name, RT_NAME_MAX);
        }
        /* 截断拷贝名称 */
        rt_strncpy(object->name, name, RT_NAME_MAX - 1);
        /* 确保字符串以 '\0' 结尾 */
        object->name[RT_NAME_MAX - 1] = '\0';
    }
    /* 如果传入名称为空 */
    else
    {
        /* 将对象名称第一个字节置为结束符 */
        object->name[0] = '\0';
    }
#else
    /* 直接赋值名称指针 */
    object->name = name;
#endif

    /* 调用对象附加钩子函数 */
    RT_OBJECT_HOOK_CALL(rt_object_attach_hook, (object));

    /* 获取自旋锁并关中断 */
    level = rt_spin_lock_irqsave(&(information->spinlock));

/* 如果开启了动态模块功能 */
#ifdef RT_USING_MODULE
    /* 如果当前处于某个模块中 */
    if (module)
    {
        /* 将对象插入到模块的对象链表中 */
        rt_list_insert_after(&(module->object_list), &(object->list));
        /* 设置对象所属的模块 ID */
        object->module_id = (void *)module;
    }
    /* 如果不在模块中 */
    else
#endif /* RT_USING_MODULE */
    {
        /* insert object into information object list 将对象插入到全局对象容器的链表中 */
        rt_list_insert_after(&(information->object_list), &(object->list));
    }
    /* 释放自旋锁并恢复中断 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    /* 返回分配好的对象指针 */
    return object;
}

/**
 * @brief This function will delete an object and release object memory.
 * 此函数将删除一个动态对象并释放其占用的内存。
 *
 * @param object The specified object to be deleted.
 * 参数 object 是待删除的对象指针。
 */
void rt_object_delete(rt_object_t object)
{
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义对象信息结构体指针 */
    struct rt_object_information *information;

    /* object check 对象指针检查，确保不为空 */
    RT_ASSERT(object != RT_NULL);
    /* 断言对象不是静态对象（静态对象不能使用 delete 释放内存） */
    RT_ASSERT(!(object->type & RT_Object_Class_Static));

    /* 调用对象脱离钩子函数 */
    RT_OBJECT_HOOK_CALL(rt_object_detach_hook, (object));


    /* 根据对象类型获取对应的信息结构 */
    information = rt_object_get_information((enum rt_object_class_type)object->type);
    /* 断言信息结构不为空 */
    RT_ASSERT(information != RT_NULL);

    /* 获取自旋锁并关中断 */
    level = rt_spin_lock_irqsave(&(information->spinlock));

    /* remove from old list 将对象从所在的链表中移除 */
    rt_list_remove(&(object->list));

    /* 释放自旋锁并恢复中断 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    /* reset object type 重置对象类型为 Null */
    object->type = RT_Object_Class_Null;

    /* free the memory of object 释放对象占用的堆内存 */
    RT_KERNEL_FREE(object);
}
#endif /* RT_USING_HEAP */

/**
 * @brief This function will judge the object is system object or not.
 * 此函数判断一个对象是否是系统对象（静态对象）。
 *
 * @note  Normally, the system object is a static object and the type
 *        of object set to RT_Object_Class_Static.
 * 注意：通常系统对象是静态对象，其类型包含 RT_Object_Class_Static 标志。
 *
 * @param object The specified object to be judged.
 * 参数 object 是待判断的对象指针。
 *
 * @return RT_TRUE if a system object, RT_FALSE for others.
 * 如果是系统对象返回 RT_TRUE，否则返回 RT_FALSE。
 */
rt_bool_t rt_object_is_systemobject(rt_object_t object)
{
    /* object check 对象指针检查，确保不为空 */
    RT_ASSERT(object != RT_NULL);

    /* 判断对象类型是否包含静态标志 */
    if (object->type & RT_Object_Class_Static)
        /* 包含则返回真 */
        return RT_TRUE;

    /* 不包含则返回假 */
    return RT_FALSE;
}

/**
 * @brief This function will return the type of object without
 *        RT_Object_Class_Static flag.
 * 此函数返回对象的基本类型（去除静态标志位）。
 *
 * @param object is the specified object to be get type.
 * 参数 object 是待获取类型的对象指针。
 *
 * @return the type of object.
 * 返回对象的基本类型。
 */
rt_uint8_t rt_object_get_type(rt_object_t object)
{
    /* object check 对象指针检查，确保不为空 */
    RT_ASSERT(object != RT_NULL);

    /* 通过位与操作去除静态标志位，返回纯粹的对象类型 */
    return object->type & ~RT_Object_Class_Static;
}

/**
 * @brief This function will iterate through each object from object
 *        container.
 * 此函数遍历对象容器中的每一个对象。
 *
 * @param type is the type of object
 * 参数 type 是对象类型
 * @param iter is the iterator
 * 参数 iter 是迭代器回调函数
 * @param data is the specified data passed to iterator
 * 参数 data 是传递给迭代器的自定义数据
 *
 * @return RT_EOK on succeed, otherwise the error from `iter`
 * 成功返回 RT_EOK，否则返回迭代器产生的错误码
 *
 * @note this function shall not be invoked in interrupt status.
 * 注意：此函数不应在中断上下文中调用。
 */
rt_err_t rt_object_for_each(rt_uint8_t type, rt_object_iter_t iter, void *data)
{
    /* 定义对象指针 */
    struct rt_object *object = RT_NULL;
    /* 定义链表节点指针 */
    struct rt_list_node *node = RT_NULL;
    /* 定义对象信息结构体指针 */
    struct rt_object_information *information = RT_NULL;
    /* 定义中断级别变量 */
    rt_base_t level;
    /* 定义错误码变量 */
    rt_err_t error;

    /* 获取指定类型的对象信息 */
    information = rt_object_get_information((enum rt_object_class_type)type);

    /* parameter check 参数检查，如果类型无效 */
    if (information == RT_NULL)
    {
        /* 返回无效参数错误 */
        return -RT_EINVAL;
    }

    /* which is invoke in interrupt status 调试断言：确保不在中断上下文中调用 */
    RT_DEBUG_NOT_IN_INTERRUPT;

    /* enter critical 进入临界区，获取自旋锁并关中断 */
    level = rt_spin_lock_irqsave(&(information->spinlock));

    /* try to find object 遍历对象链表 */
    rt_list_for_each(node, &(information->object_list))
    {
        /* 获取当前节点对应的对象结构体 */
        object = rt_list_entry(node, struct rt_object, list);
        /* 调用迭代器回调函数，如果返回值不为 RT_EOK */
        if ((error = iter(object, data)) != RT_EOK)
        {
            /* 释放自旋锁并恢复中断 */
            rt_spin_unlock_irqrestore(&(information->spinlock), level);

            /* 如果错误码大于0（表示提前结束遍历但不是错误），返回 RT_EOK，否则返回错误码 */
            return error >= 0 ? RT_EOK : error;
        }
    }

    /* 释放自旋锁并恢复中断 */
    rt_spin_unlock_irqrestore(&(information->spinlock), level);

    /* 遍历完成，返回成功 */
    return RT_EOK;
}

/* 定义对象查找参数结构体，用于 rt_object_find 函数传参 */
struct _obj_find_param
{
    const char *match_name;   /* 需要匹配的对象名称 */
    rt_object_t matched_obj;  /* 匹配成功的对象指针 */
};

/**
 * @brief 内部回调函数：用于匹配对象名称
 */
static rt_err_t _match_name(struct rt_object *obj, void *data)
{
    /* 获取查找参数结构体指针 */
    struct _obj_find_param *param = data;
    /* 获取待匹配的名称 */
    const char *name = param->match_name;
    /* 定义截断后的名称缓冲区 */
    char truncated_name[RT_NAME_MAX];

    /* Truncate input name to RT_NAME_MAX - 1 to match object name storage 
     * 将输入名称截断到 RT_NAME_MAX - 1 长度，以匹配对象内部存储的名称
     */
    rt_strncpy(truncated_name, name, RT_NAME_MAX - 1);
    /* 确保截断后的名称以 '\0' 结尾 */
    truncated_name[RT_NAME_MAX - 1] = '\0';

    /* 比较对象名称和截断后的待匹配名称 */
    if (rt_strcmp(obj->name, truncated_name) == 0)
    {
        /* 如果匹配成功，记录找到的对象 */
        param->matched_obj = obj;

        /* notify an early break of loop, but not on error 
         * 通知外层循环提前结束遍历，返回1（大于0，不是错误）
         */
        return 1;
    }

    /* 未匹配成功，返回 RT_EOK 继续遍历 */
    return RT_EOK;
}

/**
 * @brief This function will find specified name object from object
 *        container.
 * 此函数从对象容器中查找指定名称的对象。
 *
 * @param name is the specified name of object.
 * 参数 name 是待查找的对象名称。
 *
 * @param type is the type of object
 * 参数 type 是对象类型
 *
 * @return the found object or RT_NULL if there is no this object
 * in object container.
 * 返回找到的对象指针，如果未找到则返回 RT_NULL。
 *
 * @note this function shall not be invoked in interrupt status.
 * 注意：此函数不应在中断上下文中调用。
 */
rt_object_t rt_object_find(const char *name, rt_uint8_t type)
{
    /* 初始化查找参数结构体 */
    struct _obj_find_param param =
    {
        .match_name = name,       /* 设置待匹配名称 */
        .matched_obj = RT_NULL,   /* 初始化匹配结果为空 */
    };

    /* parameter check 参数检查，如果名称为空或类型无效 */
    if (name == RT_NULL || rt_object_get_information(type) == RT_NULL)
        /* 返回空指针 */
        return RT_NULL;

    /* which is invoke in interrupt status 调试断言：确保不在中断上下文中调用 */
    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 调用遍历函数，使用 _match_name 回调进行名称匹配 */
    rt_object_for_each(type, _match_name, &param);
    /* 返回匹配到的对象指针（可能为空） */
    return param.matched_obj;
}

/**
 * @brief This function will return the name of the specified object container
 * 此函数获取指定对象的名称字符串
 *
 * @param object    the specified object to be get name
 * 参数 object 是待获取名称的对象
 * @param name      buffer to store the object name string
 * 参数 name 是用于存储对象名称的缓冲区
 * @param name_size  maximum size of the buffer to store object name
 * 参数 name_size 是缓冲区的最大容量
 *
 * @return -RT_EINVAL if any parameter is invalid or RT_EOK if the operation is successfully executed
 * 参数无效返回 -RT_EINVAL，成功执行返回 RT_EOK
 *
 * @note this function shall not be invoked in interrupt status
 * 注意：此函数不应在中断上下文中调用
 */
rt_err_t rt_object_get_name(rt_object_t object, char *name, rt_uint8_t name_size)
{
    /* 初始化结果为无效参数错误 */
    rt_err_t result = -RT_EINVAL;
    /* 检查对象指针、名称缓冲区指针不为空，且缓冲区大小不为0 */
    if ((object != RT_NULL) && (name != RT_NULL) && (name_size != 0U))
    {
        /* 获取对象内部的名称指针 */
        const char *obj_name = object->name;
        /* 拷贝对象名称到缓冲区，最多拷贝 name_size 长度 */
        rt_strncpy(name, obj_name, (rt_size_t)name_size);
        /* Ensure null-termination 确保缓冲区最后一个字节是字符串结束符 */
        name[name_size - 1] = '\0';
        /* 设置结果为成功 */
        result = RT_EOK;
    }

    /* 返回执行结果 */
    return result;
}

/* 如果开启了堆内存管理（动态内存分配） */
#ifdef RT_USING_HEAP
/**
 * This function will create a custom object
 * container.
 * 此函数创建一个自定义对象。
 *
 * @param name the specified name of object.
 * 参数 name 是对象名称。
 * @param data the custom data
 * 参数 data 是自定义数据指针。
 * @param data_destroy the custom object destroy callback
 * 参数 data_destroy 是自定义对象的销毁回调函数。
 *
 * @return the found object or RT_NULL if there is no this object
 * in object container.
 * 返回创建的对象指针，失败返回 RT_NULL。
 *
 * @note this function shall not be invoked in interrupt status.
 * 注意：此函数不应在中断上下文中调用。
 */

rt_object_t rt_custom_object_create(const char *name, void *data, rt_err_t (*data_destroy)(void *))
{
    /* 定义自定义对象指针并初始化为空 */
    struct rt_custom_object *cobj = RT_NULL;

    /* 动态分配一个自定义类型的对象 */
    cobj = (struct rt_custom_object *)rt_object_allocate(RT_Object_Class_Custom, name);
    /* 如果分配失败 */
    if (!cobj)
    {
        /* 返回空指针 */
        return RT_NULL;
    }
    /* 设置自定义对象的销毁回调函数 */
    cobj->destroy = data_destroy;
    /* 设置自定义对象的私有数据 */
    cobj->data = data;
    /* 将自定义对象指针转换为基类对象指针并返回 */
    return (struct rt_object *)cobj;
}

/**
 * This function will destroy a custom object
 * container.
 * 此函数销毁一个自定义对象。
 *
 * @param obj the specified name of object.
 * 参数 obj 是待销毁的对象指针。
 *
 * @note this function shall not be invoked in interrupt status.
 * 注意：此函数不应在中断上下文中调用。
 */
rt_err_t rt_custom_object_destroy(rt_object_t obj)
{
    /* 初始化返回值为 -1 */
    rt_err_t ret = -1;

    /* 将基类对象指针转换为自定义对象指针 */
    struct rt_custom_object *cobj = (struct rt_custom_object *)obj;

    /* 检查对象有效且类型为自定义对象 */
    if (obj && obj->type == RT_Object_Class_Custom)
    {
        /* 如果设置了销毁回调函数 */
        if (cobj->destroy)
        {
            /* 调用销毁回调函数处理私有数据 */
            ret = cobj->destroy(cobj->data);
        }
        /* 从对象系统删除该对象并释放内存 */
        rt_object_delete(obj);
    }
    /* 返回执行结果 */
    return ret;
}
#endif

/** @} group_object_management 对象管理分组结束 */
