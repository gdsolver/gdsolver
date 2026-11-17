// cfg `vtime=<seconds>` (not given: the fast loop's dt while the fast loop runs, vtimeNow in fxcensus.hpp; 0 = off):
// while the mod drives, the scheduler is handed this fixed dt in place of the frame's wall-clock one. A cube's rotation
// on the tick it starts to fall (a custom level, t=740) moved with the frame's wall time (cfg lagms 0 / 10 / 30 gave
// 528.0 / 528.5 / 800.2 degrees) and with fastloops, so a plan that clears with 1,800 updates a frame dies at t=924
// with 1 to 16.
#include "mod/playlayer_helpers.hpp"
#include "mod/replaystep.hpp"
#include <Geode/modify/CCActionManager.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cstdlib>
#include <intrin.h>

using namespace p1;

class $modify(VTimeScheduler, cocos2d::CCScheduler) {
    GDSOLVER_HOOK_GUARD()
    void update(float dt) {
        if (botDriving()) {
            if (replaystep::active()) {
                dt = replaystep::feed(dt);
            } else {
                replaystep::reset();
                const float vt = vtimeNow();
                if (vt > 0.f) dt = vt;
            }
        }
        CCScheduler::update(dt);
    }
};

// cfg `rotdbg=<t0>,<t1>` (print only): every setRotation on player 1 in the tick range, with its return address.
class $modify(RotDbgPlayer, PlayerObject) {
    GDSOLVER_HOOK_GUARD()
    void setRotation(float r) {
        if (g_rotDbg1 >= g_rotDbg0 && (long long)g_tick >= g_rotDbg0 && (long long)g_tick <= g_rotDbg1 && botDriving()
            && GJBaseGameLayer::get() && this == GJBaseGameLayer::get()->m_player1) {
            std::string trace;
            {
                const uintptr_t lo = (uintptr_t)geode::base::get(), hi = lo + 0x1000000;
                const uintptr_t* sp = (const uintptr_t*)_AddressOfReturnAddress();
                int found = 0;
                for (int i = 0; i < 200 && found < 10; ++i) {
                    const uintptr_t v = sp[i];
                    if (v >= lo && v < hi) { char q[40]; snprintf(q, sizeof(q), " %llx", (unsigned long long)(v - lo)); trace += q; ++found; }
                }
            }
            writeResult("rotdbg stack:" + trace);
            char b[200];
            snprintf(b, sizeof(b), "rotdbg: t=%lld setRotation(%.4f) was %.4f ra=base+0x%llx", (long long)g_tick, r, this->getRotation(),
                     (unsigned long long)((uintptr_t)_ReturnAddress() - (uintptr_t)geode::base::get()));
            writeResult(b);
        }
        PlayerObject::setRotation(r);
    }
};

// cfg `camrand` (on; 0 = off): GJBaseGameLayer::updateCamera (0x23bcf0) draws the camera shake's offsets from the C library's
// rand(), once per shake interval, and nothing seeds that generator: the draws follow whatever else the process drew
// before. The shake moves the camera, and getMinPortalY / getMaxPortalY (the flight band, pmin / pmax in the dump) are
// made from the camera, so the band a flier is held to jittered by about 0.5 from one run to the next and with
// fastloops. With camrand the generator is seeded from the tick before each camera update, so a draw depends on the
// tick alone.
class $modify(CamRandLayer, GJBaseGameLayer) {
    GDSOLVER_HOOK_GUARD()
    void updateCamera(float dt) {
        if (g_camRand && botDriving()) std::srand((unsigned)(g_tick * 2654435761ull + 12345ull));
        GJBaseGameLayer::updateCamera(dt);
    }
};

// The action manager. Two jobs.
//  - cfg `replaystep` (replaystep.hpp): a frame that has gathered less than one step gives the actions no update at all, not an update of 0 s.
//    A cocos action drops the time of its first update (CCActionInterval::step starts its clock at 0 on the first call), so a zero-length
//    update spent on a freshly started action costs it nothing, where the same first update in the fast loop costs one step: on a screen of
//    144 fps the dash sprite of a custom level came out 16 degrees further along than at 60 fps and the player died.
//  - cfg `nodedbg=t0,t1` (print only): each update in the window, with its dt and caller.
class $modify(NodeDbgActionManager, cocos2d::CCActionManager) {
    GDSOLVER_HOOK_GUARD()
    void update(float dt) {
        if (replaystep::g_calls == 0 && replaystep::active()) return;
        if (g_nodeDbg1 >= g_nodeDbg0 && (long long)g_tick >= g_nodeDbg0 && (long long)g_tick <= g_nodeDbg1 && botDriving()) {
            char b[200];
            snprintf(b, sizeof(b), "amupdate: t=%lld frame=%lld dt=%.7f ra=base+0x%llx", (long long)g_tick, (long long)g_frame, dt,
                     (unsigned long long)((uintptr_t)_ReturnAddress() - (uintptr_t)geode::base::get()));
            writeResult(b);
        }
        CCActionManager::update(dt);
    }
};