#ifndef _CONNECTION_H
#define _CONNECTION_H
#include <unordered_map>
#include "Buffer.h"

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
    Buffer *buff;
    Buffer *upBuff;

    Connection(int fd, int fd_up);
    ~Connection();
private:
    Connection(){};
};

typedef std::unordered_map<int, Connection *> ConnectionMap;
typedef ConnectionMap::iterator ConnectionMapItrator;

#endif