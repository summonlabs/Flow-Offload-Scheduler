// Flow Offload Scheduler - self-contained test harness.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FOS_TEST_HARNESS_HPP
#define FOS_TEST_HARNESS_HPP

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace fostest {

struct TestCase {
  std::string name;
  void (*fn)();
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, void (*fn)());
};

/// Runs every registered test. Returns 0 only when all of them passed.
int run_all(int argc, char** argv);

extern std::uint64_t g_checks;
extern std::uint64_t g_failures;

/// Absolute path of the auxiliary executable passed as the first argument (the
/// service daemon). Empty when the test was run without one.
extern std::string g_auxiliary_binary;
/// Directory containing the first-party executables under test.
extern std::string g_binary_directory;

void report_failure(const char* file, int line, const std::string& message);

/// Deterministic pseudorandom generator used by the property tests. The
/// algorithm is fixed, so a seed reproduces a run exactly on any platform.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed == 0U ? 0x9E3779B97F4A7C15ULL : seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    return bound == 0U ? 0U : next() % bound;
  }

  [[nodiscard]] bool chance(std::uint32_t percent) noexcept { return below(100U) < percent; }

 private:
  std::uint64_t state_;
};

}  // namespace fostest

#define FOS_TEST(name)                                    \
  static void name();                                     \
  static ::fostest::Registrar fos_registrar_##name(#name, &name); \
  static void name()

#define FOS_CHECK(condition)                                                          \
  do {                                                                                \
    ++::fostest::g_checks;                                                            \
    if (!(condition)) {                                                               \
      ::fostest::report_failure(__FILE__, __LINE__, std::string{"check failed: " #condition}); \
    }                                                                                 \
  } while (false)

#define FOS_CHECK_MSG(condition, message)                                             \
  do {                                                                                \
    ++::fostest::g_checks;                                                            \
    if (!(condition)) {                                                               \
      std::ostringstream fos_stream;                                                  \
      fos_stream << "check failed: " #condition << " -- " << message;                 \
      ::fostest::report_failure(__FILE__, __LINE__, fos_stream.str());                \
    }                                                                                 \
  } while (false)

#define FOS_CHECK_EQ(lhs, rhs)                                                        \
  do {                                                                                \
    ++::fostest::g_checks;                                                            \
    const auto& fos_lhs = (lhs);                                                      \
    const auto& fos_rhs = (rhs);                                                      \
    if (!(fos_lhs == fos_rhs)) {                                                      \
      std::ostringstream fos_stream;                                                  \
      fos_stream << "check failed: " #lhs " == " #rhs;                                \
      ::fostest::report_failure(__FILE__, __LINE__, fos_stream.str());                \
    }                                                                                 \
  } while (false)

#define FOS_REQUIRE(condition)                                                        \
  do {                                                                                \
    ++::fostest::g_checks;                                                            \
    if (!(condition)) {                                                               \
      ::fostest::report_failure(__FILE__, __LINE__,                                   \
                                std::string{"requirement failed: " #condition});       \
      return;                                                                         \
    }                                                                                 \
  } while (false)

#endif  // FOS_TEST_HARNESS_HPP
