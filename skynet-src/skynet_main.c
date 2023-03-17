#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

// 设置默认值 将opt的值当作int转换为string类型存放到环境变量中
static int
optint(const char *key, int opt) {
    // 从环境变量获取value值
	const char * str = skynet_getenv(key);
	if (str == NULL) {
        // 将int转换为字符串
		char tmp[20];
		sprintf(tmp,"%d",opt);
        // 将string类型的值写入环境变量
		skynet_setenv(key, tmp);
		return opt;
	}
    // 将字符串转换为对应base进制的字符串
	return strtol(str, NULL, 10);
}

// 设置默认值 将opt的值当作bool值转换为string类型存放到环境变量中
static int
optboolean(const char *key, int opt) {
    // 获取环境变量的value值
	const char * str = skynet_getenv(key);
	if (str == NULL) {
        // 转换为bool字符串写入环境变量表
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

// 设置默认值 将opt的值当作string值转换为string类型存放到环境变量中
static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

// 初始化环境变量
static void
_init_env(lua_State *L) {
    // 将空值压栈
	lua_pushnil(L);  /* first key */
    // 此处应该是遍历之前load的配置文件模块 将对应的变量存放到环境变量表里面
	while (lua_next(L, -2) != 0) {
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

int
main(int argc, char *argv[]) {
    // 获取配置文件路径
	const char * config_file = NULL ;
	if (argc > 1) {
		config_file = argv[1];
	} else {
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

    // 初始化节点全局内容
	skynet_globalinit();
    // 初始化环境变量
	skynet_env_init();

    // 信号处理 ？？？
	sigign();

	struct skynet_config config;

#ifdef LUA_CACHELIB
	// init the lock of code cache
	luaL_initcodecache();
#endif

    // 创建处理配置文件的虚拟机
	struct lua_State *L = luaL_newstate();
	luaL_openlibs(L);	// link lua lib
    // 从字符串中加载一段代码块
    // 官方文档注释：加载一段 Lua 代码块，但不运行它。 如果没有错误， lua_load 把一个编译好的代码块作为一个 Lua 函数压到栈顶。 否则，压入错误消息
    // 此时栈顶-1位置是这个Lua函数
	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);

    // 将配置文件名压栈 此时栈顶-1位置是配置文件名
	lua_pushstring(L, config_file);

    // 执行上面加载的函数
    //  官方文档注释：同 lua_call 一样， lua_pcall 总是把函数本身和它的参数从栈上移除。
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
    // 初始化环境变量表
	_init_env(L);

    // 给环境变量表设置默认值 并且赋值给config结构对象
	config.thread =  optint("thread",8);  // 系统线程数量
	config.module_path = optstring("cpath","./cservice/?.so"); // C服务模块路径
	config.harbor = optint("harbor", 1);    // harborID
	config.bootstrap = optstring("bootstrap","snlua bootstrap"); // 启动文件
	config.daemon = optstring("daemon", NULL);  // 守护模式
	config.logger = optstring("logger", NULL);  // 日志输出文件名
	config.logservice = optstring("logservice", "logger");  // 日志服务名
	config.profile = optboolean("profile", 1);  // 是否启动profile

    // 释放处理配置而创建的临时lua虚拟机
	lua_close(L);

    // 执行skynet的启动 传入配置参数
	skynet_start(&config);

    // 执行skynet的退出行为
	skynet_globalexit();

	return 0;
}
