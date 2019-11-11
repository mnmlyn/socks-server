#ifndef _BUFFER_H
#define _BUFFER_H

class Buffer {
public:
    char *head;
    int len;
    int dataLen;

public:
    Buffer(int len);
    char *getStart();
    int getRemainLen();
    void push(int n);
    void empty();
    void pop(int n);
    bool isEmpty();
    ~Buffer();

    void print();
};



#endif