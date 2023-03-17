#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"
#include "spinlock.h"
#include "atomic.h"

#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
#define CHECKCALLING_END(ctx) spinlock_unlock(&ctx->calling);
#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
#define CHECKCALLING_DESTROY(ctx) spinlock_destroy(&ctx->calling);
#define CHECKCALLING_DECL struct spinlock calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DESTROY(ctx)
#define CHECKCALLING_DECL

#endif

struct skynet_context {
	void * instance; // 服务实例模块实例对象
	struct skynet_module * mod; // 服务实例模块对象
	void * cb_ud;
	skynet_cb cb; // 消息回调处理函数
	struct message_queue *queue; // 服务实例消息对象
	ATOM_POINTER logfile; // 服务实例log文件
	uint64_t cpu_cost;	// in microsec
	uint64_t cpu_start;	// in microsec
	char result[32]; // 服务状态的一些结果存储
	uint32_t handle; // 服务实例句柄
	int session_id; // 服务实例session_id 自增获取
	ATOM_INT ref;   // 引用计数管理内存的释放
	int message_count; // 服务实例处理消息的数量
	bool init; // 标志服务是否初始化完毕
	bool endless; // 标志服务是否出现死循环
	bool profile;   // 是否开启性能分析

	CHECKCALLING_DECL
};

struct skynet_node {
	ATOM_INT total; // 节点服务实例数量
	int init;
	uint32_t monitor_exit; // 可以设置监控退出的服务实例 有服务实例退出 会给monitor_exit服务实例发消息
	pthread_key_t handle_key;
	bool profile;	// default is on
};

static struct skynet_node G_NODE;

// 获取节点总服务实例数量
int 
skynet_context_total() {
	return ATOM_LOAD(&G_NODE.total);
}

// 服务实例数量 + 1
static void
context_inc() {
	ATOM_FINC(&G_NODE.total);
}

// 服务实例数量 - 1
static void
context_dec() {
	ATOM_FDEC(&G_NODE.total);
}

// 获取当前线程正在处理的服务实例
uint32_t 
skynet_current_handle(void) {
	if (G_NODE.init) {
        // 获取当前线程正在跑的服务实例，这里使用了线程局部变量
        // 线程局部变量可以存储只属于某一个线程内部的数据，其他线程无法访问到
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {
        // 如果节点还未初始化完成 这时候当前运行的肯定是主线程
		uint32_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}

// 将十六进制正整数转成字符串表示
static void
id_to_hex(char * str, uint32_t id) {
	int i;
    // 十六进制的字符
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];
	}
	str[9] = '\0';
}

// 消息丢弃时处理函数的参数结构
struct drop_t {
	uint32_t handle;
};

// 消息丢弃处理 做一些善后工作
static void
drop_message(struct skynet_message *msg, void *ud) {
	struct drop_t *d = ud;
	skynet_free(msg->data);
	uint32_t source = d->handle;
	assert(source);
	// report error to the message source
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

// skynet服务实例对象的创建
//
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
    //  根据模块名找到模块对象
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;

    // 基于模块创建模块实例
	void *inst = skynet_module_instance_create(mod);
	if (inst == NULL)
		return NULL;
    // 实例化服务
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	CHECKCALLING_INIT(ctx)

	ctx->mod = mod;
	ctx->instance = inst;
	ATOM_INIT(&ctx->ref , 2); // 这里一开始初始化为2
	ctx->cb = NULL;
	ctx->cb_ud = NULL;
	ctx->session_id = 0;
	ATOM_INIT(&ctx->logfile, (uintptr_t)NULL);

	ctx->init = false;
	ctx->endless = false;

	ctx->cpu_cost = 0;
	ctx->cpu_start = 0;
	ctx->message_count = 0;
	ctx->profile = G_NODE.profile;
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	ctx->handle = 0;

    // 给服务实例ctx绑定一个句柄handle 并且放入handle_storage中管理
	ctx->handle = skynet_handle_register(ctx);
    // 创建服务的消息队列，服务内部会持有消息队列的指针
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);
	// init function maybe use ctx->handle, so it must init at last
	context_inc();

	CHECKCALLING_BEGIN(ctx)
    // 执行服务初始化函数
	int r = skynet_module_instance_init(mod, inst, ctx, param);
	CHECKCALLING_END(ctx)
	if (r == 0) { // 服务初始化成功
		struct skynet_context * ret = skynet_context_release(ctx); // 当前实例的计数器变成1
		if (ret) {
			ctx->init = true;
		}
        // 将服务消息队列放入全局队列
		skynet_globalmq_push(queue);
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else { // 服务初始化失败
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;
		skynet_context_release(ctx); // 释放服务实例（当前实例的计数器变成1 还没有真正释放）
		skynet_handle_retire(handle);   // 当前实例的计数器变成0 真正释放掉了
		struct drop_t d = { handle };
        // 清空服务实例消息队列的消息 发送drop的回调消息执行善后处理
		skynet_mq_release(queue, drop_message, &d);
		return NULL;
	}
}

