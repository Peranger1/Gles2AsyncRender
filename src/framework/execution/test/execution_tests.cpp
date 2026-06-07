#include "execution_test_cases.h"

#include "framework/execution/test/test_harness.h"

#include <vector>

int main()
{
    std::vector<execution_test::TestCase> tests;
    execution_test::appendTests(tests, execution_component_test::taskSchedulerTestCases());
    execution_test::appendTests(tests, execution_component_test::executionIntegrationTestCases());
    execution_test::appendTests(tests, execution_component_test::serialLaneTestCases());
    execution_test::appendTests(tests, execution_component_test::latestLaneTestCases());
    execution_test::appendTests(tests, execution_component_test::mergeLaneTestCases());
    return execution_test::runTests(tests, "execution", "All execution tests passed.");
}
