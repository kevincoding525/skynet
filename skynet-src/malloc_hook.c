#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <lua.h>
#include <stdio.h>

#include "malloc_hook.h"
#include "skynet.h"
#include "atomic.h"

// turn on MEMORY_CHECK can do more memory check, such as double free
// #define MEMORY_CHECK

#define MEMORY_ALLOCTAG 0x20140605
#define MEMORY_FREETAG 0x0badf00d

static ATOM_SIZET _used_memory = 0;
static ATOM_SIZET _memory_block = 0;

struct mem_data {
	ATOM_ULONG handle;
	ATOM_SIZET allocated;
};

struct mem_cookie {
	uint32_t handle;
#ifdef MEMORY_CHECK
	uint32_t dogtag;
#endif
};

#define SLOT_SIZE 0x10000
#define PREFIX_SIZE sizeof(struct mem_cookie)

// 内存统计
static struct mem_data mem_stats[SLOT_SIZE];


#ifndef NOUSE_JEMALLOC

#include "jemalloc.h"

// for skynet_lalloc use
#define raw_realloc je_realloc
#define raw_free je_free

static ATOM_SIZET *
get_allocated_field(uint32_t handle) {
    // 根据handle和槽位的总大小计算出handle的槽位信息
	int h = (int)(handle & (SLOT_SIZE - 1));
	struct mem_data *data = &mem_stats[h];
	uint32_t old_handle = data->handle;
	ssize_t old_alloc = (ssize_t)data->allocated;
	if(old_handle == 0 || old_alloc <= 0) {
		// data->allocated may less than zero, because it may not count at start.
		if(!ATOM_CAS_ULONG(&data->handle, old_handle, handle)) {
			return 0;
		}
		if (old_alloc < 0) {
			ATOM_CAS_SIZET(&data->allocated, (size_t)old_alloc, 0);
		}
	}
	if(data->handle != handle) {
		return 0;
	}
	return &data->allocated; // 返回已分配的内存大小
}

// 更新内存统计信息（申请内存时）
inline static void
update_xmalloc_stat_alloc(uint32_t handle, size_t __n) {
	ATOM_FADD(&_used_memory, __n); // 统计进程使用总内存大小（+）
	ATOM_FINC(&_memory_block); // 统计进程使用总内存块（+）
	ATOM_SIZET * allocated = get_allocated_field(handle); // 获取当前服务实例申请过的内存大小
	if(allocated) {
		ATOM_FADD(allocated, __n); // 累加已分配的内存数量
	}
}

// 更新内存统计信息（释放内存时）
inline static void
update_xmalloc_stat_free(uint32_t handle, size_t __n) {
	ATOM_FSUB(&_used_memory, __n); // 统计进程使用总内存大小（-）
	ATOM_FDEC(&_memory_block);  // 统计进程使用总内存块（-）
	ATOM_SIZET * allocated = get_allocated_field(handle); // 获取当前服务实例申请过的内存大小
	if(allocated) {
		ATOM_FSUB(allocated, __n); // 减掉分配的数量
	}
}

// 填充内存后面的prefix信息 skynet每个申请的内存块后面都附上了一个mem_cookie信息
// 里面存放了当前申请内存的服务实例句柄ID
inline static void*
fill_prefix(char* ptr) {
	uint32_t handle = skynet_current_handle(); // 获取当前线程正在处理的服务实例句柄
	size_t size = je_malloc_usable_size(ptr); // 获取指针分配的内存大小
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie)); // 拿到mem_cookie的偏移位置
	memcpy(&p->handle, &handle, sizeof(handle)); // 将服务实例句柄放入mem_cookie
#ifdef MEMORY_CHECK
	uint32_t dogtag = MEMORY_ALLOCTAG;
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
    // 内存统计处理
	update_xmalloc_stat_alloc(handle, size);
	return ptr;
}

