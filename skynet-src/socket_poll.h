#ifndef socket_poll_h
#define socket_poll_h

#include <stdbool.h>


/*
 * socket_poll是一个接口文件，封装了IO复用模型的上层接口，skynet网络复用模型linux下支持的是epoll，BSD系统（macosx，freeBSD）支持的是kqueue
 * skynet 网络复用封装了几个通用的接口：
 * sp_invalid()
 * sp_create()
 * sp_release()
 * sp_add()
 * sp_del()
 * sp_enable()
 * sp_wait()
 * sp_nonblocking()
 * */
typedef int poll_fd;

struct event {
	void * s;
	bool read;
	bool write;
	bool error;
	bool eof;
};

static bool sp_invalid(poll_fd fd);
static poll_fd sp_create();
static void sp_release(poll_fd fd);
static int sp_add(poll_fd fd, int sock, void *ud);
static void sp_del(poll_fd fd, int sock);
static int sp_enable(poll_fd, int sock, void *ud, bool read_enable, bool write_enable);
static int sp_wait(poll_fd, struct event *e, int max);
static void sp_nonblocking(int sock);

#ifdef __linux__
#include "socket_epoll.h"
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined (__NetBSD__)
#include "socket_kqueue.h"
#endif

#endif
