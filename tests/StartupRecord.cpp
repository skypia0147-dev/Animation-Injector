// Startup record (core/StartupRecord.h).
//   (no argument)  ctest: stages, a recorded exception, the next launch's report, Finish, a non-ASCII folder.
//   --e2e          by hand: two child launches really die (an unhandled access violation, a fail-fast) and the
//                  next launch must report Windows' error record of each. It leaves two Application Error
//                  events of this test in the Windows log.
//   --crash <file> <av|fastfail>  one such child.
//   --windows-errors <exe> <pid> <days>  by hand: the reader against records already in the Windows log (a PC with
//                  Windows Error Reporting off records no crash, so --e2e cannot pass there).
#include "core/StartupRecord.h"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace AnimationInjector;
namespace fs = std::filesystem;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Has(const std::vector<std::string>& lines, std::string_view text) {
    for (const auto& line : lines)
        if (line.find(text) != line.npos) return true;
    return false;
}
std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream all;
    all << in.rdbuf();
    return all.str();
}
void Dump(const StartupRecord::Previous& p) {
    for (const auto& line : p.lines) std::printf("  | %s\n", line.c_str());
}

// No C++ objects with destructors in a function with __try.
void WriteNull() {
    __try {
        *static_cast<volatile int*>(nullptr) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

fs::path Folder() {
    // A user name in another script: the record is opened through its wide path.
    return fs::temp_directory_path() / (L"ai-startup-시험-" + std::to_wstring(::GetCurrentProcessId()));
}

int Units() {
    const auto dir = Folder();
    fs::create_directories(dir);
    const auto file = dir / "AnimationInjector.startup.txt";

    auto first = StartupRecord::Begin(file, "first");
    Check(!first.found && first.lines.empty(), "a first launch has nothing to report");
    StartupRecord::Mark("declarations-ready");
    WriteNull();  // handled: written down, harmless
    const auto record = Read(file);
    Check(record.find("stage declarations-ready") != record.npos, "the stage is written at once");
    Check(record.find("exception C0000005 (access violation)") != record.npos, "the exception is written at once");
    Check(record.find("AnimationInjectorStartupRecordTests.exe+0x") != record.npos, "module+offset");
    Check(record.find("writing 0x0") != record.npos, "the access and its address");

    // The next launch: the first did not finish.
    auto second = StartupRecord::Begin(file, "second");
    Dump(second);
    Check(second.found && !second.complete, "an unfinished launch is reported as such");
    Check(Has(second.lines, "did not finish starting (first): last stage 'declarations-ready'"), "its last stage");
    Check(Has(second.lines, "exception C0000005"), "its exception");
    Check(Has(second.lines, "no Windows error record"), "no Windows record for a launch that did not crash");

    StartupRecord::Finish("data-loaded");
    WriteNull();  // no longer watched
    Check(Read(file).find("exception") == std::string::npos, "Finish stops watching exceptions");

    auto third = StartupRecord::Begin(file, "third");
    Dump(third);
    Check(third.found && third.complete, "a finished launch is reported as such");
    Check(Has(third.lines, "started fully (second): 'data-loaded'"), "its last stage");
    StartupRecord::Finish("data-loaded");

    std::error_code ignored;
    fs::remove_all(dir, ignored);
    std::printf(failures ? "%d failed\n" : "StartupRecord: all passed\n", failures);
    return failures ? 1 : 0;
}

int Crash(const fs::path& file, std::string_view how) {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    StartupRecord::Begin(file, std::string("child ") + std::string(how));
    StartupRecord::Mark("declarations-ready");
    if (how == "fastfail") __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    *static_cast<volatile int*>(nullptr) = 1;  // unhandled
    return 0;
}

std::uint64_t Now() {
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

bool RunChild(const fs::path& file, const wchar_t* how, DWORD& pid, DWORD& exitCode) {
    wchar_t self[MAX_PATH];
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --crash \"" + file.wstring() + L"\" " + how;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    ::WaitForSingleObject(pi.hProcess, 60000);
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    pid = pi.dwProcessId;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

int EndToEnd() {
    const auto dir = Folder();
    fs::create_directories(dir);
    const auto file = dir / "AnimationInjector.startup.txt";
    wchar_t self[MAX_PATH];
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring exe = fs::path(self).filename().wstring();
    for (const auto* how : {L"av", L"fastfail"}) {
        const auto since = Now() - 2ull * 10'000'000ull;
        DWORD pid = 0, exitCode = 0;
        Check(RunChild(file, how, pid, exitCode), "the child starts");
        std::printf("child %ls: pid %lu exit 0x%08lX\n", how, pid, exitCode);
        // Windows Error Reporting writes its record shortly after the process is gone.
        bool recorded = false;
        for (int i = 0; i < 60 && !recorded; ++i) {
            for (const auto& line : StartupRecord::WindowsErrors(exe, since, pid, 5))
                recorded |= line.find("(that launch)") != line.npos;
            if (!recorded) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        Check(recorded, "Windows recorded the child's crash");
        auto next = StartupRecord::Begin(file, "next");
        Dump(next);
        const bool av = std::wstring_view(how) == L"av";
        Check(next.found && !next.complete, "the child did not finish");
        Check(Has(next.lines, "(that launch)"), "its Windows record is reported");
        Check(Has(next.lines, av ? "c0000005" : "c0000409"), "with its exception code");
        if (av) Check(Has(next.lines, "exception C0000005 (access violation)"), "and the handler wrote it first");
        StartupRecord::Finish("data-loaded");
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    std::printf(failures ? "%d failed\n" : "StartupRecord e2e: all passed\n", failures);
    return failures ? 1 : 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring_view(argv[1]) == L"--crash") {
        const std::wstring how(argv[3]);
        return Crash(argv[2], how == L"fastfail" ? "fastfail" : "av");
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--e2e") return EndToEnd();
    if (argc >= 5 && std::wstring_view(argv[1]) == L"--windows-errors") {
        const auto since = Now() - std::stoull(argv[4]) * 24ull * 3600ull * 10'000'000ull;
        const auto pid = static_cast<std::uint32_t>(std::wcstoul(argv[3], nullptr, 0));
        const auto lines = StartupRecord::WindowsErrors(argv[2], since, pid, 5);
        for (const auto& line : lines) std::printf("%s\n", line.c_str());
        return lines.empty() ? 1 : 0;
    }
    return Units();
}
