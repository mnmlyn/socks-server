#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<sys/epoll.h> 
#include<netinet/in.h>
#include<unistd.h>
#include<fcntl.h>
#include<unordered_map>

#include "Connection.h"
#include "logging.h"

#define MAXLINE 4096*2
char recvbuff[MAXLINE];
char sendbuff[MAXLINE];
ConnectionMap connMap;

void setnonblocking(int sock)
{
    int opts;     
    opts = fcntl(sock,F_GETFL);     
    if (opts < 0)
    {         
        perror("fcntl(sock,GETFL)");         
        exit(1);
    }
    opts = opts|O_NONBLOCK;
    if (fcntl(sock, F_SETFL, opts) < 0)
    {
        perror("fcntl(sock,SETFL,opts)");
        exit(1);
    }
}

void setreuseaddr(int sock)
{
    int opt;
    opt = 1;    
    if (setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(&opt)) < 0)     
    {         
        perror("setsockopt");         
        exit(1);     
    }  
}

void printMap() {
    LOGP(DEBUG, "======PRINT MAP======\n");
    for (auto entry : connMap) {
        int fd = entry.first;
        Connection *conn = entry.second;
        LOGP(DEBUG, "fd=%d, conn=%p\n", fd, conn);
    }
    LOGP(DEBUG, "=====================\n");
}

void acceptOne(int listenfd, int epfd) {
    int connfd;
    struct epoll_event ev;
    LOGP(DEBUG, "acceptOne\n");
    if( (connfd = accept(listenfd,(struct sockaddr*)0,0)) == -1 )
    {
        printf("accept socket error: %s(errno: %d)\n",strerror(errno),errno);
        return;
    }
    setnonblocking(connfd);
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = connfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, connfd ,&ev) == -1) {
        perror("epoll_ctl: connfd");
        exit(-1);
    }

    Connection* conn = new Connection(connfd, CONN_FD_NOTSET);
    ConnectionMapItrator it = connMap.begin();
    if ((it = connMap.find(connfd)) != connMap.end()) {
        // error
        perror("connfd conflict in connMap");
        exit(-1);
    }
    connMap[connfd] = conn;
}

void epollDelFd(int connfd, int epfd) {
    struct epoll_event ev;
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, connfd ,&ev) == -1) {
        perror("epoll_ctl: connfd");
        exit(-1);
    }
}

void closeConnection(Connection *conn, int epfd) {
    struct epoll_event ev;
    LOGP(DEBUG, "closeConnection\n");
    if (conn == NULL) {
        // bug
        perror("closeConnection, NULL conn\n");
        exit(-1);
    }
    int fds[2] = {conn->fd, conn->fd_up};
    for (int i=0; i<2; ++i) {
        if (fds[i] == CONN_FD_NOTSET)continue;
        if (epoll_ctl(epfd, EPOLL_CTL_DEL, fds[i] ,&ev) == -1) {
            perror("epoll_ctl: connfd");
            exit(-1);
        }
        close(fds[i]);

        /* debug */
        if (!connMap.count(fds[i])) {
            perror("closeConnection, fd not found");
        }

        connMap.erase(fds[i]);
        LOGP(TRACE, "closeConnection, fd = %d\n", fds[i]);
    }
    delete conn;
    printMap();
}

bool checkSocksProtocol(Buffer *buff) {
    return !(buff == NULL || buff->dataLen < 9
        || buff->head[8] != 0 || buff->head[0] != 4
        || buff->head[1] != 1);
}

uint32_t getIpFromSocksBuffer(Buffer *buff) {
    return *(uint32_t *)(buff->head + 4);
}

uint16_t getPortFromSocksBuffer(Buffer *buff) {
    return *(uint16_t *)(buff->head + 2);
}

/**
 * 返回-1，代表出错，调用者应该立即关闭这个连接
 * 返回1，说明正在连接，且已经将其加入事件监听
 * 返回0，连接已经建立，应该立即响应
 */
int connectUp(Connection *conn, int epfd, uint32_t ip, uint16_t port) {
    int fd, ret;
    struct sockaddr_in servaddr;
    struct epoll_event ev; 
    if (conn == NULL) {
        // bug
        perror("connectUp, conn == NULL\n");
        exit(-1);
    }

    if (conn->fd_up != CONN_FD_NOTSET) {
        // bug
        perror("connectUp, fd_up already set\n");
        connMap.erase(conn->fd_up);
        close(conn->fd_up);
    }
    conn->state = ConnectionState::CS_CONNECT;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("connectUp, create socket error\n");
        exit(-1);
    }
    setnonblocking(fd);

    ev.data.fd = fd;  
    ev.events = EPOLLOUT|EPOLLIN|EPOLLET;
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    if (ret != 0)
    {
        printf("epoll_ctl error: %s(errno: %d)\n",strerror(errno),errno);
        exit(-1);
    }

    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = ip;// ip and port: network byte order
    servaddr.sin_port = port;
    ret = connect(fd, (struct sockaddr *)(&servaddr), sizeof(servaddr));
    LOGP(TRACE, "connecting upstream, fd=%d\n", fd);

    conn->fd_up = fd;
    connMap[fd] = conn;
    if (ret == -1 && errno == EINPROGRESS) {
        return 1;
    }
    return ret;
}

