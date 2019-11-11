#include "Buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Buffer::Buffer(int len) {
    if (len <= 0) {
        // bug
        perror("Buffer len <= 0\n");
        exit(-1);
    }
    this->head = new char(len);
    this->len = len;
    this->dataLen = 0;
}

Buffer::~Buffer() {
    delete this->head;
}

char *Buffer::getStart() {
    return this->head + this->dataLen;
}

int Buffer::getRemainLen() {
    return this->len - this->dataLen;
}

void Buffer::push(int n) {
    if (n <= 0) {
        // bug
        perror("Buffer push n <= 0\n");
        exit(-1);
    }
    if (n > this->getRemainLen()) {
        // bug
        perror("Buffer push Overflow\n");
        exit(-1);
    }
    this->dataLen += n;
}

void Buffer::empty() {
    this->dataLen = 0;
}

void Buffer::pop(int n) {
    if (n <= 0) {
        // bug
        perror("Buffer pop n <= 0\n");
        exit(-1);
    }
    if (n > this->dataLen) {
        // bug
        perror("Buffer pop Overflow\n");
        exit(-1);
    }
    if (n == this->dataLen) {
        this->empty();
    } else {
        this->dataLen -= n;
        memmove(this->head, this->head + n, this->dataLen);
    }
}

bool Buffer::isEmpty() {
    return this->dataLen == 0;
}