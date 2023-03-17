#include "skynet.h"
#include "skynet_harbor.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_handle.h"

#include <string.h>
#include <stdio.h>
#include <assert.h>

static struct skynet_context * REMOTE = 0;
static unsigned int HARBOR = ~0;

static inline int
invalid_type(int type) {
	return type != PTYPE_SYSTEM && type != PTYPE_HARBOR;
}

void 
skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session) {
    // 检测消息有效性 以及harbor服务是否存在
	assert(invalid_type(rmsg->type) && REMOTE);
    // 将消息发给harbor服务
	skynet_context_send(REMOTE, rmsg, sizeof(*rmsg) , source, PTYPE_SYSTEM , session);
}

int 
skynet_harbor_message_isremote(uint32_t handle) {
	assert(HARBOR != ~0);
	int h = (handle & ~HANDLE_MASK);
	return h != HARBOR && h !=0;
}

void
skynet_harbor_init(int harbor) {
    // 设置当前节点的harbor绝对值
	HARBOR = (unsigned int)harbor << HANDLE_REMOTE_SHIFT;
}

void
skynet_harbor_start(void *ctx) {
	// the HARBOR must be reserved to ensure the pointer is valid.
	// It will be released at last by calling skynet_harbor_exit
    // 这里强行对实例的引用计数进行了 + 1 但是没有 - 1
    // 上面解释的很清楚 这样做是为了保证harbor服务实例不被意外释放，后面在harbor退出的时候会进行引用计数 -1 来释放
	skynet_context_reserve(ctx);
	REMOTE = ctx;
}

void
skynet_harbor_exit() {
	struct skynet_context * ctx = REMOTE;
	REMOTE= NULL;
	if (ctx) {
        // 对服务实例计数器 - 1
		skynet_context_release(ctx);
	}
}
