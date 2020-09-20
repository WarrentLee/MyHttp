#include"requestData.h"
#include "epoll.h"
#include "util.h"

#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <queue>
#include <unordered_map>
#include <string.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/opencv.hpp>
using namespace cv;

#include <iostream>
using namespace std;

pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
//静态成员初始化
pthread_mutex_t MimeType::lock = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<string, string>  MimeType::mime;

//根据文件后缀添加Mime类型
std::string MimeType::getMime(const std::string &suffix)
{
    if(mime.size() == 0)
    {
        pthread_mutex_lock(&lock);
        if(mime.size() == 0)
        {
            //定义文件后缀和Mime类型之间的对应关系
            //后缀：大类别/具体种类
            mime[".html"] = "text/html";
            mime[".htm"] = "text/html";
            mime[".txt"] = "text/plain";
            mime[".c"] = "text/plain";

            mime[".jpg"] = "image/jpeg";
            mime[".jpeg"] = "image/jpeg";
            mime[".png"] = "image/png";
            mime[".bmp"] = "image/bmp";
            mime[".gif"] = "image/gif";

            mime[".avi"] = "video/x-msvideo";
            mime[".mp3"] = "audio/mp3";

            mime[".doc"] = "application/msword";
            mime[".gz"] = "application/x-gzip";
            mime[".ico"] = "application/x-ico";
            mime[".json"] = "application/json";

            mime["default"] = "text/html";
        }
        pthread_mutex_unlock(&lock);
    }
    if(mime.find(suffix) == mime.end())
        return mime["default"];
    else 
        return mime[suffix];
}

//优先队列<数据类型，容器，比较(仿)函数>
//按排序后的优先级出队列
priority_queue<mytimer*, deque<mytimer*>, timerCmp> myTimerQueue;

//请求数据默认构造函数
//初始化读指针到开头位置，进入解析URI状态，头部状态为开头
//默认关闭长连接，不自动重试，定时器为空
requestData::requestData():
    now_read_pos(0),state(STATE_PARSE_URI), h_state(h_start),
    keep_alive(false), againTimes(0), timer(NULL)
{
    std::cout << "requestedData constructed!" << endl;
}

requestData::requestData(int _epollfd, int _fd, string _path):
now_read_pos(0), state(STATE_PARSE_URI), h_state(h_start),
keep_alive(false), againTimes(0), timer(NULL),
path(_path), fd(_fd), epollfd(_epollfd)
{
    std::cout << "requestedData constructed!" << endl;
}

requestData::~requestData()
{
    struct epoll_event ev;
    //超时的一定是读请求
    ev.events = EPOLLIN|EPOLLET|EPOLLONESHOT;
    ev.data.ptr = (void*)this;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
    if(timer != NULL)
    {
        timer->clearReq();
        timer = NULL;
    }
    close(fd);
}


int requestData::getFd()
{
    return fd;
}

void requestData::setFd(int _fd)
{
    fd = _fd;
}

//重置为初始状态，等待新请求
void requestData::reset()
{
    againTimes = 0;
    content.clear();
    file_name.clear();
    path.clear();
    now_read_pos = 0;
    state = STATE_PARSE_URI;
    h_state = h_start;
    headers.clear();
    keep_alive = false;
}

//增加定时器
void requestData::addTimer(mytimer *mtimer)
{
    if(timer == NULL)
        timer = mtimer;
}

//分离定时器
void requestData::seperateTimer()
{
    if(timer)
    {
        timer->clearReq();
        timer = NULL;
    }
}

