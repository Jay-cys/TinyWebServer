#include "webserver.h"

WebServer::WebServer()
{
    // http_conn类对象
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}
// 服务器初始化配置
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

// 触发模式配置
void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 日志配置
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        // 初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

// 数据库连接池配置
void WebServer::sql_pool()
{
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

// 线程池配置
void WebServer::thread_pool()
{
    // 线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// 网络编程基础步骤
void WebServer::eventListen()
{
    // 网络编程基础步骤
    //  创建用于监听的套接字
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0); // 断言，若返回错误，则终止程序运行

    /* 优雅关闭连接
        端口复用函数setsockopt中SO_LINGER参数
        struct linger {
            int l_onoff;
            int l_linger;
        };
        l_onoff为0，则该选项关闭，l_linger的值被忽略，close()用上述缺省方式关闭连接。
        l_onoff非0，l_linger为0，close()用上述a方式关闭连接。
        l_onoff非0，l_linger非0，close()用上述b方式关闭连接。
        a.立即关闭该连接，通过发送RST分组(而不是用正常的FIN|ACK|FIN|ACK四个分组)来关闭该连接。
            至于发送缓冲区中如果有未发送完的数据，则丢弃。主动关闭一方的TCP状态则跳过TIMEWAIT，直接进入CLOSED。
            网上很多人想利用这一点来解决服务器上出现大量的TIMEWAIT状态的socket的问题，但是，这并不是一个好主意，
            这种关闭方式的用途并不在这儿，实际用途在于服务器在应用层的需求。
        b.将连接的关闭设置一个超时。如果socket发送缓冲区中仍残留数据，进程进入睡眠，内核进入定时状态去尽量去发送这些数据。
            在超时之前，如果所有数据都发送完且被对方确认，内核用正常的FIN|ACK|FIN|ACK四个分组来关闭该连接，close()成功返回。
            如果超时之时，数据仍然未能成功发送及被确认，用上述a方式来关闭此连接。close()返回EWOULDBLOCK。
     */
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    // 绑定
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)); // SO_REUSEADDR是让端口释放后立即就可以被再次使用。
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    // 监听
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);
    // 定时器初始化
    utils.init(TIMESLOT);

    // epoll创建内核事件表
    // 此结构体用来保存内核态返回给用户态发生改变的文件描述符信息
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 将内核事件表注册读事件，ET模式，选择是否开启EPOLLONESHOT
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    // 将上述epollfd赋值给http类对象的m_epollfd属性
    http_conn::m_epollfd = m_epollfd;

    /* 创建管道，注册pipefd[0]上的可读事件
        socketpair()函数用于创建一对无名的、相互连接的套接字。
            1. 这对套接字可以用于全双工通信，每一个套接字既可以读也可以写。
            2. 如果往一个套接字(如sv[0])中写入后，再从该套接字读时会阻塞，只能在另一个套接字中(sv[1])上读成功；
            3. 读、写操作可以位于同一个进程，也可以分别位于不同的进程，如父子进程。如果是父子进程时，一般会功能分离，
                一个进程用来读，一个用来写。因为文件描述副sv[0]和sv[1]是进程共享的，所以读的进程要关闭写描述符, 反之，写的进程关闭读描述符。
     */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    // 设置管道写端为非阻塞
    utils.setnonblocking(m_pipefd[1]);
    // 设置管道读端为ET非阻塞，并添加到epoll内核事件表中
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    // 传递给主循环的信号值，这里只关注SIGALRM和SIGTERM，注册SIGALRM和SIGTERM信号捕捉函数
    // alarm函数会定期触发SIGALRM信号，这个型号交由sig_handler来处理，每当检测到有这个信号的时候，都会将信号写到pipefd[1]中
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);

    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 定时器处理非活动链接
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    // 初始化连接
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);
    // 初始化该连接对应的连接资源
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    // 创建定时器临时变量
    util_timer *timer = new util_timer;
    // 初始化定时器：设置连接资源、回调函数、绝对超时时间
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    // 创建该连接对应的定时器,初始化为上述的临时变量
    users_timer[connfd].timer = timer;
    // 添加到链表中
    utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟3个单位，并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}
// 处理定时器,包括异常事件、非活动连接
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}
// 处理客户端数据
bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode) // 水平触发
    {
        // 接收客户端连接
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address); // 成功连接，进行定时
    }
    else // 边沿触发,需要循环接收数据
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}
// 处理信号,若信号为SIGALRM,timeout设置为true;若信号为SIGTERM,stop_server设置为true
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    /* 从管道读端读出信号值，成功返回字节数，失败返回-1
    正常情况下，这里的ret返回值总是1，只有14(SIGALRM)和15(SIGTERM)两个ASCII码对应的字符
        int recv( SOCKET s, char *buf, int len, int flags);
        参数：
            sockfd：建立连接的套接字
            buf：接收到的数据保存在该数组中
            len：数组的长度
            flags：一般设置为0
        返回值：
            > 0 : 表示执行成功，返回实际接收到的字符个数
            = 0 : 另一端关闭此连接
            < 0 : 执行失败,可以通过errno来捕获错误原因（errno.h）
    */
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}
// 处理客户连接上的读事件
void WebServer::dealwithread(int sockfd)
{
    // 创建定时器临时变量,将该连接对应的定时器取出来
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == m_actormodel)
    {
        // 若有数据传输,将定时器往后延迟3个单位,并对其在链表上的位置进行调整
        if (timer)
        {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列,users + sockfd是啥？将它赋给T *request，
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            // 服务器关闭连接,移除对于你的定时器
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}
// 向连接写入
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}
// 循环查询事件
void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        /* 监测发生事件的文件描述符
            int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
            功能：检测哪些文件描述符发生了改变
            参数：
                - `epfd`：epoll实例对应的文件描述符
                - `events`：传出参数，保存了发生了变化的文件描述符的信息
                - `maxevents`：第二个参数结构体数组的大小
                - `timeout`：阻塞时长
                    - 0：不阻塞
                    - -1：阻塞，当检测到需要检测的文件描述符有变化，解除阻塞
                    - >0：具体的阻塞时长(ms)
            返回值：
                - > 0：成功，返回发送变化的文件描述符的个数
                - -1：失败
        */
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        // 轮询文件描述符
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }
            // 处理异常事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理定时器信号,管道读端对应文件描述符发生读事件
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        // 处理定时器为非必须事件，收到信号并不是立马处理；完成读写事件后，再进行处理
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}