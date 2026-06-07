#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace execution_test
{
// 轻量手写测试 harness：保持测试目标不依赖 QtTest/testlib。
class TestFailure final : public std::runtime_error
{
public:
    explicit TestFailure(const std::string &message)
        : std::runtime_error(message)
    {
    }
};

using TestCase = std::pair<const char *, std::function<void()>>;

// require/requireThrows 抛 TestFailure，让 runner 可以统一打印失败用例名。
inline void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw TestFailure(message);
    }
}

template <typename Exception, typename F>
void requireThrows(F &&func, const std::string &message)
{
    try {
        func();
    } catch (const Exception &) {
        return;
    } catch (const std::exception &e) {
        throw TestFailure(message + ": threw unexpected exception `" + e.what() + "`");
    } catch (...) {
        throw TestFailure(message + ": threw unexpected non-standard exception");
    }

    throw TestFailure(message + ": did not throw");
}

inline void appendTests(std::vector<TestCase> &target, std::vector<TestCase> source)
{
    // 各拆分测试文件返回自己的 TestCase 列表，聚合入口只负责拼接和运行。
    target.reserve(target.size() + source.size());
    for (TestCase &test : source) {
        target.push_back(std::move(test));
    }
}

inline int runTests(const std::vector<TestCase> &tests,
                    const char *suiteName,
                    const char *successMessage)
{
    // 不中断地运行完整 suite，方便一次看到所有失败用例。
    int failedCount = 0;
    for (const TestCase &test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
        } catch (const std::exception &e) {
            ++failedCount;
            std::cerr << "[FAIL] " << test.first << ": " << e.what() << '\n';
        } catch (...) {
            ++failedCount;
            std::cerr << "[FAIL] " << test.first << ": unknown exception\n";
        }
    }

    if (failedCount != 0) {
        std::cerr << failedCount << ' ' << suiteName << " test(s) failed.\n";
        return 1;
    }

    std::cout << successMessage << '\n';
    return 0;
}
}
