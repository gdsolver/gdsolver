// seedlists_test: dp/src/dp/shadow_lite.hpp SeedLists under a writer that keeps growing it and readers that keep asking for ids that exist.
//   cl /std:c++20 /O2 /EHsc /I dp/src tools/seedlists_test.cpp
// A list's content is a function of its id (the list for id k holds k, k + 1, ... k + (k % 7)), so a reader can check what it was handed without asking the table; the writer interns the lists in a
// different order from the readers' ids, from several threads at once (two threads interning the same content must get one id), and goes on until the deque and the content index have grown many times.
#include "dp/shadow_lite.hpp"
#include <cstdio>
#include <thread>

using namespace dp::gdcol;

static std::vector<int> listFor(int k) { std::vector<int> v; for (int i = 0; i <= k % 7; ++i) v.push_back(k + i); return v; }

int main() {
    SeedLists& S = seedLists();
    constexpr int kLists = 60000, kWriters = 3, kReaders = 4;
    std::atomic<int> published{0};       // lists 1..published are interned (by whichever writer got there), in id order of the first writer
    std::atomic<bool> stop{false};
    std::atomic<long long> reads{0}, bad{0};
    // the first writer defines the ids: it interns list k as id k; the others re-intern the same contents (they must get the same ids back)
    std::thread w0([&] { for (int k = 1; k <= kLists; ++k) { const uint32_t id = S.intern(listFor(k)); if ((int)id != k) { ++bad; std::printf("writer 0: list %d got id %u\n", k, id); } published.store(k, std::memory_order_release); } });
    std::vector<std::thread> ws;
    for (int w = 1; w < kWriters; ++w) ws.emplace_back([&, w] {
        for (int round = 0; !stop.load(); ++round) {
            const int hi = published.load(std::memory_order_acquire);
            for (int k = 1 + (w * 977 + round * 131) % 50; k <= hi; k += 53) { const uint32_t id = S.intern(listFor(k)); if ((int)id != k) { ++bad; if (bad < 5) std::printf("writer %d: list %d got id %u\n", w, k, id); } }
        }
    });
    std::vector<std::thread> rs;
    for (int r = 0; r < kReaders; ++r) rs.emplace_back([&, r] {
        unsigned x = 12345u + (unsigned)r * 7919u;
        while (!stop.load()) {
            const int hi = published.load(std::memory_order_acquire);
            if (hi < 1) continue;
            for (int i = 0; i < 2000; ++i) {
                x = x * 1664525u + 1013904223u;
                const int k = 1 + (int)(x >> 8) % hi;
                const std::vector<int>* p = S.get((uint32_t)k);
                ++reads;
                if (!p || *p != listFor(k)) { if (++bad < 5) std::printf("reader %d: id %d %s\n", r, k, p ? "has other contents" : "is not there"); }
            }
        }
    });
    w0.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);
    for (auto& t : ws) t.join();
    for (auto& t : rs) t.join();
    std::printf("seedlists_test: %lld reads, %lld bad, overflow %lld\n", reads.load(), bad.load(), S.overflow());
    return bad.load() == 0 ? 0 : 1;
}
