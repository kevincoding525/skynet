#include "skynet.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 日志内容最大长度
#define LOG_MESSAGE_SIZE 256

/*
 * 日志打印
 * 参数1：context 服务实例指针
 * 参数2： 日志文本内容
 * 参数...: 可选参数
 * */
void 
skynet_error(struct skynet_context * context, const char *msg, ...) {
    // 先获取日志服务
	static uint32_t logger = 0;
	if (logger == 0) {
		logger = skynet_handle_findname("logger");
	}
	if (logger == 0) {
		return;
	}

    // 初始化一个最大长度的字符数组
	char tmp[LOG_MESSAGE_SIZE];
	char *data = NULL;

    // 提取可变参数
    //-------------------------
	va_list ap;
	va_start(ap,msg);
	int len = vsnprintf(tmp, LOG_MESSAGE_SIZE, msg, ap);
	va_end(ap);
    //-------------------------

	if (len >=0 && len < LOG_MESSAGE_SIZE) {
		data = skynet_strdup(tmp);
	} else {
        // 如果数据长度超过了最大长度 重新分配更大的内存空间
		int max_size = LOG_MESSAGE_SIZE;
		for (;;) {
			max_size *= 2;
			data = skynet_malloc(max_size);
			va_start(ap,msg);
			len = vsnprintf(data, max_size, msg, ap);
			va_end(ap);
			if (len < max_size) {
				break;
			}
			skynet_free(data);
		}
	}
	if (len < 0) {
		skynet_free(data);
		perror("vsnprintf error :");
		return;
	}


    // 声明一个消息对象
	struct skynet_message smsg;
	if (context == NULL) {
		smsg.source = 0;
	} else {
        // 消息源 存放服务的句柄
		smsg.source = skynet_context_handle(context);
	}
	smsg.session = 0;
	smsg.data = data;
    // 消息的sz字段还需要拼上消息内容的类型 这里是文本类型
	smsg.sz = len | ((size_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);
    // 将消息推给目标服务的消息队列
	skynet_context_push(logger, &smsg);
}

