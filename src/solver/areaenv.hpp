#pragma once
// ============================================================
// Where GD's own random numbers can put an Area Move's objects (grouptrace's `env` rows)
//
// An Area Move (3006) can give its length, its offset, its move distance, its angle and its
// x/y moves a variance (the editor's "+-"). The variance is not drawn when the effect runs.
// Each such value is
//     base + variance * V[i]
// where V is GJBaseGameLayer's table at +0x10cc (m_varianceValues, 2000 floats), which
// GJBaseGameLayer::init fills on every level load from the LCG seed at 0x6c2ef8
// (V = 2 * (((seed >> 16) & 0x7fff) / 32767) - 1, so every V lies in [-1, 1] and both ends are
// reachable), and i is the object's own index (the short at +0x3f4) plus a fixed offset per
// quantity. GameObject::resetObject draws that index again for every object on every attempt,
// from the seed at 0x6c2ee0. Neither seed is ever reseeded, so where such an object sits
// depends on how many attempts, and which levels, the game has played before: the same plan
// flies a slightly different level each time, and a replay on someone else's game flies yet
// another.
//
// Measured on lv22 (2026-09-19/20): the 13 blocks of group 330 (Area Move uid 10705: length
// 1500 +- 500, direction away from centre group 304, which follows the player; an Edit Area
// Move sets its distance to 700 +- 150) land up to 278 px apart under three seed states, and a
// one-session run (lv21 first) moved them by up to 2 px against lv22 alone.
//
// No single V is the right one to plan against, so none is used: for every object an Area Move
// processes, this computes the box its displacement can take over EVERY value of the variances
// involved, and grouptrace writes that box, marked `env`, in place of the rect. The model treats
// the box as deadly while the object is enabled (dp's level loader). The box depends only on the
// trigger, the object's position without its area offset and the effect's centre -- not on V --
// so the recording, and every decision built on it, is the same whatever the seeds were.
//
// The formula is GD's, read from the 2.2081 binary:
//   processAreaMoveGroupAction 0x22a4d0  filter, distance / angle / x-y, direction, V offsets
//   resetAreaObjectValues      0x227c30  clears the object's area offset once per frame
//                                        (stamp obj+0x4e0 against layer+0x3e0)
//   getAreaObjectValue         0x228070  where the object is inside the area (offset, length)
//   (leaf)                     0x227b60  clamp((s / L - dz) / (1 - dz), 0, 1)
//   getEasedAreaValue          0x228260  101-point easing tables at layer+0x3020
//   moveAreaObject             0x22aab0  applies dy, and dx unless obj+0x2c8
// Everything is computed right after GD's own resetAreaObjectValues for the object, i.e. from
// the same state GD reads next, and checked against GD while it runs: the displacement this
// code predicts from the V actually drawn must be the one GD hands moveAreaObject, and it must
// lie inside the box. The session line `areaenv:` counts both.
//
// Not covered, and counted instead (`unenveloped`): Area Rotate / Area Scale actions with a
// variance, which move collision boxes through the same table. Advanced Follow reads the table
// too and is neither covered nor counted.
// ============================================================
#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace areaenv {

// ---- raw offsets, 2.2081 (the functions above) ----
constexpr size_t kLayerVar = 0x10cc;        // float[2000]
constexpr size_t kLayerEase = 0x3020;       // float*, easing tables
constexpr size_t kLayerGroupDicts = 0xf78;  // CCArray* of CCDictionary, targetGroups lookup
// EnterEffectInstance
constexpr size_t kLen = 0x10, kLenV = 0x14, kOffV = 0x1c, kOffYV = 0x24, kModFront = 0x28,
                 kModBack = 0x2c, kDeadzone = 0x30, kDist = 0x34, kDistV = 0x38, kAngle = 0x3c,
                 kAngleV = 0x40, kMoveX = 0x44, kMoveXV = 0x48, kMoveY = 0x4c, kMoveYV = 0x50,
                 kRotV = 0x74, kScaleXV = 0x5c, kScaleYV = 0x64, kTrigger = 0xa0,
                 kGroupIndex = 0xc0;
