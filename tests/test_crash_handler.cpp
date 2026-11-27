/**
 * @file test_crash_handler.cpp
 * @brief Unit tests for CrashHandler and stacktrace capturing.
 */
#include "core/CrashHandler.hpp"
#include <csignal>
#include <gtest/gtest.h>

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

TEST(CrashHandlerTest, LongStringStackTraceHeader) {
    // 1024-character stress header to verify buffer bounds safety
    std::string longReason(1024, 'X');
    core::PrintStackTrace(longReason);
    SUCCEED();
}

TEST(CrashHandlerTest, MultipleConsecutiveInstallCalls) {
    struct sigaction before{};
    ASSERT_EQ(::sigaction(SIGSEGV, nullptr, &before), 0);

    // Nested Install calls must remain idempotent and Uninstall must restore the original handler
    core::InstallCrashHandler();
    core::InstallCrashHandler();
    core::InstallCrashHandler();

    struct sigaction during{};
    ASSERT_EQ(::sigaction(SIGSEGV, nullptr, &during), 0);
    EXPECT_NE((during.sa_flags & SA_SIGINFO), 0);
    EXPECT_NE(during.sa_sigaction, nullptr);

    core::UninstallCrashHandler();

    struct sigaction after{};
    ASSERT_EQ(::sigaction(SIGSEGV, nullptr, &after), 0);
    EXPECT_EQ(after.sa_flags, before.sa_flags);
    if ((before.sa_flags & SA_SIGINFO) != 0) {
        EXPECT_EQ(after.sa_sigaction, before.sa_sigaction);
    } else {
        EXPECT_EQ(after.sa_handler, before.sa_handler);
    }
}
