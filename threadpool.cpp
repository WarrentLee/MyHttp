#include "threadpool.h"
#include <iostream>

threadpool_t *threadpool_create(int thread_count, int queue_size, int flags)
{
    threadpool_t *pool;
    do                                      //do...while(false)语句可以使用break简化多级判断
    {
        if(thread_count <= 0 || thread_count > MAX_THREADS || 
            queue_size <= 0 || queue_size > MAX_QUEUE)
            return NULL;
        
        //为线程池分配空间
        if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL)
            break;
        
        //初始化线程池
        pool->thread_count = 0;
        pool->queue_size = queue_size;
        pool->head = pool->tail = pool->count = pool->started = 0;
        pool->shutdown  = 0;

        //为执行队列和任务队列分配空间
        pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
        pool->queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t) * queue_size);

        //初始化互斥锁和条件变量
        if( (pthread_mutex_init( &(pool->lock), NULL) != 0) || 
               (pthread_cond_init( &(pool->notify), NULL ) != 0) ||
               (pool->threads == NULL) || (pool->queue == NULL))
               break;

        //开启工作线程worker
        for(int i = 0; i < thread_count; i++)
        {
            if(pthread_create( &(pool->threads[i]), NULL, threadpool_thread, (void*)pool) != 0)
            {
                threadpool_destroy(pool, 0);
                return NULL;
            }
            pool->thread_count ++;
            pool->started ++;
        }
        return pool;
    } while (false);
    
    //创建失败，释放线程池
    if( pool != NULL)
    {
        threadpool_free(pool);
    }
    return NULL;
    }

int threadpool_add(threadpool_t *pool, void (*function)(void *), void *argument, int flags)
{
    int err = 0;
    int next;

    //线程池或回调函数无效
    if(pool == NULL || function == NULL)
        return THREADPOOL_INVALID;
    //上锁失败
    if(pthread_mutex_lock(&(pool->lock)) != 0)
        return THREADPOOL_LOCK_FAILURE;
    
    next = (pool->tail + 1) % pool->queue_size;
    do{
        //等待任务数达到队列容量
        if(pool->count == pool->queue_size)
        {
            err = THREADPOOL_QUEUE_FULL;
            break;
        }
        //线程池已关闭
        if(pool->shutdown)
        {
            err = THREADPOOL_SHUTDOWN;
            break;
        }
        //添加任务到队列尾部
        pool->queue[pool->tail].function = function;
        pool->queue[pool->tail].argument = argument;
        pool->tail = next;
        pool->count++;

        //使用条件变量通知工作线程
        std::cout << "唤醒线程" << std::endl;
        if(pthread_cond_signal( &(pool->notify)) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }
    }while(false);
    if(pthread_mutex_unlock( &(pool->lock)) != 0)
    {
        err = THREADPOOL_LOCK_FAILURE;
    } 
    return err;
}

int threadpool_destroy(threadpool_t *pool, int flags)
{
    int err = 0;

    //线程池无效
    if(pool == NULL)
        return THREADPOOL_INVALID;
    //上锁失败
    if(pthread_mutex_lock( &(pool->lock)) != 0)
        return THREADPOOL_LOCK_FAILURE;
    
    do{
        //线程池已关闭
        if(pool->shutdown)
        {
            err = THREADPOOL_SHUTDOWN;
            break;
        }
        pool->shutdown = (flags & THREADPOOL_GRACEFUL) ?
                                        graceful_shutdown : immediate_shutdown;

        //唤醒所有线程
        if(pthread_cond_broadcast( &(pool->notify)) != 0 ||
        pthread_mutex_unlock( &(pool->lock)) != 0)
        {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }

        //加入所有线程至主线程
        //join函数会阻塞等待线程结束并回收线程资源，第二个参数接收线程返回值
        //create线程时默认是jointable，可指定为detach，此时线程自回收
        for(int i = 0; i < pool->thread_count; i++)
        {
            if (pthread_join(pool->threads[i], NULL) != 0)
                err = THREADPOOL_THREAD_FAILURE;
        }
    }while(false);

    //没有错误时才能释放线程池
    if( !err )
    {
        threadpool_free(pool);
    }
    return err;
}

int threadpool_free(threadpool_t *pool)
{
    if(pool == NULL || pool->started >0)
        return THREADPOOL_INVALID;
    
    //由于初始化线程池时是我们分配并初始化threads和queue
    //释放线程池时也需要我们释放threads和queue的内存
    if(pool->threads)
    {
        free(pool->threads);
        free(pool->queue);

        //销毁互斥锁和条件变量前先上锁，以防别人正在使用
        pthread_mutex_lock( &(pool->lock));
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
    }
    free(pool);
    return 0;
}


//静态函数只在当前文件可见
static void* threadpool_thread(void *threadpool)
{
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;

    //while(1)和for(;;)都是无限循环
    //while(1)会对1作判断后再跳转
    //for(;;)空语句不需要判断，直接跳转，不占用寄存器
    for(;;)
    {
        //等待条件变量之前必须先上锁
        pthread_mutex_lock( &(pool->lock));

        while( (pool->count == 0) && (!pool->shutdown))
        {
            std::cout << "线程休眠" << std::endl;
            pthread_cond_wait( &(pool->notify), &(pool->lock));
        }

        if((pool->shutdown == immediate_shutdown) || 
        ( (pool->shutdown == graceful_shutdown) && (pool->count == 0)) )
        {
            break;
        }

        //取出任务
        std::cout << "取出任务" << std::endl;
        task.function = pool->queue[pool->head].function;
        task.argument = pool->queue[pool->head].argument;
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;

        //解锁
        pthread_mutex_unlock( &(pool->lock));

        //执行任务
        (*(task.function)) (task.argument);

        
        pool->started--;
        pthread_mutex_unlock( &(pool->lock));
    }
    //线程池关闭时退出线程
    //exit主动退出，参数为线程返回值，资源由主线程join函数回收
    pthread_exit(NULL);
    return NULL;
}