// EnterEffectObject (the trigger)
constexpr size_t kFixedDir = 0x77c, kDirVec = 0x780, kRelative = 0x788, kRelFade = 0x78c,
                 kEaseInType = 0x790, kEaseInRate = 0x794, kEaseInBuf = 0x798,
                 kEaseOutType = 0x79c, kEaseOutRate = 0x7a0, kEaseOutBuf = 0x7a4,
                 kDirType = 0x7c0, kXY = 0x7c4, kEaseOut = 0x7c5, kInwards = 0x7ec,
                 kSkipParent = 0x7f5;
// GameObject
constexpr size_t kLockX = 0x2c8, kOffX = 0x2a0, kOffY = 0x2a4, kGroupKey = 0x39c,
                 kVarIdx = 0x3f4, kStamp = 0x4e0, kMoveSkip = 0x520;
constexpr size_t kPosSlot = 0x4a8;          // virtual, the position getAreaObjectValue reads
// V offsets per quantity (processAreaMoveGroupAction / getAreaObjectValue)
constexpr int kVLen = 0, kVOff = 1, kVOffY = 2, kVMoveX = 8, kVMoveY = 9, kVDist = 10,
              kVAngle = 11;

template <class T>
inline T& fld(const void* p, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<char*>(const_cast<void*>(p)) + off);
}

// The position GD's area code reads: the object's virtual at +0x4a8, called the way
// processAreaMoveGroupAction calls it (this in rcx, the result through rdx).
inline cocos2d::CCPoint areaPos(GameObject* o) {
    using Fn = cocos2d::CCPoint* (*)(GameObject*, cocos2d::CCPoint*);
    auto fn = reinterpret_cast<Fn>((*reinterpret_cast<void***>(o))[kPosSlot / 8]);
    cocos2d::CCPoint p;
    return *fn(o, &p);
}

struct Range {
    double lo = 0, hi = 0;
    static Range of(double a) { return {a, a}; }
    void add(double a) { lo = std::min(lo, a); hi = std::max(hi, a); }
};
inline Range mul(const Range& a, const Range& b) {
    Range r = Range::of(a.lo * b.lo);
    r.add(a.lo * b.hi);
    r.add(a.hi * b.lo);
    r.add(a.hi * b.hi);
    return r;
}
// base + var * v over v in [-1, 1]
inline Range varied(float base, float var) {
    Range r = Range::of(base);
    if (var != 0.f) {
        r.add((double)base - std::fabs((double)var));
        r.add((double)base + std::fabs((double)var));
    }
    return r;
}

// ---- GD's arithmetic, float for float ----
// 0x227b60
inline float leaf(float s, int len, float dz) {
    const float x = s / (float)len;
    const float r = (dz == 0.f) ? x : (x - dz) / (1.f - dz);
    if (r > 1.f) return 1.f;
    if (r < 0.f) return 0.f;
    return r;
}

struct Ctx {
    GJBaseGameLayer* l;
    void* inst;
    void* trig;
    cocos2d::CCPoint p;   // the object, its area offset already cleared for this frame
    cocos2d::CCPoint c;   // the effect's centre
};

