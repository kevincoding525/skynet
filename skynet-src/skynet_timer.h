#ifndef SKYNET_TIMER_H
#define SKYNET_TIMER_H

#include <stdint.h>

/*
 * skynet时间模块 包括时间的获取更新以及定时器的实现
 *
 * */

/*
 * 添加定时器接口
 * 参数 handle ： 添加定时器的服务实例句柄
 * 参数 time ： 定时器超时时间
 * 参数 session ：服务传过来的session 上层处理异步回调使用
 * */
int skynet_timeout(uint32_t handle, int time, int session);

/*
 * 定时器tick驱动相应接口
 * */
void skynet_updatetime(void);

/*
 * 获取进程启动的绝对时间 单位是秒（即从UTC1970-1-1 0:0:0开始计时）
 * */
uint32_t skynet_starttime(void);

/*
 * 获取线程时间 单位是微妙
 * */
uint64_t skynet_thread_time(void);	// for profile, in micro second

/*
 * 定时器管理容器初始化
 * */
void skynet_timer_init(void);

#endif
