/**
 * @file CrashHandler.hpp
 * @brief Crash interception, POSIX signal handling, and stacktrace diagnostics.
 */
#pragma once

#include <string_view>

namespace bgl::core {

/**
 * @brief Installs POSIX signal handlers for fatal crash signals.
 *
 * Intercepts SIGSEGV, SIGABRT, SIGFPE, SIGILL, and SIGBUS to output a
 * full diagnostic report, process telemetry, and stacktrace before
 * passing control back to the operating system.
 */
void InstallCrashHandler() noexcept;

/**
 * @brief Restores default OS signal handlers for crash signals.
 */
void UninstallCrashHandler() noexcept;

/**
 * @brief Captures and prints the current callstack to standard error.
 * @param reason Optional human-readable message or header.
 */
void PrintStackTrace(std::string_view reason = "") noexcept;

/**
 * @brief Deliberately triggers an intentional crash for testing signal handling.
 * @param signalType 0 for SIGSEGV (null dereference), 1 for SIGABRT, 2 for SIGFPE.
 */
[[noreturn]] void TriggerTestCrash(int signalType = 0);

} // namespace bgl::core