// getAreaObjectValue for chosen values of V[i + kVLen], V[i + kVOff], V[i + kVOffY]
inline float valueAt(const Ctx& k, float v0, float v1, float v2) {
    const float len = fld<float>(k.inst, kLen), lenV = fld<float>(k.inst, kLenV);
    const float offV = fld<float>(k.inst, kOffV), offYV = fld<float>(k.inst, kOffYV);
    const int dt = fld<int>(k.trig, kDirType);
    float s;
    if (dt == 1 || dt == 2) {
        const float j = offV == 0.f ? 0.f : offV * v1;
        const float s0 = (dt == 1 ? k.p.x - k.c.x : k.p.y - k.c.y) + j;
        s = s0 * (0.f <= s0 ? fld<float>(k.inst, kModBack) : fld<float>(k.inst, kModFront));
    } else {
        const float jy = offYV == 0.f ? 0.f : offYV * v2;
        const float jx = offV == 0.f ? 0.f : offV * v1;
        s = cocos2d::ccpDistance(k.p, cocos2d::CCPoint(jx + k.c.x, jy + k.c.y));
    }
    const float lv = lenV == 0.f ? 0.f : lenV * v0;
    float u = leaf(s, (int)(lv + len), fld<float>(k.inst, kDeadzone));
    if (fld<char>(k.trig, kInwards)) u = 1.f - u;
    return u;
}

// getEasedAreaValue's curve, without its state (which curve an ease-out trigger is on is decided
// by per-object state the caller supplies; both are evaluated where that matters)
inline float easeAt(GJBaseGameLayer* l, int buf, int type, float rate, float u) {
    if (buf == -1) return u;
    if (buf == -2) return GameToolbox::getEasedValue(u, type, rate);
    const float* tab = fld<float*>(l, kLayerEase);
    const int i = buf - (int)(u * -100.0f);
    return (u - (float)(int)(u * 100.0f) * 0.01f) * (tab[i + 1] - tab[i]) * 100.0f + tab[i];
}

// ---- session tallies (the `areaenv:` line) ----
inline long long g_objects = 0;     // objects an Area Move processed
inline long long g_varying = 0;     // ...whose box has a width
inline long long g_calls = 0;       // moveAreaObject calls checked
inline long long g_match = 0, g_miss = 0;     // predicted displacement vs GD's
inline long long g_outside = 0;     // GD's displacement outside the box
inline long long g_valueMiss = 0;   // valueAt vs GD's getAreaObjectValue
inline long long g_offMiss = 0;     // summed applied vs the object's own offset at record time
inline long long g_rectMiss = 0;    // GD's rect not where position + shape + offset puts it
inline long long g_compound = 0;    // two varying actions on one object in one frame
inline long long g_sampled = 0;     // boxes whose easing curve was sampled, not bounded exactly
inline long long g_degenerate = 0;  // length range reaching 0 -> position range taken as [0,1]
inline long long g_unenveloped = 0; // Area Rotate / Scale actions with a variance
inline long long g_rows = 0;        // env rows grouptrace wrote
inline double g_maxErr = 0.0;
inline std::string g_firstMiss;
// The solver's kills by these boxes (dp's hazard twins), counted from the level's entry. The box
// is conservative -- it also closes routes one game's seeds would leave open -- so non-zero means
// the search was pruned by uncertainty, not only by the level (audit AUD-20260920-01).
inline long long g_killsBase = 0;
inline long long solverKills() { return ::dpbridge::envKillsTotal() - g_killsBase; }

inline void resetTallies() {
    g_killsBase = ::dpbridge::envKillsTotal();
    g_objects = g_varying = g_calls = g_match = g_miss = g_outside = g_valueMiss = 0;
    g_offMiss = g_compound = g_sampled = g_degenerate = g_unenveloped = g_rows = 0;
    g_rectMiss = 0;
    g_maxErr = 0.0;
    g_firstMiss.clear();
}

// Per object: the box of its area displacement for the frame it was last processed in.
struct Entry {
    int stamp = INT_MIN;   // obj+0x4e0 when processed; valid while the object still carries it
    Range bx, by;          // displacement box, summed over the frame's actions
    double appx = 0, appy = 0;   // what GD applied, summed
    bool varies = false;
    int varyingActions = 0;
};
inline std::unordered_map<GameObject*, Entry> g_env;

