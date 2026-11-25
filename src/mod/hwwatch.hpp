// cfg `hwwatch=<offset>` (default 0 = off, observation only): which instruction writes player 1's field at <offset>?
//
// A hardware data breakpoint (DR0, write, 4 bytes) on &m_player1 + <offset>, set on the game's main thread, with a vectored exception handler that records the instruction pointer and the
// code addresses found on the stack at the moment of the write, and lets the thread go on. Only a write that CHANGES the 4 bytes is a row (a flag block rewritten every tick would fill it). One run names the writer; nothing is patched or called. The rows (module-relative, so they can be
// looked up in the decompile) go to hwwatch.csv: tick, rip, up to six return-looking addresses, the 4 bytes now in the field.
//
// Armed when the pointer is first seen (and re-armed if the player object moves), taken off by disarm() when the session ends or the level is left: a debug register left on the thread would
// trap every later write to a freed address. The debug registers are set from a helper thread that suspends the main thread (SetThreadContext on the running thread is not reliable).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#if defined(GEODE_IS_WINDOWS) || defined(_WIN32)
#include <Windows.h>
#endif

namespace hwwatch {

inline long long g_off = 0;    // cfg hwwatch=<offset in the player>; 0 = off
inline long long g_from = 0;   // cfg hwfrom=<tick>: armed from this tick on, and disarmed after hwto (a field written every tick would fill the rows long before the tick that matters)
inline long long g_to = 0;     // cfg hwto=<tick>; 0 = no end
inline bool g_anyModule = false;   // cfg hwmodules=1: the stack words that point into other modules are rows too (printed with the module name)
inline long long g_depth = 128;   // cfg hwdepth=<words>: how many stack words are looked at for return addresses (a hook thunk and its frames can use more than 128)
inline long long g_absRva = 0;  // cfg hwabs=<rva>: watch the 4 bytes at <game base + rva> instead of a player field (a global such as a random generator state); needs no hwwatch offset
inline long long g_uid = 0;   // cfg hwuid=<uid>: the watched field is <offset> inside the level object with that unique id (a moving platform, a portal) instead of inside the player; the hook resolves the pointer
inline void* g_uidObj = nullptr; inline void* g_uidLayer = nullptr;
inline long long g_deref = 0;  // cfg hwderef=<offset>: the watched address is *(player + hwderef) + hwwatch -- a field of an object the player points at (a child sprite)

#if defined(GEODE_IS_WINDOWS) || defined(_WIN32)
struct Row { long long tick; uintptr_t rip; uintptr_t st[6]; unsigned val, prev; };
inline constexpr int kMaxRows = 4096;
inline Row g_rows[kMaxRows];
inline std::atomic<int> g_n{0};
inline int g_written = 0;
inline std::atomic<long long> g_tickNow{0};
inline uintptr_t g_addr = 0, g_base = 0, g_codeLo = 0, g_codeHi = 0;
inline unsigned g_last = 0;   // the field's value after the last logged write: a write that leaves it as it was (the per-tick stores of a flag block) is not a row
inline HANDLE g_mainThread = nullptr;
inline PVOID g_veh = nullptr;
inline std::ofstream g_file;

inline LONG CALLBACK handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1) || g_addr == 0) return EXCEPTION_CONTINUE_SEARCH;
    unsigned cur = 0;
    __try { cur = *reinterpret_cast<const unsigned*>(g_addr); } __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
    if (cur == g_last) { c->Dr6 = 0; return EXCEPTION_CONTINUE_EXECUTION; }
    const unsigned prevVal = g_last;
    g_last = cur;
    const int i = g_n.fetch_add(1);
    if (i < kMaxRows) {
        Row& r = g_rows[i];
        r.tick = g_tickNow.load();
        r.rip = (uintptr_t)c->Rip;
        for (auto& s : r.st) s = 0;
        __try {
            const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
            int k = 0;
            for (int j = 0; j < (int)g_depth && k < 6; ++j) {
                if (sp[j] >= g_codeLo && sp[j] < g_codeHi) { r.st[k++] = sp[j]; continue; }
                if (g_anyModule && sp[j] > 0x10000) {   // a return address into another module (cocos2d, Geode, the mod): committed and executable
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQuery(reinterpret_cast<LPCVOID>(sp[j]), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) r.st[k++] = sp[j];
                }
            }
            r.val = cur;
            r.prev = prevVal;
        } __except (EXCEPTION_EXECUTE_HANDLER) { r.val = cur; r.prev = prevVal; }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct DrArgs { HANDLE th; uintptr_t addr; bool ok; };

inline bool applyDr(uintptr_t addr) {
    if (!g_mainThread) return false;
    DrArgs args{g_mainThread, addr, false};
    HANDLE h = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* a = static_cast<DrArgs*>(p);
        if (SuspendThread(a->th) == (DWORD)-1) return 1;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(a->th, &c)) {
            if (a->addr) {
                c.Dr0 = a->addr;
                c.Dr7 = (c.Dr7 & ~((3ull << 16) | (3ull << 18))) | 1ull | (1ull << 16) | (3ull << 18);   // L0, write, 4 bytes
            } else {
                c.Dr7 &= ~1ull;
                c.Dr0 = 0;
            }
            c.Dr6 = 0;
            a->ok = SetThreadContext(a->th, &c) != 0;
        }
        ResumeThread(a->th);
        return 0;
    }, &args, 0, nullptr);
    if (!h) return false;
    WaitForSingleObject(h, 3000);
    CloseHandle(h);
    return args.ok;
}

