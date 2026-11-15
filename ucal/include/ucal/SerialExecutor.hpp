#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace Ucal::Backend {

// Own all calls into one vendor runtime on a single thread. Process isolation
// supplies the deadline; a stuck vendor call must never be detached in-process.
class SerialExecutor final {
  public:
    SerialExecutor()
        : thread_([this] {
              for (;;) {
                  std::function<void()> task;
                  {
                      std::unique_lock<std::mutex> lock(mutex_);
                      changed_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                      if (tasks_.empty())
                          return;
                      task = std::move(tasks_.front());
                      tasks_.pop_front();
                  }
                  task();
              }
          }) {
    }
    ~SerialExecutor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        thread_.join();
    }
    SerialExecutor(const SerialExecutor &) = delete;
    SerialExecutor &operator=(const SerialExecutor &) = delete;

    template <class Function> auto invoke(Function &&function) -> decltype(function()) {
        if (std::this_thread::get_id() == thread_.get_id())
            return function();
        using Result = decltype(function());
        auto task =
            std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.emplace_back([task] { (*task)(); });
        }
        changed_.notify_one();
        return future.get();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_{};
    std::thread thread_;
};
} // namespace Ucal::backend