// The object's rect relative to getPosition(), taken while it carries no area offset. The box
// is built from these and getPosition() -- which an Area Move does not change -- and NOT from
// the rect GD has now: that rect is a float at the displaced position, so taking the offset
// back out of it leaves a rounding error of up to half an ulp (0.001 px at x = 16,000) that
// depends on the offset, i.e. on V. Measured on lv22: 384 of 18,902 box rows differed in the
// third decimal between lv22 alone and lv22 after lv21 when the box was built that way.
struct Shape {
    float dx = 0, dy = 0, w = 0, h = 0;
    bool ok = false;
};
inline std::unordered_map<GameObject*, Shape> g_shape;

inline void takeShape(GameObject* o) {
    const auto r = o->getObjectRect();
    const auto& p = o->getPosition();
    Shape& s = g_shape[o];
    s.dx = (r.origin.x + r.size.width * 0.5f) - p.x;
    s.dy = (r.origin.y + r.size.height * 0.5f) - p.y;
    s.w = r.size.width;
    s.h = r.size.height;
    s.ok = true;
}

// The action being run (set by the processAreaMoveGroupAction hook), and the object it is on.
struct Action {
    bool on = false;
    GJBaseGameLayer* l = nullptr;
    void* inst = nullptr;
    void* trig = nullptr;
    cocos2d::CCPoint c;
    bool targetGroups = false;
};
inline Action g_act;
inline int g_inMove = 0;   // inside moveAreaObject: its own reset call is not a new object

// One processed object: what it should do to its targets, checked when the next one starts.
struct Pending {
    GameObject* parent = nullptr;
    bool call = false;         // GD should call moveAreaObject with (dx, dy)
    double dx = 0, dy = 0;
    bool alt = false;          // ease-out: the other curve's answer is also acceptable
    double dx2 = 0, dy2 = 0;
    Range bx, by;
    bool varies = false;
    std::vector<GameObject*> targets;
    std::vector<int> seen;     // moveAreaObject calls per target
};
inline Pending g_pend;

inline void clear() {
    g_env.clear();
    g_shape.clear();
    g_act = Action{};
    g_pend = Pending{};
    g_inMove = 0;
}

inline void noteMiss(const char* what, GameObject* o, double px, double py, double gx,
                     double gy) {
    if (!g_firstMiss.empty()) return;
    char b[200];
    snprintf(b, sizeof(b), "%s uid=%d predicted (%.4f,%.4f) game (%.4f,%.4f)", what,
             o ? o->m_uniqueID : -1, px, py, gx, gy);
    g_firstMiss = b;
}

// Close the previous object's check: every target got exactly the calls predicted, with the
// displacement predicted, and inside the box.
inline void settle() {
    Pending& p = g_pend;
    if (!p.parent) return;
    for (size_t i = 0; i < p.targets.size(); ++i) {
        const int want = p.call ? 1 : 0;
        if (p.seen[i] != want) {
            ++g_miss;
            noteMiss(p.call ? "no call where one was predicted" : "unpredicted call",
                     p.targets[i], p.dx, p.dy, NAN, NAN);
        }
    }
    p = Pending{};
}

