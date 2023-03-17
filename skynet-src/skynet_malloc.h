#ifndef skynet_malloc_h
#define skynet_malloc_h

#include <stddef.h>

/*
 * skynet的内存管理模块
 * */

// 定义统一的内存管理接口 ///
//-------------------------------宏定义---------------------------------------------
// 申请内存 并将内存初始化为0值
#define skynet_malloc malloc

// 申请内存
#define skynet_calloc calloc

// 对已经分配过内存的指针重新按新的大小分配内存
#define skynet_realloc realloc

// 释放指针内存
#define skynet_free free

/*
 * 函数memalign将分配一个由size指定大小，地址是boundary的倍数的内存块。参数boundary必须是2的幂！
 * 函数memalign可以分配较大的内存块，并且可以为返回的地址指定粒度。
 * */
#define skynet_memalign memalign

/*
 * 分配size由其指定对齐的未初始化存储的字节alignment。该size参数必须是的整数倍alignment。
aligned_alloc 是线程安全的：它的行为就好像只访问通过参数可见的内存位置，而不是任何静态存储。
先前调用free或realloc释放内存区域的同步 -调用aligned_alloc该内存分配同一区域或部分内存区域。
在通过释放函数访问内存之后以及在通过内存访问内存之前，会发生此同步aligned_alloc。
所有分配和解除分配功能在内存的每个特定区域都有一个总的顺序。
返回值
成功时，将指针返回到新分配的内存的开始位置。返回的指针必须用free()或来解除分配realloc()。
失败时，返回一个空指针。
 * */
#define skynet_aligned_alloc aligned_alloc

/*
 * 头文件：#include <stdlib.h>

函数原型：int posix_memalign (void **memptr,
                     size_t alignment,
                     size_t size);

参数：
 *      memptr           分配好的内存空间的首地址
 *      alignment        对齐边界，Linux中，32位系统是8字节，64位系统是16字节
 *      size                  指定分配size字节大小的内存

返回值：调用posix_memalign( )成功时会返回size字节的动态内存，并且这块内存的地址是alignment的倍数。参数alignment必须是2的幂，还是void指针的大小的倍数。返回的内存块的地址放在了memptr里面，函数返回值是0。
调用失败时，没有内存会被分配，memptr的值没有被定义，返回如下错误码之一：
EINVAL：参数不是2的幂，或者不是void指针的倍数。
ENOMEM：没有足够的内存去满足函数的请求。
要注意的是，对于这个函数，errno不会被设置，只能通过返回值得到。
 * */
#define skynet_posix_memalign posix_memalign


//-------------------------------函数定义---------------------------------------------
/*
 * 这一块单独和jemalloc统一分析
 * */
void * skynet_malloc(size_t sz);
void * skynet_calloc(size_t nmemb,size_t size);
void * skynet_realloc(void *ptr, size_t size);
void skynet_free(void *ptr);
char * skynet_strdup(const char *str);
void * skynet_lalloc(void *ptr, size_t osize, size_t nsize);	// use for lua
void * skynet_memalign(size_t alignment, size_t size);
void * skynet_aligned_alloc(size_t alignment, size_t size);
int skynet_posix_memalign(void **memptr, size_t alignment, size_t size);

#endif
