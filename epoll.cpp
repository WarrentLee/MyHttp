#include "epoll.h"
#include <sys/epoll.h>
#include <errno.h>
//#include "threadpool.h"

//就绪事件数组，结构内包含事件类型(32位整型数)和用户数据(联合体)
//epoll_wait检测到事件时，将所有就绪事件从内核事件表复制到它第二个参数events所指定的事件数组中
//此数组只用于接收epoll_wait检测到的就绪事件
struct epoll_event *events;

//初始化描述符
int epoll_init()
{
    //创建一个epoll文件描述符，唯一标识内核中的一个事件表，参数表明需要的事件表大小
    int epoll_fd = epoll_create(LISTENQ + 1);
    if(epoll_fd == -1)
        return -1;
    events = new epoll_event[MAXEVENTS];
    return epoll_fd;
}

//注册新描述符
int epoll_add(int epoll_fd, int fd, void *request, __uint32_t events)
{
    struct epoll_event event;
    event.data.ptr = request;
    event.events = events;

    //内核事件表操作函数，操作类型有3种：ADD、MOD、DEL
    //event参数指定事件类型，epoll支持两个额外的事件类型：EPOLLET和EPOLLONESHOT
    if( epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)
    {
        //打印错误到标准输出
        perror("epoll_add error");
        return -1;
    }
    return 0;
}

//修改描述符状态
int epoll_mod(int epoll_fd, int fd, void *request, __uint32_t events)
{
    struct epoll_event event;
    event.data.ptr = request;
    event.events = events;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0)
    {
        perror("epoll_mod error");
        return -1;
    }
    return 0;
}

//从内核事件表中删除描述符
int epoll_del(int epoll_fd, int fd, void *request, __uint32_t events)
{
    struct epoll_event event;
    event.data.ptr = request;
    event.events = events;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &event) < 0)
    {
        perror("epoll_del error");
        return -1;
    }
    return 0;
}

//返回就绪事件
int my_epoll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout)
{
    //max_events指明事件数组大小，不能超过epoll_create时的事件表大小
    int ret_count = epoll_wait(epoll_fd, events, max_events, timeout);
    if(ret_count < 0)
    {
        perror("epoll_wait error");
    }
    return ret_count;
}


    //EPOLLONESHOT可以避免多个线程同时操作一个socket连接，系统只能触发一次可读 可写或异常事件
    //一个工作线程处理完某个socket上的一次请求后，该socket上还有新的客户请求，则该线程继续为这个socket服务
    //如果处理完请求后没有接收到新的客户请求，重置该socket上的ONESHOT事件，使epoll能再次检测到该socket上的EPOLLIN事件，其他线程有机会为该socket服务
    //同一时间只有一个线程为一个socket连接服务，保证了连接的完整性，避免了很多可能的竞态条件