//处理请求数据
void requestData::handleRequest()
{
    char buff[MAX_BUFF];
    bool isError = false;
    //无限循环+break方便找到跳出地点，continue继续读取数据
    while(true)
    {
        //1.读取失败    2.读到文件末尾      3.读取长度达到MAX_BUFF
        int read_num = readn(fd, buff, MAX_BUFF);
        if(read_num < 0)
        {
            perror("read_fail");
            isError = true;
            break;
        }
        //有请求但是读不到数据，可能是请求中止，或数据没能到达
        else if(read_num == 0)
        {
            perror("read_num == 0");
            //可能是数据未到达，接收缓冲区为空，重试
            if(errno == EAGAIN)
            {
                //重试次数已满
                if(againTimes > AGAIN_MAX_TIMES)
                    {
                        std::cout << "重试次数已满" << endl;
                        isError = true;
                    }
                else    
                    againTimes++;
            }
            //其他错误
            else if(errno != 0)
                isError = true;
            break;
        }
        //使用char*初始化string，左闭右开
        string now_read(buff, buff + read_num);
        content += now_read;

        if(state == STATE_PARSE_URI)                //解析URI状态
        {
            std::cout << "解析URI" << endl;
            int flag = this->parse_URI();
            //数据读取不完整时，重新解析
            if(flag == PARSE_URI_AGAIN)
                break;
            else if(flag == PARSE_URI_ERROR)
            {
                perror("2");
                isError = true;
                break;
            }
        }
        if(state == STATE_PARSE_HEADERS)        //解析头部状态
        {
            int flag = this->parse_Headers();
            if(flag == PARSE_HEADER_AGAIN)
            {
                break;
            }
            else if(flag == PARSE_HEADER_ERROR)
            {
                perror("3");
                isError = true;
                break;
            }
            if(method == METHOD_POST)           //头部解析出POST方法
            {
                state = STATE_RECV_BODY;
            }
            else
            {
                state = STATE_ANALYSIS;
            }
        }
        if(state == STATE_RECV_BODY)
        {
            int content_length = -1;
            if(headers.find("Content-Length") != headers.end())
            {
                content_length = stoi(headers["Content-Length"]);
            }
            else
            {
                isError = true;
                std::cout << "STATE_RECV_BODY error"<< endl;
                break;
            }
            //读取消息实体
            //content_length为消息实体长度
            if(content.size() < content_length)
                continue;
            state = STATE_ANALYSIS;
        }
        if(state == STATE_ANALYSIS)
        {
            int flag = this->analysisRequest();
            if(flag < 0)
            {
                isError = true;
                break;
            }
            else if (flag == ANALYSIS_SUCCESS)
            {
                state = STATE_FINISH;
                 break;
            }
            else 
            {
                isError = true;
                break;
            }
        }
    }

    //发生错误，释放这个requestData实例
    if(isError)
    {
        std::cout << "handle request error"<< endl;
        delete this;
        return;
    }

    //处理完一个请求，检查是不是长连接
    if(state == STATE_FINISH)
    {
        printf("finish\n");
        if(keep_alive)
        {
            printf("ok\n");
            this->reset();
        }
        else
        {
            printf("~requestData\n");
            delete this;
            return;
        }
    }
    //新增时间信息
    pthread_mutex_lock(&qlock);
    mytimer *mtimer = new mytimer(this, 500);
    timer = mtimer;
    myTimerQueue.push(mtimer);
    pthread_mutex_unlock(&qlock);

    //执行完handleRequest()后线程退出
    //重置ONESHOT，让新线程处理新请求
    __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
    int ret = epoll_mod(epollfd, fd, static_cast<void*> (this), _epo_event);
    if(ret < 0)
    {
        delete this;
        return;
    }
}

int requestData::parse_URI()
{
    string &str = content;
    //从当前位置往后查找回车符，读取完整的请求行再开始解析
    int pos = str.find('\r', now_read_pos);
    if(pos < 0)
        return PARSE_URI_AGAIN;

    //提取请求行
    string request_line = str.substr(0, pos);
    if(str.size() > pos + 1)
    {
        str = str.substr(pos + 1);
    }
    else str.clear();
    //提取get或post
    pos = request_line.find("GET");
    if(pos < 0)
    {
        pos = request_line.find("POST");
        if(pos < 0)
            return PARSE_URI_ERROR;
        else
            method = METHOD_POST;
    }
    else
        method = METHOD_GET;
    pos = request_line.find("/", pos);
    if(pos < 0)
        return PARSE_URI_ERROR;
    else
    {
        //提取URI，即method之后的'/'到' '字段
       int _pos = request_line.find(' ', pos);
       if(_pos < 0)
            return PARSE_URI_ERROR;
        else
        {
            if(_pos - pos > 1)
            {
                file_name = request_line.substr(pos + 1, _pos - pos -1);
                //将file_name的'?'字符之后的字段去除，即去除URI后面的参数
                int __pos = file_name.find("?");
                if(__pos >= 0)
                    file_name = file_name.substr(0, __pos);
            }
            //URI为根路径"/"，默认返回index.html
            else    file_name = "index.html";
        }
        pos = _pos;
    }

    //提取HTTP版本号
    pos = request_line.find("/", pos);
    if(pos < 0)
        return PARSE_URI_ERROR;
    else
    {
        //版本号需要3个字符
        if(request_line.size() - pos <= 3)
            return PARSE_URI_ERROR;
        else 
        {
            string ver = request_line.substr(pos + 1, 3);
            if(ver == "1.0")
                HTTPversion = HTTP_10;
            else if(ver == "1.1")
                HTTPversion = HTTP_11;
            else 
                return PARSE_URI_ERROR;
        }
    }
    state = STATE_PARSE_HEADERS;
    return PARSE_URI_SUCCESS;
}

