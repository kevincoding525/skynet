#ifndef SKYNET_CONTEXT_HANDLE_H
#define SKYNET_CONTEXT_HANDLE_H

#include <stdint.h>

// reserve high 8 bits for remote id
#define HANDLE_MASK 0xffffff // C里面定义整数的长度为啥喜欢用16进制？
#define HANDLE_REMOTE_SHIFT 24  //  handleID 高8位是harbor，低24位是本节点内的handle自增ID

struct skynet_context;

// 注册服务实例到handle管理器
uint32_t skynet_handle_register(struct skynet_context *);
// 回收句柄对应的服务实例对象
int skynet_handle_retire(uint32_t handle);
// 根据句柄获得服务实例对象
struct skynet_context * skynet_handle_grab(uint32_t handle);
// 回收所有的服务实例对象
void skynet_handle_retireall();

// 根据服务名字查找
uint32_t skynet_handle_findname(const char * name);

const char * skynet_handle_namehandle(uint32_t handle, const char *name);

// 节点服务handle管理器初始化
void skynet_handle_init(int harbor);

#endif
