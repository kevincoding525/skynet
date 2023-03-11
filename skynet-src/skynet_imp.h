#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

// 配置结构体 启动的时候传入配置对象
struct skynet_config {
	int thread;     // 启动的线程数量
	int harbor;     // harborID
	int profile;    // ？？？
	const char * daemon;    // 是否以守护进程形式存在
	const char * module_path;   // 模块路径
	const char * bootstrap;     // bootstrap启动文件
	const char * logger;        // logger服务路径
	const char * logservice;    // logger服务
};

#define THREAD_WORKER 0 // 工作线程
#define THREAD_MAIN 1   // 主线程
#define THREAD_SOCKET 2 // socket线程
#define THREAD_TIMER 3  // 定时器线程
#define THREAD_MONITOR 4    // 监控线程

// skynet 启动函数
void skynet_start(struct skynet_config * config);

#endif
