// Flow Offload Scheduler - independent OS process support for tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FOS_TEST_PROCESS_HPP
#define FOS_TEST_PROCESS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace fostest {

/// A child process whose standard output is captured through an anonymous pipe
/// and whose standard input is connected to a pipe the parent owns.
///
/// Synchronisation is by stream completion: read_line() blocks until the child
/// writes a line or closes the pipe, and wait_for_exit() blocks until the child
/// terminates. No polling and no timeouts are used anywhere.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Launches \p executable with \p arguments. Returns an empty string on
  /// success, or a human-readable error.
  std::string launch(const std::string& executable, const std::vector<std::string>& arguments);

  /// Reads one newline-terminated line from the child's standard output.
  /// Returns false at end of stream, which is what makes a failed launch fail a
  /// test instead of hanging it.
  bool read_line(std::string& line);

  /// Blocks until the child exits. Returns false if that could not be observed.
  bool wait_for_exit(int& exit_code);

  /// Terminates the child immediately, without giving it a chance to clean up.
  void kill();

  /// Closing the child's standard input is an orderly end-of-input signal.
  void close_stdin();

  [[nodiscard]] bool running() const noexcept { return running_; }

 private:
  void release();

  void* process_{nullptr};
  void* thread_{nullptr};
  void* stdout_read_{nullptr};
  void* stdin_write_{nullptr};
  bool running_{false};
#if !defined(_WIN32)
  long long pid_{0};
#endif
};

/// Creates a fresh empty directory under the system temporary directory and
/// returns its path. Used so tests never share store state.
std::string make_temp_directory(const std::string& prefix);

/// Removes a directory tree. Best effort.
void remove_directory(const std::string& path);

}  // namespace fostest

#endif  // FOS_TEST_PROCESS_HPP
