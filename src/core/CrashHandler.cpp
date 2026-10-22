/**
 * @file CrashHandler.cpp
 * @brief Crash interception, POSIX signal handling, and stacktrace diagnostics.
 */
#include "core/CrashHandler.hpp"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unistd.h>

#if defined(__linux__) || defined(__unix__)
#include <execinfo.h>
#include <dlfcn.h>
#include <cxxabi.h>
#include <sys/syscall.h>
#include <sys/types.h>
#endif

#if defined(BGL_HAS_STD_STACKTRACE)
#include <stacktrace>
#include <iostream>
#endif

namespace bgl::core {

namespace {

#if defined(__linux__) || defined(__unix__)
static stack_t g_altStack{};
static void* g_altStackMemory = nullptr;
static constexpr size_t kAltStackSize = 64 * 1024; // 64 KB

static struct sigaction g_oldActionSegv{};
static struct sigaction g_oldActionAbrt{};
static struct sigaction g_oldActionFpe{};
static struct sigaction g_oldActionIll{};
static struct sigaction g_oldActionBus{};

static std::atomic<bool> g_isHandlingCrash{false};

/**
 * @brief Async-signal-safe string length.
 */
size_t SafeStrLen(const char* str) noexcept {
    if (!str) return 0;
    size_t len = 0;
    while (str[len] != '\0') ++len;
    return len;
}

/**
 * @brief Async-signal-safe stderr writer.
 */
void SafeWriteStderr(const char* str) noexcept {
    if (!str) return;
    size_t len = SafeStrLen(str);
    while (len > 0) {
        ssize_t written = ::write(STDERR_FILENO, str, len);
        if (written <= 0) break;
        str += written;
        len -= static_cast<size_t>(written);
    }
}

/**
 * @brief Async-signal-safe pointer-to-hex formatter.
 */
void SafeFormatHex(uintptr_t val, char* out, size_t outSize) noexcept {
    if (outSize < 19) return;
    out[0] = '0';
    out[1] = 'x';
    const char hexDigits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        int shift = (15 - i) * 4;
        out[2 + i] = hexDigits[(val >> shift) & 0x0F];
    }
    out[18] = '\0';
}

/**
 * @brief Async-signal-safe integer-to-decimal formatter.
 */
void SafeFormatDec(int64_t val, char* out, size_t outSize) noexcept {
    if (outSize < 22) return;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    bool neg = val < 0;
    uint64_t uval = neg ? static_cast<uint64_t>(-val) : static_cast<uint64_t>(val);
    char tmp[24];
    size_t idx = 0;
    while (uval > 0) {
        tmp[idx++] = static_cast<char>('0' + (uval % 10));
        uval /= 10;
    }
    size_t outIdx = 0;
    if (neg) out[outIdx++] = '-';
    while (idx > 0) {
        out[outIdx++] = tmp[--idx];
    }
    out[outIdx] = '\0';
}

const char* GetSignalName(int sig) noexcept {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation Fault / Invalid Memory Reference)";
        case SIGABRT: return "SIGABRT (Process Aborted / Assertion Failure)";
        case SIGFPE:  return "SIGFPE  (Erroneous Arithmetic Operation / Divide by Zero)";
        case SIGILL:  return "SIGILL  (Illegal Hardware Instruction)";
        case SIGBUS:  return "SIGBUS  (Bus Error / Misaligned Memory Access)";
        default:      return "UNKNOWN (Fatal Signal)";
    }
}

const char* GetSignalCodeDesc(int sig, int code) noexcept {
    if (sig == SIGSEGV) {
        switch (code) {
            case SEGV_MAPERR: return "Address not mapped to object";
            case SEGV_ACCERR: return "Invalid permissions for mapped object";
            default: return "Memory access violation";
        }
    } else if (sig == SIGFPE) {
        switch (code) {
            case FPE_INTDIV: return "Integer divide by zero";
            case FPE_INTOVF: return "Integer overflow";
            case FPE_FLTDIV: return "Floating-point divide by zero";
            case FPE_FLTOVF: return "Floating-point overflow";
            default: return "Arithmetic fault";
        }
    } else if (sig == SIGILL) {
        switch (code) {
            case ILL_ILLOPC: return "Illegal opcode";
            case ILL_ILLOPN: return "Illegal operand";
            default: return "Illegal instruction fault";
        }
    } else if (sig == SIGBUS) {
        switch (code) {
            case BUS_ADRALN: return "Invalid address alignment";
            case BUS_ADRERR: return "Nonexistent physical address";
            default: return "Bus error";
        }
    }
    return "Standard signal code";
}