// 从服务实例内获取一个session_id
int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	int session = ++ctx->session_id;
    // session_id溢出了 重新从1开始
	if (session <= 0) {
		ctx->session_id = 1;
		return 1;
	}
	return session;
}

// 将服务实例的引用计数 + 1
void 
skynet_context_grab(struct skynet_context *ctx) {
	ATOM_FINC(&ctx->ref);
}

// 强行将服务实例计数器 + 1 主要是给harbor服务实例用 harbor服务实例是一个全局对象
// 为了保证指针不被释放 而这么做
void
skynet_context_reserve(struct skynet_context *ctx) {
    // 将服务实例引用计数 + 1
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.
	context_dec();
}

// 释放服务实例
static void 
delete_context(struct skynet_context *ctx) {
    // 先释放服务实例的log文件
	FILE *f = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (f) {
		fclose(f);
	}
    // 调用模块的release函数 释放模块实例
	skynet_module_instance_release(ctx->mod, ctx->instance);
    // 标记服务实例消息队列可释放
	skynet_mq_mark_release(ctx->queue);
	CHECKCALLING_DESTROY(ctx)
    // 释放服务实例
	skynet_free(ctx);
    // 全局服务实例计数器 - 1
	context_dec();
}

// 将服务实例的引用计数 - 1 一般和skynet_context_grab配合使用
// 先通过skynet_context_grab抓取实例 使用完以后再调用skynet_context_release将计数器 - 1
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
    // 这里因为ATOM_FDEC返回的是执行操作之前的值 所有这里是判断值是不是1
	if (ATOM_FDEC(&ctx->ref) == 1) {
		delete_context(ctx);
		return NULL;
	}
	return ctx;
}

int
skynet_context_push(uint32_t handle, struct skynet_message *message) {
    // 通过句柄抓取服务实例（这里服务实例计数器加了1）
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return -1;
	}
    // 将消息写入消息队列
	skynet_mq_push(ctx->queue, message);
    // 服务实例计数器 - 1
	skynet_context_release(ctx);

	return 0;
}

// 标志服务实例进入了死循环
// skynet对于死循环可以通过monitor服务检测出来 但是只是给服务打个标志 并不做善后操作
// 进一步的善后操作交给开发者自行处理
void 
skynet_context_endless(uint32_t handle) {
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	ctx->endless = true;
	skynet_context_release(ctx);
}

// 判断目标服务句柄是不是远程节点的服务
// 将harborID写回harbor字段
int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {
	int ret = skynet_harbor_message_isremote(handle);
	if (harbor) {
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}

// 服务实例分发处理消息
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {
	assert(ctx->init);
	CHECKCALLING_BEGIN(ctx)
	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));
	int type = msg->sz >> MESSAGE_TYPE_SHIFT;
	size_t sz = msg->sz & MESSAGE_TYPE_MASK;
    // 如果服务实例有单独的日志文件 打印消息日志
	FILE *f = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (f) {
		skynet_log_output(f, msg->source, type, msg->session, msg->data, sz);
	}
	++ctx->message_count;
	int reserve_msg;
    // 开启了性能分析
	if (ctx->profile) {
        // 本线程的当前CPU时间
		ctx->cpu_start = skynet_thread_time();
        // 执行消息处理回调函数
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
        // 统计线程cpu耗时占比
		uint64_t cost_time = skynet_thread_time() - ctx->cpu_start;
		ctx->cpu_cost += cost_time;
	} else {
        // 执行消息处理回调函数
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
	}
	if (!reserve_msg) {
		skynet_free(msg->data);
	}
	CHECKCALLING_END(ctx)
}