// Called right after GD's resetAreaObjectValues(obj, true) inside processAreaMoveGroupAction:
// obj is the next object the action processes, in GD's order and after GD's filter.
inline void onProcessed(GameObject* o) {
    settle();
    const Action& a = g_act;
    GJBaseGameLayer* l = a.l;
    const float* V = reinterpret_cast<const float*>(reinterpret_cast<char*>(l) + kLayerVar);
    const int idx = fld<short>(o, kVarIdx);
    Ctx k{l, a.inst, a.trig, areaPos(o), a.c};
    ++g_objects;

    // --- the answer for the V GD drew ---
    cocos2d::CCPoint cc = a.c;
    bool show = false;
    const float uGame = l->getAreaObjectValue(static_cast<EnterEffectInstance*>(a.inst), o, cc,
                                              show);
    const float uMine = valueAt(k, V[idx + kVLen], V[idx + kVOff], V[idx + kVOffY]);
    if (!(std::fabs((double)uGame - (double)uMine) <= 1e-5)) ++g_valueMiss;

    const bool xy = fld<char>(a.trig, kXY) != 0;
    const bool rel = fld<char>(a.trig, kRelative) != 0;
    const bool fixedDir = fld<char>(a.trig, kFixedDir) != 0;
    const bool easeOut = fld<char>(a.trig, kEaseOut) != 0;
    const int inBuf = fld<int>(a.trig, kEaseInBuf), outBuf = fld<int>(a.trig, kEaseOutBuf);
    const int inType = fld<int>(a.trig, kEaseInType), outType = fld<int>(a.trig, kEaseOutType);
    const float inRate = fld<float>(a.trig, kEaseInRate), outRate = fld<float>(a.trig, kEaseOutRate);
    float relFade = fld<float>(a.trig, kRelFade);
    if (0.f > relFade) relFade = 0.f;
    const float M = fld<float>(a.inst, kDist), D = fld<float>(a.inst, kDistV);
    const float A = fld<float>(a.inst, kAngle), AV = fld<float>(a.inst, kAngleV);
    const float X = fld<float>(a.inst, kMoveX), XV = fld<float>(a.inst, kMoveXV);
    const float Y = fld<float>(a.inst, kMoveY), YV = fld<float>(a.inst, kMoveYV);

    // The direction for the non-x/y modes, and the relative mode's fade (no V in either)
    float dirx = 0.f, diry = 0.f, fade = 1.f;
    if (!xy) {
        if (rel) {
            const cocos2d::CCPoint d = areaPos(o) - a.c;
            const float len = std::sqrt(d.x * d.x + d.y * d.y);
            if (relFade > len) fade = len * (1.f / relFade);
            if (len > 0.f) { dirx = d.x / len; diry = d.y / len; }
            else { dirx = d.x; diry = d.y; }
        } else if (fixedDir) {
            const auto& v = fld<cocos2d::CCPoint>(a.trig, kDirVec);
            dirx = v.x; diry = v.y;
        }
    }

    Pending& p = g_pend;
    p.parent = o;
    if (uGame < 1.f) {
        float mx = 0.f, my = 0.f, m = 0.f;
        bool zero;
        if (xy) {
            mx = V[idx + kVMoveX] * XV + X;
            my = V[idx + kVMoveY] * YV + Y;
            zero = (mx == 0.f && my == 0.f);
        } else {
            m = (D == 0.f ? 0.f : D * V[idx + kVDist]) + M;
            zero = (m == 0.f);
        }
        p.call = true;
        if (!zero) {
            auto disp = [&](float e, double& ox, double& oy) {
                const float kk = 1.f - e;
                if (xy) { ox = kk * mx; oy = kk * my; return; }
                float ddx = dirx, ddy = diry;
                if (!rel && !fixedDir) {
                    const cocos2d::CCPoint u =
                        cocos2d::ccpForAngle(((V[idx + kVAngle] * AV + A) - 90.f) * 0.017453292f);
                    ddx = u.x; ddy = u.y;
                }
                const float t = kk * (m * fade);
                ox = t * ddx; oy = t * ddy;
            };
            disp(easeAt(l, inBuf, inType, inRate, uGame), p.dx, p.dy);
            if (easeOut) {
                p.alt = true;
                disp(easeAt(l, outBuf, outType, outRate, uGame), p.dx2, p.dy2);
            }
        }
    }

    // --- the box over every V ---
    const float lenV = fld<float>(a.inst, kLenV), offV = fld<float>(a.inst, kOffV);
    const float offYV = fld<float>(a.inst, kOffYV), len = fld<float>(a.inst, kLen);
    const float dz = fld<float>(a.inst, kDeadzone);
    const int dt = fld<int>(a.trig, kDirType);
    // position within the area, s
    Range sR;
    if (dt == 1 || dt == 2) {
        const double b = (dt == 1) ? (double)k.p.x - k.c.x : (double)k.p.y - k.c.y;
        const float mb = fld<float>(a.inst, kModBack), mf = fld<float>(a.inst, kModFront);
        auto g = [&](double s0) { return s0 * (0.0 <= s0 ? mb : mf); };
        const double e = std::fabs((double)offV);
        sR = Range::of(g(b - e));
        sR.add(g(b + e));
        if (b - e < 0.0 && 0.0 < b + e) sR.add(0.0);
    } else {
        const double ex = std::fabs((double)offV), ey = std::fabs((double)offYV);
        const double cx0 = k.c.x - ex, cx1 = k.c.x + ex, cy0 = k.c.y - ey, cy1 = k.c.y + ey;
        const double nx = std::max({0.0, cx0 - k.p.x, k.p.x - cx1});
        const double ny = std::max({0.0, cy0 - k.p.y, k.p.y - cy1});
        const double fx = std::max(std::fabs(k.p.x - cx0), std::fabs(k.p.x - cx1));
        const double fy = std::max(std::fabs(k.p.y - cy0), std::fabs(k.p.y - cy1));
        sR = Range::of(std::sqrt(nx * nx + ny * ny));
        sR.add(std::sqrt(fx * fx + fy * fy));
    }
    const int L0 = (int)(-std::fabs(lenV) + len), L1 = (int)(std::fabs(lenV) + len);
    Range uR;
    if (std::min(L0, L1) <= 0) {
        uR = Range::of(0.0);
        uR.add(1.0);
        ++g_degenerate;
    } else {
        uR = Range::of(leaf((float)sR.lo, L0, dz));
        uR.add(leaf((float)sR.lo, L1, dz));
        uR.add(leaf((float)sR.hi, L0, dz));
        uR.add(leaf((float)sR.hi, L1, dz));
        if (fld<char>(a.trig, kInwards)) uR = Range{1.0 - uR.hi, 1.0 - uR.lo};
    }
    Range bx = Range::of(0.0), by = Range::of(0.0);
    if (uR.lo < 1.0) {
        const double uhi = std::min(uR.hi, 1.0);
        // GD never eases u >= 1 (it does not move the object then, which kR adds below), and a
        // table read at exactly 1 would step past the curve's last point
        const float uTop = std::min((float)uhi, std::nextafter(1.f, 0.f));
        auto easeR = [&](int buf, int type, float rate) {
            Range e = Range::of(easeAt(l, buf, type, rate, (float)uR.lo));
            e.add(easeAt(l, buf, type, rate, uTop));
            if (buf >= 0) {
                // piecewise linear between the table's points: its extremes are points
                for (int q = (int)std::floor(uR.lo * 100.0) + 1; q < 100 && q * 0.01 < uhi; ++q)
                    e.add(easeAt(l, buf, type, rate, q * 0.01f));
            } else if (buf == -2) {
                for (int q = 1; q < 100; ++q)
                    e.add(easeAt(l, buf, type, rate, (float)(uR.lo + (uTop - uR.lo) * q / 100.0)));
                ++g_sampled;
            }
            return e;
        };
        Range eR = easeR(inBuf, inType, inRate);
        if (easeOut) {
            const Range e2 = easeR(outBuf, outType, outRate);
            eR.add(e2.lo);
            eR.add(e2.hi);
        }
        Range kR{1.0 - eR.hi, 1.0 - eR.lo};
        if (uR.hi >= 1.0) kR.add(0.0);   // positions where GD does not move it at all
        if (xy) {
            bx = mul(kR, varied(X, XV));
            by = mul(kR, varied(Y, YV));
        } else {
            const Range tR = mul(kR, mul(varied(M, D), Range::of(fade)));
            if (rel || fixedDir) {
                bx = mul(tR, Range::of(dirx));
                by = mul(tR, Range::of(diry));
            } else {
                // the angle's arc: cos / sin extremes at its ends and at the axes inside it
                const Range aR = varied(A, AV);
                const double a0 = (aR.lo - 90.0) * 0.017453292519943295;
                const double a1 = (aR.hi - 90.0) * 0.017453292519943295;
                Range cR = Range::of(std::cos(a0)), snR = Range::of(std::sin(a0));
                cR.add(std::cos(a1));
                snR.add(std::sin(a1));
                const double q = 1.5707963267948966;
                for (double t = std::ceil(a0 / q) * q; t < a1; t += q) {
                    cR.add(std::cos(t));
                    snR.add(std::sin(t));
                }
                bx = mul(tR, cR);
                by = mul(tR, snR);
            }
        }
    }
    const double w = std::max(bx.hi - bx.lo, by.hi - by.lo);
    p.varies = w > 1e-4;
    p.bx = bx;
    p.by = by;

    // --- the targets: the object itself, or the members of its group (targetGroups) ---
    if (!a.targetGroups) {
        p.targets.push_back(o);
    } else {
        auto* dicts = fld<cocos2d::CCArray*>(l, kLayerGroupDicts);
        const int gi = fld<int>(a.inst, kGroupIndex) + 1;
        auto* dict = (dicts && gi >= 0 && (unsigned)gi < dicts->count())
                         ? static_cast<cocos2d::CCDictionary*>(dicts->objectAtIndex(gi))
                         : nullptr;
        auto* kids = dict ? static_cast<cocos2d::CCArray*>(
                                dict->objectForKey((intptr_t)fld<int>(o, kGroupKey)))
                          : nullptr;
        const bool skipSelf = fld<char>(a.trig, kSkipParent) != 0;
        if (kids)
            for (unsigned i = 0; i < kids->count(); ++i) {
                auto* c = static_cast<GameObject*>(kids->objectAtIndex(i));
                if (!c) break;
                if ((c == o && skipSelf) || fld<char>(c, kMoveSkip)) continue;
                p.targets.push_back(c);
            }
    }
    p.seen.assign(p.targets.size(), 0);
    if (p.varies) ++g_varying;
    for (GameObject* t : p.targets) {
        // First time an area reaches it: it has never carried an area offset, so its rect is
        // its shape (after that, envelope() keeps the shape current)
        if (g_shape.find(t) == g_shape.end()) takeShape(t);
        Entry& e = g_env[t];
        const int st = fld<int>(t, kStamp);
        if (e.stamp != st) e = Entry{st};
        Range tx = p.bx;
        if (fld<char>(t, kLockX)) tx = Range::of(0.0);
        e.bx = {e.bx.lo + tx.lo, e.bx.hi + tx.hi};
        e.by = {e.by.lo + p.by.lo, e.by.hi + p.by.hi};
        if (p.varies) {
            e.varies = true;
            if (++e.varyingActions == 2) ++g_compound;
        }
    }
}

