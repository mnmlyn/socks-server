#ifndef _BUFFER_POOL_H
#define _BUFFER_POOL_H

#include "Buffer.h"
#include <vector>
#include <unordered_set>

class BufferPool {
private:
    std::vector<Buffer *> freeBuffers;
    std::unordered_set<Buffer *> inuseBuffers;
    int poolSize;
    int poolExpectSize;
    int buffLen;

public:
    BufferPool(int poolExpectSize, int buffLen);
    ~BufferPool();
    Buffer *getBuffer();
    void freeBuffer(Buffer *buff);
};

typedef std::unordered_set<Buffer *>::iterator InuseBufferIterator;

#endif