// 一次分发消费消息队列里面所有的消息
void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	while (!skynet_mq_pop(q,&msg)) {
		dispatch_message(ctx, &msg);
	}
}

/*
 * 工作线程消费消息
 * 参数 sm：当前线程的监控器
 * 参数 q: 当前线程需要处理的消息队列
 * 参数 weight：当前线程的处理权重
 * */
struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
    // 没有指定消息队列 则从全局队列中拿出一个服务的消息队列
	if (q == NULL) {
        // 从全局队列里面弹出一个服务的消息队列
		q = skynet_globalmq_pop();
		if (q==NULL)
			return NULL;
	}

	uint32_t handle = skynet_mq_handle(q);

    // 引用服务实例变量 计数器 + 1
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) { // 服务实例已经被销毁
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
        // 弹出下一个消息队列
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;

    // 默认执行2次 根据权重调整n
	for (i=0;i<n;i++) {
        // 从消息队列中弹出一条消息
		if (skynet_mq_pop(q,&msg)) {
            // 释放服务实例引用 计数器 - 1
			skynet_context_release(ctx);
            // 弹出下一个消息队列
			return skynet_globalmq_pop();
		} else if (i==0 && weight >= 0) {
			n = skynet_mq_length(q);
			n >>= weight;
		}
        // 如果消息量过载 仅仅打印日志
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		skynet_monitor_trigger(sm, msg.source , handle);

        // 服务没有设置相应回调函数 直接销毁消息数据
		if (ctx->cb == NULL) {
			skynet_free(msg.data);
		} else {
			dispatch_message(ctx, &msg);
		}

		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)
		skynet_globalmq_push(q);
		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}

// 将addr的字符串拷贝到name字符数组里面
static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) {
		name[i] = addr[i];
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0';
	}
}

uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
        // strtoul - 将参数nptr字符串根据参数base来转换成无符号的长整型数。
		return strtoul(name+1,NULL,16);
	case '.': // .开头的服务属于节点内部的服务
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) {
		handle = context->handle;
		skynet_error(context, "KILL self");
	} else {
		skynet_error(context, "KILL :%0x", handle);
	}
    // 如果有服务实例退出了 给监听退出的服务实例发送消息
	if (G_NODE.monitor_exit) {
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}
	skynet_handle_retire(handle);
}

// skynet command

struct command_func {
	const char *name;
	const char * (*func)(struct skynet_context * context, const char * param);
};

// 设置超时任务 - lua层命令
static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;
    // strtol函数会将参数nptr字符串根据参数base来转换成长整型数，参数base范围从2至36。
	int ti = strtol(param, &session_ptr, 10);
    // 获得一个sessionId 方便超时到了以后抛给上层任务根据sessionId调用对应的回调函数
	int session = skynet_context_newsession(context);
    // 将超时信息加入时间轮定时器
	skynet_timeout(context->handle, ti, session);
	sprintf(context->result, "%d", session);
	return context->result;
}

// 给当前服务实例注册绑定一个名字 - lua层命令
static const char *
cmd_reg(struct skynet_context * context, const char * param) {
	if (param == NULL || param[0] == '\0') {
		sprintf(context->result, ":%x", context->handle);
		return context->result;
	} else if (param[0] == '.') {
		return skynet_handle_namehandle(context->handle, param + 1);
	} else {
		skynet_error(context, "Can't register global name %s in C", param);
		return NULL;
	}
}