int replyReadyToRelay(Connection *conn) {
    static char msg[8] = {0, 0x5A};
    int fd;
    LOGP(DEBUG, "replyReadyToRelay\n");
    if (conn == NULL) {
        // bug
        perror("replyReadyToRelay, conn == NULL\n");
        exit(-1);
    }

    if (conn->state != ConnectionState::CS_CONNECT) {
        // bug
        perror("replyReadyToRelay, conn->state != CS_CONNECT\n");
    }

    if (conn->fd == CONN_FD_NOTSET) {
        // bug
        perror("replyReadyToRelay, fd not set\n");
        exit(-1);
    }
    fd = conn->fd;
    if (send(fd, msg, 8, 0) == -1) {
        perror("replyReadyToRelay, send error\n");
        return -1;
    }
    LOGP(TRACE, "send data, fd=%d, n=%d\n", fd, 8);
    conn->state = ConnectionState::CS_RELAY;
    return 0;
}

/**
 * 返回-2代表发送出错，应该直接干掉这个连接
 * 返回大于0代表发送成功
 * 返回-1代表发送会阻塞
 */
int sendBufferToFd(Buffer *buff, int connfd, int epfd) {
    int n;
    struct epoll_event ev;
    LOGP(DEBUG, "sendBufferToFd\n");
    if (buff == NULL) {
        // bug
        perror("sendBufferToFd, buff == NULL\n");
        exit(-1);
    }

    n = send(connfd, buff->head, buff->dataLen, 0);
    if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("sendBufferToFd, send error\n");
        return -2;
    }

    if (n == -1) {
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.fd = connfd;
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, connfd ,&ev) == -1) {
            perror("epoll_ctl: connfd");
            exit(-1);
        }
    }
    LOGP(TRACE, "send buff, connfd=%d, n=%d\n", connfd, n);

    return n;
}

/**
 * 返回-1，代表接收出错，连接将被关闭。关闭连接时，缓冲区也将被释放
 */
int parseRecvData(Buffer *buff, int connfd, Connection *conn, int epfd) {
    LOGP(DEBUG, "parseRecvData\n");
    if (buff->dataLen <= 0) {
        // bug
        perror("parseRecvData, buff->dataLen should > 0");
    }
    int fd = conn->fd == connfd ? conn->fd_up : conn->fd;
    bool err = false;

    switch(conn->state) {
        case ConnectionState::CS_INIT:
            if (conn->fd == connfd) {
                if (buff->dataLen > 9) {
                    perror("conn->state == CS_INIT, buff->dataLen > 9, good bye");
                    err = true;
                }
                if (buff->dataLen < 9 ) {
                    LOGP(DEBUG, "buff->dataLen < 9, wait next packet\n");
                    break;
                } else {
                    if (!checkSocksProtocol(buff)) {
                        LOGP(DEBUG, "!checkSocksProtocol\n");
                        err = true;
                    } else {
                        int ret = connectUp(conn, epfd, getIpFromSocksBuffer(buff), getPortFromSocksBuffer(buff));
                        LOGP(DEBUG, "connectUp, ret=%d\n", ret);
                        buff->pop(9);
                        if (ret == -1) {
                            err = true;
                        } else if (ret == 0) {
                            // 立即响应
                            if (replyReadyToRelay(conn) == -1) {
                                err = true;
                            }
                        }
                    }
                }
            } else {
                // bug
                perror("conn->state == CS_INIT, upstream packet recv, close it\n");
                err = true;
            }
            break;
        case ConnectionState::CS_CONNECT:
            perror("conn->state == CS_CONNECT, packet recv, bad one, close it\n");
            err = true;
            break;
        case ConnectionState::CS_RELAY:
            if (fd == CONN_FD_NOTSET) {
                perror("conn->state == CS_RELAY, fd not set\n");
                err = true;
            } else {
                // 代理
                LOGP(DEBUG, "CS_RELAY, recv packet\n");
                int ret = sendBufferToFd(buff, fd, epfd);
                if (ret >= 0) {
                    LOGP(DEBUG, "sendBufferToFd success, pop %d\n", ret);
                    buff->pop(ret);
                } else if (ret == -2) {
                    LOGP(DEBUG, "sendBufferToFd err, good bye\n");
                    err = true;
                } else {
                    LOGP(DEBUG, "sendBufferToFd block, wait next send\n");
                }
            }
            break;
        default:
            // bug
            perror("conn->state unknown\n");
            exit(-1);
            break;
    }
    if (err) {
        closeConnection(conn, epfd);
        return -1;
    }
    return 0;
}

