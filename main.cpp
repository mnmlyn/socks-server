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

#define MAXLINE 4096
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
    ev.events = EPOLLIN | EPOLLET;
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
    }
    delete conn;
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
    conn->state = ConnectionState::CS_RELAY;
    return 0;
}

/**
 * 返回-1，代表接收出错，连接将被关闭。关闭连接时，缓冲区也将被释放
 */
int parseRecvData(Buffer *buff, int connfd, Connection *conn, int epfd) {
    LOGP(DEBUG, "parseRecvData\n");
    if (buff->getRemainLen() <= 0) {
        // bug
        perror("parseRecvData, RemainLen should > 0");
    }
    int fd = conn->fd == connfd ? conn->fd_up : conn->fd;
    struct epoll_event ev;
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
            break;// ????
        case ConnectionState::CS_RELAY:
            if (fd == CONN_FD_NOTSET) {
                perror("conn->state == CS_RELAY, fd not set\n");
                err = true;
            } else {
                // 代理
                LOGP(DEBUG, "CS_RELAY, recv packet\n");
                
                buff->empty();

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

void printMap() {
    LOGP(DEBUG, "======PRINT MAP======\n");
    for (auto entry : connMap) {
        int fd = entry.first;
        Connection *conn = entry.second;
        LOGP(DEBUG, "fd=%d, conn=%p\n", fd, conn);
    }
    LOGP(DEBUG, "=====================\n");
}

void dataIn(int connfd, int epfd) {
    LOGP(DEBUG, "dataIn\n");
    LOGP(DEBUG, "connfd = %d\n", connfd);
    printMap();
    ConnectionMapItrator it = connMap.begin();
    LOGP(DEBUG, "1\n");
    connMap.count(connfd);
    LOGP(DEBUG, "1\n");
    printMap();

    if ((it = connMap.find(connfd)) == connMap.end()) {
        // error
        perror("dataIn, connfd not find in connMap");
        exit(-1);
    }

    LOGP(DEBUG, "dataIn, getConn\n");
    printMap();
    Connection *conn = it->second;
    LOGP(DEBUG, "%p\n", conn);
    LOGP(DEBUG, "conn->buff = %p\n", conn->buff);
    LOGP(DEBUG, "conn->upBuff = %p\n", conn->upBuff);
    Buffer **pBuff = conn->fd == connfd ? &conn->buff : &conn->upBuff;
    Buffer *buff = *pBuff ? *pBuff : (*pBuff = new Buffer(MAXLINE));
    LOGP(DEBUG, "buff = %p\n", buff);
    LOGP(DEBUG, "*pBuff = %p\n", *pBuff);
    LOGP(DEBUG, "conn->buff = %p\n", conn->buff);
    LOGP(DEBUG, "conn->upBuff = %p\n", conn->upBuff);
    
    printMap();
    dumpConnection(conn);

    LOGP(DEBUG, "dataIn, getRemainLen\n");
    if (buff->getRemainLen() <= 0) {
        // bug
        perror("dataIn, Buffer full, but not parsed");
        exit(-1);
    }
    printMap();

    LOGP(DEBUG, "dataIn, recv\n");
    buff->print();
    LOGP(DEBUG, "start = %ld, len = %d\n", buff->getStart() - buff->head, buff->len);
    LOGP(DEBUG, "remainLen = %d, dataLen = %d\n", buff->getRemainLen(), buff->dataLen);
    printMap();// ?终于定位了错误，是在这里出错，调用完recv之后，connMap被破坏
    int n = recv(connfd, recvbuff, buff->getRemainLen(), 0);
    
    //int n = recv(connfd, buff->getStart(), buff->getRemainLen(), 0);
    printMap(); // ? 这里出错，看来不是recv的问题，而是将内容复制到缓冲区出错
    LOGP(DEBUG, "n = %d\n", n);
    memcpy(buff->getStart(), recvbuff, n);
    printMap();
    if (n > 0) {
        buff->push(n);
        LOGP(DEBUG, "%d bytes recv\n", n);
        LOGP(DEBUG, "%d data in buff\n", buff->dataLen);
        LOGP(DEBUG, "%d size remain in buff\n", buff->getRemainLen());
        LOGP(DEBUG, "%d buff len\n", buff->getRemainLen() + buff->dataLen);
        buff->print();
        LOGP(DEBUG, "before parseRecvData\n");
        printMap();
        int ret = parseRecvData(buff, connfd, conn, epfd);
        LOGP(DEBUG, "after parseRecvData\n");
        printMap();
        if (!ret && buff->isEmpty()) {
            // 释放接收缓存，之后改用缓存池
            LOGP(DEBUG, "dataIn, delete buff\n");
            *pBuff = NULL;
            delete buff;
        }
        printMap();
    } else if (n == 0) {
        closeConnection(conn, epfd);
    } else {
        LOGP(DEBUG, "dataIn, recv n<0\n");
    }
}

void dataOut(int connfd, int epfd) {
    ConnectionMapItrator it;
    LOGP(DEBUG, "dataOut\n");
    if ((it = connMap.find(connfd)) == connMap.end()) {
        // bug
        perror("dataOut, connfd not find in connMap");
        exit(-1);
    }
    Connection *conn = it->second;
    bool isUpfd = conn->fd_up == connfd;
    int fd = isUpfd ? conn->fd : conn->fd_up;

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
                if (events[i].events & EPOLLIN) {
                    dataIn(connfd, epfd);
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
