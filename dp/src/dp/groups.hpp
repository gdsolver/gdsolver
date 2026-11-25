#pragma once
#include "dp/stairs.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>

namespace dp {

// --groups <file>: the MOD's `grouptrace=1` output, `tick,uid,cx,cy,w,h`.
// Keyed by uid because that is the only identifier both dumps share.
// --groups can be given more than once, and a later file OVERRIDES an earlier
// one for as far as it reaches. That is not a convenience -- it is the only way
// the timeline can be right.
//
// The cheap way to record it is one bootstrap pass with an empty plan and
// `nodeath=1`, which reaches the level end and fires everything that fires off
// the screen. But not every trigger does: lv19's gate at x=27,705 is opened by
// the player getting there, and in the bootstrap pass (where the player is dead
// and falling) it never opens at all. Measured on the two recordings of the
// same level: the bootstrap has ONE sample for those two blocks, the real
// replay has 99 each, sliding the 60 px gap open from t=19,606 to t=19,704 --
// exactly when the player arrives.
//
// So the driver records again on every GD replay of its own plan, and that
// recording wins wherever it reaches (its last tick). Beyond that the bootstrap
// fills in, and beyond THAT every object holds its last known rect.
using GroupTimeline = std::unordered_map<int, std::vector<DynSample>>;

// A file's bytes, read in one go. The in-process caches (here, triggers.hpp and cli.hpp) reuse a
// parse only when the file holds the same bytes as the one it came from, and read the file on every
// call to find out; read a character at a time (istreambuf_iterator) that read alone was most of a
// call's preparation on a custom level's 85 MB recording. False when the file cannot be opened.
inline bool readFileBytes(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize((size_t)n);
    if (n > 0 && !in.read(&out[0], n)) return false;
    return true;
}

// The uid the level loader gives an object's hazard twin (DynSample::env; the base is in
// object.hpp, where the kill counter reads it).
inline int envTwinUid(int uid) { return kEnvTwinUidBase + uid; }
// `endOut`, when the file carries the trailer the recorder writes (`end,<tick>`),
// comes back as the tick the RUN reached. That is what the overlay below has to
// use as the newer recording's reach: its last ROW stops as soon as nothing
// moves, and the ticks between that and the end of the run are covered by the
// run just as much (SubZero 4002, measured 2026-09-23). -1 = an older
// recording without the trailer, where the last row is all there is.
// The recordings' t=0 on/off (their `init,` lines), by uid, for the objects an activator
// switches (--activators; the loader reads it). Every recording of a level carries the same
// snapshot -- it is taken right after the game's reset, before anything the run did -- so the
// first one that has it is enough. Empty when none does.
inline std::unordered_map<int, uint8_t> g_groupInit;
// --bootauto (ON since 2026-10-24; --no-bootauto is the off arm): a recording the loop left out of the layers because it was made on another clock than the live record (the mod's groupsretime)
// still has the whole level's moving geometry in it, and an object that only an x-crossing trigger moves is placed from it at the model's OWN crossing
// tick (the autonomous re-timing), whatever clock the recording was made on. --groupsauto <file> names such a recording (weakest first, the later wins);
// the layers loader parses it with the others and keeps it apart (groupLayersFor), and the level loader reads it for an autonomously moved object that the
// layered record holds at rest (level_loader.hpp).
// Why another clock does not matter for these objects: the rows are read at the tick the MODEL's own x crosses the trigger (the autonomous re-timing: shift =
// the model's fire tick - the recording's first change), never at the recording's tick. Measured on a long custom level's run (its second iteration): the recording's
// x=24,645 crossing is at t=19,186, the plan's at 17,671 (the two routes took different speed-portal lanes before it); the saw's rows are read 1,516 ticks
// earlier and the model kills it at t=18,235, the tick GD did. One example, and it does not cover a speed change BETWEEN two triggers of one object: that is
// one shift for the object, as for every recording the model re-times (here both runs crossed x=24,645..24,855 at the same speed, 108 and 107 ticks).
// Cold rule: the recording is this same run's own no-death pass (startBootstrapRecord), not a previous run's.
inline bool g_bootAuto = true;
inline std::vector<std::string> g_groupAutoPaths;
inline std::vector<const std::unordered_map<int, std::vector<DynSample>>*> g_groupAutoTL;

// sscanf's %d and %f, one field at a time, on a recording held in memory. The reader was
// getline + sscanf("%d,%d,%f,%f,%f,%f,%d,%f,%d"), and on a custom level's 1.5M-row recording that
// was over two seconds of every call that found a recording changed (a custom level, measured 2026-09-30).
// These read the same text the same way: blanks before a number are skipped, the comma after it
// must follow directly, and the first field that does not convert ends the line with the count so
// far -- so a line cut off mid-write still loads or is skipped exactly as before.
namespace groupscan {

inline bool blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
inline bool digit(char c) { return c >= '0' && c <= '9'; }

inline bool scanInt(const char*& p, const char* e, int& out) {
    const char* q = p;
    while (q < e && blank(*q)) ++q;
    bool neg = false;
    if (q < e && (*q == '+' || *q == '-')) neg = *q++ == '-';
    if (q >= e || !digit(*q)) return false;
    long long v = 0;
    for (; q < e && digit(*q); ++q)
        if (v < 100000000000LL) v = v * 10 + (*q - '0');
    out = (int)(neg ? -v : v);
    p = q;
    return true;
}

// A plain decimal (the recorder writes %.3f) is m / 10^k with m below 2^53: the quotient of two
// exact doubles is the correctly rounded double, and with at most 8 fractional digits a value that
// is not a float's rounding midpoint cannot be within half a double ulp of one, so narrowing it
// gives the correctly rounded float -- the value %f produces. Anything else (an exponent, inf, nan,
// hex, more digits) goes to strtof, the C library's own reading of the same grammar.
inline bool scanFloat(const char*& p, const char* e, float& out) {
    static const double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8};
    const char* q = p;
    while (q < e && blank(*q)) ++q;
    const char* s = q;
    bool neg = false;
    if (q < e && (*q == '+' || *q == '-')) neg = *q++ == '-';
    unsigned long long m = 0;
    int nInt = 0, nFrac = 0;
    for (; q < e && digit(*q); ++q, ++nInt)
        if (nInt < 18) m = m * 10 + (unsigned long long)(*q - '0');
    if (q < e && *q == '.') {
        ++q;
        for (; q < e && digit(*q); ++q, ++nFrac)
            if (nInt + nFrac < 18) m = m * 10 + (unsigned long long)(*q - '0');
    }
    const bool letterNext = q < e && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z'));
    if (nInt + nFrac > 0 && nInt + nFrac <= 15 && nFrac <= 8 && !letterNext) {
        const double d = (double)m / kPow10[nFrac];
        out = (float)(neg ? -d : d);
        p = q;
        return true;
    }
    // strtof skips blanks itself, and from the line's end that would read the next line's number.
    if (s >= e) return false;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return false;
    out = v;
    p = end;
    return true;
}

// A literal in the format: must be the very next character.
inline bool scanChar(const char*& p, const char* e, char c) {
    if (p >= e || *p != c) return false;
    ++p;
    return true;
}

}  // namespace groupscan

