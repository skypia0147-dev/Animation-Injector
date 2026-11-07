#include "core/StartupRecord.h"

#include <Windows.h>
#include <winevt.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <tuple>
#include <format>
#include <fstream>

namespace AnimationInjector::StartupRecord {
namespace {
constexpr std::string_view kMagic = "AI-STARTUP 1";
constexpr int kMaxExceptions = 64;

// Written once by Begin before the exception handler is added, and never closed: the handler of another thread
// may still be writing when Finish removes it.
HANDLE g_file = INVALID_HANDLE_VALUE;
std::uint64_t g_startTick = 0;
PVOID g_handler = nullptr;
std::atomic<int> g_exceptions{0};

std::uint64_t Elapsed() noexcept { return ::GetTickCount64() - g_startTick; }

// One WriteFile per line on a handle opened for appending: lines of several threads never interleave.
void Write(const char* text, std::size_t size) noexcept {
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(g_file, text, static_cast<DWORD>(size), &written, nullptr);
}
void Write(std::string_view text) noexcept { Write(text.data(), text.size()); }

// Text built inside the exception handler: a fixed buffer, no allocation, no locale, no lock.
struct Line {
    char text[512];
    std::size_t size = 0;
    void Put(std::string_view s) noexcept {
        for (const char c : s)
            if (size + 1 < sizeof text) text[size++] = c;
    }
    void Number(std::uint64_t value, unsigned base) noexcept {
        char digits[24];
        int count = 0;
        do {
            digits[count++] = "0123456789ABCDEF"[value % base];
            value /= base;
        } while (value);
        while (count) Put({&digits[--count], 1});
    }
};

bool Serious(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_BREAKPOINT:
    case 0xC0000374:  // STATUS_HEAP_CORRUPTION
    case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN (also every fail-fast)
        return true;
    default: return false;
    }
}
std::string_view Name(DWORD code) noexcept {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun / fail-fast";
    default: return "exception";
    }
}

// module.dll+0xOFFSET, or the bare address outside every module.
void PutLocation(Line& line, const void* address) noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH];
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module) &&
        module) {
        const DWORD length = ::GetModuleFileNameW(module, path, MAX_PATH);
        if (length && length < MAX_PATH) {
            const wchar_t* name = path;
            for (DWORD i = 0; i < length; ++i)
                if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
            char utf8[MAX_PATH * 3];
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(path + length - name), utf8,
                static_cast<int>(sizeof utf8), nullptr, nullptr);
            if (size > 0) {
                line.Put({utf8, static_cast<std::size_t>(size)});
                line.Put("+0x");
                line.Number(at - reinterpret_cast<std::uintptr_t>(module), 16);
                return;
            }
        }
    }
    line.Put("0x");
    line.Number(at, 16);
}

__declspec(noinline) void Record(const EXCEPTION_RECORD& record) noexcept {
    Line line;
    line.Put("exception ");
    line.Number(record.ExceptionCode, 16);
    line.Put(" (");
    line.Put(Name(record.ExceptionCode));
    line.Put(") thread ");
    line.Number(::GetCurrentThreadId(), 10);
    line.Put(" at ");
    PutLocation(line, record.ExceptionAddress);
    if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2) {
        const auto kind = record.ExceptionInformation[0];
        line.Put(kind == 0 ? ", reading 0x" : kind == 1 ? ", writing 0x" : ", executing 0x");
        line.Number(record.ExceptionInformation[1], 16);
    }
    line.Put(" +");
    line.Number(Elapsed(), 10);
    line.Put("ms\n");
    Write(line.text, line.size);
}

