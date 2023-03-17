#ifndef SKYNET_MALLOC_HOOK_H
#define SKYNET_MALLOC_HOOK_H

#include <stdlib.h>
#include <stdbool.h>
#include <lua.h>

// 申请的总内存大小
extern size_t malloc_used_memory(void);

// 申请的总内存块数
extern size_t malloc_memory_block(void);

// 输出内存统计数据（调用jemalloc自带的接口）
extern void   memory_info_dump(const char *opts);

// 内存分析接口
//---------------------------------------------------------------
extern size_t mallctl_int64(const char* name, size_t* newval);
extern int    mallctl_opt(const char* name, int* newval);
extern bool   mallctl_bool(const char* name, bool* newval);
extern int    mallctl_cmd(const char* name);
extern void   dump_c_mem(void);
extern int    dump_mem_lua(lua_State *L);
extern size_t malloc_current_memory(void);
//---------------------------------------------------------------
#endif /* SKYNET_MALLOC_HOOK_H */