// 根据名字查找服务实例 - lua层命令
static const char *
cmd_query(struct skynet_context * context, const char * param) {
	if (param[0] == '.') {
		uint32_t handle = skynet_handle_findname(param+1);
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

// 给一个服务实例注册绑定一个名字 - lua层命令
static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	sscanf(param,"%s %s",name,handle);
	if (handle[0] != ':') {
		return NULL;
	}
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	if (name[0] == '.') {
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}
	return NULL;
}

// 当前服务实例执行退出命令 - lua层命令
static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

// 提取真正的句柄ID
// 如果是：开头 表示传入的是一个句柄ID
// 如果是.开头 表示传入的是一个服务实例名字
static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	if (param[0] == ':') {
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "Can't convert %s to handle",param);
	}

	return handle;
}

// 杀掉指定的一个服务实例 可以传入句柄ID或者名字 - lua层命令
static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

// 创建一个服务实例 - lua层命令
static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;
	char * mod = strsep(&args, " \t\r\n");
	args = strsep(&args, "\r\n");
	struct skynet_context * inst = skynet_context_new(mod,args);
	if (inst == NULL) {
		return NULL;
	} else {
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

// 获取环境变量指令 - lua层命令
static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

// 设置环境变量指令 - lua层命令
static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char key[sz+1];
	int i;
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;
	
	skynet_setenv(key,param);
	return NULL;
}

// 获取节点启动时间 - lua层命令
static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	uint32_t sec = skynet_starttime();
	sprintf(context->result,"%u",sec);
	return context->result;
}

// 中断的语义 释放节点内所有服务实例 - lua层命令
static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

// 监控服务实例退出 - lua层命令
// 如果监控的服务退出了 会抛出消息给上层
static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle=0;
	if (param == NULL || param[0] == '\0') { // 空字符串或者空指针
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

// 统计指令 - lua层命令
static const char *
cmd_stat(struct skynet_context * context, const char * param) {
	if (strcmp(param, "mqlen") == 0) {
        // 统计当前服务的消息队列长度
		int len = skynet_mq_length(context->queue);
		sprintf(context->result, "%d", len);
	} else if (strcmp(param, "endless") == 0) {
        // 检查当前服务是否进入死循环
		if (context->endless) {
			strcpy(context->result, "1");
			context->endless = false;
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "cpu") == 0) {
        // 获取当前服务实例CPU耗时
		double t = (double)context->cpu_cost / 1000000.0;	// microsec
		sprintf(context->result, "%lf", t);
	} else if (strcmp(param, "time") == 0) {
        // 获取当前服务实例线程执行时间
		if (context->profile) {
			uint64_t ti = skynet_thread_time() - context->cpu_start;
			double t = (double)ti / 1000000.0;	// microsec
			sprintf(context->result, "%lf", t);
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "message") == 0) {
        // 获取当前服务实例处理的消息数量
		sprintf(context->result, "%d", context->message_count);
	} else {
		context->result[0] = '\0';
	}
	return context->result;
}

// 打开服务日志文件 - lua层命令
static const char *
cmd_logon(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE *f = NULL;
	FILE * lastf = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (lastf == NULL) {
		f = skynet_log_open(context, handle);
		if (f) {
			if (!ATOM_CAS_POINTER(&ctx->logfile, 0, (uintptr_t)f)) {
				// logfile opens in other thread, close this one.
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

// 关闭服务日志文件 - lua层命令
static const char *
cmd_logoff(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE * f = (FILE *)ATOM_LOAD(&ctx->logfile);
	if (f) {
		// logfile may close in other thread
		if (ATOM_CAS_POINTER(&ctx->logfile, (uintptr_t)f, (uintptr_t)NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

// 给服务实例发送信号通知 - lua层命令
static const char *
cmd_signal(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	param = strchr(param, ' ');
	int sig = 0;
	if (param) {
		sig = strtol(param, NULL, 0);
	}
	// NOTICE: the signal function should be thread safe.
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);
	return NULL;
}

// 命令注册表
static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "EXIT", cmd_exit },
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "STAT", cmd_stat },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ "SIGNAL", cmd_signal },
	{ NULL, NULL },
};

// lua层命令处理函数
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];
	while(method->name) {
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);
		}
		++method;
	}

	return NULL;
}

