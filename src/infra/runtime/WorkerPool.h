#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rbank::infra {

/// 固定线程数的任务池。队列满即拒绝任务，让接入层立刻回报「银行繁忙」。
class WorkerPool {
    std::mutex                        mutex;
    std::condition_variable           available;
    std::deque<std::function<void()>> queue;
    std::vector<std::thread>          threads;
    size_t                            capacity;
    bool                              stopping = false;

public:
    WorkerPool(unsigned count, size_t maxQueue);
    ~WorkerPool();

    bool submit(std::function<void()> task);
    void stop();
};

} // namespace rbank::infra
