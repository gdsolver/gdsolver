// DPSAMPLE=<file>: a poor man's CPU sampler for the solver's own code (print / file only, nothing the search reads).
// A thread wakes every few milliseconds, suspends each other thread of the process for the length of GetThreadContext, and keeps the instruction pointer of the ones that stand
// inside this module (the game's own threads, the kernel and the idle threads are left out). flush() appends the call's samples to the file as 32-bit module offsets, a call to
// a header [magic 'DPSM', module size, count]; the offsets are resolved to function and line offline against the module's pdb (the resolving script is a private lab tool, not part of this tree).
// DPSAMPLE_MS sets the period (default 4).
#pragma once
#ifdef _WIN32
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#ifdef near   // minwindef.h's empty near / far: the solver has members of those names (StepCtx::near)
#undef near
#endif
#ifdef far
#undef far
#endif
#ifdef NEAR   // ...and Dynamics::NEAR
#undef NEAR
#endif
#ifdef FAR
#undef FAR
#endif

namespace dp {
namespace psamp {

inline std::atomic<bool> g_run{false};
inline std::atomic<size_t> g_n{0};
inline std::vector<uint32_t> g_buf;
inline std::thread g_thread;
inline uintptr_t g_base = 0;
inline uint32_t g_size = 0;
inline std::atomic<bool> g_started{false};
inline std::mutex g_mu;   // between the sampler's push and flush's copy (no thread of the solver ever takes it)

inline void loop(int ms) {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::vector<DWORD> ids;
    ids.reserve(512);
    while (g_run.load(std::memory_order_relaxed)) {
        Sleep((DWORD)ms);
        ids.clear();
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) ids.push_back(te.th32ThreadID); } while (Thread32Next(snap, &te));
        CloseHandle(snap);
        for (DWORD id : ids) {
            const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
            if (!h) continue;
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            uint64_t rip = 0;
            if (SuspendThread(h) != (DWORD)-1) {
                if (GetThreadContext(h, &ctx)) rip = ctx.Rip;
                ResumeThread(h);
            }
            CloseHandle(h);
            if (rip >= g_base && rip < g_base + g_size) {
                std::lock_guard<std::mutex> lk(g_mu);
                const size_t k = g_n.load(std::memory_order_relaxed);
                if (k < g_buf.size()) { g_buf[k] = (uint32_t)(rip - g_base); g_n.store(k + 1, std::memory_order_relaxed); }
            }
        }
    }
}

inline void start() {
    if (g_started.exchange(true)) return;
    const char* f = std::getenv("DPSAMPLE");
    if (!f || !*f) return;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((const void*)&start, &mbi, sizeof mbi) || !mbi.AllocationBase) return;
    g_base = (uintptr_t)mbi.AllocationBase;
    const auto* dos = (const IMAGE_DOS_HEADER*)g_base;
    const auto* nt = (const IMAGE_NT_HEADERS*)(g_base + (uintptr_t)dos->e_lfanew);
    g_size = (uint32_t)nt->OptionalHeader.SizeOfImage;
    g_buf.assign((size_t)1 << 24, 0u);   // 16 M samples (64 MiB)
    int ms = 4;
    if (const char* m = std::getenv("DPSAMPLE_MS")) ms = std::max(1, std::atoi(m));
    g_run = true;
    g_thread = std::thread(loop, ms);
    g_thread.detach();
}

// the samples since the last flush, appended to the file; the sampler keeps running
inline void flush() {
    if (!g_run.load()) return;
    const char* f = std::getenv("DPSAMPLE");
    std::lock_guard<std::mutex> lk(g_mu);
    const size_t n = g_n.exchange(0);
    if (!f || !n) return;
    if (std::FILE* fp = std::fopen(f, "ab")) {
        const uint32_t hdr[3] = {0x4d535044u, g_size, (uint32_t)n};
        std::fwrite(hdr, sizeof hdr, 1, fp);
        std::fwrite(g_buf.data(), sizeof(uint32_t), n, fp);
        std::fclose(fp);
    }
}

}  // namespace psamp
}  // namespace dp
#else
namespace dp { namespace psamp { inline void start() {} inline void flush() {} } }
#endif
