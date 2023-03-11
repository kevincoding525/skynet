#ifndef skynet_daemon_h
#define skynet_daemon_h

/*
 * skynet可以以守护进程的形式启动，守护进程可以在服务器启动的时候自举，并且没有控制台终端
 * */
// skynet 以守护进程模式进行初始化
int daemon_init(const char *pidfile);

// skynet 以守护进程的模式退出
int daemon_exit(const char *pidfile);

#endif