// Sees every exception of every thread first, before any handler decides on it; it only writes it down.
LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (!record || !Serious(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    const int seen = g_exceptions.fetch_add(1, std::memory_order_relaxed);
    if (seen == kMaxExceptions) Write("exception: more not recorded\n");
    if (seen >= kMaxExceptions) return EXCEPTION_CONTINUE_SEARCH;
    // An overflowed stack has little room left: no module lookup.
    if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
        Write("exception C00000FD (stack overflow)\n");
    else
        Record(*record);
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string Narrow(const wchar_t* text, int length = -1) {
    if (!text) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
    if (length < 0) out.pop_back();  // the terminator
    return out;
}
std::wstring Widen(std::string_view text) {
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>((std::max)(size, 0)), L'\0');
    if (size > 0) ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
bool SameName(std::wstring_view a, std::wstring_view b) {
    return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}
std::wstring ExeName() {
    wchar_t path[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring_view view(path, length);
    const auto slash = view.find_last_of(L"\\/");
    return std::wstring(slash == view.npos ? view : view.substr(slash + 1));
}

std::string Text(const EVT_VARIANT& value) {
    switch (value.Type & EVT_VARIANT_TYPE_MASK) {
    case EvtVarTypeString: return Narrow(value.StringVal);
    case EvtVarTypeAnsiString: return value.AnsiStringVal ? value.AnsiStringVal : "";
    case EvtVarTypeHexInt32: return std::format("0x{:x}", value.UInt32Val);
    case EvtVarTypeHexInt64: return std::format("0x{:x}", value.UInt64Val);
    case EvtVarTypeUInt32: return std::to_string(value.UInt32Val);
    case EvtVarTypeUInt64: return std::to_string(value.UInt64Val);
    case EvtVarTypeInt32: return std::to_string(value.Int32Val);
    case EvtVarTypeInt64: return std::to_string(value.Int64Val);
    case EvtVarTypeUInt16: return std::to_string(value.UInt16Val);
    case EvtVarTypeByte: return std::to_string(value.ByteVal);
    case EvtVarTypeNull: return {};
    default: return "?";
    }
}

struct Seconds {
    std::uint64_t ms;
};
}  // namespace
}  // namespace AnimationInjector::StartupRecord

template<>
struct std::formatter<AnimationInjector::StartupRecord::Seconds> : std::formatter<double> {
    auto format(AnimationInjector::StartupRecord::Seconds s, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "+{:.1f} s", static_cast<double>(s.ms) / 1000.0);
    }
};

namespace AnimationInjector::StartupRecord {
namespace {
// "<name> +<ms>ms" after the line's keyword.
std::pair<std::string, std::uint64_t> Stage(std::string_view rest) {
    const auto plus = rest.rfind(" +");
    if (plus == rest.npos) return {std::string(rest), 0};
    return {std::string(rest.substr(0, plus)), std::strtoull(std::string(rest.substr(plus + 2)).c_str(), nullptr, 10)};
}

Previous Report(const std::filesystem::path& file) {
    Previous previous;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line)) return previous;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kMagic) return previous;
    std::uint64_t start = 0;
    std::uint32_t pid = 0;
    std::string about, stage = "plugin-load";
    std::uint64_t stageMs = 0;
    std::vector<std::string> exceptions;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string_view view(line);
        if (view.starts_with("start ")) {
            // start <filetime> pid <pid> | <about>
            char* end = nullptr;
            start = std::strtoull(line.c_str() + 6, &end, 10);
            if (const auto at = view.find(" pid "); at != view.npos)
                pid = static_cast<std::uint32_t>(std::strtoul(line.c_str() + at + 5, nullptr, 10));
            if (const auto bar = view.find(" | "); bar != view.npos) about = line.substr(bar + 3);
        } else if (view.starts_with("stage ")) {
            std::tie(stage, stageMs) = Stage(view.substr(6));
        } else if (view.starts_with("done ")) {
            std::tie(stage, stageMs) = Stage(view.substr(5));
            previous.complete = true;
        } else if (view.starts_with("exception")) {
            exceptions.push_back(line);
        }
    }
    previous.found = true;
    if (previous.complete) {
        previous.lines.push_back(
            std::format("previous launch started fully ({}): '{}' at {}", about, stage, Seconds{stageMs}));
        return previous;
    }
    previous.lines.push_back(std::format(
        "previous launch did not finish starting ({}): last stage '{}' at {}", about, stage, Seconds{stageMs}));
    for (const auto& e : exceptions) previous.lines.push_back("  " + e);
    if (!exceptions.empty())
        previous.lines.push_back("  (exceptions are written as they are raised; one the game or a plugin handled "
                                 "did no harm)");
    // A record is written within seconds of the start it carries; two seconds back covers clock rounding.
    constexpr std::uint64_t kMargin = 2ull * 10'000'000ull;
    auto errors = WindowsErrors(ExeName(), start > kMargin ? start - kMargin : 0, pid, 3);
    if (errors.empty())
        previous.lines.push_back(
            "  no Windows error record of the game since then: it was closed without a crash (by hand, or "
            "ended by a program), or Windows Error Reporting is off");
    for (auto& e : errors) previous.lines.push_back("  " + std::move(e));
    return previous;
}
}  // namespace

