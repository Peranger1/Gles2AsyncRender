#include "future_test_cases.h"

#include "framework/execution/test/test_harness.h"

#include <vector>

// async_future_tests 的聚合入口；具体测试目的写在各拆分测试文件中。

int main()
{
    std::vector<execution_test::TestCase> tests;
    execution_test::appendTests(tests, async_future_test::lifecycleTestCases());
    execution_test::appendTests(tests, async_future_test::continuationTestCases());
    execution_test::appendTests(tests, async_future_test::executorTestCases());
    execution_test::appendTests(tests, async_future_test::combinatorTestCases());
    execution_test::appendTests(tests, async_future_test::timeoutTestCases());
    execution_test::appendTests(tests, async_future_test::splitterTestCases());
    return execution_test::runTests(tests, "async future", "All async future tests passed.");
}
