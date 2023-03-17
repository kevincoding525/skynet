#include "skynet.h"

#include "skynet_monitor.h"
#include "skynet_server.h"
#include "skynet.h"
#include "atomic.h"

#include <stdlib.h>
#include <string.h>

// 监控器结构体
struct skynet_monitor {
	ATOM_INT version;
	int check_version;
	uint32_t source;
	uint32_t destination;
};

// 创建skynet_monitor对象
struct skynet_monitor * 
skynet_monitor_new() {
    // 分配内存
	struct skynet_monitor * ret = skynet_malloc(sizeof(*ret));
    // 初始化内存区域
	memset(ret, 0, sizeof(*ret));
	return ret;
}

// 销毁skynet_monitor对象
void 
skynet_monitor_delete(struct skynet_monitor *sm) {
	skynet_free(sm);
}

void 
skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
	sm->source = source;
	sm->destination = destination;
    // 监控器版本号 + 1
	ATOM_FINC(&sm->version);
}

// 通过监控器来检测死循环
void 
skynet_monitor_check(struct skynet_monitor *sm) {
    // 版本一直没变
	if (sm->version == sm->check_version) {
		if (sm->destination) {
            // 处理处于死循环的服务实例
			skynet_context_endless(sm->destination);
            // 打印日志报警
			skynet_error(NULL, "A message from [ :%08x ] to [ :%08x ] maybe in an endless loop (version = %d)", sm->source , sm->destination, sm->version);
		}
	} else {
		sm->check_version = sm->version;
	}
}
