#include "requestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "util.h"

#include <sys/epoll.h>
#include <queue>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <unistd.h>

using namespace std;

const int THREADPOOL_THREAD_NUM = 4;
const int QUEUE_SIZE = 65535;

const int PORT = 8888;
const int ASK_STATIC_FILE = 1;
const int ASK_IMAGE_STITCH = 2;

const string PATH = "/";

const int TIMER_TIME_OUT = 500;

extern pthread_mutex_t qlock;
extern struct epoll_event *events;
void acceptConnection(int listen_fd, int epoll_fd, const string &path);

extern priority_queue<mytimer*, deque<mytimer*>, timerCmp> myTimerQueue;

int socket_bind_listen(int port)
{
    if(port < 1024 || port > 65535)
        return -1;

    int listen_fd = 0;
    if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;
    
    //设置地址复用，避免bind时 "Address already in use"
    int optval = 1;
    if(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
        return -1;
    
    //设置服务器IP和Port
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short) port);
    if(bind(listen_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1)
        return -1;

    //设置最长等待队列并监听
    if(listen(listen_fd, LISTENQ) == -1)
        return -1;

    //无效的监听描述符
    if(listen_fd == -1)
    {
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

//线程入口函数
void myHandler(void *args)
{
    requestData *req_data = (requestData*)args;
    req_data ->handleRequest();
}

void acceptConnection(int listen_fd, int epoll_fd, const string &path)
{
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = sizeof(client_addr);
    int accept_fd = 0;
    while((accept_fd = accept(listen_fd, (struct sockaddr*) &client_addr, &client_addr_len)) > 0)
    {
        //设置非阻塞模式
        int ret = setSocketNonBlocking(accept_fd);
        if(ret < 0)
        {
            perror("Set non block failed");
            return;
        }
        cout << "新连接fd:" << accept_fd << endl;
        //为客户端连接构造一个requestData实例
        requestData *req_info = new requestData(epoll_fd, accept_fd, path);

        //将客户端连接注册到epoll内核事件表
        __uint32_t epoll_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        epoll_add(epoll_fd, accept_fd, static_cast<void *> (req_info), epoll_event);

        //新增时间信息
        mytimer *mtimer = new mytimer(req_info, TIMER_TIME_OUT);
        req_info -> addTimer(mtimer);
        pthread_mutex_lock(&qlock);
        myTimerQueue.push(mtimer);
        pthread_mutex_unlock(&qlock);
    }
}

//分发处理函数
void handle_events(int epoll_fd, int listen_fd, struct epoll_event *events, int events_num, const string &path, threadpool_t *tp)
{
    for(int i = 0; i < events_num; i++)
    {
        //从就绪事件列表中提取requestData，再获取文件描述符
        requestData *request = (requestData *)(events[i].data.ptr);
        int fd = request -> getFd();

        //如果是监听描述符就绪，接受新连接并注册到epoll内核事件表
        if(fd == listen_fd)
        {
            cout << "接受连接" << endl;
            acceptConnection(listen_fd, epoll_fd, path);
        }
        else 
        {
            //排除错误事件
            if((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || !(events[i].events & EPOLLIN))
            {
                perror("error event");
                delete request;
                continue;
            }

            //将请求任务加入线程池
            //加入之前将Timer和request分离
            //分离时清空Timer的requestData并设置删除标志
            cout << "将请求加入线程池" << endl;
            request -> seperateTimer();
            int deal = threadpool_add(tp, myHandler, events[i].data.ptr, 0);
        }
    }
}

//由于优先队列不支持随机访问，这里只处理队首的过期事件
//当一个请求被处理时，对应的定时器被分离并设置删除标志
//被设置删除标志的定时器最迟会在超时(TIMER_TIME_OUT)后被删除
//因为此时优先队列中该定时器前方的定时器必定已经被删除或者超时
//如果该定时器超时之前，队列前方的定时器都已经被删除，则可以提前删除
void handle_expired_event()
{
    pthread_mutex_lock(&qlock);
    while(!myTimerQueue.empty())
    {
        mytimer *timer_front = myTimerQueue.top();
        if(timer_front -> isDeleted())
        {
            
            myTimerQueue.pop();
            delete timer_front;
            //cout << "删除计时器" << endl;
        }
        else if( !(timer_front -> isvalid()) )
        {
            
            myTimerQueue.pop();
            delete timer_front;
        }
        else 
            break;
    }
    pthread_mutex_unlock(&qlock);
}

int main()
{
    handle_for_sigpipe();
    int epoll_fd = epoll_init();
    if(epoll_fd < 0)
    {
        perror("epoll init failed");
        return 1;
    }
    //线程池创建函数最后一个参数flags用于优雅关闭
    threadpool_t *threadpool = threadpool_create(THREADPOOL_THREAD_NUM, QUEUE_SIZE, 0);
    
    cout << "监听端口:" << PORT << endl;
    int listen_fd = socket_bind_listen(PORT);
    if(listen_fd < 0)
    {
        perror("socket bind failed");
        return 1;
    }
    if(setSocketNonBlocking(listen_fd) < 0)
    {
        perror("set socket non block failed");
        return 1;
    }

    __uint32_t event = EPOLLIN | EPOLLET;
    requestData *req = new requestData();
    req->setFd(listen_fd);
    epoll_add(epoll_fd, listen_fd, static_cast<void *> (req), event);
    while(true)
    {
        //cout << "等待事件就绪" << endl;
        int events_num = my_epoll_wait(epoll_fd, events, MAXEVENTS, -1);
        if(events_num == 0)
            continue;
        
        handle_events(epoll_fd, listen_fd, events, events_num, PATH, threadpool);
        handle_expired_event();
    }
    return 0;
}