// `initOut`, when given, receives the `init,<uid>,<on>` lines: each tracked object's on/off as
// the game's reset left it, before the first update (the mod's grouptrace::snapshotInit) -- the
// t=0 an object's switches (OnEvent) are applied to. A recording made before those lines existed
// leaves it empty. Without `initOut` the lines are skipped, as they always were.
// `bytes` is the whole file; `path` only names it in the line this prints.
// The rows of one recording, a line at a time: `line` takes one line as getline on a text-mode stream gives it
// (split at '\n', a '\r' before it dropped), the header NOT included. The whole-bytes reader and the file
// streamer below feed the same lines to the same code, so they build the same timeline.
struct GroupRowParser {
    GroupTimeline g;
    std::vector<int> order;   // the uids in the order their first row came: the order a table that adds to a parse has to insert in to iterate as a whole parse does
    long long rows = 0;
    long long* endOut;
    std::unordered_map<int, uint8_t>* initOut;
    GroupRowParser(long long* eo, std::unordered_map<int, uint8_t>* io) : endOut(eo), initOut(io) {
        if (endOut) *endOut = -1;
        if (initOut) initOut->clear();
    }
    void line(const char* b, const char* e) {
        using namespace groupscan;
        int t = 0, uid = 0, on = 1, env = 0;
        float cx = 0, cy = 0, w = 0, h = 0, rot = 0;
        // a run that is cut off mid-write leaves one short line; skip it.
        // `on` and `rot` are optional so a trace recorded before those columns
        // existed still loads (as "always there, never turned", which is what it
        // used to mean). `env` is written only on the rows that have it (the box
        // GD's random numbers can put the object in, DynSample::env), always
        // after a `rot`.
        const size_t len = (size_t)(e - b);
        if (len >= 4 && !std::memcmp(b, "end,", 4)) {
            if (endOut) *endOut = std::atoll(std::string(b + 4, e).c_str());
            return;
        }
        if (len >= 5 && !std::memcmp(b, "init,", 5)) {
            const char* q = b + 5;
            int u = 0, o = 1;
            if (initOut && scanInt(q, e, u) && scanChar(q, e, ',') && scanInt(q, e, o))
                (*initOut)[u] = o ? 1 : 0;
            return;
        }
        const char* q = b;
        int n = 0;
        if (scanInt(q, e, t) && ++n && scanChar(q, e, ',') && scanInt(q, e, uid) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, cx) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, cy) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, w) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, h) && ++n
            && scanChar(q, e, ',') && scanInt(q, e, on) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, rot) && ++n
            && scanChar(q, e, ',') && scanInt(q, e, env))
            ++n;
        if (n < 6) return;
        const auto slot = g.try_emplace(uid);
        if (slot.second) order.push_back(uid);
        slot.first->second.push_back({t, cx, cy, w * 0.5f, h * 0.5f,
                                      (uint8_t)(on ? 1 : 0), rot, (uint8_t)(env ? 1 : 0)});
        ++rows;
    }
    GroupTimeline finish(const std::string& path) {
        for (auto& kv : g) {
            std::sort(kv.second.begin(), kv.second.end(),
                      [](const DynSample& a, const DynSample& b2) { return a.t < b2.t; });
            kv.second.shrink_to_fit();   // push_back growth leaves up to a half again unused; at 113 million rows that is gigabytes
        }
        std::printf("groups: %lld samples for %zu objects (%s)\n", rows, g.size(),
                    path.c_str());
        return std::move(g);
    }
};

inline GroupTimeline parseGroupTimeline(const std::string& bytes, const std::string& path,
                                        long long* endOut = nullptr,
                                        std::unordered_map<int, uint8_t>* initOut = nullptr) {
    GroupRowParser P(endOut, initOut);
    const char* p = bytes.data();
    const char* const end = p + bytes.size();
    // Lines as getline on a text-mode stream gives them: split at '\n', a '\r' before it dropped.
    auto nextLine = [&](const char*& b, const char*& e) {
        if (p >= end) return false;
        b = p;
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', (size_t)(end - p)));
        e = nl ? nl : end;
        p = nl ? nl + 1 : end;
        if (nl && e > b && e[-1] == '\r') --e;
        return true;
    };
    const char* b = nullptr;
    const char* e = nullptr;
    nextLine(b, e);   // header
    while (nextLine(b, e)) P.line(b, e);
    return P.finish(path);
}

