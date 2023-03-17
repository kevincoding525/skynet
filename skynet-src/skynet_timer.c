#include "skynet.h"

#include "skynet_timer.h"
#include "skynet_mq.h"
#include "skynet_server.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <time.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// 定时器相应函数的函数指针类型声明
typedef void (*timer_execute_func)(void *ud,void *arg);

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

// timer_event数据内存是直接拼接在timer_node内存后面的
struct timer_event {
	uint32_t handle; // 定时器事件触发的服务实例句柄
	int session;
};

// 链表的节点 节点内存储了过期时间
struct timer_node {
	struct timer_node *next; // 下一个节点（维护一个链表）
	uint32_t expire;    // 定时器节点的超时时间
};

// 实现了一个链表
struct link_list {
	struct timer_node head; // 注意这里head是一个结构体不是指针 因此链表头部是一个空节点
	struct timer_node *tail;
};

// 定时器管理结构体
struct timer {
    //--------------------------------------------------------------------------------------------------
    // 下面两个数据结构是理解skynet时间轮实现的核心
    // 将一个32位的int时间值 分配到5个时间轮上（最外层的工作轮盘表示8位，内层的级联轮盘有4个级别，每一个级别表示6位）总共就是32位
    //   -----------------------------------------------------------------------------------------
    //   ｜      t3      |       t2      |       t1      |       t0      |       near            |
    //   ｜---------------------------------------------------------------------------------------
    //   ｜      6位     ｜       6位     ｜      6位     ｜       6位     ｜        8位           ｜
    //   -----------------------------------------------------------------------------------------
	struct link_list near[TIME_NEAR]; // 存储了256个链表的数组（工作轮盘）类似时钟里的秒针
	struct link_list t[4][TIME_LEVEL]; // 存储了[4][64] 个链表的二维数组（级联轮盘）类似时钟里的分针和时针

    //--------------------------------------------------------------------------------------------------
	struct spinlock lock; // 自旋锁
	uint32_t time;  //  timer线程执行的总tick数
	uint32_t starttime; // timer线程启动的时间
 	uint64_t current; //    // 这个是用来记录进程启动以后过去的时间值
	uint64_t current_point; // timer线程当前tick的时间点 用来计算两次tick的差值
};

// 全局定时器管理对象
static struct timer * TI = NULL;

// 清空链表
static inline struct timer_node *
link_clear(struct link_list *list) {
	struct timer_node * ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);

	return ret;
}

// 把节点插入链表的尾部
static inline void
link(struct link_list *list,struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next=0;
}

// 将节点加入新的轮盘刻度
static void
add_node(struct timer *T,struct timer_node *node) {
	uint32_t time=node->expire;
	uint32_t current_time=T->time;

    // 这个位操作其实就是判断 当前时间和定时器超时时间的级联轮盘刻度是否一样
    // 低8位做或运算就是对低8位做掩码 屏蔽低8位的差异 然后比较高24位的差异
	if ((time|TIME_NEAR_MASK)==(current_time|TIME_NEAR_MASK)) {
        // 如果级联轮盘刻度一样 放到工作轮盘的对应刻度上去
		link(&T->near[time&TIME_NEAR_MASK],node);
	} else {
        // 如果级联轮盘刻度不一样 检测定时器的超时时间是落在哪一级的级联轮盘
		int i;
		uint32_t mask=TIME_NEAR << TIME_LEVEL_SHIFT;
        // 一级一级的进行检测 定级 确定节点是落在哪一级
		for (i=0;i<3;i++) {
			if ((time|(mask-1))==(current_time|(mask-1))) {
				break;
			}
			mask <<= TIME_LEVEL_SHIFT;
		}

        // 将节点放到对应级联轮盘的刻度处
		link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
	}
}


