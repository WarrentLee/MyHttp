#ifndef THREADPOOL
#define THREADPOOL
#include <pthread.h>
#include<stdlib.h>

//定义线程池状态码
const int THREADPOOL_INVALID = -1;                                  //无效
const int THREADPOOL_LOCK_FAILURE = -2;                   //上锁失败
const int THREADPOOL_QUEUE_FULL = -3;                       //任务队列已满
const int THREADPOOL_SHUTDOWN = -4;                         //关闭
const int THREADPOOL_THREAD_FAILURE = -5;              //线程失败

const int THREADPOOL_GRACEFUL = 1;             //允许优雅关闭
const int MAX_THREADS = 1024;                           //执行队列容量(最大线程数)
const int MAX_QUEUE = 65535;                            //任务队列容量

//定义线程池关闭方式的枚举类型
typedef enum
{
    immediate_shutdown = 1,             //立即关闭
    graceful_shutdown = 2                    //优雅关闭
}threadpool_shutdown_t;

//定义任务结构
typedef struct 
{
    void(*function)(void*);                     //任务函数
    void* argument;                                  //函数参数
}threadpool_task_t;

//定义线程池结构
typedef struct
{
    pthread_mutex_t lock;                       //互斥锁
    pthread_cond_t notify;                      //用于通知线程的条件变量
    pthread_t *threads;                             //执行队列数组，存线程ID
    threadpool_task_t *queue;              //任务队列数组
    int thread_count;                                  //执行队列容量(总线程数)
    int queue_size;                                      //任务队列容量
    int head;                                                   //待执行任务头部(含任务)
    int tail;                                                       //待执行任务尾部(不含任务)
    int count;                                                  //等待任务数
    int started;                                               //已开启线程数
    int shutdown;                                         //线程池关闭标志
}threadpool_t;

//封装线程池函数
threadpool_t* threadpool_create(int thread_count, int queue_size, int flags);
int threadpool_add(threadpool_t* pool, void (*function)(void*), void* argument, int flags);
int threadpool_destroy(threadpool_t* pool, int flags);
int threadpool_free(threadpool_t* pool);  
static void* threadpool_thread(void* threadpool);                                                                                                   //新线程回调函数

#endif