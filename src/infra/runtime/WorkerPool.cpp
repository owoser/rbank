#include "infra/runtime/WorkerPool.h"

#include <algorithm>

namespace rbank::infra {

WorkerPool::WorkerPool(unsigned count, size_t maxQueue) : capacity(maxQueue) {
    for (unsigned i = 0; i < std::max(count, 1u); ++i) {
        threads.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mutex);
                    available.wait(lock, [this] { return stopping || !queue.empty(); });
                    if (stopping && queue.empty()) {
                        return;
                    }

                    task = std::move(queue.front());
                    queue.pop_front();
                }

                try {
                    task();
                } catch (...) {
                    // 任务自带错误呈现；工作线程必须存活。
                }
            }
        });
    }
}

WorkerPool::~WorkerPool() { stop(); }

bool WorkerPool::submit(std::function<void()> task) {
    {
        std::lock_guard lock(mutex);
        if (stopping || queue.size() >= capacity) {
            return false;
        }

        queue.push_back(std::move(task));
    }

    available.notify_one();

    return true;
}

void WorkerPool::stop() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
        queue.clear();
    }

    available.notify_all();
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads.clear();
}

} // namespace rbank::infra