// 解析参数
// 消息数据体是否需要拷贝
// 是否需要分配sessionId
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;
	type &= 0xff;

	if (allocsession) {
		assert(*session == 0);
		*session = skynet_context_newsession(context);
	}

	if (needcopy && *data) {
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	*sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}

// 发送服务消息
// 参数 context： 当前函数执行的服务
// 参数 source：消息源服务
// 参数 destination：消息目标服务
// 参数 type：消息类型
// 参数 session：
// 参数 data：消息数据体
// 参数 sz：消息长度
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {
	if ((sz & MESSAGE_TYPE_MASK) != sz) {
        // 消息大小字段高8位用来存储消息类型，所以消息的长度最大是2^24次方
		skynet_error(context, "The message to %x is too large", destination);
		if (type & PTYPE_TAG_DONTCOPY) {
			skynet_free(data);
		}
		return -2;
	}
    //
	_filter_args(context, type, &session, (void **)&data, &sz);

    // 如果没有设置消息源 默认是当前服务实例
	if (source == 0) {
		source = context->handle;
	}

    // 如果要发送数据必须要有消息目标
	if (destination == 0) {
		if (data) {
			skynet_error(context, "Destination address can't be 0");
			skynet_free(data);
			return -1;
		}

		return session;
	}
	if (skynet_harbor_message_isremote(destination)) {
        // 如果目标服务是远程节点的服务
        // 把消息发给harbor去进行转发
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz & MESSAGE_TYPE_MASK; // 低24位存长度
		rmsg->type = sz >> MESSAGE_TYPE_SHIFT; // 高8位存类型
		skynet_harbor_send(rmsg, source, session); // 转发给harbor
	} else {
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

        // 将消息放入目标服务实例的消息队列中
		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

// 根据服务实例名字给服务实例发送消息
int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
    // 源服务默认是本服务实例
	if (source == 0) {
		source = context->handle;
	}
	uint32_t des = 0;
	if (addr[0] == ':') {
        // 传入的是服务实例句柄
		des = strtoul(addr+1, NULL, 16);
	} else if (addr[0] == '.') {
        // 传入的是服务名字
		des = skynet_handle_findname(addr + 1);
		if (des == 0) {
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
	} else {
        // 消息大小检测 这里跟上面的消息发送函数有点出入 为啥节点内服务消息不需要检测大小 ？？？
		if ((sz & MESSAGE_TYPE_MASK) != sz) {
			skynet_error(context, "The message to %s is too large", addr);
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -2;
		}
		_filter_args(context, type, &session, (void **)&data, &sz);

		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz & MESSAGE_TYPE_MASK;
		rmsg->type = sz >> MESSAGE_TYPE_SHIFT;
        // 转发消息给harbor
		skynet_harbor_send(rmsg, source, session);
		return session;
	}
    // 发送给目标服务实例
	return skynet_send(context, source, des, type, session, data, sz);
}

// 获取服务实例的句柄ID
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

// 设置服务实例的回调信息
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}

// 给节点内的服务实例发送服务消息
void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

	skynet_mq_push(ctx->queue, &smsg);
}

void 
skynet_globalinit(void) {
    // G_NODE是一个静态全局变量 管理整个节点的全局信息
	ATOM_INIT(&G_NODE.total , 0);
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;

    // 该函数有两个参数，第一个参数就是声明的pthread_key_t变量，
    // 第二个参数是一个清理函数，用来在线程释放该线程存储的时候被调用。该函数指针可以设成NULL，这样系统将调用默认的清理函数。
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key
	skynet_initthread(THREAD_MAIN);
}

void 
skynet_globalexit(void) {
    //注销一个TSD，这个函数并不检查当前是否有线程正使用该TSD，也不会调用清理函数（destr_function），
    // 而只是将TSD释放以供下一次调用pthread_key_create()使用。
	pthread_key_delete(G_NODE.handle_key);
}

void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);
    // 当线程中需要存储特殊值的时候调用该函数，该函数有两个参数，第一个为前面声明的pthread_key_t变量，
    // 第二个为void*变量，用来存储任何类型的值。
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

void
skynet_profile_enable(int enable) {
	G_NODE.profile = (bool)enable;
}
