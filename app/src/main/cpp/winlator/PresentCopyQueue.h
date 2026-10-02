#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

// Single consumer, bounded admission including the in-flight copy. Work and
// completion never run under the queue mutex. A ticket is a drawable generation,
// so a removed/reused drawable cannot revive an older queued frame.
class PresentCopyQueue {
public:
    enum Result { Failed = -1, Copied = 0, Skipped = 1 };
    using Ticket = std::shared_ptr<std::atomic<bool>>;
    using Work = std::function<Result(const Ticket&)>;
    using Completion = std::function<void(Result)>;

    explicit PresentCopyQueue(size_t capacity = 8) : capacity_(capacity) {}

    bool push(int64_t id, Work work, Completion complete) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || failed_ || outstanding_ >= capacity_) return false;
        auto& ticket = tickets_[id];
        if (!ticket) ticket = std::make_shared<std::atomic<bool>>(true);
        jobs_.push_back({ticket, std::move(work), std::move(complete)});
        ++outstanding_;
        return true;
    }

    bool hasWork() {
        std::lock_guard<std::mutex> lock(mutex_);
        return !jobs_.empty();
    }

    void invalidate(int64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = tickets_.find(id);
        if (found != tickets_.end()) {
            found->second->store(false);
            tickets_.erase(found);
        }
    }

    bool runOne() {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (jobs_.empty()) return false;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Result result = Skipped;
        if (job.ticket->load()) {
            try { result = job.work(job.ticket); }
            catch (...) { result = Failed; }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result == Failed) {
                failed_ = true;
                for (auto& entry : tickets_) entry.second->store(false);
            }
        }
        try { job.complete(result); } catch (...) {}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --outstanding_;
        }
        return true;
    }

    // The owner joins the consumer before draining. Already submitted work must
    // finish its fence wait; invalidation alone never makes its AHB reusable.
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        for (auto& entry : tickets_) entry.second->store(false);
        tickets_.clear();
    }

private:
    struct Job { Ticket ticket; Work work; Completion complete; };
    const size_t capacity_;
    size_t outstanding_ = 0;
    bool closed_ = false, failed_ = false;
    std::mutex mutex_;
    std::deque<Job> jobs_;
    std::unordered_map<int64_t, Ticket> tickets_;
};
