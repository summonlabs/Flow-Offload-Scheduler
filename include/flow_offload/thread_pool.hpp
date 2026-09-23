// Flow Offload Scheduler - bounded worker pool.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Locking contract
// ----------------
//  * queue_mutex_ protects queue_, stop_ and active_. It is never held while a
//    job runs and never held while another lock is acquired.
//  * shutdown() sets stop_, notifies, releases the lock and only then joins.
//    A worker therefore never needs queue_mutex_ to finish, so joining while
//    holding it cannot deadlock.
//  * try_submit() acquires exactly one lock and releases it before returning.

#ifndef FLOW_OFFLOAD_THREAD_POOL_HPP
#define FLOW_OFFLOAD_THREAD_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace flow_offload {

class ThreadPool {
 public:
  using Job = std::function<void()>;

  ThreadPool() = default;
  ThreadPool(std::uint32_t workers, std::size_t max_queue_depth);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /// Starts p workers threads. A worker count of zero makes the pool a
  /// pass-through that runs every job on the calling thread.
  void start(std::uint32_t workers, std::size_t max_queue_depth);

  /// Enqueues p job. Returns false when the queue is at its bound; the caller
  /// reports that as explicit backpressure, never as a lost job.
  bool try_submit(Job job);

  /// Signals every worker to drain and exit, then joins. Idempotent.
  void shutdown();

  [[nodiscard]] std::size_t pending() const;
  [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }
  [[nodiscard]] bool running() const noexcept { return running_; }
  /// True when jobs run on the calling thread rather than on workers.
  [[nodiscard]] bool inline_mode() const noexcept { return inline_mode_; }

 private:
  void worker_loop();

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::condition_variable queue_drained_;
  std::deque<Job> queue_;
  std::vector<std::thread> workers_;
  std::size_t max_queue_depth_{0};
  std::size_t active_{0};
  bool stop_{false};
  bool running_{false};
  bool inline_mode_{false};
};

}  // namespace flow_offload

#endif  // FLOW_OFFLOAD_THREAD_POOL_HPP
