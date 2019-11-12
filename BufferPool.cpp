#include "BufferPool.h"
#include "logging.h"

BufferPool::BufferPool(int poolExpectSize, int buffLen) {
    if (poolExpectSize <= 0) {
        LOGP(ERROR, "poolExpectSize <= 0\n");
        exit(-1);
    }
    for (int i = 0; i < poolExpectSize; ++i) {
        freeBuffers.push_back(new Buffer(buffLen));
    }
    this->poolSize = this->poolExpectSize = poolExpectSize;
    this->buffLen = buffLen;
}

BufferPool::~BufferPool() {
    for (Buffer *buff : this->freeBuffers) {
        delete buff;
    }
    for (Buffer *buff : this->inuseBuffers) {
        delete buff;
    }
}

Buffer * BufferPool::getBuffer() {
    LOGP(DEBUG, "getBuffer, poolSize=%d, inuse=%ld, free=%ld\n", this->poolSize, this->inuseBuffers.size(), this->freeBuffers.size());
    Buffer *buff = NULL;
    if (this->freeBuffers.size()) {
        buff = this->freeBuffers.back();
        this->freeBuffers.pop_back();
    } else {
        buff = new Buffer(this->buffLen);
        ++this->poolSize;
    }
    this->inuseBuffers.insert(buff);
    return buff;
}

void BufferPool::freeBuffer(Buffer *buff) {
    LOGP(DEBUG, "freeBuffer, poolSize=%d, inuse=%ld, free=%ld\n", this->poolSize, this->inuseBuffers.size(), this->freeBuffers.size());
    InuseBufferIterator it;
    if ((it = this->inuseBuffers.find(buff)) == this->inuseBuffers.end()) {
        LOGP(ERROR, "freeBuffer, buff not found\n");
        exit(-1);
    }
    this->inuseBuffers.erase(it);
    if (this->poolSize > this->poolExpectSize && this->poolSize / (this->freeBuffers.size() + 1) < 3) {
        delete buff;
        --this->poolSize;
        int count = this->freeBuffers.size();
        for (int i = 0; i < count/2 && this->poolSize > this->poolExpectSize; ++i) {
            buff = this->freeBuffers.back();
            this->freeBuffers.pop_back();
            delete buff;
            --this->poolSize;
        }
        LOGP(DEBUG, "freeBuffer, scale, poolSize=%d, inuse=%ld, free=%ld\n", this->poolSize, this->inuseBuffers.size(), this->freeBuffers.size());
    } else {
        this->freeBuffers.push_back(buff);
    }
    LOGP(DEBUG, "after freeBuffer, poolSize=%d, inuse=%ld, free=%ld\n", this->poolSize, this->inuseBuffers.size(), this->freeBuffers.size());
}