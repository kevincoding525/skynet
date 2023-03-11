#ifndef SKYNET_ENV_H
#define SKYNET_ENV_H

/*
 * skynet 全局环境变量模块
 * */

// 获取环境变量
const char * skynet_getenv(const char *key);

// 设置环境变量
void skynet_setenv(const char *key, const char *value);

// 初始化环境变量表
void skynet_env_init();

#endif
