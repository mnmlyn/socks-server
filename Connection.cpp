#include "Connection.h"

Connection::Connection(int fd, int fd_up) {
    this->fd = fd;
    this->fd_up = fd_up;
    this->state = ConnectionState::CS_INIT;
    this->buff = NULL;
    this->upBuff = NULL;
}

Connection::~Connection() {
    if (this->buff)delete this->buff;
    if (this->upBuff)delete this->upBuff;
}