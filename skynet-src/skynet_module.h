#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

/*
 * skynet模块 所有的C服务被加载进内存都是以模块的形式存在
 * */

struct skynet_context;

// 定义一些函数指针
typedef void * (*skynet_dl_create)(void);
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
typedef void (*skynet_dl_release)(void * inst);
typedef void (*skynet_dl_signal)(void * inst, int signal);

// 模块结构体
struct skynet_module {
	const char * name;  // 模块名
	void * module;  // 模块指针
	skynet_dl_create create;  // 创建函数
	skynet_dl_init init;  // 初始化函数
	skynet_dl_release release;  // 释放函数
	skynet_dl_signal signal; // 信号相应函数
};

// 根据名字查询模块
struct skynet_module * skynet_module_query(const char * name);

// 调用服务实例的create函数
void * skynet_module_instance_create(struct skynet_module *);
// 调用服务实例的init函数
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
// 调用服务实例的release函数
void skynet_module_instance_release(struct skynet_module *, void *inst);
// 调用服务实例的signal函数
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);
// 初始化模块管理容器
void skynet_module_init(const char *path);

#endif