Previous Begin(const std::filesystem::path& file, std::string_view about) noexcept {
    Previous previous;
    try {
        previous = Report(file);
    } catch (...) {
        previous = {};
    }
    // A second Begin (the tests' next launch) starts over.
    if (g_handler) {
        ::RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_exceptions = 0;
    ::DeleteFileW(file.c_str());
    // Opened for appending only: every WriteFile lands whole at the end, from any thread.
    const HANDLE handle = ::CreateFileW(file.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return previous;
    g_file = handle;
    g_startTick = ::GetTickCount64();
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    try {
        const std::uint64_t start = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        Write(std::format(
            "{}\nstart {} pid {} | {}\nstage plugin-load +0ms\n", kMagic, start, ::GetCurrentProcessId(), about));
    } catch (...) {
    }
    g_handler = ::AddVectoredExceptionHandler(1, OnException);
    return previous;
}

void Mark(std::string_view stage) noexcept {
    if (g_file == INVALID_HANDLE_VALUE) return;
    try {
        Write(std::format("stage {} +{}ms\n", stage, Elapsed()));
    } catch (...) {
    }
}

void Finish(std::string_view stage) noexcept {
    if (g_handler) {
        ::RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
    if (g_file == INVALID_HANDLE_VALUE) return;
    try {
        Write(std::format("done {} +{}ms\n", stage, Elapsed()));
    } catch (...) {
    }
}

std::vector<std::string> WindowsErrors(
    std::wstring_view exe, std::uint64_t since, std::uint32_t pid, std::size_t limit) {
    std::vector<std::string> out;
    if (exe.empty() || !limit) return out;
    const HMODULE library = ::LoadLibraryExW(L"wevtapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return out;
    const auto query = reinterpret_cast<decltype(&::EvtQuery)>(::GetProcAddress(library, "EvtQuery"));
    const auto next = reinterpret_cast<decltype(&::EvtNext)>(::GetProcAddress(library, "EvtNext"));
    const auto render = reinterpret_cast<decltype(&::EvtRender)>(::GetProcAddress(library, "EvtRender"));
    const auto context =
        reinterpret_cast<decltype(&::EvtCreateRenderContext)>(::GetProcAddress(library, "EvtCreateRenderContext"));
    const auto close = reinterpret_cast<decltype(&::EvtClose)>(::GetProcAddress(library, "EvtClose"));
    if (!query || !next || !render || !context || !close) {
        ::FreeLibrary(library);
        return out;
    }
    try {
        FILETIME fileTime{static_cast<DWORD>(since), static_cast<DWORD>(since >> 32)};
        SYSTEMTIME at{};
        ::FileTimeToSystemTime(&fileTime, &at);
        const auto xpath = std::format(L"*[System[Provider[@Name='Application Error'] and EventID=1000 and "
                                       L"TimeCreated[@SystemTime>='{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.000Z']]]",
            at.wYear, at.wMonth, at.wDay, at.wHour, at.wMinute, at.wSecond);
        const EVT_HANDLE results =
            query(nullptr, L"Application", xpath.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection);
        const EVT_HANDLE values = results ? context(0, nullptr, EvtRenderContextUser) : nullptr;
        std::vector<BYTE> buffer;
        EVT_HANDLE events[16];
        DWORD returned = 0;
        while (values && out.size() < limit && next(results, 16, events, 2000, 0, &returned)) {
            for (DWORD i = 0; i < returned; ++i) {
                DWORD used = 0, count = 0;
                if (out.size() < limit) {
                    render(values, events[i], EvtRenderEventValues, 0, nullptr, &used, &count);
                    buffer.resize(used);
                }
                if (out.size() < limit && used &&
                    render(values, events[i], EvtRenderEventValues, used, buffer.data(), &used, &count)) {
                    // Event 1000: AppName, AppVersion, AppTimeStamp, ModuleName, ModuleVersion, ModuleTimeStamp,
                    // ExceptionCode, FaultOffset, ProcessId, ProcessCreationTime, AppPath, ModulePath, ...
                    const auto* fields = reinterpret_cast<const EVT_VARIANT*>(buffer.data());
                    const auto field = [&](DWORD k) { return k < count ? Text(fields[k]) : std::string{}; };
                    const auto app = field(0);
                    if (SameName(Widen(app), exe)) {
                        const auto process = static_cast<std::uint32_t>(std::strtoul(field(8).c_str(), nullptr, 0));
                        out.push_back(std::format(
                            "Windows error record: {} {} stopped in module '{}' {} with exception {} at offset {}{}; "
                            "module file {}",
                            app, field(1), field(3), field(4), field(6), field(7),
                            process == pid ? " (that launch)" : std::format(" (another process {})", field(8)),
                            field(11)));
                    }
                }
                close(events[i]);
            }
        }
        if (values) close(values);
        if (results) close(results);
    } catch (...) {
    }
    ::FreeLibrary(library);
    return out;
}
}  // namespace AnimationInjector::StartupRecord
