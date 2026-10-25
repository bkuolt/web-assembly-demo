/**
 * @file test_main.cpp
 * @brief GTest main entrypoint.
 */
#include "core/Log.hpp"
#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    bgl::core::InitLogging("warn");
    return RUN_ALL_TESTS();
}