// 移除内存后缀信息
inline static void*
clean_prefix(char* ptr) {
	size_t size = je_malloc_usable_size(ptr); // 获取指针分配的内存大小
	struct mem_cookie *p = (struct mem_cookie *)(ptr + size - sizeof(struct mem_cookie)); // 通过偏移获取cookie数据地址
	uint32_t handle;
	memcpy(&handle, &p->handle, sizeof(handle));
#ifdef MEMORY_CHECK
	uint32_t dogtag;
	memcpy(&dogtag, &p->dogtag, sizeof(dogtag));
	if (dogtag == MEMORY_FREETAG) {
		fprintf(stderr, "xmalloc: double free in :%08x\n", handle);
	}
	assert(dogtag == MEMORY_ALLOCTAG);	// memory out of bounds
	dogtag = MEMORY_FREETAG;
	memcpy(&p->dogtag, &dogtag, sizeof(dogtag));
#endif
	update_xmalloc_stat_free(handle, size);
	return ptr;
}

// 内存发生oom时的处理
static void malloc_oom(size_t size) {
	fprintf(stderr, "xmalloc: Out of memory trying to allocate %zu bytes\n",
		size);
	fflush(stderr);
	abort();
}

// jemalloc自带的 输出内存统计数据
void
memory_info_dump(const char* opts) {
	je_malloc_stats_print(0,0, opts);
}


// ---------------------------------------------- memory profile -----------------------------------------
bool
mallctl_bool(const char* name, bool* newval) {
	bool v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(bool));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	return v;
}

int
mallctl_cmd(const char* name) {
	return je_mallctl(name, NULL, NULL, NULL, 0);
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	size_t v = 0;
	size_t len = sizeof(v);
	if(newval) {
		je_mallctl(name, &v, &len, newval, sizeof(size_t));
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}
	// skynet_error(NULL, "name: %s, value: %zd\n", name, v);
	return v;
}

int
mallctl_opt(const char* name, int* newval) {
	int v = 0;
	size_t len = sizeof(v);
	if(newval) {
		int ret = je_mallctl(name, &v, &len, newval, sizeof(int));
		if(ret == 0) {
			skynet_error(NULL, "set new value(%d) for (%s) succeed\n", *newval, name);
		} else {
			skynet_error(NULL, "set new value(%d) for (%s) failed: error -> %d\n", *newval, name, ret);
		}
	} else {
		je_mallctl(name, &v, &len, NULL, 0);
	}

	return v;
}
// ---------------------------------------------- memory profile -----------------------------------------

// hook : malloc, realloc, free, calloc

/*
 * 按大小分配内存
 * 参数 size：需要分配的内存大小
 * */
void *
skynet_malloc(size_t size) {
    // 调用jemalloc的malloc函数分配内存
	void* ptr = je_malloc(size + PREFIX_SIZE);
    // 如果内存分配失败 应该是内存不够 打印日志 终止程序运行
	if(!ptr) malloc_oom(size);

    // 填充内存后面的prefix信息 skynet每个申请的内存块后面都附上了一个mem_cookie信息
    // 里面存放了当前申请内存的服务实例句柄ID
	return fill_prefix(ptr);
}

/*
 * realloc的语义：先判断当前的指针是否有足够的连续空间，如果有，扩大mem_address指向的地址，并且将mem_address返回，
 * 如果空间不够，先按照newsize指定的大小分配空间，将原有数据从头到尾拷贝到新分配的内存区域，
 * 而后释放原来mem_address所指内存区域（注意：原来指针是自动释放，不需要使用free），同时返回新分配的内存区域的首地址。
 * 即重新分配存储器块的地址。
 * --------------------------------------------------------------------------------
 * 在已有指针上 按大小重新分配内存
 * 参数 ptr：已有指针
 * 参数 size：需要分配的内存大小
 * */
void *
void *
skynet_realloc(void *ptr, size_t size) {
	if (ptr == NULL) return skynet_malloc(size);

    // 先清除掉指针之前内存的后缀数据信息
	void* rawptr = clean_prefix(ptr);
	void *newptr = je_realloc(rawptr, size+PREFIX_SIZE);
	if(!newptr) malloc_oom(size);
	return fill_prefix(newptr);
}

void
skynet_free(void *ptr) {
	if (ptr == NULL) return;
	void* rawptr = clean_prefix(ptr); // 移除后缀数据
	je_free(rawptr); // 释放内存
}

