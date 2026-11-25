#pragma once
// P9 (restructure draft 7.18 / 7.19): the field-by-field census of "the old tick's field = the field derived from the shadow", every tick (--fieldcheck, default off, observation only).
//
// A family (tools/state_families.py: input, collision, ring_pad_orb, portal, band_dash, rotation_frame, world_trigger) registers the names of the State members it derives and a derive function. After a tick the shadow
// stepped, shadowAdvanceImpl calls run(): the finished state `c` (the old tick's fields, the shadow's where it writes them) is copied, the family's function overwrites the copy's fields with what it derives from the
// shadow's side (Ctx: the state before the tick, the shadow thread and its world), and the two records (state_serial.hpp) are compared field by field. The counters say, per family and per field, in how many ticks
// the derived value differed; the first few cases are kept in full. A family whose counters are 0 over the calls that matter is ready for the next step: a flag that skips the old tick's write of those fields.
// Nothing here changes `c`; with the flag off run() is not called.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dp/state_serial.hpp"

namespace dp {
namespace fcheck {

inline bool g_on = false;
struct Ctx {
    const State& s;       // the state before the tick
    const State& c;       // the state after it (the old tick's fields; the shadow's where it writes them)
    long long t;
    const void* shadow;   // gdcol::SelfShadow* of the tick (null outside the shadow path)
    const void* thread;   // gdcol::ShadowThread*
    int input = 0;        // the plan's input level of this tick (0 / 1)
    const void* layer = nullptr;   // gdcol::ShadowLayer* the tick ended with (the band family)
    const State* old = nullptr;    // the old tick's own result, before the shadow wrote anything (the census of what a tick without the old tick would leave)
};
// the byte range of a member inside State (the table's order), taken from a State object
struct MemberRange { size_t off, size; };
inline const std::vector<MemberRange>& memberRanges() {
    static const std::vector<MemberRange> v = [] {
        State base{};
        std::vector<MemberRange> r;
#define X(k, n) r.push_back({(size_t)((const char*)&base.n - (const char*)&base), sizeof(base.n)});
        DP_STATE_FIELDS(X)
#undef X
        return r;
    }();
    return v;
}
// ...and a list of members as byte spans of State, in offset order with neighbours joined: what applyFamilies copies for a family every stepped tick (13 families a tick;
// looking each member up in the table and calling memcpy for it was most of what a family cost there)
struct Span { uint32_t off, size; };
inline std::vector<Span> spansOf(const std::vector<size_t>& ks) {
    std::vector<Span> v;
    for (size_t k : ks) { const MemberRange& m = memberRanges()[k]; v.push_back({(uint32_t)m.off, (uint32_t)m.size}); }
    std::sort(v.begin(), v.end(), [](const Span& a, const Span& b) { return a.off < b.off; });
    std::vector<Span> o;
    for (const Span& x : v) {
        if (!o.empty() && o.back().off + o.back().size >= x.off) { o.back().size = std::max(o.back().off + o.back().size, x.off + x.size) - o.back().off; continue; }   // (joined; a member named twice is one span)
        o.push_back(x);
    }
    return o;
}
inline void copySpans(State& to, const State& from, const std::vector<Span>& sp) {
    for (const Span& m : sp) {
        char* d = (char*)&to + m.off; const char* q = (const char*)&from + m.off;
        switch (m.size) {
            case 1: *d = *q; break;
            case 2: std::memcpy(d, q, 2); break;
            case 4: std::memcpy(d, q, 4); break;
            case 8: std::memcpy(d, q, 8); break;
            default: std::memcpy(d, q, m.size);
        }
    }
}
using Derive = std::function<void(const Ctx&, State& derived)>;
struct Example { long long t; std::string field; std::string oldHex, derivedHex; };
struct Family {
    std::string name;
    std::vector<std::string> fields;
    Derive derive;
    std::vector<size_t> idx;                              // the fields' indices in stser::fields()
    std::vector<size_t> extra;                            // members derive writes in its copy besides its own (declareExtraWrites): applyFamilies puts them back
    std::vector<Span> idxSpans, extraSpans;               // idx and extra as byte spans (spansOf), kept in step with them by registerFamily / declareExtraWrites
    std::atomic<long long> ticks{0}, anyDiff{0};
    std::unique_ptr<std::atomic<long long>[]> diff;       // per field
    std::vector<Example> ex;
    std::vector<int> exPer;                               // examples kept per field (3)
};
inline std::vector<std::unique_ptr<Family>>& registry() { static std::vector<std::unique_ptr<Family>> r; return r; }
inline std::mutex& mu() { static std::mutex m; return m; }

inline Family* registerFamily(const std::string& name, const std::vector<std::string>& fields, Derive derive) {
    auto f = std::make_unique<Family>();
    f->name = name; f->fields = fields; f->derive = std::move(derive);
    f->diff.reset(new std::atomic<long long>[fields.size()]());
    f->exPer.assign(fields.size(), 0);
    const auto& all = stser::fields();
    for (const std::string& n : fields) {
        size_t k = 0;
        for (; k < all.size(); ++k) if (n == all[k].name) break;
        if (k == all.size()) { std::printf("fieldcheck: family %s names %s, which is not a member of State\n", name.c_str(), n.c_str()); std::abort(); }
        f->idx.push_back(k);
    }
    f->idxSpans = spansOf(f->idx);
    registry().push_back(std::move(f));
    return registry().back().get();
}
// A derive that writes members of its copy other than its own fields names them here. applyFamilies keeps one copy for all the families and
// relies on this list to keep the copy equal to the state between them (--familycopycheck tests the list against a fresh copy a family).
inline void declareExtraWrites(Family* f, const std::vector<std::string>& fields) {
    const auto& all = stser::fields();
    for (const std::string& n : fields) {
        size_t k = 0;
        for (; k < all.size(); ++k) if (n == all[k].name) break;
        if (k == all.size()) { std::printf("fieldcheck: family %s names %s (extra write), which is not a member of State\n", f->name.c_str(), n.c_str()); std::abort(); }
        f->extra.push_back(k);
    }
    f->extraSpans = spansOf(f->extra);
}

inline std::string hexOf(const uint8_t* p, size_t n) { static const char* d = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n && i < 32; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }

// --fieldcheck-default NAME:f1,f2,...: a "census" family whose derived value is the member's default (State{}): the counts say in how many ticks each member is NOT at its default -- the fields a call never moves
// need no derivation at all
inline Family* registerDefaultCensus(const std::string& name, const std::vector<std::string>& fields) {
    Family* f = registerFamily(name, fields, nullptr);
    std::vector<size_t> idx = f->idx;
    f->derive = [idx](const Ctx&, State& d) {
        static const State dflt{};
        for (size_t k : idx) { const MemberRange& m = memberRanges()[k]; std::memcpy((char*)&d + m.off, (const char*)&dflt + m.off, m.size); }
    };
    return f;
}

// compare one family's derived copy of c with c
inline void runFamily(Family& f, const Ctx& cx) {
    State d = cx.c;
    f.derive(cx, d);
    std::vector<uint8_t> a, b;
    stser::serializeState(cx.c, a);
    stser::serializeState(d, b);
    std::vector<std::pair<uint32_t, uint32_t>> sa, sb;
    stser::fieldSlices(a, sa); stser::fieldSlices(b, sb);
    ++f.ticks;
    bool any = false;
    for (size_t j = 0; j < f.idx.size(); ++j) {
        const size_t k = f.idx[j];
        if (sa[k].second != sb[k].second || std::memcmp(a.data() + sa[k].first, b.data() + sb[k].first, sa[k].second) != 0) {
            ++f.diff[j]; any = true;
            std::lock_guard<std::mutex> lk(mu());
            if (f.exPer[j] < 3) { ++f.exPer[j]; f.ex.push_back({cx.t, f.fields[j], hexOf(a.data() + sa[k].first, sa[k].second), hexOf(b.data() + sb[k].first, sb[k].second)}); }
        }
    }
    if (any) ++f.anyDiff;
}
// The census of what the old tick moves (State before -> the old tick's result) and what the shadow (and the families' flags) then rewrite (the old tick's result -> the finished state), per member.
// A member the old tick moves, that no registered family names and that the finished state never differs from the old tick's in, is one nobody else writes: it would stay at its value before the tick.
// the members shadowAdvanceImpl writes itself (the c.* assignments of writeBody / writeBody2 and the derivations below them), by name
inline bool writtenByShadow(const char* n) { static const char* const k[] = {"xAbs","y","vy","mode","flip","mini","grounded","rot","rev","boost","dashing","dx","dual","y2","vy2","mode2","flip2","mini2","grounded2","rot2","dashing2","rev2","dx2","slRef","slRef2","prevWX","prevWY","onSlope","slopeT","trig","trigT","ctrlT","gravTouch","lockOff","usedOrb","usedOrbOld","held","jumpBuf","holdDead"}; for (const char* q : k) if (!std::strcmp(q, n)) return true; return false; }
struct Census { std::unique_ptr<std::atomic<long long>[]> moved, written; size_t n = 0; std::atomic<long long> ticks{0}; };
inline Census& census() { static Census c; return c; }
inline void runCensus(const Ctx& cx) {
    Census& C = census();
    if (!C.n) { std::lock_guard<std::mutex> lk(mu()); if (!C.n) { const size_t n = stser::fields().size(); C.moved.reset(new std::atomic<long long>[n]()); C.written.reset(new std::atomic<long long>[n]()); C.n = n; } }
    std::vector<uint8_t> a, b, d;
    stser::serializeState(cx.s, a); stser::serializeState(*cx.old, b); stser::serializeState(cx.c, d);
    std::vector<std::pair<uint32_t, uint32_t>> sa, sb, sd;
    stser::fieldSlices(a, sa); stser::fieldSlices(b, sb); stser::fieldSlices(d, sd);
    ++C.ticks;
    for (size_t k = 0; k < C.n; ++k) {
        if (sa[k].second != sb[k].second || std::memcmp(a.data() + sa[k].first, b.data() + sb[k].first, sa[k].second) != 0) ++C.moved[k];
        if (sb[k].second != sd[k].second || std::memcmp(b.data() + sb[k].first, d.data() + sd[k].first, sb[k].second) != 0) ++C.written[k];
    }
}
// --familycopycheck (off): applyFamilies also runs the families the old way (a fresh copy of the state for each) and aborts on the first
// tick the two results differ in -- the test that every derive writes only its own fields and the ones declareExtraWrites names.
inline bool g_familyCopyCheck = false;
inline void copyMembers(State& to, const State& from, const std::vector<size_t>& ks) {
    for (size_t k : ks) { const MemberRange& m = memberRanges()[k]; std::memcpy((char*)&to + m.off, (const char*)&from + m.off, m.size); }
}
// Lay every registered family's derivation over `c` (the State the tick has so far): each family writes only its own members.
// One scratch copy for all the families (a State is 736 bytes and there are 13 families: the copy a family was ~11% of a 1-thread step,
// gd-05 11-13): a family derives into it, its own fields go to `c`, and the other members it wrote (Family::extra) are put back from `c`,
// so the scratch equals `c` again before the next family -- what a fresh copy a family gave.
inline void applyFamiliesFresh(const Ctx& cx, State& c) {
    for (auto& f : registry()) {
        State d = c;
        f->derive(cx, d);
        copyMembers(c, d, f->idx);
    }
}
inline void applyFamilies(const Ctx& cx, State& c) {
    std::unique_ptr<State> refP;   // (--familycopycheck only: the State is not built otherwise)
    if (g_familyCopyCheck) {
        refP = std::make_unique<State>(c);
        const Ctx rx{cx.s, *refP, cx.t, cx.shadow, cx.thread, cx.input, cx.layer, cx.old};
        applyFamiliesFresh(rx, *refP);
    }
    static thread_local State d;
    d = c;
    for (auto& f : registry()) {
        const float y0 = c.y;
        f->derive(cx, d);
        copySpans(c, d, f->idxSpans);
        static const char* const envCyTrace = std::getenv("CYTRACE_FILE");   // read once: getenv takes the CRT's environment lock (shadow_step.hpp shDbgUid)
        if (c.y != y0 && envCyTrace) { if (std::FILE* lf = std::fopen(envCyTrace, "a")) { std::fprintf(lf, "applyFamilies: family %s moved c.y %.4f -> %.4f (d.y %.4f)\n", f->name.c_str(), (double)y0, (double)c.y, (double)d.y); std::fclose(lf); } }
        copySpans(d, c, f->extraSpans);
    }
    if (g_familyCopyCheck) {
        std::vector<uint8_t> a, b;
        stser::serializeState(*refP, a); stser::serializeState(c, b);
        if (a != b) { std::printf("familycopycheck: t=%lld the one-copy result differs from a fresh copy a family\n", cx.t); std::fflush(stdout); std::abort(); }
    }
}
inline void run(const Ctx& cx) { for (auto& f : registry()) runFamily(*f, cx); if (cx.old) runCensus(cx); }
inline void printCensus() {
    Census& C = census();
    if (!C.n) return;
    const auto& all = stser::fields();
    std::vector<char> covered(C.n, 0);
    for (auto& f : registry()) for (size_t k : f->idx) covered[k] = 1;
    std::printf("fieldcensus: %lld ticks. A member the old tick moves that no family names:\n", C.ticks.load());
    for (size_t k = 0; k < C.n; ++k) {
        if (!C.moved[k]) continue;
        std::printf("fieldcensus:   %-16s moved %9lld  rewritten after the old tick %9lld  %s\n", all[k].name, C.moved[k].load(), C.written[k].load(), covered[k] ? "family" : (writtenByShadow(all[k].name) ? "shadow" : "STALE"));
    }
}

inline void print() {
    {
        if (!g_on) return;
        for (auto& f : registry()) {
            std::printf("fieldcheck: family %-14s %lld ticks compared, %lld with a difference (%.4f%%)\n", f->name.c_str(), f->ticks.load(), f->anyDiff.load(), f->ticks ? 100.0 * (double)f->anyDiff.load() / (double)f->ticks.load() : 0.0);
            for (size_t j = 0; j < f->fields.size(); ++j) if (f->diff[j]) std::printf("fieldcheck:   %-16s differs in %lld ticks\n", f->fields[j].c_str(), f->diff[j].load());
            for (const Example& e : f->ex) std::printf("fieldcheck:   e.g. t=%lld %s old %s derived %s\n", e.t, e.field.c_str(), e.oldHex.c_str(), e.derivedHex.c_str());
        }
        if (registry().empty()) std::printf("fieldcheck: no family is registered\n");
        printCensus();
        if (const char* lp = std::getenv("FIELDCHECK_LOG")) {   // one tab-separated block per process (a harness that discards the stdout sums them): C field moved written covered / F family field diffs / T family ticks
            if (std::FILE* lf = std::fopen(lp, "a")) {
                Census& C = census();
                std::vector<char> covered(C.n, 0);
                for (auto& f : registry()) for (size_t k : f->idx) covered[k] = 1;
                const auto& all = stser::fields();
                for (size_t k = 0; k < C.n; ++k) if (writtenByShadow(all[k].name)) covered[k] = 2;
                std::fprintf(lf, "P\t%lld\n", C.ticks.load());
                for (size_t k = 0; k < C.n; ++k) if (C.moved[k] || C.written[k]) std::fprintf(lf, "C\t%s\t%lld\t%lld\t%d\n", all[k].name, C.moved[k].load(), C.written[k].load(), (int)covered[k]);
                for (auto& f : registry()) { std::fprintf(lf, "T\t%s\t%lld\t%lld\n", f->name.c_str(), f->ticks.load(), f->anyDiff.load()); for (size_t j = 0; j < f->fields.size(); ++j) if (f->diff[j]) std::fprintf(lf, "F\t%s\t%s\t%lld\n", f->name.c_str(), f->fields[j].c_str(), f->diff[j].load()); }
                std::fclose(lf);
            }
        }
    }
}

}  // namespace fcheck
}  // namespace dp