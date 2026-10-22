/**
 * @file test_main.cpp
 * @brief GTest main entrypoint.
 */
#include <gtest/gtest.h>
#include "core/Log.hpp"

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    bgl::core::InitLogging("warn");
    return RUN_ALL_TESTS();
}

