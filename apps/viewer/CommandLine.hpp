/**
 * @file CommandLine.hpp
 * @brief Command-line argument parsing and CLI utilities for `bgl_viewer`.
 */
#ifndef BGL_VIEWER_COMMANDLINE_HPP
#define BGL_VIEWER_COMMANDLINE_HPP

#include "viewer/ApplicationConfig.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bgl::app {

/**
 * @struct CliOptions
 * @brief Parsed `bgl_viewer` startup configuration extended with CLI execution flags.
 */
struct CliOptions : ApplicationConfig {
    bool showHelp{false};
    bool showVersion{false};
    bool listFormats{false};
    bool testCrash{false};
    int testCrashSignal{0}; ///< 0=SIGSEGV, 1=SIGABRT, 2=SIGFPE
    bool headless{false};   ///< Headless CI smoke-test mode (renders 10 frames and exits)
    std::string logLevel{"info"};

    /** @brief Parses a span of command-line arguments into a validated `CliOptions` struct. */
    [[nodiscard]] static CliOptions Parse(std::span<const char* const> args);

    /** @brief Parses `argc`/`argv` into a validated `CliOptions` struct. */
    [[nodiscard]] static CliOptions Parse(int argc, char** argv);

    /**
     * @brief Executes early-exit informational flags (`--help`, `--version`, `--formats`).
     * @param programName Executable name (typically `argv[0]`).
     * @return Process exit code (`0`) if an info command ran, or `std::nullopt` to continue.
     */
    [[nodiscard]] std::optional<int> DispatchInfoAction(std::string_view programName) const;

    /** @brief Prints CLI help generated from the declarative option table plus runtime controls. */
    static void PrintHelp(std::string_view programName);

    /** @brief Prints engine and viewer version information. */
    static void PrintVersion();

    /** @brief Prints compiled asset, texture, font, and audio support. */
    static void PrintFormats();
};

} // namespace bgl::app

#endif // BGL_VIEWER_COMMANDLINE_HPP