// The same timeline, read from the file in chunks of kGroupChunk bytes: the recording of a long level is 6 GB of text,
// and holding it whole next to the parse doubled the process's peak. False (nothing parsed, nothing printed but the
// complaint) when the file cannot be opened, as loadGroupTimeline answers.
constexpr size_t kGroupChunk = 32u << 20;
inline bool parseGroupTimelineFile(const std::string& path, GroupTimeline& out, long long* endOut = nullptr,
                                   std::unordered_map<int, uint8_t>* initOut = nullptr) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (endOut) *endOut = -1;
        if (initOut) initOut->clear();
        std::fprintf(stderr, "groups: cannot open %s\n", path.c_str());
        out = GroupTimeline{};
        return false;
    }
    GroupRowParser P(endOut, initOut);
    std::string buf;
    buf.resize(kGroupChunk);
    size_t have = 0;         // bytes of an unfinished line carried at the front of buf
    bool header = true;      // the first line of the file
    auto lines = [&](const char* s, const char* const end) {
        while (s < end) {
            const char* nl = static_cast<const char*>(std::memchr(s, '\n', (size_t)(end - s)));
            const char* e = nl ? nl : end;
            const char* next = nl ? nl + 1 : end;
            if (nl && e > s && e[-1] == '\r') --e;
            if (header) header = false;
            else P.line(s, e);
            s = next;
        }
    };
    while (true) {
        in.read(&buf[have], (std::streamsize)(buf.size() - have));
        const size_t got = (size_t)in.gcount();
        const size_t total = have + got;
        if (total == 0) break;
        const bool eof = got < buf.size() - have;
        size_t cut = total;
        if (!eof) {
            const char* nl = nullptr;
            for (size_t i = total; i > 0; --i) if (buf[i - 1] == '\n') { nl = &buf[i - 1]; break; }
            if (!nl) { buf.resize(buf.size() * 2); have = total; continue; }   // one line longer than the chunk
            cut = (size_t)(nl - buf.data()) + 1;
        }
        lines(buf.data(), buf.data() + cut);
        have = total - cut;
        if (have) std::memmove(&buf[0], &buf[cut], have);
        if (eof) break;
    }
    out = P.finish(path);
    return true;
}

