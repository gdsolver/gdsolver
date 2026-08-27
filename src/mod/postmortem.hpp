#pragma once
// Crash post-mortem: terminate / invalid-parameter / SEH handlers that write one last line.
#include "mod/hookdepth.hpp"

namespace p1 {

// ============================================================
// Post-mortem of hard deaths (postmortem). Geode's crash handler only covers the SEH path, and
// uncaught C++ exceptions (bad_alloc etc.) or CRT invalid parameter leave not a single line, so
// we insert a one-line write to result.txt into the dying path itself.
// std::ofstream / std::string are not used for the write — this can run in the middle of a
// bad_alloc, so no allocating tool can be used (fopen + fprintf only).
// ============================================================
namespace postmortem {

// Private bytes of the process (MB). The OOM hypothesis can only be judged by "the value at the
// moment of the crash". Resolve kernel32's K32GetProcessMemoryInfo dynamically to avoid
// linking psapi
inline int privateMB() {
#ifdef GEODE_IS_WINDOWS
    struct PMC {
        DWORD cb; DWORD PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize;
        SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
        SIZE_T QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
        SIZE_T PagefileUsage, PeakPagefileUsage;
    };
    using Fn = BOOL(WINAPI*)(HANDLE, PMC*, DWORD);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                      "K32GetProcessMemoryInfo");
    if (!fn) return -1;
    PMC pmc{};
    pmc.cb = sizeof(pmc);
    if (!fn(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
    return (int)(pmc.PagefileUsage / (1024 * 1024));
#else
    return -1;
#endif
}

// Emit one line to both result.txt and crash_postmortem.txt.
// result.txt is truncated at the next session start, so keep an append-only copy
inline void emit(const char* line) {
    static const char* names[2] = { "/result.txt", "/crash_postmortem.txt" };
    for (int k = 0; k < 2; ++k) {
        char path[512];
        snprintf(path, sizeof(path), "%s%s", DATA_DIR, names[k]);
        if (FILE* f = fopen(path, "a")) {
            fputs(line, f);
            fputc('\n', f);
            fflush(f);
            fclose(f);
        }
    }
}

// Early detection of heap corruption (cfg `heapcheck=<attempts>`, 0=disabled). Heap corruption
// surfaces the next time the allocator touches it (0xC0000374, instant kill, no handler runs),
// so validate every heap every N attempts and bracket the broken interval by attempt number.
// Slow, so disabled by default.
// HeapValidate can effectively do nothing on the segment heap, so also return the number of
// walked blocks so the caller can judge whether the check had any teeth
inline bool heapOk(int* heapsOut = nullptr, long long* blocksOut = nullptr) {
#ifdef GEODE_IS_WINDOWS
    HANDLE heaps[64];
    DWORD n = GetProcessHeaps(64, heaps);
    if (n > 64) n = 64;
    if (heapsOut) *heapsOut = (int)n;
    long long blocks = 0;
    bool ok = true;
    for (DWORD i = 0; i < n; ++i) {
        if (!HeapValidate(heaps[i], 0, nullptr)) { ok = false; break; }
        // HeapWalk actually traverses the blocks = evidence that the check was not a no-op
        PROCESS_HEAP_ENTRY e{};
        e.lpData = nullptr;
        while (HeapWalk(heaps[i], &e)) {
            if (++blocks > 2000000) break;   // cap (so a pathologically large heap cannot hang us)
        }
    }
    if (blocksOut) *blocksOut = blocks;
    return ok;
#else
    (void)heapsOut; (void)blocksOut;
    return true;
#endif
}

inline void writeRaw(const char* what) {
    char depths[256];
    hookdepth::format(depths, sizeof(depths));
    char line[1024];
    snprintf(line, sizeof(line), "FATAL: %s (privateMB=%d)%s", what, privateMB(), depths);
    emit(line);
}

inline std::terminate_handler g_prevTerminate = nullptr;

inline void onTerminate() {
    // Uncaught C++ exceptions pass through here. This is the only point where the type and
    // what() can be recorded
    char buf[512];
    const char* kind = "terminate() with no active exception";
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
    } catch (const std::exception& e) {
        snprintf(buf, sizeof(buf), "uncaught std::exception: %s", e.what());
        kind = buf;
    } catch (...) {
        kind = "uncaught non-std exception";
    }
    writeRaw(kind);
    if (g_prevTerminate) g_prevTerminate();   // also run Geode's handler
    std::abort();
}

#ifdef GEODE_IS_WINDOWS
inline void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                               unsigned int, uintptr_t) {
    writeRaw("CRT invalid parameter");
}

inline LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

// Record the crash site as module+offset. On a stack overflow (0xC00000FD) Geode's crash log
// produces not a single line (there is no stack left to run the handler on).
// No allocating tool can be used, so CaptureStackBackTrace + fixed buffers only
inline void writeBacktrace() {
    void* frames[32];
    USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        char line[512];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)frames[i], &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name));
            const char* slash = strrchr(name, '\\');
            if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
            snprintf(line, sizeof(line), "FATAL:   #%02u %s+0x%llX", i, name,
                (unsigned long long)((char*)frames[i] - (char*)mod));
        } else {
            snprintf(line, sizeof(line), "FATAL:   #%02u %p (no module)", i, frames[i]);
        }
        emit(line);
    }
}

