#ifndef SKYNET_MESSAGE_QUEUE_H
#define SKYNET_MESSAGE_QUEUE_H

#include <stdlib.h>
#include <stdint.h>
/*
 * skynet的消息以及消息队列模型 整个skynet的并发模型都是基于消息队列来进行调度的
 * skynet整体的消息队列模型是，顶层维护一个基于链表实现的全局队列（global_queue），
 * 队列的每个节点是一个单独的消息队列（message_queue），message_queue是基于数组实现的一个循环队列
 * */

// 消息结构体
struct skynet_message {
	uint32_t source;    // 消息源
	int session;       // 消息session
	void * data;       // 消息数据
	size_t sz;          // 消息数据大小
};

// type is encoding in skynet_message.sz high 8bit
#define MESSAGE_TYPE_MASK (SIZE_MAX >> 8)
#define MESSAGE_TYPE_SHIFT ((sizeof(size_t)-1) * 8)

struct message_queue;

// 把消息队列插入全局队列的队尾
void skynet_globalmq_push(struct message_queue * queue);

// 从全局队列的队头弹出一个消息队列
struct message_queue * skynet_globalmq_pop(void);

// 创建一个消息队列实例
struct message_queue * skynet_mq_create(uint32_t handle);

// 将消息队列标志为可销毁
void skynet_mq_mark_release(struct message_queue *q);

// 消息销毁的回调函数指针
typedef void (*message_drop)(struct skynet_message *, void *);

// 真正的销毁消息队列
void skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud);

// 返回消息队列的句柄ID
uint32_t skynet_mq_handle(struct message_queue *);

// 0 for success
// 消息出队列（队头弹出）
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message);

// 消息入队列（队尾插入）
void skynet_mq_push(struct message_queue *q, struct skynet_message *message);

// return the length of message queue, for debug
// 获取消息队列长度
int skynet_mq_length(struct message_queue *q);

// 获取过载的消息数量
int skynet_mq_overload(struct message_queue *q);

// 全局队列初始化
void skynet_mq_init();

#endif