void dumpConnection(Connection *conn) {
    LOGP(DEBUG, "Connection: fd=%d, fd_up=%d\n", conn->fd, conn->fd_up);
}

/**
 * conn对方向的buff不应为NULL
 */
void transferData(Connection *conn, bool isUp, int epfd) {
    int n1 = -2, n2 = -2, fromfd, tofd;
    Buffer *buff;
    LOGP(DEBUG, "transferData\n");

    if (conn == NULL) {
        // bug
        LOGP(WARNING, "transferData, conn == NULL\n");
        exit(-1);
    }

    if (conn->state != ConnectionState::CS_RELAY) {
        // bug
        LOGP(WARNING, "transferData, conn->state err\n");
        exit(-1);
    }

    if (isUp) {
        fromfd = conn->fd;
        tofd = conn->fd_up;
        buff = conn->buff;
    } else {
        fromfd = conn->fd_up;
        tofd = conn->fd;
        buff = conn->upBuff;
    }

    if (buff == NULL) {
        LOGP(WARNING, "transferData, buff NULL\n");
        exit(-1);
    }

    if (tofd == CONN_FD_NOTSET) {
        LOGP(DEBUG, "transferData, tofd closed\n");
        closeConnection(conn, epfd);
        return;
    }

    bool err = false;
    for (;;) {
        LOGP(DEBUG, "transferData, n1=%d, n2=%d, fromfd=%d, tofd=%d, err=%d\n"
            , n1, n2, fromfd, tofd, err);
        if (!buff->isFull() && fromfd != CONN_FD_NOTSET && n1 != -1) {
            n1 = recv(fromfd, buff->getStart(), buff->getRemainLen(), 0);
            if (n1 == 0) {
                LOGP(DEBUG, "transferData, n1 == 0\n");
                connMap.erase(fromfd);
                epollDelFd(fromfd, epfd);
                close(fromfd);
                fromfd = *(fromfd == conn->fd ? &conn->fd : &conn->fd_up) = CONN_FD_NOTSET;
            } else if (n1 == -1) {
                LOGP(DEBUG, "transferData, n1 == -1\n");
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOGP(DEBUG, "transferData, n1 err\n");
                    err = true;
                }
            } else {
                buff->push(n1);
                LOGP(DEBUG, "transferData, push n1=%d\n", n1);
            }
        }
        if (!buff->isEmpty() && n2 != -1) {
            n2 = send(tofd, buff->head, buff->dataLen, 0);
            if (n2 >= 0) {
                LOGP(DEBUG, "transferData, pop n2=%d\n", n2);
                buff->pop(n2);
            } else {
                LOGP(DEBUG, "transferData, n2 == -1 ? %d\n", n2);
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOGP(DEBUG, "transferData, n2 err\n");
                    err = true;
                }
            }
        }

        if (err || fromfd == CONN_FD_NOTSET && buff->isEmpty()) {
            LOGP(DEBUG, "transferData, err occur || all done\n");
            closeConnection(conn, epfd);
            return;
        }

        if (n1 == -1 && buff->isEmpty() || n2 == -1 && buff->isFull() || n1 == -1 && n2 == -1) {
            LOGP(DEBUG, "transferData, block\n");
            return;
        }
    }
}

void dataIn(int connfd, int epfd) {
    LOGP(DEBUG, "dataIn\n");
    LOGP(DEBUG, "connfd = %d\n", connfd);
    ConnectionMapItrator it;

    if ((it = connMap.find(connfd)) == connMap.end()) {
        // error
        perror("dataIn, connfd not find in connMap");
        exit(-1);
    }

    Connection *conn = it->second;
    Buffer **pBuff = conn->fd == connfd ? &conn->buff : &conn->upBuff;
    Buffer *buff = *pBuff ? *pBuff : (*pBuff = new Buffer(MAXLINE));

    dumpConnection(conn);

    if (conn->state == ConnectionState::CS_RELAY) {
        LOGP(DEBUG, "conn->state == ConnectionState::CS_RELAY\n");
        transferData(conn, conn->fd == connfd, epfd);
        return;
    }

    int n = recv(connfd, buff->getStart(), buff->getRemainLen(), 0);
    if (n > 0) {
        buff->push(n);
        LOGP(DEBUG, "before parseRecvData\n");
        LOGP(TRACE, "recv data, connfd=%d, n=%d\n", connfd, n);
        // buff->print();
        // printMap();
        int ret = parseRecvData(buff, connfd, conn, epfd);
        LOGP(DEBUG, "after parseRecvData\n");
        //printMap();
        if (!ret && buff->isEmpty()) {
            // 释放接收缓存，之后改用缓存池
            LOGP(DEBUG, "dataIn, delete buff\n");
            *pBuff = NULL;
            delete buff;
        }
    } else if (n == 0) {
        closeConnection(conn, epfd);
    } else {
        LOGP(DEBUG, "dataIn, recv n<0\n");
    }
}

