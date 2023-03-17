#ifndef SKYNET_SERVER_H
#define SKYNET_SERVER_H

#include <stdint.h>
#include <stdlib.h>

struct skynet_context;
struct skynet_message;
struct skynet_monitor;

// skynet服务实例对象的创建
struct skynet_context * skynet_context_new(const char * name, const char * parm);

// 抓取服务实例 其实就是对服务实例进行引用计数 + 1
void skynet_context_grab(struct skynet_context *);

// 强行持有住服务实例（对计数器 + 1）
void skynet_context_reserve(struct skynet_context *ctx);

// 释放服务实例 其实就是对服务实例进行引用计数 - 1 如果计数器为0 则进行释放操作
struct skynet_context * skynet_context_release(struct skynet_context *);

// 获取服务实例的句柄
uint32_t skynet_context_handle(struct skynet_context *);

// 将消息放入服务实例的消息队列中
int skynet_context_push(uint32_t handle, struct skynet_message *message);

// 给服务实例发送消息
void skynet_context_send(struct skynet_context * context, void * msg, size_t sz, uint32_t source, int type, int session);

// 从服务实例中获取一个sessionId
int skynet_context_newsession(struct skynet_context *);

// 根据权重进行消息分发处理
struct message_queue * skynet_context_message_dispatch(struct skynet_monitor *, struct message_queue *, int weight);	// return next queue

// 获取节点内总服务实例数量
int skynet_context_total();

// 这个函数主要在启动的时候如果启动服务（bootstrap）创建失败 则将logger服务的消息都处理掉 保证日志能打印出来
void skynet_context_dispatchall(struct skynet_context * context);	// for skynet_error output before exit

// 设置服务实例处于死循环
void skynet_context_endless(uint32_t handle);	// for monitor

// 初始化节点全局信息
void skynet_globalinit(void);

// 节点退出善后处理
void skynet_globalexit(void);

// 初始化线程
void skynet_initthread(int m);

// 设置节点性能分析开关
void skynet_profile_enable(int enable);

#endif