// ---- the differential parse (the loop's recording grows by appending) ----
//
// The mod rewrites dp_groups.txt every round it records a replay, and the file of the next round is the last one up to where the new flight parts from the old
// one, then other rows: the recorder writes ticks in order, the game's moving geometry is deterministic, and the loop's next flight starts from an anchor a few
// ticks to a few thousand before the last one's death, so every row before that anchor's tick is the same row and the old file's last stretch is not.
// Measured on a large custom level (gsqhrpd2b cold, 2026-11-23, 14 generations): the groups part of a call's preparation was 243 s of a 2,334 s cold, every
// generation parsed from its first byte (the first version of this, which needed the new file to begin with the whole old file, was used 4 times of 14: the
// others part from the old file a little before its end). At the two generations written at the deaths t=4046 and t=18765 (158,638,514 and 3,314,273,004
// bytes) the first is, byte for byte, the first 158,638,505 bytes of the second.
//
// A parse keeps, beside its rows, one mark per tick (the file offset where the tick's lines begin and a 128-bit hash of the bytes before it) and the order the uids
// first appeared in. The next generation of the same path is read once: its bytes are hashed up to each of the old marks in turn, the last mark at which the two
// hashes are the same is where the files part, the old rows of ticks before it stand, the old rows from it on are dropped, and the new file is parsed from there.
// When nothing holds (the first tick differs), or an appended row could not be where a whole parse would put it, the whole file is parsed, as before.
// The table is built in the order a whole parse would build it (the uids by their first row in the new file), so it iterates the same.
// The hash is not cryptographic (two 64-bit lanes over 8-byte words, a different multiplier each): a prefix that differs from the old one and still hashes equal
// has a chance of about 2^-128 per mark and is taken to be none, the way a file signature is taken; DP_GROUPCHECK=1 builds the timeline again from nothing
// and compares it row for row.
struct PrefixHash {
    uint64_t a = 0x9E3779B97F4A7C15ULL, b = 0xC2B2AE3D27D4EB4FULL;
    uint64_t len = 0;
    unsigned char tail[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    unsigned tn = 0;   // bytes in `tail`, 0..7
    static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    void word(uint64_t w) {
        a = rotl((a ^ w) * 0x9E3779B97F4A7C15ULL, 31) * 0xC2B2AE3D27D4EB4FULL;
        b = rotl(b + w * 0x165667B19E3779F9ULL, 27) * 0x85EBCA77C2B2AE63ULL;
    }
    // the same value whatever the pieces the bytes come in
    void update(const char* p, size_t n) {
        len += n;
        if (tn) {
            while (n && tn < 8) { tail[tn++] = (unsigned char)*p++; --n; }
            if (tn < 8) return;
            uint64_t w;
            std::memcpy(&w, tail, 8);
            word(w);
            tn = 0;
        }
        while (n >= 8) {
            uint64_t w;
            std::memcpy(&w, p, 8);
            word(w);
            p += 8;
            n -= 8;
        }
        while (n) { tail[tn++] = (unsigned char)*p++; --n; }
    }
    bool same(const PrefixHash& o) const {
        return a == o.a && b == o.b && len == o.len && tn == o.tn && std::memcmp(tail, o.tail, tn) == 0;
    }
};

// One tick of the file: where its first row begins and what the bytes before that hash to. The rows of a recording come in tick order, so "the old file
// and the new one agree up to this mark" says which old rows are still the new file's rows.
struct TickMark {
    int tick = 0;
    uint64_t off = 0;
    PrefixHash h;
};

// What a parse of one recording keeps so the next generation of the file can be read as an addition to it.
struct GroupParseMeta {
    bool valid = false;
    long long rows = 0;         // rows in the parse
    long long endIn = -1;       // the last `end,<tick>` that is not part of the trailer (a mid-file one), -1 = none
    uint64_t pfxBytes = 0;      // the file's bytes before its trailer
    PrefixHash pfx;             // ...and their hash
    bool dupFree = false;       // no uid has two rows on one tick: an unstable sort of a longer vector could order those differently
    bool sorted = false;        // the rows' ticks never go back (the marks are then in order, and a tick's rows are one run of lines)
    std::vector<TickMark> marks;   // one per tick that has rows, in file order
    std::vector<int> order;     // the uids in the order their first row came (the insertion order of the table)
};

// parseGroupTimelineFile, keeping `meta`. With `base` (the cached timeline of an earlier generation of this path, whose meta is `baseMeta` and whose init
// lines are `baseInit`), the file is read as a continuation of it: the new file is read once and hashed, the last tick mark of the old parse at which the two
// files still hash the same is where they part (or the end of the old rows, when they never do), the old rows of earlier ticks stand, those from that tick on are
// dropped, and the new file is parsed from there. `out` is then `*base` cut and extended, and the function returns 1 (`*base` is moved from only in that case).
// Returns 0 for a whole parse (no base, or nothing of it held: `*base` is untouched), -1 when the file cannot be opened. What it prints is the line
// parseGroupTimelineFile prints. keptBytes / parsedBytes: the old file's bytes the rows of which were kept, and the bytes parsed.
inline int parseGroupTimelineFileMeta(const std::string& path, GroupTimeline& out, long long* endOut,
                                      std::unordered_map<int, uint8_t>* initOut, GroupParseMeta& meta,
                                      GroupTimeline* base = nullptr, const GroupParseMeta* baseMeta = nullptr,
                                      const std::unordered_map<int, uint8_t>* baseInit = nullptr,
                                      long long baseEnd = -1, uint64_t* keptBytes = nullptr,
                                      uint64_t* parsedBytes = nullptr) {
    meta = GroupParseMeta{};
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (endOut) *endOut = -1;
        if (initOut) initOut->clear();
        std::fprintf(stderr, "groups: cannot open %s\n", path.c_str());
        out = GroupTimeline{};
        return -1;
    }
    std::string buf;
    buf.resize(kGroupChunk);
    PrefixHash h;
    uint64_t resumeOff = 0;                 // where the parse of this read begins (0: the file's start)
    int resumeTick = std::numeric_limits<int>::max();   // the old rows of ticks from here on are dropped
    size_t keepMarks = 0;                   // the old marks that stand (those before the resume point)
    bool fullKeep = false;                  // the old parse stands whole, the new rows come after it
    const bool tryInc = base && baseMeta && baseMeta->valid && baseMeta->dupFree && baseMeta->sorted && baseMeta->pfxBytes > 0
                        && !baseMeta->marks.empty();
    if (tryInc) {
        // The new file's bytes are fed to the hash up to each mark's offset in turn (pieces that are multiples of 8, so the hash does not depend on where a
        // piece ends) and compared with the hash the old parse had there. The first mark that differs says the files part somewhere before it; the one
        // before it is the last place they are known to agree.
        const std::vector<TickMark>& mk = baseMeta->marks;
        uint64_t pos = 0;
        auto feedTo = [&](uint64_t target) {
            while (pos < target) {
                const size_t want = (size_t)std::min<uint64_t>(target - pos, buf.size());
                in.read(&buf[0], (std::streamsize)want);
                if ((size_t)in.gcount() != want) return false;
                h.update(buf.data(), want);
                pos += want;
            }
            return true;
        };
        size_t j = 0;
        bool allMarks = true;
        for (; j < mk.size(); ++j) {
            if (!feedTo(mk[j].off) || !h.same(mk[j].h)) { allMarks = false; break; }
        }
        if (allMarks) {
            if (feedTo(baseMeta->pfxBytes) && h.same(baseMeta->pfx)) {   // the whole old file: its trailer's start is where they part (or never)
                resumeOff = baseMeta->pfxBytes;
                keepMarks = mk.size();
                fullKeep = true;
            } else if (!mk.empty()) {   // they part inside the last tick's lines
                resumeOff = mk.back().off;
                resumeTick = mk.back().tick;
                keepMarks = mk.size() - 1;
                h = mk.back().h;
            }
        } else if (j > 0) {
            resumeOff = mk[j - 1].off;
            resumeTick = mk[j - 1].tick;
            keepMarks = j - 1;
            h = mk[j - 1].h;
        }
        // a mid-file `end,` line of the old file would be lost with the rows after the resume point
        if (resumeOff > 0 && !fullKeep && baseMeta->endIn >= 0) resumeOff = 0;
        in.clear();
        if (resumeOff > 0) in.seekg((std::streamoff)resumeOff, std::ios::beg);
        else { in.seekg(0, std::ios::beg); h = PrefixHash{}; }
    }
    const bool inc = resumeOff > 0;
    long long tailEnd = -1;
    GroupRowParser P(&tailEnd, initOut);
    long long lastEnd = -1, endIn = (inc && fullKeep) ? baseEnd : -1;
    bool haveSnap = false;
    PrefixHash snap;
    uint64_t snapOff = 0;
    uint64_t off = resumeOff;     // the file offset of buf[0]
    size_t have = 0;              // bytes of an unfinished line carried at the front of buf
    bool header = !inc;           // the first line of the file
    const char* hp = nullptr;     // the start of the bytes of the current piece not yet in `h`
    std::vector<TickMark> marks;
    bool sorted = true;
    int lastTick = inc && keepMarks > 0 ? baseMeta->marks[keepMarks - 1].tick : std::numeric_limits<int>::min();
    bool haveTick = inc && keepMarks > 0;
    auto lines = [&](const char* s0, const char* const end) {
        const char* s = s0;
        hp = s0;
        while (s < end) {
            const char* nl = static_cast<const char*>(std::memchr(s, '\n', (size_t)(end - s)));
            const char* e = nl ? nl : end;
            const char* next = nl ? nl + 1 : end;
            const char* le = e;
            if (nl && le > s && le[-1] == '\r') --le;
            if (header) header = false;
            else {
                const size_t len = (size_t)(le - s);
                const bool isEnd = len >= 4 && !std::memcmp(s, "end,", 4);
                if (isEnd) {
                    if (!haveSnap) {   // the first line of a trailer: the prefix ends here
                        h.update(hp, (size_t)(s - hp));
                        hp = s;
                        snap = h;
                        snapOff = off + (uint64_t)(s - buf.data());
                        haveSnap = true;
                    }
                    lastEnd = std::atoll(std::string(s + 4, le).c_str());
                } else {
                    if (haveSnap) {   // rows after an end line: it was not a trailer
                        haveSnap = false;
                        endIn = lastEnd;
                    }
                    if (len >= 5 && std::memcmp(s, "init,", 5) != 0) {   // a row's tick: a mark where a new tick's lines begin
                        const char* q = s;
                        int t = 0;
                        if (groupscan::scanInt(q, le, t) && q < le && *q == ',') {
                            if (!haveTick || t != lastTick) {
                                if (haveTick && t < lastTick) sorted = false;
                                h.update(hp, (size_t)(s - hp));
                                hp = s;
                                TickMark m;
                                m.tick = t;
                                m.off = off + (uint64_t)(s - buf.data());
                                m.h = h;
                                marks.push_back(m);
                                lastTick = t;
                                haveTick = true;
                            }
                        }
                    }
                }
                P.line(s, le);
            }
            s = next;
        }
    };
    while (true) {
        in.read(&buf[have], (std::streamsize)(buf.size() - have));
        const size_t got = (size_t)in.gcount();
        const size_t total = have + got;
        if (total == 0) break;
        const bool eof = got < buf.size() - have;
        size_t cut = total;
        if (!eof) {
            const char* nl = nullptr;
            for (size_t i = total; i > 0; --i) if (buf[i - 1] == '\n') { nl = &buf[i - 1]; break; }
            if (!nl) { buf.resize(buf.size() * 2); have = total; continue; }   // one line longer than the chunk
            cut = (size_t)(nl - buf.data()) + 1;
        }
        lines(buf.data(), buf.data() + cut);
        h.update(hp, (size_t)(buf.data() + cut - hp));
        hp = nullptr;
        off += cut;
        have = total - cut;
        if (have) std::memmove(&buf[0], &buf[cut], have);
        if (eof) break;
    }
    // the rows of this read, sorted as a whole parse sorts them, and a check that they can be put behind the old ones
    bool dupFree = true;
    GroupTimeline tailG = std::move(P.g);
    for (auto& kv : tailG) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const DynSample& x, const DynSample& y) { return x.t < y.t; });
        for (size_t i = 1; i < kv.second.size(); ++i)
            if (kv.second[i].t == kv.second[i - 1].t) { dupFree = false; break; }
    }
    long long rows = P.rows;
    std::vector<int> order;
    if (inc) {
        // the old rows of ticks before the resume tick, the uids in the order they first appeared (a whole parse's insertion history)
        GroupTimeline kept;
        order.reserve(baseMeta->order.size() + P.order.size());
        long long keptRows = 0;
        bool ok = dupFree && sorted;
        for (int uid : baseMeta->order) {
            const auto f = base->find(uid);
            if (f == base->end()) { ok = false; break; }
            std::vector<DynSample>& v = f->second;
            if (resumeTick != std::numeric_limits<int>::max()) {
                const auto cut = std::lower_bound(v.begin(), v.end(), resumeTick, [](const DynSample& x, int t) { return x.t < t; });
                v.erase(cut, v.end());
            }
            if (v.empty()) continue;
            keptRows += (long long)v.size();
            order.push_back(uid);
            kept.emplace(uid, std::move(v));
        }
        for (const auto& kv : tailG) {
            if (!ok) break;
            const auto d = kept.find(kv.first);
            if (d != kept.end() && !d->second.empty() && !(kv.second.front().t > d->second.back().t)) ok = false;
        }
        if (!ok) {   // a row that would not sort behind the old ones, two on one tick, ticks that go back: parse the whole file
            in.close();
            return parseGroupTimelineFileMeta(path, out, endOut, initOut, meta, nullptr, nullptr, nullptr, -1,
                                              keptBytes, parsedBytes);
        }
        // (`*base` has been cut and emptied of what moved: it is not used again by the caller on a return of 1)
        out = std::move(kept);
        for (int uid : P.order) {
            std::vector<DynSample>& src = tailG[uid];
            auto d = out.find(uid);
            if (d == out.end()) { out.emplace(uid, std::move(src)); order.push_back(uid); continue; }
            d->second.reserve(d->second.size() + src.size());
            d->second.insert(d->second.end(), src.begin(), src.end());
        }
        rows += keptRows;
        if (initOut) {   // the old init lines, then this read's over them, as one parse of the file would apply them
            std::unordered_map<int, uint8_t> in2 = baseInit ? *baseInit : std::unordered_map<int, uint8_t>{};
            for (const auto& kv : *initOut) in2[kv.first] = kv.second;
            *initOut = std::move(in2);
        }
        if (endOut) *endOut = tailEnd >= 0 ? tailEnd : (fullKeep ? baseEnd : -1);
        if (keptBytes) *keptBytes = resumeOff;
        if (parsedBytes) *parsedBytes = off - resumeOff;
        meta.marks.assign(baseMeta->marks.begin(), baseMeta->marks.begin() + (std::ptrdiff_t)keepMarks);
        meta.marks.insert(meta.marks.end(), marks.begin(), marks.end());
    } else {
        order = std::move(P.order);
        out = std::move(tailG);   // the table itself, as GroupRowParser::finish hands it back
        for (auto& kv : out) kv.second.shrink_to_fit();   // (sorted above)
        if (endOut) *endOut = tailEnd;
        if (keptBytes) *keptBytes = 0;
        if (parsedBytes) *parsedBytes = off;
        meta.marks = std::move(marks);
    }
    meta.valid = true;
    meta.rows = rows;
    meta.dupFree = inc ? true : dupFree;
    meta.sorted = sorted;
    meta.order = std::move(order);
    meta.endIn = haveSnap ? endIn : (lastEnd >= 0 ? lastEnd : endIn);
    if (haveSnap) {
        meta.pfxBytes = snapOff;
        meta.pfx = snap;
    } else {
        meta.pfxBytes = off;
        meta.pfx = h;
    }
    std::printf("groups: %lld samples for %zu objects (%s)\n", rows, out.size(), path.c_str());
    return inc ? 1 : 0;
}
inline GroupTimeline loadGroupTimeline(const std::string& path,
                                       long long* endOut = nullptr,
                                       std::unordered_map<int, uint8_t>* initOut = nullptr) {
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (endOut) *endOut = -1;
        if (initOut) initOut->clear();
        std::fprintf(stderr, "groups: cannot open %s\n", path.c_str());
        return GroupTimeline{};
    }
    return parseGroupTimeline(bytes, path, endOut, initOut);
}

