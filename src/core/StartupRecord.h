#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// A launch that dies during startup with no crash log (a fail-fast such as heap corruption skips every crash
// handler, and SKSE's guard covers only the plugin being loaded) leaves its answer to the next launch. Each stage
// is appended to a small file as it is reached, and until the record is finished the serious exceptions of every
// thread are appended too, as they happen. The next launch reports the stage the previous one reached and, when
// it did not finish starting, the Windows "Application Error" records of the game since it began (faulting
// module, exception code, offset). Win32 and the standard library only: tests/StartupRecord.cpp runs it as is.
namespace AnimationInjector::StartupRecord {
struct Previous {
    bool found = false;              // a record of an earlier launch was read
    bool complete = false;           // it reached Finish
    std::vector<std::string> lines;  // the report, one log line each
};

// Reads the previous launch's record in `file`, then starts this launch's there and watches exceptions.
// `about`: this launch, as written into the record (version, runtime). Never throws.
Previous Begin(const std::filesystem::path& file, std::string_view about) noexcept;
// A stage reached, from any thread. Nothing before Begin.
void Mark(std::string_view stage) noexcept;
// The launch has started: the last stage, and exceptions are no longer watched.
void Finish(std::string_view stage) noexcept;

// The Windows "Application Error" records (event 1000) of the executable named `exe` created at or after
// `since` (a FILETIME), newest first, at most `limit`, each as one line; `pid` marks the launch's own.
// wevtapi.dll is loaded only here. An empty list when there are none or the log cannot be read.
std::vector<std::string> WindowsErrors(
    std::wstring_view exe, std::uint64_t since, std::uint32_t pid, std::size_t limit);
}
