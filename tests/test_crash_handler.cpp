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

TEST(CrashHandlerTest, MultipleInstallUninstallCycles) {
    // Verify idempotence across multiple cycles
    for (int i = 0; i < 3; ++i) {
        core::InstallCrashHandler();
        core::UninstallCrashHandler();
    }
}

TEST(CrashHandlerTest, PrintStackTraceExecution) {
    // Verifies PrintStackTrace does not throw or corrupt memory
    core::PrintStackTrace("GTest stacktrace test invocation");
    SUCCEED();
}

TEST(CrashHandlerTest, PrintStackTraceSubsliceAndEmpty) {
    // Test empty reason
    core::PrintStackTrace("");

    // Test non-null-terminated string view sub-slice (security bounds test)
    std::string composite = "PREFIX_CRASH_REASON_POSTFIX";
    std::string_view slice = std::string_view(composite).substr(7, 12); // "CRASH_REASON"
    EXPECT_EQ(slice, "CRASH_REASON");
    core::PrintStackTrace(slice);
    SUCCEED();
}

