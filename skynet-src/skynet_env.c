#include "skynet.h"
#include "skynet_env.h"
#include "spinlock.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <assert.h>

struct skynet_env {
	struct spinlock lock;  // 自旋锁
	lua_State *L;   // 虚拟机对象
};

static struct skynet_env *E = NULL;

// 获取环境变量
const char * 
skynet_getenv(const char *key) {
    // 用自旋锁锁住环境变量
	SPIN_LOCK(E)

    // 获取环境变量中持有的虚拟机指针
	lua_State *L = E->L;

    // 从虚拟机的全局表中获取对应key的值放到栈顶
	lua_getglobal(L, key);

    // 将栈顶元素转成string类型
	const char * result = lua_tostring(L, -1);

    // 弹出栈顶元素
	lua_pop(L, 1);

    // 释放自旋锁
	SPIN_UNLOCK(E)

    // 返回结果
	return result;
}

void 
skynet_setenv(const char *key, const char *value) {
    // 用自旋锁锁住环境变量
	SPIN_LOCK(E)

    // 获取环境变量中持有的虚拟机指针
	lua_State *L = E->L;
    // 从虚拟机的全局表中获取对应key的值放到栈顶
	lua_getglobal(L, key);
    // 判断栈顶元素非空
	assert(lua_isnil(L, -1));
    // 弹出栈顶元素
	lua_pop(L,1);

    // 将需要写入的value值放到栈顶
	lua_pushstring(L,value);

    // 将栈顶value值写到全局表的key字段里面去
	lua_setglobal(L,key);

    // 释放自旋锁
	SPIN_UNLOCK(E)
}

void
skynet_env_init() {
    // 初始化环境变量
	E = skynet_malloc(sizeof(*E));
    // 初始化自旋锁
	SPIN_INIT(E)
    // 创建一个lua虚拟机对象
	E->L = luaL_newstate();
}
