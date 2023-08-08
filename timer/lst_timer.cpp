#include "lst_timer.h"
#include "../http/http_conn.h"
// 定时器链表
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}
// 添加定时器，内部调用私有成员add_timer，函数重载
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    // 如果定时器的终止时间比头节点还小，那么直接添加到头节点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 否则调用私有成员，调整内部节点
    add_timer(timer, head);
}
// 调整定时器位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    // 被调整的目标定时器在尾部，或定时器新的超时值仍然小于下一个定时器的超时，不用调整，否则先将定时器从链表取出，重新插入链表
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    // 被调整的定时器是链表头节点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else    // 被调整的定时器在内部，将定时器取出，重新插入
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
// 删除定时器，时间复杂度O(1)
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    // 链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
// 定时任务处理函数
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)
    {
        // 超时时间大于当前时间，那么直接跳出循环
        if (cur < tmp->expire)
        {
            break;
        }
        // 超时时间小于当前时间，也就是需要进行处理了，调用回调函数，执行定时事件
        tmp->cb_func(tmp->user_data);
        // 将处理后的定时器从链表容器中删除，并重置头节点
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}
// 添加定时器，为私有成员，时间复杂度O(n)，被公有成员add_timer和adujst_time调用
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        // 遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // temp为空，即需要添加到队尾
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}
// 设置最小超时单位
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    // 获得文件状态标记
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    // 设置文件状态标记
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数，仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响。
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    /*  将信号从管道写端写入，传输字符类型
        send(int sockfd, const void *buf, size_t len, int flags);
        send()函数只能在套接字处于连接状态的时候才能使用。
            - sockfd：接收消息的套接字的文件描述符。
            - buf：要发送的消息。
            - len：要发送的字节数。
            - flags：flags参数表示标志的个数，具体标志，百度
     */
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    // 创建sigaction结构体变量
    struct sigaction sa;
    /* 
        void *memset(void *s, int c, size_t n); 
        memset是一个初始化函数，作用是将某一块内存中的全部设置为指定的值。
            - s指向要填充的内存块。
            - c是要被设置的值。
            - n是要被设置该值的字符数。
            - 返回类型是一个指向存储区s的指针。
     */
    memset(&sa, '\0', sizeof(sa));
    // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    // 定时处理任务
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)
{
    /*  删除非活动连接在socket上的注册事件，即计时器时间到，断开连接
        int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
        功能：对epoll实例进行管理：添加文件描述符信息，删除信息，修改信息
            - `epfd`：epoll实例对应的文件描述符
            - `op`：要进行什么操作
                - 添加：`EPOLL_CTL_ADD`
                - 删除：`EPOLL_CTL_DEL`
                - 修改：`EPOLL_CTL_MOD`
            - `fd`：要检测的文件描述符
            - `event`：检测文件描述符什么事情，通过设置 `epoll_event.events`，常见操作
                - 读事件：`EPOLLIN`
                - 写事件：`EPOLLOUT `
                - 错误事件：`EPOLLERR`
                - 设置边沿触发：`EPOLLET`（默认水平触发）
     */
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