// The last tick the --groups recordings cover (the largest `end,<tick>` trailer among the
// files; -1 = no trailer, or no recording). Read by --ridebox to tell a ride the recording
// saw end from one it was still inside when it stopped.
inline long long g_groupsEndT = -1;

// Bootstrap re-timing: an object that is still MOVING where the live recording ends
// continues on the bootstrap's rows, and those are on the bootstrap run's
// timeline, not this one's. The bootstrap (empty plan, nodeath) reaches each
// trigger's x a little later than a real run, and the lag grows along the level.
// Measured on lv21 (2026-09-22, cold it 2, live recording ending at t=11,033):
// aligning the two recordings object by object gives a shift of 1-3 ticks up to
// t=3,000, 5-6 over 3,000-8,000 and 8 over 10,000-11,033 (35 of 35 moving
// objects, residual < 0.05 px). For the rotating bar at x 14,355 (group 97, a
// rotate with no move, which rotsplit leaves on its recording) the seam puts the
// bar 8 ticks back on t=11,034: the model flies through a gap that GD has
// already closed, and GD kills the same plan four times (uid 14623 at t=11,033).
// Shifting the bootstrap's rows by those 8 ticks makes the same call end at
// t=11,033 as GD does.
//
// So: for an object that moves in the last kBootAlignWin ticks of the live
// recording, find the shift at which the bootstrap's rows reproduce the live
// ones there, and join the bootstrap on at that shift. An object that is at rest
// at the seam is left as it was -- its next motion starts on a trigger, and the
// trigger's anchor already re-times that (AutoTrig).
//
// On by default since the 22-level colds with it (2026-09-22): coin off changed
// lv21 alone (12 -> 8 iterations, the other 21 levels' [fp] identical), and coin
// on likewise (lv21 14 -> 5, every level 3/3). Always on since the 0.2.0 flag
// clean-up (it was --bootretime).
constexpr int kBootAlignWin = 60;      // ticks before the seam that are compared
constexpr int kBootAlignMaxShift = 240;
constexpr double kBootAlignMinTravel = 3.0;   // px inside the window
constexpr double kBootAlignTol = 0.05;        // px, mean |dcx| + |dcy|

