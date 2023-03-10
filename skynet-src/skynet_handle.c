#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define DEFAULT_SLOT_SIZE 4
#define MAX_SLOT_SIZE 0x40000000

 /*
  * 服务名字结构体
  * */
struct handle_name {
	char * name;		//	服务的名字
	uint32_t handle;	// 服务的句柄ID
};

struct handle_storage {
	struct rwlock lock;		// 因为可能多个线程进行服务操作，需要读写锁

	uint32_t harbor;	//	当前节点的harbor
	uint32_t handle_index;	// 当前节点的句柄自增ID
	int slot_size;		// 哈希槽大小

	// 实现一个哈希表 存储节点上的服务实例
	// 这里是通过平坦地址的方式来解决哈希冲突
	struct skynet_context ** slot;
	
	int name_cap;	//	服务名字数组的容量
	int name_count;	//	服务名字数组的大小
	struct handle_name *name;	//	服务名字数组
};

// 维护节点的服务管理容器 定义成static变量
static struct handle_storage *H = NULL;

uint32_t
skynet_handle_register(struct skynet_context *ctx) {
	struct handle_storage *s = H;

	rwlock_wlock(&s->lock);	// --%-- 添加写锁 --%--
	
	for (;;) {
		int i;
		uint32_t handle = s->handle_index;
		// 通过遍历的方式找到当前哈希槽的空闲位置
		// 这里用了一个取巧的方式，因为最差情况就是当前所有的位置都满了
		// 遍历的次数所以最大业就是哈希槽的当前大小
		for (i=0;i<s->slot_size;i++,handle++) {
			// 如果handle超过了24位的大小（16777216） 则重新从1开始
			// 一般单个节点内的服务数量不太可能超过这个数量级
			if (handle > HANDLE_MASK) {
				// 0 is reserved
				handle = 1;
			}
			// 计算哈希值
			int hash = handle & (s->slot_size-1);
			if (s->slot[hash] == NULL) {
				s->slot[hash] = ctx;

				// 写入哈希槽成功以后再将ID自增
				s->handle_index = handle + 1;

				// --%-- 释放写锁 --%--
				rwlock_wunlock(&s->lock);

				// 合并harbor生成完整的句柄ID
				handle |= s->harbor;
				return handle;
			}
		}

		// 没找到空闲的哈希槽位 则进行哈希槽扩充 然后rehash

		// 哈希槽的最大长度不能超过节点的句柄最大值
		assert((s->slot_size*2 - 1) <= HANDLE_MASK);
		//在当前槽位大小的基础上翻倍扩充哈希槽
		struct skynet_context ** new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));
		// 对原有数据进行rehash
		for (i=0;i<s->slot_size;i++) {
			if (s->slot[i]) {
				// 重算哈希值
				int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1);
				assert(new_slot[hash] == NULL);
				new_slot[hash] = s->slot[i];
			}
		}
		skynet_free(s->slot);
		s->slot = new_slot;
		s->slot_size *= 2;
	}
}

int
skynet_handle_retire(uint32_t handle) {
	int ret = 0;
	struct handle_storage *s = H;

	// 添加写锁
	rwlock_wlock(&s->lock);

	// 通过句柄找到服务实例对象
	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];

	if (ctx != NULL && skynet_context_handle(ctx) == handle) {
		// 先从哈希槽中移除
		s->slot[hash] = NULL;
		ret = 1;
		int i;
		int j=0, n=s->name_count;

		// 遍历名字数组移除对应handle的服务实例名字
		// 这里有个取巧的方法 在遍历的过程中用j记录上次被释放的空间，下个数据直接放到上一个空位 这样保持数组的紧凑
		for (i=0; i<n; ++i) {
			// 从名字数组中找到对应的handle_name对象
			if (s->name[i].handle == handle) {
				// 释放名字字符串指针内存
				skynet_free(s->name[i].name);
				continue;
			} else if (i!=j) {
				s->name[j] = s->name[i];
			}
			++j;
		}

		s->name_count = j;
	} else {
		ctx = NULL;
	}

	// 释放写锁
	rwlock_wunlock(&s->lock);

	if (ctx) {
		// release ctx may call skynet_handle_* , so wunlock first.
		// 释放服务实例对象
		skynet_context_release(ctx);
	}

	return ret;
}

void 
skynet_handle_retireall() {
	struct handle_storage *s = H;
	// 不断的扫描哈希槽释放服务实例，直到哈希槽为空
	for (;;) {
		int n=0;  // 统计现有的服务实例数量
		int i;

		// 遍历整个服务实例哈希槽
		for (i=0;i<s->slot_size;i++) {
			// 添加读锁
			rwlock_rlock(&s->lock);
			struct skynet_context * ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx) {
				handle = skynet_context_handle(ctx);
				++n;
			}

			// 释放读锁
			rwlock_runlock(&s->lock);
			if (handle != 0) {
				skynet_handle_retire(handle);
			}
		}
		if (n==0)
			return;
	}
}

struct skynet_context * 
skynet_handle_grab(uint32_t handle) {
	struct handle_storage *s = H;
	struct skynet_context * result = NULL;

	// 添加读锁
	rwlock_rlock(&s->lock);

	uint32_t hash = handle & (s->slot_size-1);
	struct skynet_context * ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle) {
		result = ctx;
		// ？？？
		skynet_context_grab(result);
	}

	// 释放读锁
	rwlock_runlock(&s->lock);

	return result;
}

uint32_t 
skynet_handle_findname(const char * name) {
	struct handle_storage *s = H;

	// 添加读锁
	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;

	// 对名字结构数组进行二分查找
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		// 进行名字比较
		int c = strcmp(n->name, name);
		if (c==0) {
			handle = n->handle;
			break;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}

	// 释放读锁
	rwlock_runlock(&s->lock);

	return handle;
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before) {
	if (s->name_count >= s->name_cap) {
		// 数量超过了容量 按原来的两倍扩容
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name * n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i=0;i<before;i++) {
			n[i] = s->name[i];
		}
		for (i=before;i<s->name_count;i++) {
			n[i+1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	} else {
		int i;
		// before之后的后移一个位置 给要插入的数据腾出位置
		for (i=s->name_count;i>before;i--) {
			s->name[i] = s->name[i-1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count ++;
}

static const char *
_insert_name(struct handle_storage *s, const char * name, uint32_t handle) {
	int begin = 0;
	int end = s->name_count - 1;
	// 对名字结构数组进行二分查找，如果找到了对应名字的handle 说明同名的handle已经存在了
	while (begin<=end) {
		int mid = (begin+end)/2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c==0) {
			return NULL;
		}
		if (c<0) {
			begin = mid + 1;
		} else {
			end = mid - 1;
		}
	}
	// 进行字符串拷贝
	char * result = skynet_strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

const char * 
skynet_handle_namehandle(uint32_t handle, const char *name) {
	rwlock_wlock(&H->lock);

	const char * ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

void 
skynet_handle_init(int harbor) {
	assert(H==NULL);
	// 初始化服务存储器全局变量
	struct handle_storage * s = skynet_malloc(sizeof(*H));
	// 设置哈希槽的默认大小
	s->slot_size = DEFAULT_SLOT_SIZE;
	// 初始化哈希槽
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	// 初始化读写锁
	rwlock_init(&s->lock);
	// reserve 0 for system
	// 初始化harbor值 这里harbor值已经进行了移位操作
	s->harbor = (uint32_t) (harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1;
	// 名字结构数组默认容量是2
	s->name_cap = 2;
	s->name_count = 0;

	// 初始化名字结构数组
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	H = s;

	// Don't need to free H
}

