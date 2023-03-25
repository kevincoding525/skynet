#ifndef poll_socket_kqueue_h
#define poll_socket_kqueue_h

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


/*
 * skynet 网络模块 - 对kqueue网络复用模型的封装（kqueue主要用在BSD系统上）
 * */
// 检测IO的文件描述符是否可用
static bool
sp_invalid(int kfd) {
    return kfd == -1;
}

// 创建一个kqueue对象
static int
sp_create() {
    return kqueue();
}


// 释放IO文件描述符
static void
sp_release(int kfd) {
    close(kfd);
}

// 移除一个socket的网络监听
static void
sp_del(int kfd, int sock) {

    // 向内核队列移除一个文件描述符的事件监听
    // 移除读事件监听
    struct kevent ke;
    EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(kfd, &ke, 1, NULL, 0, NULL);
    // 移除写事件监听
    EV_SET(&ke, sock, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(kfd, &ke, 1, NULL, 0, NULL);
}

// 添加一个socket的网络监听
static int
sp_add(int kfd, int sock, void *ud) {
    // 构造一个kevent结构体
    struct kevent ke;

    // 向内核队列中添加一个读事件
    EV_SET(&ke, sock, EVFILT_READ, EV_ADD, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        return 1;
    }

    // 向内核队列中添加一个写事件
    EV_SET(&ke, sock, EVFILT_WRITE, EV_ADD, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        // 如果写事件添加失败 则把读事件移除掉
        EV_SET(&ke, sock, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(kfd, &ke, 1, NULL, 0, NULL);
        return 1;
    }

    // 向内核队列中禁用一个写事件
    EV_SET(&ke, sock, EVFILT_WRITE, EV_DISABLE, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        sp_del(kfd, sock);
        return 1;
    }
    return 0;
}

// 设置socket的网络事件监听状态
static int
sp_enable(int kfd, int sock, void *ud, bool read_enable, bool write_enable) {
    int ret = 0;

    // 改变内核队列中指定文件描述符的读写监听状态
    struct kevent ke;
    EV_SET(&ke, sock, EVFILT_READ, read_enable ? EV_ENABLE : EV_DISABLE, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        ret |= 1;
    }
    EV_SET(&ke, sock, EVFILT_WRITE, write_enable ? EV_ENABLE : EV_DISABLE, 0, 0, ud);
    if (kevent(kfd, &ke, 1, NULL, 0, NULL) == -1 || ke.flags & EV_ERROR) {
        ret |= 1;
    }
    return ret;
}

// 获取可相应的网络事件
static int
sp_wait(int kfd, struct event *e, int max) {
    struct kevent ev[max];
    // 相应事件
    // ev 返回的是触发的事件队列， max是触发的事件数量
    int n = kevent(kfd, NULL, 0, ev, max, NULL);

    int i;
    // 遍历返回的可相应事件
    for (i = 0; i < n; i++) {
        e[i].s = ev[i].udata;
        unsigned filter = ev[i].filter;
        bool eof = (ev[i].flags & EV_EOF) != 0;
        e[i].write = (filter == EVFILT_WRITE) && (!eof);
        e[i].read = (filter == EVFILT_READ);
        e[i].error = (ev[i].flags & EV_ERROR) != 0;
        e[i].eof = eof;
    }

    return n;
}

// 设置文件描述符为非阻塞
static void
sp_nonblocking(int fd) {
    /*
     * fcntl是一个在Unix和类Unix系统中使用的系统调用，用于对一个已经打开的文件描述符进行控制操作。
     * fcntl可以用于改变文件描述符的属性、状态和行为，
     * 例如设置文件描述符为非阻塞模式、获取文件描述符的状态标志、调整文件描述符的读写指针等等。
     * fcntl通常需要指定一个操作命令和一些相关的参数。常用的操作命令有：
     *      F_GETFL：获取文件描述符的状态标志。
     *      F_SETFL：设置文件描述符的状态标志。
     *      F_GETFD：获取文件描述符的文件描述符标志。
     *      F_SETFD：设置文件描述符的文件描述符标志。
     * 除了上述命令，fcntl还可以用于获取和设置文件描述符的锁定状态、获取和设置文件描述符的异步I/O处理模式、获取和设置文件描述符的信号驱动I/O模式等等。
     * 需要注意的是，fcntl系统调用是一个较为底层的系统调用，一般不直接用于应用程序中，而是作为其他高级API的基础，
     * 例如POSIX标准中的fcntl函数和C标准库中的flock函数等等。
     * */
    int flag = fcntl(fd, F_GETFL, 0);
    if (-1 == flag) {
        return;
    }

    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