void dataOut(int connfd, int epfd) {
    ConnectionMapItrator it;
    LOGP(DEBUG, "dataOut\n");
    LOGP(DEBUG, "connfd = %d\n", connfd);
    if ((it = connMap.find(connfd)) == connMap.end()) {
        // bug
        perror("dataOut, connfd not find in connMap");
        exit(-1);
    }
    Connection *conn = it->second;
    dumpConnection(conn);
    bool isUpfd = conn->fd_up == connfd;
    int fd = isUpfd ? conn->fd : conn->fd_up;

    Buffer **pBuff = isUpfd ? &conn->buff : &conn->upBuff;
    Buffer *buff = *pBuff ? *pBuff : (*pBuff = new Buffer(MAXLINE));

    if (conn->state == ConnectionState::CS_CONNECT) {
        if (isUpfd) {
            replyReadyToRelay(conn);
        } else {
            perror("dataOut, downfd can out in CS_CONNECT state\n");
        }
    } else if (conn->state == ConnectionState::CS_INIT) {
        perror("dataOut, fd can out in CS_INIT state\n");
    } else if (conn->state == ConnectionState::CS_RELAY) {
        LOGP(DEBUG, "dataOut, CS_RELAY\n");
        transferData(conn, isUpfd, epfd);
        // if (buff != NULL && !buff->isEmpty()) {
        //     int ret = sendBufferToFd(buff, connfd, epfd);
        //     if (ret >= 0) {
        //         LOGP(DEBUG, "dataOut, pop %d\n", ret);
        //         buff->pop(ret);
        //     } else if (ret == -2) {
        //         LOGP(DEBUG, "dataOut, CS_RELAY, kill conn\n");
        //         closeConnection(conn, epfd);
        //     } else {
        //         LOGP(DEBUG, "dataOut, CS_RELAY, block, wait next\n");
        //     }
        // }
        
    }
}

int main(int argc,char** argv)
{
    int listenfd,connfd,n,epfd,nfds,i,ret;
    struct sockaddr_in servaddr;
    struct epoll_event ev;                     //事件临时变量
    const int MAXEVENTS = 1024;                //最大事件数
    struct epoll_event events[MAXEVENTS];    //监听事件数组
 
    epfd = epoll_create(5);
    if (epfd < 0)
    {
        printf("epoll_create error: %s(errno: %d)\n",strerror(errno),errno);
        return 0;
    }

    if( (listenfd = socket(AF_INET,SOCK_STREAM,0)) == -1)
    {
        printf("create socket error: %s(errno: %d)\n",strerror(errno),errno);
        close(epfd);
        return 0;
    }
    setnonblocking(listenfd);
    setreuseaddr(listenfd);
   
    ev.data.fd = listenfd;
    ev.events = EPOLLIN|EPOLLET;
    ret = epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev);
    if (ret != 0)
    {
        printf("epoll_ctl error: %s(errno: %d)\n",strerror(errno),errno);
        close(listenfd);
        close(epfd);
        return 0;
    }   
 
    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(1080);
 
    if( bind(listenfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) == -1)
    {
        printf("bind socket error: %s(errno: %d)\n",strerror(errno),errno);
        close(listenfd);
        close(epfd);
        return 0;
    }
 
    if( listen(listenfd,10) == -1)
    {
        printf("listen socket error: %s(errno: %d)\n",strerror(errno),errno);
        close(listenfd);
        close(epfd);
        return 0;
    }
 
    printf("========waiting for client's request========\n");
    while(1)
    {
        nfds = epoll_wait(epfd, events, MAXEVENTS, -1);
        if (nfds == -1) {
            printf("epoll_wait error: %s(errno: %d)\n",strerror(errno),errno);
            return 0;
        }
        for (i = 0; i < nfds; ++i) {
            if (events[i].data.fd == listenfd) {
                acceptOne(listenfd, epfd);
            } else {
                connfd = events[i].data.fd;
                if (!connMap.count(connfd)) {
                    LOGP(WARNING, "connfd = %d, not in connMap\n", connfd);
                    continue;
                }
                if (events[i].events & EPOLLIN) {
                    dataIn(connfd, epfd);
                }
                if (!connMap.count(connfd)) {
                    LOGP(WARNING, "connfd = %d, not in connMap\n", connfd);
                    continue;
                }
                if (events[i].events & EPOLLOUT) {
                    dataOut(connfd, epfd);
                }
            }
        }

    }
    close(listenfd);
    close(epfd);
    return 0;
}
