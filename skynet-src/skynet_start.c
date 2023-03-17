#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

struct monitor {
	int count;
	struct skynet_monitor ** m;
	pthread_cond_t cond;
	pthread_mutex_t mutex;
	int sleep;
	int quit;
};

// 工作线程启动的参数信息
struct worker_parm {
	struct monitor *m;  // 监控管理器
	int id; // 工作线程的编号 在创建线程的时候确立
	int weight; // 工作线程的权重
};

static volatile int SIG = 0;

static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

#define CHECK_ABORT if (skynet_context_total()==0) break;

static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

// socket线程执行函数
static void *
thread_socket(void *p) {
    // 监控管理器
	struct monitor * m = p;
    // 初始化线程key
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
        // socket线程轮询函数
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		wakeup(m,0);
	}
	return NULL;
}

static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

// 监控线程执行函数 p是全局监控器管理容器
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
    // 一直循环执行监控
	for (;;) {
		CHECK_ABORT
        // 扫描所有的监控器 检测死循环
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
        // 为啥要循环睡眠5次 不是一次睡眠5S ？？？
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen

	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}

static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		skynet_socket_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1);
		usleep(2500); // 每2.5ms进行一次tick
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

// 工作线程处理函数
// p 是当前工作线程的处理权重数据 worker_parm结构
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	while (!m->quit) {
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

// skynet作业启动函数
static void
start(int thread) {
    // 创建线程数组 thread个工作线程 + 3个固定线程（socket，timer，monitor）
	pthread_t pid[thread+3];

    // 创建minitor管理器
	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread;
	m->sleep = 0;

    // 为每一个工作线程创建监控器
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}

    // 初始化互斥量
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}

    // 初始化条件量
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

    // 创建监控线程
	create_thread(&pid[0], thread_monitor, m);
    // 创建定时器线程
	create_thread(&pid[1], thread_timer, m);
    // 创建socket线程
	create_thread(&pid[2], thread_socket, m);

    // 分配每个工作线程的消费权重
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
        // 创建工作线程
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

    // 线程启动 主线程阻塞等待其他子线程结束返回
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

    // 释放监控管理器
	free_monitor(m);
}

static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	int arg_pos;
	sscanf(cmdline, "%s", name);  
	arg_pos = strlen(name);
	if (arg_pos < sz) {
		while(cmdline[arg_pos] == ' ') {
			arg_pos++;
		}
		strncpy(args, cmdline + arg_pos, sz);
	} else {
		args[0] = '\0';
	}

    // 创建bootstrap服务实例失败 将logger服务的消息分发处理掉 保证日志不丢失 便于查问题
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

void 
skynet_start(struct skynet_config * config) {
	// register SIGHUP for log file reopen
    // 信号处理 ？？？
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

    // 以守护进程模式运行
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

    // 初始化harbor
	skynet_harbor_init(config->harbor);

    // 初始化服务句柄管理容器
	skynet_handle_init(config->harbor);

    // 初始化消息队列
	skynet_mq_init();

    // 初始化模块管理容器
	skynet_module_init(config->module_path);

    // 初始化定时器
	skynet_timer_init();
    // 初始化socket
	skynet_socket_init();

    // 打开性能分析
	skynet_profile_enable(config->profile);

    // 创建logger日志服务
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

    // 为日志服务取个名字
	skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

    // 执行启动过程
	bootstrap(ctx, config->bootstrap);

    // 启动线程
	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
    // harbor退出
	skynet_harbor_exit();
    // 释放socket
	skynet_socket_free();

    // 关闭守护进程模式
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
