#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

// 节点内模块数量最多只有32个
#define MAX_MODULE_TYPE 32

// 节点内所有模块的管理容器
struct modules {
	int count;  // 模块数量
	struct spinlock lock;  // 自旋锁
	const char * path;  // 模块查找路径
	struct skynet_module m[MAX_MODULE_TYPE];  // 模块数组 这里直接初始化了32个元素大小，后面也不会扩充
};

// 维护一个全局的模块容器指针 管理当前节点上的所有模块
static struct modules * M = NULL;

// 尝试打开指定名字的模块动态库（.so文件）
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
        // 根据模块路径和模块名 找到模块动态库的完整文件路径
		memset(tmp,0,sz);
		while (*path == ';') path++;
		if (*path == '\0') break;
		l = strchr(path, ';');
		if (l == NULL) l = path + strlen(path);
		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		memcpy(tmp+i,name,name_size);
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}

        // 加载动态库
        /*
         * dlopen（）是一个计算机函数，功能是以指定模式打开指定的动态链接库文件，并返回一个句柄给dlsym（）的调用进程。使用dlclose（）来卸载打开的库。
         * 百度百科链接：https://baike.baidu.com/item/dlopen?fromModule=lemma_search-box
         * */
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}


// 根据名字查找对应的模块对象 查找操作只是简单的做数组遍历
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

//
static void *
get_api(struct skynet_module *mod, const char *api_name) {
    // 解析匹配对应API函数的全名
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}

    // 通过名字从动态库中加载API函数的实际地址
    /*
     * 包含头文件：
        1
        #include<dlfcn.h>
        函数定义：
        1
        void*dlsym(void*handle,constchar*symbol)
        函数描述：
        dlsym(dynamic library symbol)
        根据 动态链接库 操作句柄(handle)与符号(symbol)，返回符号对应的地址。使用这个函数不但可以获取函数地址，也可以获取变量地址。
        handle：由dlopen打开动态链接库后返回的指针；
        symbol：要求获取的函数或全局变量的名称。
        返回值：
        void* 指向函数的地址，供调用使用。
     * */
	return dlsym(mod->module, ptr);
}

// 绑定create，init，release，signal函数指针地址
static int
open_sym(struct skynet_module *mod) {
    // 获取动态库的函数指针地址
    // 获取create函数指针地址
	mod->create = get_api(mod, "_create");
    // 获取init函数指针地址
	mod->init = get_api(mod, "_init");
    // 获取release函数指针地址
	mod->release = get_api(mod, "_release");
    // 获取signal函数指针地址
	mod->signal = get_api(mod, "_signal");

    // 这里直接判断init函数指针是否赋值正确 有点草率吧 如果其他函数指针赋值失败呢 ？？？
	return mod->init == NULL;
}

struct skynet_module * 
skynet_module_query(const char * name) {
    // 根据模块名找到模块对象
	struct skynet_module * result = _query(name);
	if (result)
        // 找到了就直接返回
		return result;

    // 没找到获取自旋锁 如果别的线程正在操作 这里需要不断轮询等待
	SPIN_LOCK(M)

    // 拿到自旋锁以后再查一次 为何要这么做 ？？？
	result = _query(name); // double check

    // 如果两次都没找到模块
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count;
        // 尝试打开模块动态库
		void * dl = _try_open(M,name);
		if (dl) {
			M->m[index].name = name;
			M->m[index].module = dl;

            // 绑定服务的几个必要API函数指针地址
			if (open_sym(&M->m[index]) == 0) {
                // 上面已经赋值为name 为何这里还要拷贝一次再赋值 ？？？
				M->m[index].name = skynet_strdup(name);
				M->count ++;
				result = &M->m[index];
			}
		}
	}

    // 释放自旋锁
	SPIN_UNLOCK(M)

    // 返回模块对象
	return result;
}

// 调用服务实例的create函数
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
        // 这个有点没看懂 ？？？
		return (void *)(intptr_t)(~0);
	}
}

int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
    // 不做任何判断直接通过函数地址执行
	return m->init(inst, ctx, parm);
}

void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

void 
skynet_module_init(const char *path) {
    // 分配模块容器的内存
	struct modules *m = skynet_malloc(sizeof(*m));
    // 模块数量初始化为0
	m->count = 0;
    // 赋值模块查找路径
	m->path = skynet_strdup(path);

    // 初始化全局模块管理容器的自旋锁
	SPIN_INIT(m)

    // 赋值给全局变量
	M = m;
}
