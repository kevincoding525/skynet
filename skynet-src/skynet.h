#ifndef SKYNET_H
#define SKYNET_H

#include "skynet_malloc.h"

#include <stddef.h>
#include <stdint.h>

#define PTYPE_TEXT 0
#define PTYPE_RESPONSE 1
#define PTYPE_MULTICAST 2
#define PTYPE_CLIENT 3
#define PTYPE_SYSTEM 4
#define PTYPE_HARBOR 5
#define PTYPE_SOCKET 6
// read lualib/skynet.lua examples/simplemonitor.lua
#define PTYPE_ERROR 7	
// read lualib/skynet.lua lualib/mqueue.lua lualib/snax.lua
#define PTYPE_RESERVED_QUEUE 8
#define PTYPE_RESERVED_DEBUG 9
#define PTYPE_RESERVED_LUA 10
#define PTYPE_RESERVED_SNAX 11

#define PTYPE_TAG_DONTCOPY 0x10000
#define PTYPE_TAG_ALLOCSESSION 0x20000

struct skynet_context;

// 日志打印
void skynet_error(struct skynet_context * context, const char *msg, ...);

// 执行lua层的命令
const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);

// 根据名字查询服务实例的句柄ID
uint32_t skynet_queryname(struct skynet_context * context, const char * name);

// 节点内发送服务消息
int skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * msg, size_t sz);

// 根据目标服务实例的名字发送服务消息
int skynet_sendname(struct skynet_context * context, uint32_t source, const char * destination , int type, int session, void * msg, size_t sz);

// 判断服务是不是远程节点的服务 harbor返回远程节点的harborID
int skynet_isremote(struct skynet_context *, uint32_t handle, int * harbor);

// 服务消息处理函数指针
typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);

// 设置服务的消息处理回调函数
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

// 获取当前线程正在处理的服务
uint32_t skynet_current_handle(void);

// 获取当前进程启动到现在的时间差值
uint64_t skynet_now(void);

// 统计当前服务占用的内存
void skynet_debug_memory(const char *info);	// for debug use, output current service memory to stderr

#endif
