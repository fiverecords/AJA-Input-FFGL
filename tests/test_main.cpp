// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Test runner entry point. The individual TEST_CASE blocks register
// themselves through static initialisation across the other test
// translation units; main() simply asks the registry to run them
// all and exits with the count of failures as the process status
// code (zero = all green, non-zero = at least one failure).

#include "test_framework.h"

int main(int, char**)
{
    std::printf("AJA-Input-FFGL test suite\n");
    std::printf("=====================\n");
    return ::ajainput::testing::TestRegistry::Instance().RunAll();
}
