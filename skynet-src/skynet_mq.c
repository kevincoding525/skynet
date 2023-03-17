#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

// 节点的消息队列
struct message_queue {
	struct spinlock lock;  // 自旋锁
	uint32_t handle;    // 该消息队列的服务实例句柄
	int cap;    // 队列容量
	int head;   // 队头
	int tail;   // 队尾
	int release;  // 标志消息队列是否可释放
	int in_global;  // 标志消息队列是否被全局队列处理
	int overload;   // 消息过载时的数量
	int overload_threshold; // 消息过载的阈值
	struct skynet_message *queue;  // 消息存储的循环队列
	struct message_queue *next; // 下一个服务的消息队列指针
};

// 节点内全局消息队列
struct global_queue {
	struct message_queue *head;  // 队头
	struct message_queue *tail;  // 队尾
	struct spinlock lock;        // 自旋锁
};

static struct global_queue *Q = NULL;

// 把消息队列插入全局队列的队尾
void 
skynet_globalmq_push(struct message_queue * queue) {
	struct global_queue *q= Q;

	SPIN_LOCK(q)
    // 只插入单个消息队列节点
	assert(queue->next == NULL);

    // 把二级消息队列节点插入全局队列的队尾
	if(q->tail) {
		q->tail->next = queue;
		q->tail = queue;
	} else {
        // 空队列
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

// 从全局队列的队头弹出一个消息队列
struct message_queue * 
skynet_globalmq_pop() {
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if(mq) {
		q->head = mq->next;
		if(q->head == NULL) {
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}

// 创建一个消息队列
// 参数1：handle对应服务实例的句柄ID
struct message_queue * 
skynet_mq_create(uint32_t handle) {
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE; // 消息队列容量默认初始化64个消息的长度
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q)
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

// 释放一个消息队列的内存
static void 
_release(struct message_queue *q) {
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}

// 返回消息队列的句柄ID
uint32_t 
skynet_mq_handle(struct message_queue *q) {
	return q->handle;
}

// 获取消息队列长度
int
skynet_mq_length(struct message_queue *q) {
	int head, tail,cap;

	SPIN_LOCK(q)  // 添加自旋锁

	head = q->head;
	tail = q->tail;
	cap = q->cap;

	SPIN_UNLOCK(q)  // 释放自旋锁
	
	if (head <= tail) {
		return tail - head;
	}
	return tail + cap - head;
}


// 获取过载消息的数量
int
skynet_mq_overload(struct message_queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	} 
	return 0;
}


// 从消息队列头部弹出一条消息
int
skynet_mq_pop(struct message_queue *q, struct skynet_message *message) {
	int ret = 1;
	SPIN_LOCK(q)

	if (q->head != q->tail) {
        // 队列不空
		*message = q->queue[q->head++];
		ret = 0;
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

        /*
         * 如果头指针的值超过或等于容量的大小 则表示弹出消息以后 队列已经把容量的偏移空间用完
         * 重新将队头指针放到数组头部
         * */
		if (head >= cap) {
			q->head = head = 0;
		}

        // 如果长度值小于0 表示队列已空
		int length = tail - head;
		if (length < 0) {
			length += cap;
		}

        // 消息超过了阈值 记录超过时的消息数量，并且将阈值扩大一倍 此处为啥要用循环不断扩充 为啥不一次到位 ？？？
		while (length > q->overload_threshold) {
			q->overload = length;
			q->overload_threshold *= 2;
		}
	} else {
		// reset overload_threshold when queue is empty
        // 队列是空的 重置消息过载的阈值
		q->overload_threshold = MQ_OVERLOAD;
	}

    // 如果队列已空弹不出消息 则把消息队列从全局队列中暂时屏蔽
	if (ret) {
		q->in_global = 0;
	}

    // 释放自旋锁
	SPIN_UNLOCK(q)

    // 返回弹出的消息
	return ret;
}

static void
expand_queue(struct message_queue *q) {
    // 按当前容量的2倍扩充队列
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
    // 将旧队列的数据迁移到新的队列上
	for (i=0;i<q->cap;i++) {
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}

    // 重置队列头部指针
	q->head = 0;
    // 重置队列尾部指针
	q->tail = q->cap;
    // 重置容量
	q->cap *= 2;
	// 释放旧的队列
	skynet_free(q->queue);
    // 绑定新的队列
	q->queue = new_queue;
}

// 把消息放入消息队列中（放到队尾）
void 
skynet_mq_push(struct message_queue *q, struct skynet_message *message) {
	assert(message);
	SPIN_LOCK(q)

    // 消息放到队尾
	q->queue[q->tail] = *message;

    // 队尾指针超过容量大小 重置到数组起始位置
	if (++ q->tail >= q->cap) {
		q->tail = 0;
	}

    // 队头指针和队尾指针一致 说明队列已经没有空间可以存放新数据 进行扩容
	if (q->head == q->tail) {
		expand_queue(q);
	}

    // 消息队列中产生了消息 重新把消息队列放回全局队列
	if (q->in_global == 0) {
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}
	
	SPIN_UNLOCK(q)
}

// 初始化全局队列
void 
skynet_mq_init() {
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q,0,sizeof(*q));
	SPIN_INIT(q);
	Q=q;
}

// 将消息队列标志为可释放 真正的销毁工作在skynet_mq_release函数中执行
void 
skynet_mq_mark_release(struct message_queue *q) {
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
    // 如果消息队列不在global队列中 则重新放回global队列
	if (q->in_global != MQ_IN_GLOBAL) {
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud) {
	struct skynet_message msg;
    // 不断从消息队列中弹出消息
	while(!skynet_mq_pop(q, &msg)) {
        // 调用回调函数相应回调 做一些消息的善后工作
		drop_func(&msg, ud);
	}
    // 释放消息队列
	_release(q);
}

// 销毁消息队列 此处才是真正的执行销毁操作
void 
skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud) {
	SPIN_LOCK(q)
	
	if (q->release) {
		SPIN_UNLOCK(q)
        // 如果标志为可销毁 清掉消息队列的消息
		_drop_queue(q, drop_func, ud);
	} else {
        // 如果没有标志销毁 则重新放入全局队列中
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
