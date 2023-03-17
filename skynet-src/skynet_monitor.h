#ifndef SKYNET_MONITOR_H
#define SKYNET_MONITOR_H

#include <stdint.h>

/*
 * skynet的监控
 * */
struct skynet_monitor;

// 创建一个监控器
struct skynet_monitor * skynet_monitor_new();
// 销毁一个监控器
void skynet_monitor_delete(struct skynet_monitor *);
// 触发一次monitor的版本更新
void skynet_monitor_trigger(struct skynet_monitor *, uint32_t source, uint32_t destination);
// 执行监控器的死循环检测
void skynet_monitor_check(struct skynet_monitor *);

#endif
