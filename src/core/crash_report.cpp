/**
 * @file        core/crash_report.cpp
 * @brief       Logs fatal exceptions and terminate calls, with a Windows minidump.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "crash_report.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/sink.h>

#include <rex/platform.h>
#include <rex/string.h>

#if REX_PLATFORM_WIN32
#include <csignal>

#include "platform_win.h"

#include <dbghelp.h>
#endif

namespace rex::detail {

namespace {

// Set by the first report; later crashes on any thread skip theirs.
std::atomic_flag g_reporting;
std::once_flag g_install_once;

// Immutable once published. Replaced instances are leaked so a crashing reader
// never sees freed memory; weak sinks let shutdown still close the log file.
struct CrashTargets {
  std::filesystem::path dump_path;
  std::vector<std::weak_ptr<spdlog::sinks::sink>> sinks;
};
std::atomic<const CrashTargets*> g_targets{nullptr};

// Writes straight to the published sinks, so the crash path never reads the
// logger registry. Only helper and reporter threads call this.
void EmitCrashLine(std::string_view text) {
  const CrashTargets* targets = g_targets.load(std::memory_order_acquire);
  if (!targets) {
    return;
  }
  const spdlog::details::log_msg msg(spdlog::source_loc{}, "crash", spdlog::level::critical,
                                     spdlog::string_view_t(text.data(), text.size()));
  for (const auto& weak : targets->sinks) {
    if (const auto sink = weak.lock()) {
      // A throw here would reach terminate, so a failing sink is skipped.
      try {
        sink->log(msg);
        sink->flush();
      } catch (...) {}
    }
  }
}

std::string CurrentExceptionText() {
  if (const auto exception = std::current_exception()) {
    try {
      std::rethrow_exception(exception);
    } catch (const std::exception& e) {
      return e.what();
    } catch (...) {
      return "non-standard exception";
    }
  }
  return "no active exception";
}

// Static so a detached helper that outlives the wait still reads live text.
// Written once, by the first report, before its helper starts.
std::string g_message;

// The failing thread may hold a sink mutex, so the line is written on a helper
// thread with a bounded wait; a lost line is better than a hung exit.
void EmitOnHelperThread(std::string message) {
  g_message = std::move(message);
  try {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    std::thread helper([done] {
      EmitCrashLine(g_message);
      done->set_value();
    });
    finished.wait_for(std::chrono::seconds(5));
    helper.detach();
  } catch (...) {
    // Without a helper thread the line is skipped rather than risk blocking.
  }
}

[[noreturn]] void OnTerminate() {
  if (!g_reporting.test_and_set()) {
    // std::current_exception is per thread, so the text is built here.
    try {
      EmitOnHelperThread("terminate called: " + CurrentExceptionText());
    } catch (...) {}
  }
  std::abort();
}

#if REX_PLATFORM_WIN32

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR:
      return "IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW:
      return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
      return "INT_OVERFLOW";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
      return "BREAKPOINT";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      return "NONCONTINUABLE_EXCEPTION";
    case 0xC0000374:
      return "HEAP_CORRUPTION";
    case 0xE06D7363:
      return "CPP_EXCEPTION";
    default:
      return "UNKNOWN";
  }
}

std::string WideToUtf8(const wchar_t* text, int length) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<size_t>(size > 0 ? size : 0), '\0');
  if (size > 0) {
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), size, nullptr, nullptr);
  }
  return result;
}

// "module.dll+0x1234", or the bare address outside any module.
std::string DescribeAddress(const void* address) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          static_cast<LPCWSTR>(address), &module)) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length > 0) {
      const wchar_t* name = path;
      for (DWORD i = 0; i < length; ++i) {
        if (path[i] == L'\\' || path[i] == L'/') {
          name = path + i + 1;
        }
      }
      return fmt::format(
          "{}+0x{:X}", WideToUtf8(name, static_cast<int>(path + length - name)),
          reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module));
    }
  }
  return fmt::format("0x{:016X}", reinterpret_cast<uintptr_t>(address));
}

std::string PathText(const std::filesystem::path& path) {
  return rex::string::to_utf8(path.u16string());
}

struct CrashReport {
  EXCEPTION_POINTERS* pointers;
  DWORD thread_id;
};

std::string CrashLine(const CrashReport& report) {
  const EXCEPTION_RECORD& record = *report.pointers->ExceptionRecord;
  std::string detail;
  if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
       record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      record.NumberParameters >= 2) {
    const ULONG_PTR operation = record.ExceptionInformation[0];
    detail = fmt::format(" ({} 0x{:016X})",
                         operation == 0   ? "reading"
                         : operation == 1 ? "writing"
                         : operation == 8 ? "executing"
                                          : "accessing",
                         record.ExceptionInformation[1]);
  }
  return fmt::format("Unhandled exception 0x{:08X} {}{} at {} [0x{:016X}] on thread {}",
                     static_cast<uint32_t>(record.ExceptionCode),
                     ExceptionName(record.ExceptionCode), detail,
                     DescribeAddress(record.ExceptionAddress),
                     reinterpret_cast<uintptr_t>(record.ExceptionAddress), report.thread_id);
}

// Returns the line that reports the outcome.
std::string WriteMinidump(const CrashReport& report, const std::filesystem::path& dump_path) {
  const HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return fmt::format("Could not create crash dump {} (error {})", PathText(dump_path),
                       GetLastError());
  }
  MINIDUMP_EXCEPTION_INFORMATION exception{report.thread_id, report.pointers, FALSE};
  // Guest memory is large, so the dump keeps stacks and module lists only.
  const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                               MiniDumpWithUnloadedModules);
  const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                         &exception, nullptr, nullptr);
  const DWORD error = written ? 0 : GetLastError();
  CloseHandle(file);
  if (written) {
    return fmt::format("Crash dump written to {}", PathText(dump_path));
  }
  DeleteFileW(dump_path.c_str());
  return fmt::format("Could not write crash dump {} (error 0x{:08X})", PathText(dump_path), error);
}

// Returns the outcome line, or empty when no dump path is published.
std::string WriteDumpIfWanted(const CrashReport& report) {
  if (IsDebuggerPresent()) {
    return "Debugger attached; skipping crash dump";
  }
  const CrashTargets* targets = g_targets.load(std::memory_order_acquire);
  if (!targets || targets->dump_path.empty()) {
    return {};
  }
  return WriteMinidump(report, targets->dump_path);
}

// The faulting thread may hold a sink mutex and block logging forever, and
// formatting can throw when memory is exhausted, so the dump comes first and
// nothing escapes the reporter.
void WriteReport(const CrashReport& report) {
  try {
    const std::string outcome = WriteDumpIfWanted(report);
    EmitCrashLine(CrashLine(report));
    if (!outcome.empty()) {
      EmitCrashLine(outcome);
    }
  } catch (...) {}
}

DWORD WINAPI ReportThread(void* parameter) {
  WriteReport(*static_cast<CrashReport*>(parameter));
  return 0;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
  if (!g_reporting.test_and_set()) {
    // Static copies: a report thread that outlives the bounded wait must not
    // read the faulting stack after a chained filter resumes execution.
    static EXCEPTION_RECORD record;
    static CONTEXT context;
    static EXCEPTION_POINTERS copied;
    static CrashReport report;
    record = *pointers->ExceptionRecord;
    record.ExceptionRecord = nullptr;
    context = *pointers->ContextRecord;
#if defined(_M_X64)
    // Extended state lives past the CONTEXT and is not copied.
    context.ContextFlags &= ~(CONTEXT_XSTATE & ~CONTEXT_AMD64);
#endif
    copied = {&record, &context};
    report = {&copied, GetCurrentThreadId()};
    // A fresh thread has the stack that a stack overflow or deep fault lacks.
    // The wait is bounded in case the loader lock keeps it from starting.
    if (const HANDLE thread = CreateThread(nullptr, 0, ReportThread, &report, 0, nullptr)) {
      WaitForSingleObject(thread, 30000);
      CloseHandle(thread);
    } else {
      // On the faulting thread a sink mutex may already be held, so only the
      // dump is written.
      try {
        WriteDumpIfWanted(report);
      } catch (...) {}
    }
  }
  // Windows Error Reporting and debuggers still see the exception.
  return g_previous_filter ? g_previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

// MSVC keeps terminate handlers per thread, so a terminate on another thread
// reaches abort without OnTerminate; the abort signal handler is process-wide.
void OnAbortSignal(int) {
  if (!g_reporting.test_and_set()) {
    try {
      EmitOnHelperThread(fmt::format("abort called on thread {}", GetCurrentThreadId()));
    } catch (...) {}
  }
}

#endif  // REX_PLATFORM_WIN32

}  // namespace

void InstallCrashReport(const std::filesystem::path& log_path,
                        std::vector<spdlog::sink_ptr> flush_sinks) {
  // The previous instance is leaked on purpose; see CrashTargets.
  g_targets.store(
      new CrashTargets{
          log_path.empty() ? log_path : std::filesystem::path(log_path).replace_extension(".dmp"),
          {flush_sinks.begin(), flush_sinks.end()}},
      std::memory_order_release);
  std::call_once(g_install_once, [] {
    std::set_terminate(OnTerminate);
#if REX_PLATFORM_WIN32
    g_previous_filter = SetUnhandledExceptionFilter(OnUnhandledException);
    std::signal(SIGABRT, OnAbortSignal);
#endif
  });
}

void ClearCrashReportTargets() {
  g_targets.store(nullptr, std::memory_order_release);
}

}  // namespace rex::detail