void SignalActionHandler(int sig, siginfo_t* info, void* /*ucontext*/) {
    // Prevent recursive signal handling
    if (g_isHandlingCrash.exchange(true)) {
        SafeWriteStderr("\n[CRASH] Recursive crash intercepted during signal handler execution! Immediate abort.\n");
        ::_exit(128 + sig);
    }

    SafeWriteStderr("\n================================================================================\n");
    SafeWriteStderr("                        bgl-Engine FATAL CRASH REPORT                          \n");
    SafeWriteStderr("================================================================================\n");

    SafeWriteStderr("Signal:        ");
    SafeWriteStderr(GetSignalName(sig));
    SafeWriteStderr("\n");

    if (info) {
        SafeWriteStderr("Detail:        ");
        SafeWriteStderr(GetSignalCodeDesc(sig, info->si_code));
        SafeWriteStderr("\n");

        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
            char addrBuf[32];
            SafeFormatHex(reinterpret_cast<uintptr_t>(info->si_addr), addrBuf, sizeof(addrBuf));
            SafeWriteStderr("Fault Address: ");
            SafeWriteStderr(addrBuf);
            SafeWriteStderr("\n");
        }
    }

    char numBuf[32];
    SafeWriteStderr("Process PID:   ");
    SafeFormatDec(::getpid(), numBuf, sizeof(numBuf));
    SafeWriteStderr(numBuf);

    SafeWriteStderr(" | Thread TID: ");
#if defined(SYS_gettid)
    SafeFormatDec(static_cast<int64_t>(::syscall(SYS_gettid)), numBuf, sizeof(numBuf));
    SafeWriteStderr(numBuf);
#else
    SafeWriteStderr("N/A");
#endif
    SafeWriteStderr("\n--------------------------------------------------------------------------------\n");
    SafeWriteStderr("Callstack Trace (execinfo):\n");

    void* callstack[64];
    int frames = ::backtrace(callstack, 64);
    ::backtrace_symbols_fd(callstack, frames, STDERR_FILENO);

    SafeWriteStderr("--------------------------------------------------------------------------------\n");

#if defined(BGL_HAS_STD_STACKTRACE)
    try {
        std::cerr << "C++23 std::stacktrace:\n"
                  << std::stacktrace::current()
                  << "--------------------------------------------------------------------------------\n";
    } catch (...) {
        // Guard against any memory failure during crash
    }
#endif

    SafeWriteStderr("Restoring default handler and re-raising signal for core dump...\n");
    SafeWriteStderr("================================================================================\n\n");

    // Unregister our handler and re-raise signal
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    ::sigemptyset(&dfl.sa_mask);
    ::sigaction(sig, &dfl, nullptr);

    ::raise(sig);
}
#endif

} // namespace

void InstallCrashHandler() noexcept {
#if defined(__linux__) || defined(__unix__)
    // 1. Allocate alternate signal stack to catch stack overflows safely
    if (!g_altStackMemory) {
        g_altStackMemory = std::malloc(kAltStackSize);
        if (g_altStackMemory) {
            g_altStack.ss_sp = g_altStackMemory;
            g_altStack.ss_size = kAltStackSize;
            g_altStack.ss_flags = 0;
            if (::sigaltstack(&g_altStack, nullptr) != 0) {
                std::free(g_altStackMemory);
                g_altStackMemory = nullptr;
            }
        }
    }

    // 2. Setup sigaction with SA_SIGINFO and SA_ONSTACK
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = SignalActionHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&sa.sa_mask);

    ::sigaction(SIGSEGV, &sa, &g_oldActionSegv);
    ::sigaction(SIGABRT, &sa, &g_oldActionAbrt);
    ::sigaction(SIGFPE,  &sa, &g_oldActionFpe);
    ::sigaction(SIGILL,  &sa, &g_oldActionIll);
    ::sigaction(SIGBUS,  &sa, &g_oldActionBus);
#endif
}

void UninstallCrashHandler() noexcept {
#if defined(__linux__) || defined(__unix__)
    ::sigaction(SIGSEGV, &g_oldActionSegv, nullptr);
    ::sigaction(SIGABRT, &g_oldActionAbrt, nullptr);
    ::sigaction(SIGFPE,  &g_oldActionFpe,  nullptr);
    ::sigaction(SIGILL,  &g_oldActionIll,  nullptr);
    ::sigaction(SIGBUS,  &g_oldActionBus,  nullptr);

    if (g_altStackMemory) {
        stack_t disableStack;
        disableStack.ss_flags = SS_DISABLE;
        ::sigaltstack(&disableStack, nullptr);
        std::free(g_altStackMemory);
        g_altStackMemory = nullptr;
    }
#endif
}

void PrintStackTrace(std::string_view reason) noexcept {
#if defined(__linux__) || defined(__unix__)
    if (!reason.empty()) {
        SafeWriteStderr("\n[STACKTRACE] ");
        SafeWriteStderr(reason.data());
        SafeWriteStderr("\n");
    } else {
        SafeWriteStderr("\n[STACKTRACE] Current execution callstack:\n");
    }

    void* callstack[64];
    int frames = ::backtrace(callstack, 64);
    ::backtrace_symbols_fd(callstack, frames, STDERR_FILENO);

#if defined(BGL_HAS_STD_STACKTRACE)
    try {
        std::cerr << "C++23 std::stacktrace:\n"
                  << std::stacktrace::current()
                  << "\n";
    } catch (...) {}
#endif
#endif
}

[[noreturn]] void TriggerTestCrash(int signalType) {
    if (signalType == 1) {
        std::abort();
    } else if (signalType == 2) {
        volatile int zero = 0;
        volatile int result = 100 / zero;
        (void)result;
        std::abort();
    } else {
        // Default: SIGSEGV (null pointer write)
        volatile int* p = nullptr;
        *p = 42;
        std::abort();
    }
}

} // namespace bgl::core