int requestData::parse_Headers()
{
    string &str = content;
    int key_start = -1, key_end = -1, value_start = -1, value_end = -1;
    int now_read_line_begin = 0;
    bool notFinish = true;
    for(int i = 0; i < str.size() && notFinish; i++)
    {
        switch (h_state)
        {
        case  h_start:
           {
               //跳过开头的回车符、换行符
               if(str[i] == '\r' || str[i] == '\n')
                    break;
                h_state = h_key;
                key_start = i;
                now_read_line_begin = i;
                break;
           }
           case h_key:
           {
                if(str[i] == ':')
                {
                    key_end = i;
                    if(key_end - key_start <= 0 )
                        return PARSE_HEADER_ERROR;
                    h_state = h_colon;
                }
                //首部行找不到':'
                else if(str[i] == '\r' || str[i] == '\n')
                    return PARSE_HEADER_ERROR;
                break;
           }
           case h_colon:
           {
               if(str [i] == ' ')
               {
                   h_state = h_spaces_after_colon;
               }
               else     
                    return PARSE_HEADER_ERROR;
                break;
           }
           case h_spaces_after_colon:
           {
               h_state = h_value;
               value_start = i;
               break;
           }
           //首部行键值对之间用'\r', '\n'隔开
           case h_value:
           {
               if(str[i] == '\r')
               {
                   h_state = h_CR;
                   value_end = i;
                   if(value_end - value_start <= 0)
                        return PARSE_HEADER_ERROR;
               }
               //规定值的最大长度为255
               else if (i - value_start > 255)
                    return PARSE_HEADER_ERROR;
                break;
           }
           case h_CR:
           {
               if(str[i] == '\n')
               {
                   h_state = h_LF;
                   //提取键值对，更新行首
                   string key(str.begin() + key_start, str.begin() + key_end);
                   string value(str.begin() + value_start, str.begin() + value_end);
                   headers[key] = value;
                   //std::cout << key << ':' << value << endl;
                   now_read_line_begin = i;
               }
               else 
                    return PARSE_HEADER_ERROR;
                break;
           }
           //首部行结束的空行
           case h_LF:
           {
               if(str[i] == '\r')
                    h_state = h_end_CR;
                else 
                {
                    key_start = i;
                    h_state = h_key;
                }
                break;
           }
           case h_end_CR:
           {
               if(str[i] == '\n')
                    h_state = h_end_LF;
                else 
                    return PARSE_HEADER_ERROR;
                break;
           }
           case h_end_LF:
           {
               notFinish = false;
               key_start = i;
               now_read_line_begin = i;
               break;
           }
        }
    }
    if(h_state == h_end_LF)
    {
        str = str.substr(now_read_line_begin);
        return PARSE_HEADER_SUCCESS;
    }
    //解析不完整
    str = str.substr(now_read_line_begin);
        return PARSE_HEADER_AGAIN;
}