// moveAreaObject(t, dx, dy), inside an Area Move: check it against the pending prediction.
inline void onMove(GameObject* t, float dx, float dy) {
    Pending& p = g_pend;
    const double ax = fld<char>(t, kLockX) ? 0.0 : (double)dx, ay = (double)dy;
    auto it = g_env.find(t);
    if (it != g_env.end()) { it->second.appx += ax; it->second.appy += ay; }
    ++g_calls;
    size_t i = 0;
    while (i < p.targets.size() && p.targets[i] != t) ++i;
    if (!p.parent || i == p.targets.size()) {
        ++g_miss;
        noteMiss("call on an object not predicted", t, NAN, NAN, dx, dy);
        return;
    }
    ++p.seen[i];
    const double e1 = std::max(std::fabs(dx - p.dx), std::fabs(dy - p.dy));
    const double e2 = p.alt ? std::max(std::fabs(dx - p.dx2), std::fabs(dy - p.dy2)) : e1;
    const double err = std::min(e1, e2);
    g_maxErr = std::max(g_maxErr, err);
    if (err <= 1e-3) ++g_match;
    else { ++g_miss; noteMiss("displacement", t, p.dx, p.dy, dx, dy); }
    const double tol = 1e-3;
    if (dx < p.bx.lo - tol || dx > p.bx.hi + tol || dy < p.by.lo - tol || dy > p.by.hi + tol) {
        ++g_outside;
        noteMiss("outside the box", t, p.bx.lo, p.bx.hi, dx, dy);
    }
}

