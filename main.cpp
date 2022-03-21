#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdbool.h>        // c语言里没有bool，要加这个头文件
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    // 可重入性：函数可以安全地并行执行。
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    /*
    struct sigaction {
        void ( * sa_handler) ( int ) ;      /*sa_handler字段包含一个信号捕捉函数的地址
        sigset_t sa_mask;       /*sa_mask字段说明了一个信号集，在调用该信号捕捉函数之前，
                                这一信号集要加进进程的信号屏蔽字中。仅当从信号捕捉函数返回时再将进程的信号屏蔽字复位为原先值。
                                
        int sa_flag;        /*sa_flag是一个选项，主要理解两个
                            SA_INTERRUPT 由此信号中断的系统调用不会自动重启
                            SA_RESTART 由此信号中断的系统调用会自动重启
        void ( * sa_sigaction) ( int , siginfo_t * , void * ) ;  /* 不太用得到
    } ;
    */
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;    //信号处理函数中仅仅发送信号值，不做对应逻辑处理
    if (restart)
        sa.sa_flags |= SA_RESTART;
        // 当执行某个阻塞系统调用(慢系统调用)时，收到信号都会返回-1，表示出错，结束进程，
        // 如果启用SA_RESTART，则收到该信号时，进程不会返回，而是重新执行该系统调用。
    sigfillset(&sa.sa_mask);        //信号处理函数执行期间屏蔽所有信号
    assert(sigaction(sig, &sa, NULL) != -1);    //修改sig信号所关联的处理动作为sa内的内容，不关心oldact
    /*int sigaction ( int signo,  *act,  *oldact) ;*/
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);    // 删除epollfd中的注册
    assert(user_data);
    close(user_data->sockfd);               // 关闭连接
    http_conn::m_user_count--;              // 用户数-1
    printf("[close sockfd]: %d", user_data->sockfd);
}

void show_error(int connfd, const char *info)
{
    printf("%s\n", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);
    /* 当服务器close一个连接时，若client端接着发数据。根据TCP协议的规定，会收到一个RST响应，
    client再往这个服务器发送数据时，系统会发出一个SIGPIPE信号给进程，告诉进程这个连接已经断开了，不要再写了。
    根据信号的默认处理规则SIGPIPE信号的默认执行动作是terminate(终止、退出),所以client会退出。
    若不想客户端退出可以把SIGPIPE设为SIG_IGN
    */
    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "123456", "webdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    //创建MAX_FD个http类对象
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭  
    // 如果选择此选项，close或 shutdown将等到所有套接字里排队的消息成功发送或到达延迟时间后才会返回。否则，调用将立即返回。
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);    // ipv4:uint32_t    ipv6:uint8_t arr[16]
    address.sin_port = htons(port);                 // uint16_t

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    // 把一个指定的端口分配给要bind的socket。 
    // 以后就可以用这个端口来“听“网络的请求。bind()用于server端，端口分配后，其他socket不能再用这个端口。
    // 相当于告诉client端“要请求服务，往这个端口发“。 client端不用bind，每建一个socket系统会分配一个临时的端口，用完后再释放。
    assert(ret >= 0);
    ret = listen(listenfd, 5);  //建立5长度的队列，保存尚未完成三次握手的连接请求
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);  // 生成一个epollfd，num是在epollfd上能关注的最大socketfd数
    assert(epollfd != -1);

    //将listenfd放在epoll树上
    addfd(epollfd, listenfd, false);  
    //将上述epollfd赋值给http类对象的m_epollfd属性
    http_conn::m_epollfd = epollfd;

    //创建管道
    /* 创建管道，注册pipefd[0]上的可读事件 */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    /* 设置管道写端为非阻塞 */
    setnonblocking(pipefd[1]);              //  这里的管道干什么用的？ 应该是定时器所用的，通过定时信号清除不活动连接
    /* 设置管道读端为ET非阻塞，并添加到epoll内核事件表 */
    addfd(epollfd, pipefd[0], false);

    addsig(SIGALRM, sig_handler, false);    // 不开启SA_RESTART
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;

    /* 每个user（http请求）对应的timer */
    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    /* 每隔TIMESLOT时间触发SIGALRM信号 */
    alarm(TIMESLOT);
    // alarm函数会定期触发SIGALRM信号，这个信号交由sig_handler来处理，
    // 每当监测到有这个信号的时候，都会将这个信号写到pipefd[1]里面，传递给主循环：

    while (!stop_server)
    {
        //等待所监控文件描述符上有事件的产生
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            printf("!!!!!epoll failure!!!!!");
            break;
        }
        //对所有就绪事件进行处理
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        if (errno == 11)
                            printf("accept error:errno is:EAGAIN\n");
                        else 
                            printf("accept error:errno is:%d\n", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        printf("Internal server busy\n");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;        // users_timer是与users一一对应的
                    util_timer *timer = new util_timer;         // 创建定时器
                    timer->user_data = &users_timer[connfd];    // 绑定用户数据
                    timer->cb_func = cb_func;                   // 设置回调
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;         // 设置超时时间
                    users_timer[connfd].timer = timer;          // 绑定定时器
                    timer_lst.add_timer(timer);                 // 添加到链表
                }
                continue;

            }
            //处理异常事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);   // 回调函数就是：删除sockfd，关闭连接，用户数-1
                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            //如果就绪的文件描述符是pipefd[0]，则处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                //从管道读端读出信号值，成功返回字节数，失败返回-1
                //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
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
                            /* 当我们在读端pipefd[0]读到这个信号的的时候，就会将timeout变量置为true并跳出循环，
                            让timer_handler()函数取出来定时器容器上的到期任务，该定时器容器是通过升序链表来实现的，
                            从头到尾对检查任务是否超时，若超时则调用定时器的回调函数cb_func()，
                            关闭该socket连接，并删除其对应的定时器del_timer。 */
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }

            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                //读入对应缓冲区
                if (users[sockfd].read_once())
                {
                    printf("deal with the client(%s)\n", inet_ntoa(users[sockfd].get_address()->sin_addr));// sin_addr是32位IP地址
                    
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("[adjust timer once]\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
                else // 读完了，关闭连接
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    printf("send data to the client(%s)\n", inet_ntoa(users[sockfd].get_address()->sin_addr));

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf("[adjust timer once]\n");
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