inline void flushRows(const std::string& dir) {
    const int n = std::min(g_n.load(), kMaxRows);
    if (g_written >= n) return;
    if (!g_file.is_open()) {
        g_file.open(dir + "/hwwatch.csv", std::ios::trunc);
        g_file << "tick,rip,st0,st1,st2,st3,st4,st5,value_now,value_before\n";
    }
    char b[480];
    for (; g_written < n; ++g_written) {
        const Row& r = g_rows[g_written];
        int m = snprintf(b, sizeof(b), "%lld,%llx", r.tick, (unsigned long long)(r.rip - g_base));
        for (auto s : r.st) {
            if (!s) { m += snprintf(b + m, sizeof(b) - (size_t)m, ",0"); continue; }
            if (s >= g_codeLo && s < g_codeHi) { m += snprintf(b + m, sizeof(b) - (size_t)m, ",%llx", (unsigned long long)(s - g_base)); continue; }
            HMODULE hm = nullptr; char nm[80] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(s), &hm) && hm) { GetModuleFileNameA(hm, nm, sizeof(nm)); const char* sl = strrchr(nm, '\\'); memmove(nm, sl ? sl + 1 : nm, strlen(sl ? sl + 1 : nm) + 1); }
            m += snprintf(b + m, sizeof(b) - (size_t)m, ",%s+%llx", nm, (unsigned long long)(s - reinterpret_cast<uintptr_t>(hm)));
        }
        m += snprintf(b + m, sizeof(b) - (size_t)m, ",%08x,%08x\n", r.val, r.prev);
        g_file.write(b, m);
    }
    g_file.flush();
}

inline void disarmDr() {
    if (g_addr) {
        applyDr(0);
        g_addr = 0;
    }
}

inline long long readQword(uintptr_t a) { long long v = -2; __try { v = *reinterpret_cast<const long long*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { v = -2; } return v; }
inline void logStart(const std::string& dir, long long attempt, long long t) {
    const long long v = readQword(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + (uintptr_t)g_absRva);
    std::ofstream sf(dir + "/hwabs_start.txt", std::ios::app);
    sf << attempt << "," << t << "," << std::hex << v << std::dec << "\n";
}
// Called once a tick from checkCollisions (the main thread) with player 1.
inline void tick(void* player, long long t, const std::string& dir) {
    if ((g_off == 0 && g_absRva == 0) || !player) return;
    g_tickNow = t;
    if (g_absRva) {   // the global's value at the first tick of every attempt (the tick counter going backwards is a new attempt): hwabs_start.txt "attempt,tick,value"
        static long long prevT = -1, attempt = 0;
        if (t < prevT || prevT < 0) {
            ++attempt;
            logStart(dir, attempt, t);
        }
        prevT = t;
    }
    if (t < g_from) return;
    if (g_to > 0 && t > g_to) { disarmDr(); return; }
    uintptr_t base0 = reinterpret_cast<uintptr_t>(player);
    if (g_uid > 0) { if (!g_uidObj) return; base0 = reinterpret_cast<uintptr_t>(g_uidObj); }
    if (g_deref) {
        __try { base0 = *reinterpret_cast<const uintptr_t*>(base0 + (uintptr_t)g_deref); } __except (EXCEPTION_EXECUTE_HANDLER) { base0 = 0; }
        if (!base0) return;
    }
    const uintptr_t want = g_absRva ? reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + (uintptr_t)g_absRva : base0 + (uintptr_t)g_off;
    if (want != g_addr) {
        if (!g_mainThread) g_mainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, GetCurrentThreadId());
        if (!g_base) {
            g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
            g_codeLo = g_base + nt->OptionalHeader.BaseOfCode;
            g_codeHi = g_codeLo + nt->OptionalHeader.SizeOfCode;
        }
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, handler);
        g_addr = want;
        __try { g_last = *reinterpret_cast<const unsigned*>(want); } __except (EXCEPTION_EXECUTE_HANDLER) { g_last = 0; }
        applyDr(want);
    }
    flushRows(dir);
}

// Off the thread, whatever the state: at a session's end and when the level is left.
inline void disarm(const std::string& dir) {
    if (g_addr) {
        applyDr(0);
        g_addr = 0;
    }
    flushRows(dir);
    if (g_file.is_open()) g_file.close();
    g_written = 0;
    g_n = 0;
}
#else
inline void tick(void*, long long, const std::string&) {}
inline void disarm(const std::string&) {}
#endif

}  // namespace hwwatch