// The shift s (bootstrap tick = live tick + s) at which `boot` reproduces the
// last kBootAlignWin ticks of `live`, or 0 when there is none or it is not clear.
// For the report only: *moving says the object was scored at all, *bestErr is
// the best mean error found and *nFit how many shifts came under the tolerance
// (the nearest of them is the one taken; more than one would be a periodic
// motion matching itself, which is not yet a reason to refuse).
inline int bootAlignShift(const std::vector<DynSample>& live,
                          const std::vector<DynSample>& boot, int cover,
                          bool* moving = nullptr, double* bestErrOut = nullptr,
                          int* nFit = nullptr) {
    std::vector<const DynSample*> win;
    for (const DynSample& s : live)
        if (s.t > cover - kBootAlignWin && s.t <= cover) win.push_back(&s);
    if (win.size() < 10 || boot.empty()) return 0;
    double travel = 0.0;
    for (const DynSample* s : win)
        travel = std::max(travel, std::fabs((double)s->cx - win.front()->cx)
                                      + std::fabs((double)s->cy - win.front()->cy));
    if (travel < kBootAlignMinTravel) return 0;
    if (moving) *moving = true;
    auto at = [&](int t) -> const DynSample* {
        const auto it = std::lower_bound(
            boot.begin(), boot.end(), t,
            [](const DynSample& a, int v) { return a.t < v; });
        return (it != boot.end() && it->t == t) ? &*it : nullptr;
    };
    int best = 0;
    double bestErr = 1e18;
    // Nearest shift first, so a periodic motion (a two-turn rotate passes every
    // point twice) keeps the smallest shift that fits.
    for (int k = 0; k <= 2 * kBootAlignMaxShift; ++k) {
        const int s = (k & 1) ? (k + 1) / 2 : -(k / 2);
        double err = 0.0;
        size_t n = 0;
        for (const DynSample* w : win) {
            const DynSample* b = at(w->t + s);
            if (!b) continue;
            err += std::fabs((double)b->cx - w->cx) + std::fabs((double)b->cy - w->cy);
            ++n;
        }
        if (n * 5 < win.size() * 4) continue;    // at least 80% of the window
        err /= (double)n;
        if (err < kBootAlignTol && nFit) ++*nFit;
        if (err < bestErr - 1e-9) { bestErr = err; best = s; }
    }
    if (bestErrOut) *bestErrOut = bestErr;
    return bestErr < kBootAlignTol ? best : 0;
}

// --groupholddeath: the LAST --groups file ended on a death, so its last row
// holds for one tick before the recordings under it take over. The producer
// says so, not dp: the mod appends the flag only when it passes the live
// recording, which it writes only after a replay died (harvestGroups), and that
// file is always the last overlay. The hold is tied to a death-terminated recording
// rather than applied to every overlay (an earlier form held every overlay's last row for a
// number of ticks; it is gone).
//
// Why a tick: the live recording stops on the tick GD killed the player, and dp
// decides that kill on its NEXT row, which otherwise comes from the recording
// underneath. Measured on lv20 (2026-09-22, cold it 12, anchor t=16,771): spike
// uid13068 is at x 24,356.64 with on=1 on the live recording's last row
// (t=16,777) and at x 27,399.96 with on=0 in the bootstrap, so the model plans
// through, and GD kills that plan at t=16,777. Holding the last row one tick,
// the same call plans a different branch, and that branch runs past 16,777 in
// GD (to 17,075, the next place on the list). An object the bootstrap re-timing
// has joined on is not held: its next row is the bootstrap's own continuation.
inline bool g_groupHoldDeath = false;
// --seamkeepon (default off): where a recording is laid over an older one, an object the overriding recording left SWITCHED (on/off differing from
// what the older recording had it at the seam) stands as the overriding one left it past the seam, until the older recording switches it itself.
// Without it the rows past the seam are the older recording's with ITS on/off, a world in which the plan never did what switched the object: the
// object is off up to the seam and on again the tick after - a custom level's spike a toggle ring and a toggle trigger had switched off by t=754
// stands again at t=812, a tick past the live recording's end at 808, and kills a plan the game lives through (the same plan lives to t=836 in the
// game). The object's on/off is still what the recorded run did, as it already is up to the seam; the chain that switches it (the ring a state may
// not have pressed) is not modelled here, only its consequence is carried over the seam.
inline bool g_seamKeepOn = false;

