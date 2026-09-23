// Flow Offload Scheduler - independent OS process support for tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "support/process.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fostest {
namespace {

std::string quote(const std::string& value) {
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

}  // namespace

#if defined(_WIN32)

ChildProcess::~ChildProcess() { release(); }

void ChildProcess::release() {
  if (stdout_read_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdout_read_));
    stdout_read_ = nullptr;
  }
  if (stdin_write_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdin_write_));
    stdin_write_ = nullptr;
  }
  if (thread_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(thread_));
    thread_ = nullptr;
  }
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
  running_ = false;
}

std::string ChildProcess::launch(const std::string& executable,
                                 const std::vector<std::string>& arguments) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE child_stdout_read = nullptr;
  HANDLE child_stdout_write = nullptr;
  if (!::CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0)) {
    return "CreatePipe failed";
  }
  if (!::SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
    ::CloseHandle(child_stdout_read);
    ::CloseHandle(child_stdout_write);
    return "SetHandleInformation failed";
  }
  HANDLE child_stdin_read = nullptr;
  HANDLE child_stdin_write = nullptr;
  if (!::CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0)) {
    ::CloseHandle(child_stdout_read);
    ::CloseHandle(child_stdout_write);
    return "CreatePipe failed";
  }
  if (!::SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
    ::CloseHandle(child_stdout_read);
    ::CloseHandle(child_stdout_write);
    ::CloseHandle(child_stdin_read);
    ::CloseHandle(child_stdin_write);
    return "SetHandleInformation failed";
  }

  std::string command = quote(executable);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote(argument));
  }

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = child_stdout_write;
  startup.hStdError = child_stdout_write;
  startup.hStdInput = child_stdin_read;
  PROCESS_INFORMATION info{};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  const BOOL created =
      ::CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &info);
  ::CloseHandle(child_stdout_write);
  ::CloseHandle(child_stdin_read);
  if (!created) {
    ::CloseHandle(child_stdout_read);
    ::CloseHandle(child_stdin_write);
    return "CreateProcess failed";
  }
  stdout_read_ = child_stdout_read;
  stdin_write_ = child_stdin_write;
  process_ = info.hProcess;
  thread_ = info.hThread;
  running_ = true;
  return std::string{};
}

bool ChildProcess::read_line(std::string& line) {
  line.clear();
  if (stdout_read_ == nullptr) {
    return false;
  }
  for (;;) {
    char ch = 0;
    DWORD read = 0;
    const BOOL ok = ::ReadFile(static_cast<HANDLE>(stdout_read_), &ch, 1, &read, nullptr);
    if (!ok || read == 0) {
      return !line.empty();
    }
    if (ch == '\n') {
      return true;
    }
    if (ch != '\r') {
      line.push_back(ch);
    }
  }
}

bool ChildProcess::wait_for_exit(int& exit_code) {
  if (process_ == nullptr) {
    return false;
  }
  const DWORD waited = ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  if (waited != WAIT_OBJECT_0) {
    return false;
  }
  DWORD code = 0;
  if (!::GetExitCodeProcess(static_cast<HANDLE>(process_), &code)) {
    return false;
  }
  exit_code = static_cast<int>(code);
  running_ = false;
  return true;
}

void ChildProcess::kill() {
  if (process_ != nullptr) {
    ::TerminateProcess(static_cast<HANDLE>(process_), 137);
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    running_ = false;
  }
}

void ChildProcess::close_stdin() {
  if (stdin_write_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdin_write_));
    stdin_write_ = nullptr;
  }
}

#else

ChildProcess::~ChildProcess() { release(); }

void ChildProcess::release() {
  if (stdout_read_ >= 0) {
    ::close(stdout_read_);
    stdout_read_ = -1;
  }
  if (stdin_write_ >= 0) {
    ::close(stdin_write_);
    stdin_write_ = -1;
  }
  running_ = false;
}

std::string ChildProcess::launch(const std::string& executable,
                                 const std::vector<std::string>& arguments) {
  int out_pipe[2] = {-1, -1};
  int in_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0 || ::pipe(in_pipe) != 0) {
    return "pipe failed";
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    return "fork failed";
  }
  if (pid == 0) {
    ::dup2(in_pipe[0], 0);
    ::dup2(out_pipe[1], 1);
    ::dup2(out_pipe[1], 2);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(out_pipe[1]);
  ::close(in_pipe[0]);
  stdout_read_ = out_pipe[0];
  stdin_write_ = in_pipe[1];
  pid_ = pid;
  running_ = true;
  return std::string{};
}

bool ChildProcess::read_line(std::string& line) {
  line.clear();
  if (stdout_read_ < 0) {
    return false;
  }
  for (;;) {
    char ch = 0;
    const ssize_t read = ::read(stdout_read_, &ch, 1);
    if (read <= 0) {
      return !line.empty();
    }
    if (ch == '\n') {
      return true;
    }
    if (ch != '\r') {
      line.push_back(ch);
    }
  }
}

bool ChildProcess::wait_for_exit(int& exit_code) {
  if (pid_ <= 0) {
    return false;
  }
  int status = 0;
  if (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) {
    return false;
  }
  exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  running_ = false;
  return true;
}

void ChildProcess::kill() {
  if (pid_ > 0) {
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    running_ = false;
  }
}

void ChildProcess::close_stdin() {
  if (stdin_write_ >= 0) {
    ::close(stdin_write_);
    stdin_write_ = -1;
  }
}

#endif

std::string make_temp_directory(const std::string& prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::filesystem::path base = std::filesystem::temp_directory_path();
  for (int attempt = 0; attempt < 64; ++attempt) {
    std::filesystem::path candidate =
        base / (prefix + "-" + std::to_string(static_cast<long long>(now)) + "-" +
                std::to_string(attempt));
    std::error_code ec;
    if (std::filesystem::create_directories(candidate, ec) && !ec) {
      return candidate.string();
    }
  }
  return std::string{};
}

void remove_directory(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

}  // namespace fostest
