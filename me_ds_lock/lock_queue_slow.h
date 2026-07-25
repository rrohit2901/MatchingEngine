#pragma once

#include <mutex>
#include <queue>
#include <memory>
#include <condition_variable>

template<typename T>
class LockQueueSlow {
private:
    std::queue<std::shared_ptr<T> > data;
    std::mutex mtx;
    std::condition_variable cv;
public:
    void push(T data);
    void wait_and_pop(T& result);
    std::shared_ptr<T> wait_and_pop();
    bool try_pop(T& result);
    std::shared_ptr<T> try_pop();
};

template<typename T>
void LockQueueSlow<T>::push(T data) {

}
