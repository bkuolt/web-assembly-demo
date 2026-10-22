/**
 * @file test_crash_handler.cpp
 * @brief Unit tests for CrashHandler and stacktrace capturing.
 */
#include <gtest/gtest.h>
#include "core/CrashHandler.hpp"

using namespace bgl;

TEST(CrashHandlerTest, InstallAndUninstall) {
    // Should execute cleanly without error or exception
    core::InstallCrashHandler();
    core::UninstallCrashHandler();
}

TEST(CrashHandlerTest, PrintStackTraceExecution) {
    // Verifies PrintStackTrace does not throw or corrupt memory
    core::PrintStackTrace("GTest stacktrace test invocation");
    SUCCEED();
}

