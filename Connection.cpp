#include "Connection.h"
#include "logging.h"

BufferPool *Connection::buffPool = NULL;

Connection::Connection(int fd, int fd_up) {
    this->fd = fd;
    this->fd_up = fd_up;
    this->state = ConnectionState::CS_INIT;
    this->buff = NULL;
    this->upBuff = NULL;
}

Connection::~Connection() {
    if (this->buff && this->buffPool)this->buffPool->freeBuffer(this->buff);
    if (this->upBuff && this->buffPool)this->buffPool->freeBuffer(this->upBuff);
}

Buffer *Connection::getBuff() {
    if (this->buff) {
        return this->buff;
    } else if (this->buffPool) {
        return (this->buff = this->buffPool->getBuffer());
    } else {
        LOGP(WARNING, "buffPool not set!!!\n");
        return NULL;
    }
}

Buffer *Connection::getUpBuff() {
    if (this->upBuff) {
        return this->upBuff;
    } else if (this->buffPool) {
        return (this->upBuff = this->buffPool->getBuffer());
    } else {
        LOGP(WARNING, "buffPool not set!!!\n");
        return NULL;
    }
}

void Connection::tryFreeBuffer() {
    if (this->buffPool == NULL) {
        LOGP(WARNING, "buffPool not set!!!\n");
        return;
    }
    if (this->buff && this->buff->isEmpty()) {
        this->buffPool->freeBuffer(this->buff);
        this->buff = NULL;
    }
    if (this->upBuff && this->upBuff->isEmpty()) {
        this->buffPool->freeBuffer(this->upBuff);
        this->upBuff = NULL;
    }
}
