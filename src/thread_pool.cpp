// Flow Offload Scheduler - bounded worker pool.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "flow_offload/thread_pool.hpp"

namespace flow_offload {

ThreadPool::ThreadPool(std::uint32_t workers, std::size_t max_queue_depth) {
  start(workers, max_queue_depth);
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::start(std::uint32_t workers, std::size_t max_queue_depth) {
  shutdown();
  std::lock_guard<std::mutex> lock(queue_mutex_);
  max_queue_depth_ = max_queue_depth == 0 ? 1 : max_queue_depth;
  stop_ = false;
  running_ = true;
  queue_.clear();
  active_ = 0;
  if (workers == 0) {
    inline_mode_ = true;
    return;
  }
  inline_mode_ = false;
  workers_.reserve(workers);
  for (std::uint32_t i = 0; i < workers; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

bool ThreadPool::try_submit(Job job) {
  if (!job) {
    return false;
  }
  bool run_inline = false;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (inline_mode_) {
      run_inline = true;
    } else {
      if (!running_ || stop_) {
        return false;
      }
      if (queue_.size() >= max_queue_depth_) {
        return false;
      }
      queue_.push_back(std::move(job));
      queue_ready_.notify_one();
      return true;
    }
  }
  // Inline execution happens outside the lock so a job may freely touch the
  // pool's own accounting.
  job();
  return true;
}

void ThreadPool::worker_loop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_ready_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) {
        queue_drained_.notify_all();
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
      ++active_;
    }
    job();
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      --active_;
      if (queue_.empty() && active_ == 0) {
        queue_drained_.notify_all();
      }
    }
  }
}

void ThreadPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stop_ = true;
  }
  queue_ready_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  // Safety net: a job queued in the instant before the stop flag was observed is
  // still drained rather than dropped.
  for (;;) {
    Job job;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (queue_.empty()) {
        break;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    job();
  }
  std::lock_guard<std::mutex> lock(queue_mutex_);
  queue_.clear();
  active_ = 0;
  running_ = false;
  stop_ = false;
  inline_mode_ = false;
}

std::size_t ThreadPool::pending() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return queue_.size();
}

}  // namespace flow_offload
