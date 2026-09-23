// Flow Offload Scheduler - self-contained test harness.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "harness.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace fostest {

std::uint64_t g_checks = 0;
std::uint64_t g_failures = 0;
std::string g_auxiliary_binary;
std::string g_binary_directory;

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registrar::Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }

void report_failure(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message.c_str());
  std::fflush(stderr);
}

int run_all(int argc, char** argv) {
  const char* filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
      filter = argv[i + 1];
      ++i;
      continue;
    }
    if (argv[i][0] != '-') {
      g_auxiliary_binary = argv[i];
      const std::size_t slash = g_auxiliary_binary.find_last_of("\\/");
      g_binary_directory = slash == std::string::npos ? std::string{"."}
                                                      : g_auxiliary_binary.substr(0, slash);
    }
  }
  std::uint64_t executed = 0;
  std::uint64_t failed_cases = 0;
  for (const TestCase& test : registry()) {
    if (filter != nullptr && test.name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    const std::uint64_t before = g_failures;
    std::printf("[ RUN  ] %s\n", test.name.c_str());
    std::fflush(stdout);
    try {
      test.fn();
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__,
                     std::string{"unhandled exception: "} + error.what() + " in " + test.name);
    } catch (...) {
      report_failure(__FILE__, __LINE__, std::string{"unhandled non-standard exception in "} + test.name);
    }
    if (g_failures != before) {
      ++failed_cases;
      std::printf("[ FAIL ] %s\n", test.name.c_str());
    } else {
      std::printf("[  OK  ] %s\n", test.name.c_str());
    }
    std::fflush(stdout);
  }
  std::printf("---- %llu checks, %llu failures, %llu/%llu cases failed ----\n",
              static_cast<unsigned long long>(g_checks),
              static_cast<unsigned long long>(g_failures),
              static_cast<unsigned long long>(failed_cases),
              static_cast<unsigned long long>(executed));
  std::fflush(stdout);
  if (executed == 0) {
    std::fprintf(stderr, "no tests were executed\n");
    return 2;
  }
  return g_failures == 0 ? 0 : 1;
}

}  // namespace fostest

int main(int argc, char** argv) { return fostest::run_all(argc, argv); }
