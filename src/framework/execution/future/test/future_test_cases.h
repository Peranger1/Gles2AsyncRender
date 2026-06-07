#pragma once

#include "framework/execution/test/test_harness.h"

#include <vector>

namespace async_future_test
{
// async_future_tests 按 future 子能力拆分，聚合入口按这里的声明收集用例。
std::vector<execution_test::TestCase> lifecycleTestCases();
std::vector<execution_test::TestCase> continuationTestCases();
std::vector<execution_test::TestCase> executorTestCases();
std::vector<execution_test::TestCase> combinatorTestCases();
std::vector<execution_test::TestCase> timeoutTestCases();
std::vector<execution_test::TestCase> splitterTestCases();
}
