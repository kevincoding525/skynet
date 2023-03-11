#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H

/*
 * skynet 全局环境变量模块
 * */

// 从环境变量表里获取一个key对应的value值
const char * skynet_getenv(const char *key);
void skynet_setenv(const char *key, const char *value);

void skynet_env_init();

#endif