// Tally the return addresses on the stack to name "what recursed".
// CaptureStackBackTrace only sees the handler's own stack, so on a stack overflow scan upward
// from the crashed thread's Rsp and count by module+offset. No allocation at all (fixed table
// of 32 entries)
inline void writeStackScan(EXCEPTION_POINTERS* ep) {
#ifdef _M_X64
    if (!ep || !ep->ContextRecord) return;
    struct Entry { HMODULE mod; unsigned long long off; unsigned count; };
    Entry tbl[32] = {};
    int used = 0;
    ULONG_PTR rsp = (ULONG_PTR)ep->ContextRecord->Rsp;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((LPCVOID)rsp, &mbi, sizeof(mbi))) return;
    ULONG_PTR end = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    if (end > rsp + 0x40000) end = rsp + 0x40000;   // 256KB is enough (bounds the time)
    for (ULONG_PTR p = rsp; p + 8 <= end; p += 8) {
        ULONG_PTR v = *(ULONG_PTR*)p;
        if (v < 0x10000) continue;
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &mod) || !mod)
            continue;
        unsigned long long off = (unsigned long long)(v - (ULONG_PTR)mod);
        int i = 0;
        for (; i < used; ++i)
            if (tbl[i].mod == mod && tbl[i].off == off) { ++tbl[i].count; break; }
        if (i == used && used < 32) { tbl[used++] = { mod, off, 1 }; }
    }
    for (int rank = 0; rank < 8; ++rank) {
        int best = -1;
        for (int i = 0; i < used; ++i)
            if (tbl[i].count > 0 && (best < 0 || tbl[i].count > tbl[best].count)) best = i;
        if (best < 0 || tbl[best].count < 2) break;
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(tbl[best].mod, name, sizeof(name));
        const char* slash = strrchr(name, '\\');
        if (slash) memmove(name, slash + 1, strlen(slash + 1) + 1);
        char line[512];
        snprintf(line, sizeof(line), "FATAL:   stack x%-6u %s+0x%llX",
            tbl[best].count, name, tbl[best].off);
        emit(line);
        tbl[best].count = 0;
    }
#else
    (void)ep;
#endif
}

inline LONG WINAPI onUnhandled(EXCEPTION_POINTERS* ep) {
    char buf[256];
    snprintf(buf, sizeof(buf), "unhandled SEH 0x%08lX at %p",
        (unsigned long)ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress);
    writeRaw(buf);
    writeBacktrace();
    writeStackScan(ep);
    // Do not clobber the previous occupant (Geode's crash log)
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

inline void onNewFailed() {
    writeRaw("operator new failed (out of memory)");
    throw std::bad_alloc();
}

inline void install() {
    g_prevTerminate = std::set_terminate(&onTerminate);
    std::set_new_handler(&onNewFailed);
#ifdef GEODE_IS_WINDOWS
    _set_invalid_parameter_handler(&onInvalidParameter);
    g_prevFilter = SetUnhandledExceptionFilter(&onUnhandled);
#endif
}

} // namespace postmortem

}  // namespace p1