/*业务代码在这里*/
int requestData::analysisRequest()
{
    char header[MAX_BUFF];
    //将格式化字符串写入缓冲区
    sprintf(header, "HTTP/1.1 %d %s\r\n", 200, "OK");
    //检查是否为长连接
    if(headers.find("Connection") != headers.end() && headers["Connection"] == "keep-alive")
    {
        //长连接有效时间为EPOLL的等待时间
        keep_alive = true;
        sprintf(header, "%sConnection: keep-alive\r\n", header);
        sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
    }

    if(method == METHOD_POST)
    {
        char *send_content = "I have received this.";
        //%zu表示size_t，即long unsigned int
        sprintf(header, "%sContent-length: %zu\r\n", header, strlen(send_content));
        sprintf(header, "%s\r\n", header);
        //发送头部
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if(send_len != strlen(header))
        {
            perror("Send header failed");
            return ANALYSIS_ERROR;
        }
        //发送主体
        send_len = (size_t)writen(fd, send_content, strlen(send_content));
        if(send_len != strlen(send_content))
        {
            perror("Send content failed");
            return ANALYSIS_ERROR;
        }
        //测试保存图片
        std::cout << "content size = " << content.size() << endl;
        vector<char> data(content.begin(), content.end());
        std::cout << content << endl;
        //Mat test = imdecode(data, IMREAD_ANYDEPTH|IMREAD_ANYCOLOR);
        //cv::imwrite("received.bmp", test);
        return ANALYSIS_SUCCESS;
    }

    else if(method == METHOD_GET)
    {
       int dot_pos = file_name.find('.');
       const char *filetype;
       if(dot_pos < 0)
            filetype = MimeType::getMime("default").c_str();
        else
            filetype = MimeType::getMime(file_name.substr(dot_pos)).c_str();
        //stat函数取得指定文件的文件属性，存储在结构体stat中
        struct stat sbuf;
        std::cout << file_name << endl;
        if(stat(file_name.c_str(), &sbuf) < 0)
        {
            handleError(fd, 404, "No Found!");
            return ANALYSIS_ERROR;
        }
        std::cout << "found" << endl;
        sprintf(header, "%sContent-type: %s\r\n", header, filetype);
        sprintf(header, "%sContent-length: %ld\r\n", header, sbuf.st_size);
        sprintf(header, "%s\r\n", header);
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if(send_len != strlen(header))
        {
            perror("Send header failed");
            return ANALYSIS_ERROR;
        }
        //打开文件并映射到内存，避免磁盘I/O
        int src_fd = open(file_name.c_str(), O_RDONLY, 0);
        char *src_addr = static_cast<char*>(mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, src_fd, 0));
        close(src_fd);

        //发送文件并检验完整性
        send_len = writen(fd, src_addr, sbuf.st_size);
        if(send_len != sbuf.st_size)
        {
            perror("Send file failed");
            return ANALYSIS_ERROR;
        }
        //解除内存映射
        munmap(src_addr, sbuf.st_size);
        return ANALYSIS_SUCCESS;
    } 
    else 
        return ANALYSIS_ERROR;
}

//发送错误页面
void requestData::handleError(int fd, int err_num, string short_msg)
{
    short_msg = " " + short_msg;
    char send_buff[MAX_BUFF];
    string body_buff, header_buff;

    body_buff += "<html><tilte>TKeed Error</title>";
    body_buff += "<body bgcolor=\"ffffff\">";
    body_buff += to_string(err_num) + short_msg;
    body_buff += "<hr><em>My Http Server</em>\n</body></html>";

    header_buff += "HTTP/1.1 "+to_string(err_num) + short_msg + "\r\n";
    header_buff += "Content-type: text/html\r\n";
    header_buff += "Connection: close\r\n";
    header_buff += "Content-length: "+to_string(body_buff.size()) + "\r\n";
    header_buff += "\r\n";

    sprintf(send_buff, "%s", header_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
    sprintf(send_buff, "%s", body_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
}

//构造定时器
//以当前时间为基准，加上超时时间，设置一个过期时间
mytimer::mytimer(requestData *_request_data, int timeout):deleted(false), request_data(_request_data)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    //失效时间以毫秒计
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

mytimer::~mytimer()
{
    if(request_data != NULL)
    {
        delete request_data;
        request_data = NULL;
    }
}

void mytimer::update(int timeout)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

//判断当前时间是否已经过了过期时间
bool mytimer::isvalid()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    size_t temp = ((now.tv_sec * 1000) + (now.tv_usec / 1000));
    if(temp < expired_time)
        return  true;
    else
    {
        this->setDeleted();
        return false;
    }
}

void mytimer::clearReq()
{
    request_data = NULL;
    this->setDeleted();
}

void mytimer::setDeleted()
{
    deleted = true;
}

bool mytimer::isDeleted() const
{
    return deleted;
}

size_t mytimer::getExpTime() const
{
    return expired_time;
}

//时间对比 仿函数
bool timerCmp::operator()(const mytimer *a, const mytimer *b)const
{
    //越大越先进栈，构成小顶堆，小元素在队首
    return a->getExpTime() > b->getExpTime();
}