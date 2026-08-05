# pragma once

#include<memory>
#include<mutex>
#include<condition_variable>

template<typename T>
class LockQueue {
private:
    struct Node {
        std::shared_ptr<T> data;
        std::unique_ptr<Node> next;
    };

    std::unique_ptr<Node> head;
    Node* tail;
    mutable std::mutex head_mutex;
    mutable std::mutex tail_mutex;
    std::condition_variable cv;

    Node* get_tail() {
        std::lock_guard<std::mutex> lk_tail(tail_mutex);
        return tail;
    }

public:
    LockQueue(): head(new Node), tail(head.get()) {}
    LockQueue(const LockQueue& other) = delete;
    LockQueue& operator=(const LockQueue& other) = delete;

    void push(T data);
    void wait_and_pop(T& result);
    std::shared_ptr<T> wait_and_pop();
    bool try_pop(T& result);
    std::shared_ptr<T> try_pop();

    bool empty() const;
};

template<typename T>
void LockQueue<T>::push(T data) {
    std::shared_ptr<T> new_data(std::make_shared<T>(std::move(data)));
    std::unique_ptr<Node> p(new Node);
    {
        std::lock_guard<std::mutex> lk(tail_mutex);
        tail->data = new_data;
        Node* const new_tail = p.get();
        tail->next = std::move(p);
        tail = new_tail;
    }
    cv.notify_one();
}

template<typename T>
void LockQueue<T>::wait_and_pop(T& result) {
    std::unique_lock<std::mutex> lk(head_mutex);
    cv.wait(lk, [&]{return get_tail()!=head.get();});
    result = std::move(*(head->data));
    std::unique_ptr<Node> old_head = std::move(head);
    head = std::move(old_head->next);
}

template<typename T>
std::shared_ptr<T> LockQueue<T>::wait_and_pop() {
    std::unique_lock<std::mutex> lk(head_mutex);
    cv.wait(lk, [&]{return get_tail()!=head.get();});
    std::unique_ptr<Node> old_head = std::move(head);
    head = std::move(old_head->next);
    return old_head->data;
}

template<typename T>
bool LockQueue<T>::try_pop(T& result) {
    std::lock_guard<std::mutex> lk(head_mutex);
    {
        std::unique_lock<std::mutex> lk_tail(tail_mutex);
        if (head.get()==tail) return false;
    }
    result = std::move(*(head->data));
    std::unique_ptr<Node> old_head = std::move(head);
    head = std::move(old_head->next);
    return true;
}

template<typename T>
std::shared_ptr<T> LockQueue<T>::try_pop() {
    std::lock_guard<std::mutex> lk(head_mutex);
    {
        std::unique_lock<std::mutex> lk_tail(tail_mutex);
        if (head.get()==tail) return std::shared_ptr<T>();
    }
    std::unique_ptr<Node> old_head = std::move(head);
    head = std::move(old_head->next);
    return old_head->data;
}

template<typename T>
bool LockQueue<T>::empty() const {
    std::lock_guard<std::mutex> lk(head_mutex);
    std::lock_guard<std::mutex> lk_tail(tail_mutex);
    return head.get()==tail;
}
