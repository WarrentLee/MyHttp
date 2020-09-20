#include "util.h"
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <iostream>

   //重新封装read函数，循环读取数据
   //size_t是与机器有关的unsigned类型，大小保证足以存储对象，即long unsigned int，64位系统中为8字节，32位系统中为为4字节
   //ssize_t是有符号的size_t，即long int
   //从fd指向的文件读取总计n个字节到buff指向的内存缓冲区
ssize_t readn(int fd, void *buff, size_t n)
{
    size_t nleft = n;                       //剩余未读字节数
    ssize_t nread = 0;                  //当次读取字节数
    ssize_t readSum = 0;            //总计读取字节数
    char *ptr = (char*)buff;       //内存写入位置
    while(nleft > 0)
    {
        if((nread = read(fd, ptr, nleft)) < 0)           //读取失败
        {
            perror("readn error");
            if(errno == EINTR)                  //被中断系统调用， 阻塞于慢系统调用的函数捕获到信号，信号处理函数中断当前系统调用
                nread = 0;
            else if(errno == EAGAIN)        //重试，非阻塞模式下资源暂时不可用，可能是内核缓冲区已满，或文件被占用，或接收缓冲区为空
                {
                    perror("resource unavailible");
                    return readSum;
                }
            else
            { 
                return -1;                             //其他错误
            }
        }
        else if (nread == 0)                        //到达文件末尾，或socket对端关闭
            break;
        readSum += nread;
        nleft -= nread;
        ptr += nread;
    }
    return readSum;
}


//重新封装write函数，循环写入数据
ssize_t writen(int fd, void *buff,  size_t n)
{
    size_t nleft = n;                                   //剩余未写字节数
    ssize_t nwritten = 0;                         //当次写入字节数
    ssize_t writeSum = 0;                      //总计写入字节数
    char *ptr = (char*)buff;                  //内存读取位置
    while(nleft > 0)
    {
        //write返回0可能是指定写入0字节
        if((nwritten = write(fd, ptr, nleft)) < 0)
        {       
            //EPIPI：fd是一个pipe或socket，且对端的读端关闭，一般写进程会收到SIGPIPE信号
            if(errno == EINTR || errno == EAGAIN)
            {
                nwritten = 0;
                continue;
            }
            else return -1;
        }
        writeSum += nwritten;
        nleft -= nwritten;
        ptr += nwritten;
    }
    return writeSum;
}

int setSocketNonBlocking(int fd)
{
    //flags以位为单位，get后与NONBLOCK相或，再set，增加非阻塞标志位

    //获取文件的flags，即open函数的第二个参数
    int flag = fcntl(fd, F_GETFL, 0);
    if(flag == -1)
        return -1;
    
    flag |= O_NONBLOCK;
    //设置文件的flags
    if(fcntl(fd, F_SETFL, flag) == -1)
        return -1;
    return 0;
}

//客户端单向关闭后收到服务端发送的数据，会产生RST
//服务端收到RST后，第二次调用write函数会产生SIGPIPE信号
//SIGPIPE信号的默认处理动作是terminate，会导致进程退出
//设置SIG_IGN后将SIGPIPE交给系统处理，忽略这个信号
void handle_for_sigpipe()
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if(sigaction(SIGPIPE, &sa, NULL))
        return;
}