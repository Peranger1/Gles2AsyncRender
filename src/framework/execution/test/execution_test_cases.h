#pragma once

#include "framework/execution/test/test_harness.h"

#include <vector>

namespace execution_component_test
{
// execution_tests 按组件拆分，聚合入口按这里的声明收集用例。
std::vector<execution_test::TestCase> taskSchedulerTestCases();
std::vector<execution_test::TestCase> executionIntegrationTestCases();
std::vector<execution_test::TestCase> serialLaneTestCases();
std::vector<execution_test::TestCase> latestLaneTestCases();
std::vector<execution_test::TestCase> mergeLaneTestCases();
}
