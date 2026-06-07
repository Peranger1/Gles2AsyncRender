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
class TestFailure final : public std::runtime_error
{
public:
    explicit TestFailure(const std::string &message)
        : std::runtime_error(message)
    {
    }
};

using TestCase = std::pair<const char *, std::function<void()>>;

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
    target.reserve(target.size() + source.size());
    for (TestCase &test : source) {
        target.push_back(std::move(test));
    }
}

inline int runTests(const std::vector<TestCase> &tests,
                    const char *suiteName,
                    const char *successMessage)
{
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