// `over` wins for every tick it covers; `base` supplies the rest. `deathEnd`
// says `over` ended on a death (--groupholddeath). `report`, when given, receives the lines this
// prints instead of stdout (cli.hpp's layer cache replays them on a hit).
// What an overlay replaced in `base`, one entry per uid it wrote, in the order it wrote them. Undone
// last to first, it gives `base` back as it was -- so the layer cache can keep the bottom recording's
// rows only once, in the merged timeline, instead of once parsed and once merged (undoGroupOverlay).
// The overlay keeps of the base rows only those after the overriding recording ends, so the entry
// holds what it dropped (`head`, the base rows up to there, as they were) and how to read the rest back
// off the merged rows: the merged rows after the overriding recording's own (`nOver` of them) are the
// base rows shifted back by `shift` ticks. That is a few percent of the base, not all of it.
// When the dropped rows are many (a custom level's bottom recording is gigabytes of them) they go to a
// file of our own (UndoSpill) instead of staying in memory, each entry remembering where and a sum of
// the bytes it wrote; the read back checks both.
struct GroupOverlayUndo {
    int uid = 0;
    bool had = false;       // the uid was in `base` before (else the overlay added it)
    int nOver = 0;          // the overriding recording's rows, which lead the merged rows
    int shift = 0;          // the re-timing shift the base rows were joined on at
    std::vector<DynSample> head;
    long long spillOff = -1;   // >= 0: `head` is in the spill file, spillN rows from this byte
    size_t spillN = 0;
    unsigned long long spillSum = 0;
    int ovrN = 0;              // --seamkeepon: this many of the merged rows after the overriding recording's got the overriding recording's on/off,
    uint8_t ovrOn = 1;         // ...in place of this one (the older recording's), which the undo puts back
};
// A 64-bit sum of the bytes of `n` rows (eight at a time).
inline unsigned long long rowsSum(const DynSample* p, size_t n) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    const size_t bytes = n * sizeof(DynSample);
    unsigned long long h = 1469598103934665603ULL;
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        unsigned long long w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ULL;
        h ^= h >> 29;
    }
    for (; i < bytes; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}
