#ifndef SKYNET_HARBOR_H
#define SKYNET_HARBOR_H

#include <stdint.h>
#include <stdlib.h>

#define GLOBALNAME_LENGTH 16
#define REMOTE_MAX 256

/*
 * skynet 构建了一个harbor的c服务来专门转发负责两个节点直接的消息通信
 * */
struct remote_name {
	char name[GLOBALNAME_LENGTH];
	uint32_t handle;
};

struct remote_message {
	struct remote_name destination;
	const void * message;
	size_t sz;
	int type;
};

void skynet_harbor_send(struct remote_message *rmsg, uint32_t source, int session);
int skynet_harbor_message_isremote(uint32_t handle);

// harbor初始化
void skynet_harbor_init(int harbor);

// harbor启动
void skynet_harbor_start(void * ctx);

// harbor退出
void skynet_harbor_exit();

#endif