// 添加定时器任务
// 参数1 T：是全局定时器管理容器
// 参数2 arg：这里传入的是一个timer_event对象
// 参数3 sz: timer_event对象的内存大小
// 参数4 time: 是定时器超时时间
static void
timer_add(struct timer *T,void *arg,size_t sz,int time) {
    // 这里采取了一个类似lua源码中的一个内存紧凑的数据结构：timer_event数据直接拼接到timer_node后面
    // 申请的内存大小是：timer_node节点指针的大小 + timer_event对象的实际内存大小
	struct timer_node *node = (struct timer_node *)skynet_malloc(sizeof(*node)+sz);
    // 将timer_event的数据拷贝到node指针对应的位置上
	memcpy(node+1,arg,sz);

	SPIN_LOCK(T);
        // expire是绝对超时时间 是当前时间 + 相对超时时间 这里时间的单位都是10ms
		node->expire=time+T->time;
		add_node(T,node);

	SPIN_UNLOCK(T);
}

// 定时器实现关键的函数 将级联轮盘的数据移到新的轮盘刻度处
static void
move_list(struct timer *T, int level, int idx) {
    // 拿出指定level级联轮盘idx刻度处的timer_node链表 将节点放到新的轮盘刻度
	struct timer_node *current = link_clear(&T->t[level][idx]);
	while (current) {
		struct timer_node *temp=current->next;
		add_node(T,current);
		current=temp;
	}
}

// 定时器实现关键的函数 转动轮盘 进行时间刻度的递进发散
//   -----------------------------------------------------------------------------------------
//   ｜      t3      |       t2      |       t1      |       t0      |       near            |
//   ｜---------------------------------------------------------------------------------------
//   ｜      6位     ｜       6位     ｜      6位     ｜       6位     ｜        8位           ｜
//   -----------------------------------------------------------------------------------------
static void
timer_shift(struct timer *T) {
	int mask = TIME_NEAR;   // 工作轮盘的掩码
	uint32_t ct = ++T->time;    // time是timer线程启动以来总的tick次数 ct就是当前的tick次数
	if (ct == 0) { // 这里ct怎么可能是0？ - 可能是时间溢出了！！
		move_list(T, 3, 0);  // 将第3级轮盘刻度0处的节点移动到新的轮盘刻度上
	} else {
		uint32_t time = ct >> TIME_NEAR_SHIFT; // 将ct右移8位 获得级联轮盘的刻度
		int i=0;

		while ((ct & (mask-1))==0) { // 一级一级的递进检测 如果某一级上有刻度则将数据移到工作轮盘上
			int idx=time & TIME_LEVEL_MASK; // 获取级联轮盘的刻度
			if (idx!=0) {
				move_list(T, i, idx); // 将i级联轮盘idx刻度的数据移到新的轮盘刻度处
				break;				
			}
			mask <<= TIME_LEVEL_SHIFT;  // 掩码左移6位 提取更高一级轮盘的刻度
			time >>= TIME_LEVEL_SHIFT;  // 时间值右移6位 获取级联轮盘的刻度
			++i;
		}
	}
}

// 分发定时器超时回调函数
static inline void
dispatch_list(struct timer_node *current) {
	do {
        // 拿出定时器事件对象（直接从node内存后面偏移获取）
		struct timer_event * event = (struct timer_event *)(current+1);

        // 构建定时器触发的消息
		struct skynet_message message;
		message.source = 0;
		message.session = event->session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

        // 以消息的形式通知服务相应回调
		skynet_context_push(event->handle, &message);

		struct timer_node * temp = current;
		current=current->next;
        // 释放节点
		skynet_free(temp);	
	} while (current);
}

// 定时器处理函数
static inline void
timer_execute(struct timer *T) {
	int idx = T->time & TIME_NEAR_MASK; // 算出当前时间点工作轮盘中的刻度

    // 从工作轮盘中拿出当前时间刻度的时间节点链表 依次分发timeout回调函数
	while (T->near[idx].head.next) {
		struct timer_node *current = link_clear(&T->near[idx]);
		SPIN_UNLOCK(T);
		// dispatch_list don't need lock T
		dispatch_list(current);
		SPIN_LOCK(T);
	}
}

// 更新工作轮盘
static void 
timer_update(struct timer *T) {
	SPIN_LOCK(T);

	// try to dispatch timeout 0 (rare condition)
    // 这里提前执行一次是处理极限情况下有timeout(0)的定时器插入
	timer_execute(T);

	// shift time first, and then dispatch timer message
	timer_shift(T);

    // 处理工作轮盘上当前时间刻度的超时任务
	timer_execute(T);

	SPIN_UNLOCK(T);
}

