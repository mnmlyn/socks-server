#ifndef _CONNECTION_H
#define _CONNECTION_H
#include <unordered_map>
#include "BufferPool.h"

enum ConnectionState {
    CS_INIT,
    CS_CONNECT,
    CS_RELAY
};

#define CONN_FD_NOTSET -1

class Connection {
public:
    int fd;
    int fd_up;
    ConnectionState state;
    static BufferPool *buffPool;

public:
    Connection(int fd, int fd_up);
    ~Connection();
    Buffer *getBuff();
    Buffer *getUpBuff();
    void tryFreeBuffer();

private:
    Buffer *buff;
    Buffer *upBuff;

private:
    Connection(){};
};

typedef std::unordered_map<int, Connection *> ConnectionMap;
typedef ConnectionMap::iterator ConnectionMapItrator;

#endif