// The spill file of one cached timeline: a temp file this process makes and removes, written once
// and read back only by the undo that wrote it.
struct UndoSpill {
    std::string path;
    std::fstream f;
    long long size = 0;
    // `nearFile`: a file whose directory the spill goes to (the first recording's: the data directory of the session, which the
    // launcher empties for the next run, so a spill file a killed game leaves behind does not outlive it); else the temp directory.
    explicit UndoSpill(const std::string& nearFile = std::string()) {
        // (no <filesystem>: the mod compiles this header a second time inside a renamed namespace std, which it does not survive)
        static unsigned long long counter = 0;
        std::string dir;
        const size_t cut = nearFile.find_last_of("/\\");
        if (cut != std::string::npos) dir = nearFile.substr(0, cut);
        if (dir.empty()) {
            const char* e = std::getenv("TEMP");
            if (!e || !*e) e = std::getenv("TMP");
            dir = (e && *e) ? e : ".";
        }
        path = dir + "/gdsolver_undo_" + std::to_string((unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count())
               + "_" + std::to_string(++counter) + ".bin";
    }
    ~UndoSpill() {
        if (f.is_open()) f.close();
        if (opened) std::remove(path.c_str());
    }
    UndoSpill(const UndoSpill&) = delete;
    UndoSpill& operator=(const UndoSpill&) = delete;
    bool opened = false;
    // The file is made when the first rows are to be written, not before.
    bool ok() {
        if (!opened) {
            f.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
            opened = true;
        }
        return f.is_open() && f.good();
    }
    // Appends the rows; false (nothing recorded in u) when the write fails.
    bool put(GroupOverlayUndo& u, const std::vector<DynSample>& rows) {
        if (!ok() || rows.empty()) return false;
        f.seekp(size, std::ios::beg);
        f.write(reinterpret_cast<const char*>(rows.data()), (std::streamsize)(rows.size() * sizeof(DynSample)));
        if (!f.good()) return false;
        u.spillOff = size;
        u.spillN = rows.size();
        u.spillSum = rowsSum(rows.data(), rows.size());
        size += (long long)(rows.size() * sizeof(DynSample));
        return true;
    }
    // The rows u spilled, checked against the sum taken when they were written; false when they differ.
    bool get(const GroupOverlayUndo& u, std::vector<DynSample>& rows) {
        rows.resize(u.spillN);
        f.clear();
        f.seekg(u.spillOff, std::ios::beg);
        f.read(reinterpret_cast<char*>(rows.data()), (std::streamsize)(u.spillN * sizeof(DynSample)));
        return f.good() && rowsSum(rows.data(), rows.size()) == u.spillSum;
    }
};
// Rows an overlay may drop before they go to the spill file instead of staying in memory (32 bytes each).
inline size_t kUndoSpillRows = 8000000;   // (a variable only so that the round-trip test can lower it)
inline void overlayGroupTimelineImpl(GroupTimeline& base, GroupTimeline& over, bool consume,
                                     bool deathEnd, long long overEnd, std::string* report,
                                     std::vector<GroupOverlayUndo>* undo, UndoSpill* spill) {
    // How far the overriding recording actually reaches. One number for the
    // whole file, not per object: an object that simply did not move inside the
    // covered window has one sample and must NOT be treated as uncovered.
    // `overEnd` (the recorder's own trailer) is the tick its RUN reached, which
    // is past the last row whenever the level went quiet before the run ended --
    // and those quiet ticks are covered too. Without it the older recording's
    // rows for them win (4002 t=22,677, measured 2026-09-23).
    int cover = -1;
    for (const auto& kv : over)
        if (!kv.second.empty()) cover = std::max(cover, kv.second.back().t);
    if (cover < 0) return;
    if (overEnd > cover) cover = (int)overEnd;
    int nRetimed = 0, sLo = 0, sHi = 0;
    int nMoving = 0, nNoFit = 0, nMulti = 0, mostFits = 0, nHeld = 0;
    double worstJoin = 0.0;
    // The rows this overlay can drop, at most: every base row of a uid it writes. Over the line they are spilled.
    bool spillNow = false;
    size_t spilled = 0;
    if (undo && spill) {
        size_t est = 0;
        for (const auto& kv : over) {
            const auto b = base.find(kv.first);
            if (b != base.end()) est += b->second.size();
        }
        spillNow = est >= kUndoSpillRows && spill->ok();
    }
    for (auto& kv : over) {
        // (`consume`: the overriding rows are moved into the merged ones, `over` is not read again.)
        std::vector<DynSample> merged;
        if (!consume) merged = kv.second;
        GroupOverlayUndo u;
        u.uid = kv.first;
        u.nOver = (int)kv.second.size();
        auto it = base.find(kv.first);
        u.had = it != base.end();
        if (it != base.end()) {
            bool moving = false;
            double bestErr = 0.0;
            int nFit = 0;
            const int s = bootAlignShift(kv.second, it->second, cover, &moving, &bestErr,
                                         &nFit);
            if (moving) {
                ++nMoving;
                if (nFit == 0) ++nNoFit;
                else worstJoin = std::max(worstJoin, bestErr);
                if (nFit > 1) ++nMulti;
                mostFits = std::max(mostFits, nFit);
            }
            if (s != 0) {
                sLo = nRetimed ? std::min(sLo, s) : s;
                sHi = nRetimed ? std::max(sHi, s) : s;
                ++nRetimed;
            }
            const int hold = (deathEnd && s == 0) ? 1 : 0;
            if (hold > 0) ++nHeld;
            u.shift = s;
            // --seamkeepon: the on/off the overriding recording left the object in, at the end of its run
            const bool keepOn = g_seamKeepOn && !kv.second.empty();
            const uint8_t liveOn = keepOn ? kv.second.back().on : (uint8_t)1;
            if (consume) merged = std::move(kv.second);
            bool seamSeen = false, ovr = false, firstTail = true;
            uint8_t bootOn = 1;   // the older recording's on/off at the seam: its last row up to there
            for (const DynSample& b : it->second) {
                DynSample c = b;
                c.t -= s;
                if (c.t > cover + hold) {
                    // the first row after the seam: the object stands as the overriding recording left it, until the older one switches it
                    if (firstTail && keepOn && seamSeen && liveOn != bootOn && c.on == bootOn) ovr = true;
                    firstTail = false;
                    if (ovr) {
                        if (c.on == bootOn) { c.on = liveOn; ++u.ovrN; }
                        else ovr = false;   // the older recording switches the object itself: from there it is followed
                    }
                    merged.push_back(c);
                } else {
                    seamSeen = true;
                    bootOn = b.on;
                    if (undo) u.head.push_back(b);
                }
            }
            u.ovrOn = bootOn;
        } else if (consume) {
            merged = std::move(kv.second);
        }
        std::sort(merged.begin(), merged.end(),
                  [](const DynSample& a, const DynSample& b) { return a.t < b.t; });
        if (undo) {
            if (spillNow && !u.head.empty() && spill->put(u, u.head)) {
                spilled += u.head.size();
                std::vector<DynSample>().swap(u.head);
            }
            undo->push_back(std::move(u));
        }
        base[kv.first] = std::move(merged);
    }
    char b[512];
    std::string lines;
    std::snprintf(b, sizeof b, "groups: overlay covers t<=%d\n", cover);
    lines += b;
    std::snprintf(b, sizeof b, "groups: the bootstrap joined %d moving object(s) at a shift "
                  "of %d..%d ticks; %d moving at the seam, %d with no fitting "
                  "shift, worst fitting error %.4f px, %d with more than one "
                  "fitting shift (at most %d)\n", nRetimed, sLo, sHi, nMoving,
                  nNoFit, worstJoin, nMulti, mostFits);
    lines += b;
    if (deathEnd) {
        std::snprintf(b, sizeof b, "groups: --groupholddeath: the last recording ended on a "
                      "death at t=%d; %d object(s) hold its last row one tick\n", cover, nHeld);
        lines += b;
    }
    if (spilled > 0) {   // (where the dropped rows went, and the line past which an overlay does that)
        std::snprintf(b, sizeof b, "groups: undo log: %zu dropped rows (%.2f GB) written to %s (an overlay that can drop %zu rows or more does)\n",
                      spilled, (double)spilled * sizeof(DynSample) / 1073741824.0, spill->path.c_str(), kUndoSpillRows);
        lines += b;
    }
    if (report) *report += lines;
    else std::fputs(lines.c_str(), stdout);
}
inline void overlayGroupTimeline(GroupTimeline& base, const GroupTimeline& over,
                                 bool deathEnd = false, long long overEnd = -1,
                                 std::string* report = nullptr,
                                 std::vector<GroupOverlayUndo>* undo = nullptr) {
    overlayGroupTimelineImpl(base, const_cast<GroupTimeline&>(over), false, deathEnd, overEnd, report, undo, nullptr);
}
// The same, taking the overriding rows out of `over` as it goes (it is not to be read afterwards), and spilling
// what the undo log would hold when that is many rows.
inline void overlayGroupTimelineConsume(GroupTimeline& base, GroupTimeline& over, bool deathEnd, long long overEnd,
                                        std::string* report, std::vector<GroupOverlayUndo>* undo, UndoSpill* spill) {
    overlayGroupTimelineImpl(base, over, true, deathEnd, overEnd, report, undo, spill);
}

// Whether two timelines hold the same rows for the same objects (every field of every row).
inline bool groupTimelinesEqual(const GroupTimeline& a, const GroupTimeline& b) {
    if (a.size() != b.size()) return false;
    for (const auto& kv : a) {
        const auto it = b.find(kv.first);
        if (it == b.end() || it->second.size() != kv.second.size()) return false;
        for (size_t i = 0; i < kv.second.size(); ++i) {
            const DynSample &x = kv.second[i], &y = it->second[i];
            if (x.t != y.t || x.cx != y.cx || x.cy != y.cy || x.hw != y.hw || x.hh != y.hh || x.on != y.on || x.rot != y.rot
                || x.env != y.env)
                return false;
        }
    }
    return true;
}

// Takes back the overlays recorded in `undo` (last first), leaving `base` as it was before them. False when
// rows that were written to the spill file do not come back as written -- `base` is then not to be used.
inline bool undoGroupOverlay(GroupTimeline& base, std::vector<GroupOverlayUndo>& undo, UndoSpill* spill = nullptr) {
    for (size_t i = undo.size(); i-- > 0;) {
        GroupOverlayUndo& u = undo[i];
        if (!u.had) {
            base.erase(u.uid);
            continue;
        }
        if (u.spillOff >= 0 && (!spill || !spill->get(u, u.head))) return false;
        std::vector<DynSample>& m = base[u.uid];   // the merged rows: the overriding recording's, then the base's after it
        std::vector<DynSample> rows = std::move(u.head);
        rows.reserve(rows.size() + (m.size() > (size_t)u.nOver ? m.size() - (size_t)u.nOver : 0));
        for (size_t k = (size_t)u.nOver; k < m.size(); ++k) {
            DynSample b = m[k];
            b.t += u.shift;
            if (k - (size_t)u.nOver < (size_t)u.ovrN) b.on = u.ovrOn;   // --seamkeepon
            rows.push_back(b);
        }
        m = std::move(rows);
    }
    undo.clear();
    return true;
}

}  // namespace dp
