#ifndef poll_socket_epoll_h
#define poll_socket_epoll_h

#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/*
 * skynet 网络模块 - 对epoll网络复用模型的封装（epoll主要用在linux系统上）
 * */
static bool
sp_invalid(int efd) {
    return efd == -1;
}

// 创建一个epoll对象 返回文件描述符
static int
sp_create() {
    /*
     * epoll_create是Linux中epoll机制的一个系统调用，用于创建一个epoll实例。
     * epoll是一种高效的I/O多路复用机制，可以监视多个文件描述符的事件，并在事件发生时通知应用程序。
     * epoll_create系统调用需要指定一个整数作为参数，表示创建的epoll实例的大小。
     * 这个参数在Linux 2.6.8之前是无用的，可以设置为任意值。在Linux 2.6.8及之后的版本中，
     * 这个参数表示epoll实例中最多能监视的文件描述符数量。
     * epoll_create系统调用返回一个整数，表示创建的epoll实例的文件描述符。
     * 应用程序可以使用这个文件描述符向epoll实例中添加、修改或删除文件描述符的事件，
     * 并使用epoll_wait系统调用等待事件的发生。
     */
    return epoll_create(1024);
}

// 释放IO文件描述符
static void
sp_release(int efd) {
    close(efd);
}

// 添加一个socket的网络监听
static int
sp_add(int efd, int sock, void *ud) {
    struct epoll_event ev;
    ev.events = EPOLLIN; // 设置可读事件
    ev.data.ptr = ud;
    /*
     * epoll_ctl是Linux中epoll机制的一个系统调用，用于向epoll实例中添加、修改或删除一个文件描述符的事件。
     * epoll_ctl可以用于监视多个文件描述符的事件，以便应用程序能够及时响应事件并进行相应的处理。
     * epoll_ctl系统调用需要指定一个epoll实例的文件描述符、一个操作命令、一个文件描述符和一个指向epoll_event结构体的指针。
     * 常用的操作命令有：
     *      EPOLL_CTL_ADD：将一个文件描述符添加到epoll实例中。
     *      EPOLL_CTL_MOD：修改一个已经添加到epoll实例中的文件描述符的事件。
     *      EPOLL_CTL_DEL：从epoll实例中删除一个文件描述符。
     * 在使用epoll_ctl向epoll实例中添加一个文件描述符时，需要指定该文件描述符的事件类型和相关的标志。
     * 例如，EPOLLIN标志表示文件描述符可读，EPOLLOUT标志表示文件描述符可写，EPOLLET标志表示使用边缘触发模式等等。
     * */
    if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) == -1) {
        return 1;
    }
    return 0;
}

// 移除一个socket的网络监听
static void
sp_del(int efd, int sock) {
    epoll_ctl(efd, EPOLL_CTL_DEL, sock, NULL);
}

// 设置socket的网络事件监听状态
static int
sp_enable(int efd, int sock, void *ud, bool read_enable, bool write_enable) {
    struct epoll_event ev;
    ev.events = (read_enable ? EPOLLIN : 0) | (write_enable ? EPOLLOUT : 0);
    ev.data.ptr = ud;
    if (epoll_ctl(efd, EPOLL_CTL_MOD, sock, &ev) == -1) {
        return 1;
    }
    return 0;
}

// 获取可相应的网络事件
static int
sp_wait(int efd, struct event *e, int max) {
    struct epoll_event ev[max];
    /*
     * epoll_wait是Linux中epoll机制的一个系统调用，用于等待文件描述符上的事件发生。
     * epoll_wait可以用于监视多个文件描述符的事件，并在事件发生时通知应用程序。
     * epoll_wait系统调用需要指定一个epoll实例的文件描述符、一个指向epoll_event结构体的指针和一个整数作为参数。
     * 这个整数表示epoll_event结构体数组的大小，即最多能返回的事件数量。
     * 当一个被监视的文件描述符上发生一个事件时，epoll_wait将会阻塞，直到至少一个事件发生或者超时。
     * 如果超时时间为0，则epoll_wait将立即返回，不会阻塞。如果超时时间为负数，则epoll_wait将永远等待，
     * 直到至少一个事件发生。
     * epoll_wait系统调用返回一个整数，表示发生事件的文件描述符数量。在epoll_event结构体数组中，
     * 可以通过events[i].data.fd获取文件描述符，通过events[i].events获取事件类型和相关的标志。
     * */
    int n = epoll_wait(efd, ev, max, -1);
    int i;
    for (i = 0; i < n; i++) {
        e[i].s = ev[i].data.ptr;
        unsigned flag = ev[i].events;
        e[i].write = (flag & EPOLLOUT) != 0;
        e[i].read = (flag & EPOLLIN) != 0;
        e[i].error = (flag & EPOLLERR) != 0;
        e[i].eof = (flag & EPOLLHUP) != 0;
    }

    return n;
}

// 设置文件描述符为非阻塞
static void
sp_nonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    if (-1 == flag) {
        return;
    }

    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

#endif