/*
 * 在内存的动态存储区中分配num个长度为size的连续空间，函数返回一个指向分配起始地址的指针；如果分配不成功，返回NULL。
 * calloc在动态分配完内存后，自动初始化该内存空间为零，而malloc不做初始化，分配到的空间中的数据是随机数据。
 * */
void *
skynet_calloc(size_t nmemb,size_t size) {
	void* ptr = je_calloc(nmemb + ((PREFIX_SIZE+size-1)/size), size );
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void *
skynet_memalign(size_t alignment, size_t size) {
	void* ptr = je_memalign(alignment, size + PREFIX_SIZE);
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

void *
skynet_aligned_alloc(size_t alignment, size_t size) {
	void* ptr = je_aligned_alloc(alignment, size + (size_t)((PREFIX_SIZE + alignment -1) & ~(alignment-1)));
	if(!ptr) malloc_oom(size);
	return fill_prefix(ptr);
}

int
skynet_posix_memalign(void **memptr, size_t alignment, size_t size) {
	int err = je_posix_memalign(memptr, alignment, size + PREFIX_SIZE);
	if (err) malloc_oom(size);
	fill_prefix(*memptr);
	return err;
}

#else

// for skynet_lalloc use
#define raw_realloc realloc
#define raw_free free

void
memory_info_dump(const char* opts) {
	skynet_error(NULL, "No jemalloc");
}

size_t
mallctl_int64(const char* name, size_t* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_int64 %s.", name);
	return 0;
}

int
mallctl_opt(const char* name, int* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_opt %s.", name);
	return 0;
}

bool
mallctl_bool(const char* name, bool* newval) {
	skynet_error(NULL, "No jemalloc : mallctl_bool %s.", name);
	return 0;
}

int
mallctl_cmd(const char* name) {
	skynet_error(NULL, "No jemalloc : mallctl_cmd %s.", name);
	return 0;
}

#endif

// 获取当前进程使用的总内存数
size_t
malloc_used_memory(void) {
	return ATOM_LOAD(&_used_memory);
}

// 获取当前进程使用的总内存块数（一个连续的内存区域为一块）
size_t
malloc_memory_block(void) {
	return ATOM_LOAD(&_memory_block);
}

// 统计所有服务的内存总使用量
void
dump_c_mem() {
	int i;
	size_t total = 0;
	skynet_error(NULL, "dump all service mem:");
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			total += data->allocated;
			skynet_error(NULL, ":%08x -> %zdkb %db", data->handle, data->allocated >> 10, (int)(data->allocated % 1024));
		}
	}
	skynet_error(NULL, "+total: %zdkb",total >> 10);
}

// 字符串拷贝函数
char *
skynet_strdup(const char *str) {
	size_t sz = strlen(str);
	char * ret = skynet_malloc(sz+1);
	memcpy(ret, str, sz+1);
	return ret;
}

// 在原指针之上直接重新分配nsize大小的内存
void *
skynet_lalloc(void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		raw_free(ptr);
		return NULL;
	} else {
		return raw_realloc(ptr, nsize);
	}
}

// 获取节点内所有服务的内存使用情况 - lua层接口
int
dump_mem_lua(lua_State *L) {
	int i;
	lua_newtable(L);
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle != 0 && data->allocated != 0) {
			lua_pushinteger(L, data->allocated);
			lua_rawseti(L, -2, (lua_Integer)data->handle);
		}
	}
	return 1;
}

// 获取当前线程正在处理的服务实例所申请的内存数量
size_t
malloc_current_memory(void) {
	uint32_t handle = skynet_current_handle();
	int i;
	for(i=0; i<SLOT_SIZE; i++) {
		struct mem_data* data = &mem_stats[i];
		if(data->handle == (uint32_t)handle && data->allocated != 0) {
			return (size_t) data->allocated;
		}
	}
	return 0;
}

// 打印当前线程正在处理的服务实例的内存使用情况
void
skynet_debug_memory(const char *info) {
	// for debug use
	uint32_t handle = skynet_current_handle();
	size_t mem = malloc_current_memory();
	fprintf(stderr, "[:%08x] %s %p\n", handle, info, (void *)mem);
}
