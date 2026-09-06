#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bh61::test {

using TestFunction = std::function<void()>;

struct TestCase {
  std::string name;
  TestFunction function;
};

inline auto registry() -> std::vector<TestCase>& {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string name, TestFunction function) {
    registry().push_back({std::move(name), std::move(function)});
  }
};

inline void require(bool condition, std::string_view expression,
                    std::string_view file, int line) {
  if (condition) {
    return;
  }
  std::ostringstream message;
  message << file << ':' << line << ": requirement failed: " << expression;
  throw std::runtime_error(message.str());
}

inline auto run_all() -> int {
  std::size_t failures = 0;
  for (const auto& test : registry()) {
    try {
      test.function();
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": unknown exception\n";
    }
  }
  std::cout << (registry().size() - failures) << '/' << registry().size()
            << " tests passed\n";
  return failures == 0 ? 0 : 1;
}

}  // namespace bh61::test

#define BH61_TEST_JOIN_IMPL(a, b) a##b
#define BH61_TEST_JOIN(a, b) BH61_TEST_JOIN_IMPL(a, b)
#define BH61_TEST(name)                                                        \
  static void BH61_TEST_JOIN(bh61_test_, __LINE__)();                          \
  static ::bh61::test::Registrar BH61_TEST_JOIN(bh61_registrar_, __LINE__)(    \
      name, BH61_TEST_JOIN(bh61_test_, __LINE__));                             \
  static void BH61_TEST_JOIN(bh61_test_, __LINE__)()
#define BH61_REQUIRE(expression)                                                \
  ::bh61::test::require(static_cast<bool>(expression), #expression, __FILE__,  \
                        __LINE__)