// For grouptrace: the box the object's rect can be in, when its placement depends on V this
// frame. (cx, cy, w, h) is the rect GD has now; the result replaces it.
constexpr double kPad = 0.01;   // float differences between this arithmetic and GD's
inline bool envelope(GameObject* o, float& cx, float& cy, float& w, float& h) {
    if (g_env.empty()) return false;
    const auto it = g_env.find(o);
    if (it == g_env.end()) return false;
    const Entry& e = it->second;
    // GD stamps the object at every reset, including the one that clears it after it leaves
    // every area, so a stale entry has a different stamp.
    if (e.stamp != fld<int>(o, kStamp) || !e.varies) {
        // Not placed by a variance this frame. Without an area offset on it, its rect is a
        // clean measurement of its shape (turns and scales by other triggers included).
        if (fld<float>(o, kOffX) == 0.f && fld<float>(o, kOffY) == 0.f) takeShape(o);
        return false;
    }
    if (std::fabs(e.appx - fld<float>(o, kOffX)) > 1e-3
        || std::fabs(e.appy - fld<float>(o, kOffY)) > 1e-3)
        ++g_offMiss;
    const auto sh = g_shape.find(o);
    if (sh == g_shape.end() || !sh->second.ok) {
        ++g_rectMiss;
        return false;
    }
    const Shape& s = sh->second;
    const auto& p = o->getPosition();
    const double bx = (double)p.x + s.dx, by = (double)p.y + s.dy;
    // The instrument's own check: GD's rect is where position + shape + offset puts it.
    if (std::fabs((double)cx - (bx + e.appx)) > 0.01 || std::fabs((double)cy - (by + e.appy)) > 0.01
        || std::fabs(w - s.w) > 0.01 || std::fabs(h - s.h) > 0.01)
        ++g_rectMiss;
    const double x0 = bx + e.bx.lo - s.w * 0.5 - kPad, x1 = bx + e.bx.hi + s.w * 0.5 + kPad;
    const double y0 = by + e.by.lo - s.h * 0.5 - kPad, y1 = by + e.by.hi + s.h * 0.5 + kPad;
    cx = (float)((x0 + x1) * 0.5);
    cy = (float)((y0 + y1) * 0.5);
    w = (float)(x1 - x0);
    h = (float)(y1 - y0);
    return true;
}

inline std::string summary() {
    char b[440];
    snprintf(b, sizeof(b),
             "areaenv: objects=%lld varying=%lld env_rows=%lld solver_box_kills=%lld | "
             "checked calls=%lld match=%lld "
             "miss=%lld (max err %.2g) outside_box=%lld value_miss=%lld offset_miss=%lld "
             "rect_miss=%lld | compound=%lld sampled=%lld degenerate=%lld unenveloped=%lld%s%s",
             g_objects, g_varying, g_rows, solverKills(), g_calls, g_match, g_miss, g_maxErr,
             g_outside,
             g_valueMiss, g_offMiss, g_rectMiss, g_compound, g_sampled, g_degenerate,
             g_unenveloped,
             g_firstMiss.empty() ? "" : " | first: ", g_firstMiss.c_str());
    return b;
}

}  // namespace areaenv
