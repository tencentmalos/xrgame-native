#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <condition_variable>
#include <thread>
#include "../xrgame_profiler.h"

// Single consumer, bounded admission including the in-flight copy. Work and
// completion never run under the queue mutex. A ticket is a drawable generation,
// so a removed/reused drawable cannot revive an older queued frame.
class PresentCopyQueue {
public:
    enum Result { Failed = -1, Copied = 0, Skipped = 1 };
    using Ticket = std::shared_ptr<std::atomic<bool>>;
    using Work = std::function<Result(const Ticket&)>;
    using Completion = std::function<void(Result)>;
    // Submission runs on the renderer; retirement waits on a separate consumer.
    // A non-empty retirement must prove GPU completion before returning Copied.
    using Retirement = std::function<Result()>;
    using Submit = std::function<Retirement(const Ticket&)>;

    explicit PresentCopyQueue(size_t capacity = 8) : capacity_(capacity) {}
    ~PresentCopyQueue() { shutdown(); }

    bool push(int64_t id, Work work, Completion complete) {
        return enqueue(id, std::move(work), {}, std::move(complete));
    }

    bool pushDeferred(int64_t id, Submit submit, Completion complete) {
        return enqueue(id, {}, std::move(submit), std::move(complete));
    }

private:
    bool enqueue(int64_t id, Work work, Submit submit, Completion complete) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || failed_ || outstanding_ >= capacity_) return false;
        auto& ticket = tickets_[id];
        if (!ticket) ticket = std::make_shared<std::atomic<bool>>(true);
        jobs_.push_back({ticket, std::move(work), std::move(submit), {}, std::move(complete)});
#ifdef XRGAME_PROFILE
        // Carry the real accepted job's identity; no extra callback wrapper/allocation.
        jobs_.back().profile = xrgame_profile_region("host.present.queue_to_complete");
#endif
        ++outstanding_;
        return true;
    }

public:
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
            try {
                if (job.submit) {
                    // Start the worker before submission, so thread creation cannot
                    // strand already submitted GPU work on allocation failure.
                    if (!retireThread_.joinable()) retireThread_ = std::thread([this] { retireLoop(); });
                    job.retire = job.submit(job.ticket);
                    if (job.retire) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        retiredJobs_.push_back(std::move(job));
                        retireCV_.notify_one();
                        return true;
                    }
                    result = Failed;
                } else result = job.work(job.ticket);
            }
            catch (...) {
                // If retirement queue allocation failed after submission, fall
                // back to waiting here. Never abandon the submitted read.
                try { result = job.retire ? job.retire() : Failed; }
                catch (...) { result = Failed; }
            }
        }
        finish(job, result);
        return true;
    }

    // Owner must stop/join the submission consumer first. Drain unsubmitted jobs,
    // then join GPU retirement before destroying Vulkan or JNI state.
    void shutdown() {
        close();
        while (runOne()) {}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            retireClosed_ = true;
        }
        retireCV_.notify_all();
        if (retireThread_.joinable()) retireThread_.join();
    }

private:
    struct Job {
        Ticket ticket; Work work; Submit submit; Retirement retire; Completion complete;
#ifdef XRGAME_PROFILE
        XrProfileRegion profile{};
#endif
    };

    void finish(Job& job, Result result) {
        if (result == Copied && !job.ticket->load()) result = Skipped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result == Failed) {
                failed_ = true;
                for (auto& entry : tickets_) entry.second->store(false);
            }
        }
#ifdef XRGAME_PROFILE
        xrgame_profile_region_end(job.profile);
#endif
        try { XrProfileScope profile("host.present.complete", true); job.complete(result); } catch (...) {}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --outstanding_;
        }
    }

    void retireLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                retireCV_.wait(lock, [this] { return retireClosed_ || !retiredJobs_.empty(); });
                if (retiredJobs_.empty()) return;
                job = std::move(retiredJobs_.front());
                retiredJobs_.pop_front();
            }
            Result result = Failed;
            // Invalidation cannot skip a wait after submission.
            try { result = job.retire(); } catch (...) {}
            finish(job, result);
        }
    }

public:
    // The owner joins the consumer before draining. Already submitted work must
    // finish its fence wait; invalidation alone never makes its AHB reusable.
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        for (auto& entry : tickets_) entry.second->store(false);
        tickets_.clear();
    }

private:
    const size_t capacity_;
    size_t outstanding_ = 0;
    bool closed_ = false, failed_ = false;
    std::mutex mutex_;
    std::deque<Job> jobs_;
    std::unordered_map<int64_t, Ticket> tickets_;
    std::deque<Job> retiredJobs_;
    std::condition_variable retireCV_;
    std::thread retireThread_;
    bool retireClosed_ = false;
};