static struct timer *
timer_create_timer() {
	struct timer *r=(struct timer *)skynet_malloc(sizeof(struct timer));
	memset(r,0,sizeof(*r));

	int i,j;

    // 初始化工作轮盘（秒针）
	for (i=0;i<TIME_NEAR;i++) {
		link_clear(&r->near[i]);
	}

    // 初始化级联轮盘（时针和分针）
	for (i=0;i<4;i++) {
		for (j=0;j<TIME_LEVEL;j++) {
			link_clear(&r->t[i][j]);
		}
	}

	SPIN_INIT(r)

	r->current = 0;

	return r;
}

// 添加定时器任务接口
int
skynet_timeout(uint32_t handle, int time, int session) {
	if (time <= 0) {
        // 如果超时时间 < 0 直接发送超时回调消息
		struct skynet_message message;
		message.source = 0;
		message.session = session;
		message.data = NULL;
		message.sz = (size_t)PTYPE_RESPONSE << MESSAGE_TYPE_SHIFT;

		if (skynet_context_push(handle, &message)) {
			return -1;
		}
	} else {

        // 添加到时间轮里面去
		struct timer_event event;
		event.handle = handle;
		event.session = session;
		timer_add(TI, &event, sizeof(event), time);
	}

	return session;
}

// centisecond: 1/100 second
static void
systime(uint32_t *sec, uint32_t *cs) {
	struct timespec ti;
    // CLOCK_REALTIME - 系统实时时间,随系统实时时间改变而改变,即从UTC1970-1-1 0:0:0开始计时,
    // 中间时刻如果系统时间被用户改成其他,则对应的时间相应改变。
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
}

// 获取当前时间（从进程启动开始算）
// 返回 64位无符号整数
static uint64_t
gettime() {
	uint64_t t;
	struct timespec ti;
    // CLOCK_MONOTONIC - 从系统启动这一刻起开始计时,不受系统时间被用户改变的影响
	clock_gettime(CLOCK_MONOTONIC, &ti);

    // 最小单位是10ms
	t = (uint64_t)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
	return t;
}

// 定时器线程处理函数
void
skynet_updatetime(void) {
	uint64_t cp = gettime(); // 获取当前时间
	if(cp < TI->current_point) {
        // 当前时间小于上次检测的时间点 这个一般不会出现，在一些操作系统上因为有自动对时服务来矫正时间可能会出现这种情况，例如mac系统
		skynet_error(NULL, "time diff error: change from %lld to %lld", cp, TI->current_point);
		TI->current_point = cp;
	} else if (cp != TI->current_point) {
		uint32_t diff = (uint32_t)(cp - TI->current_point);
		TI->current_point = cp; // 记录这次时间更新的时间点
		TI->current += diff; // 将差值累加
		int i;
        // 遍历差值进行定时器工作轮盘的转动（diff的单位是10ms 但是线程的tick时间间隔是2.5ms）
		for (i=0;i<diff;i++) {
			timer_update(TI);
		}
	}
}

// 进程启动的绝对时间（即从UTC1970-1-1 0:0:0开始计时）
uint32_t
skynet_starttime(void) {
	return TI->starttime;
}

// 获取当前进程启动到现在的差值
uint64_t 
skynet_now(void) {
	return TI->current;
}

// 初始化定时器
void 
skynet_timer_init(void) {
	TI = timer_create_timer();
	uint32_t current = 0;
	systime(&TI->starttime, &current);
	TI->current = current;
	TI->current_point = gettime();
}

// for profile

#define NANOSEC 1000000000
#define MICROSEC 1000000

// 获取线程时间 单位是微妙
uint64_t
skynet_thread_time(void) {
	struct timespec ti;
    // CLOCK_THREAD_CPUTIME_ID - 本线程到当前代码系统CPU花费的时间
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti);

	return (uint64_t)ti.tv_sec * MICROSEC + (uint64_t)ti.tv_nsec / (NANOSEC / MICROSEC);
}
