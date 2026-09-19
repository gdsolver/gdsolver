// GJBaseGameLayer's area effects: where GD's own random numbers can put an object an Area Move
// pushes (solver/areaenv.hpp). Every hook here only watches -- each calls the original with the
// arguments it was given, before or after looking.
#include "mod/playlayer_helpers.hpp"

using namespace p1;

namespace {

// Only while a session records moving geometry: the box goes into that recording and nowhere else.
bool envActive() {
    return g_cfg.areaEnv && grouptrace::g_on && g_started && !g_sessionOver;
}

// Area Rotate / Area Scale draw from the same table (their variances), and nothing here bounds
// them -- so they are counted, and the session line says how many ran.
bool rotScaleVaries(EnterEffectInstance* inst) {
    using namespace areaenv;
    return fld<float>(inst, kLenV) != 0.f || fld<float>(inst, kOffV) != 0.f
           || fld<float>(inst, kOffYV) != 0.f || fld<float>(inst, kRotV) != 0.f
           || fld<float>(inst, kScaleXV) != 0.f || fld<float>(inst, kScaleYV) != 0.f;
}

}  // namespace

class $modify(AreaEnvLayer, GJBaseGameLayer) {
    void processAreaMoveGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance,
                                    cocos2d::CCPoint position, int outerMin, int outerMax,
                                    int middleMin, int middleMax, int startIndex,
                                    bool targetGroups, bool reset) {
        const bool on = envActive() && instance
                        && areaenv::fld<void*>(instance, areaenv::kTrigger) != nullptr;
        if (on) {
            areaenv::g_act = areaenv::Action{true, this, instance,
                                             areaenv::fld<void*>(instance, areaenv::kTrigger),
                                             position, targetGroups};
        }
        GJBaseGameLayer::processAreaMoveGroupAction(objects, instance, position, outerMin,
                                                    outerMax, middleMin, middleMax, startIndex,
                                                    targetGroups, reset);
        if (on) {
            areaenv::settle();
            areaenv::g_act = areaenv::Action{};
        }
    }

    // processAreaMoveGroupAction resets each object it is about to process (GD's filter already
    // passed) and reads it straight after -- this is that moment, in that order.
    bool resetAreaObjectValues(GameObject* object, bool update) {
        const bool r = GJBaseGameLayer::resetAreaObjectValues(object, update);
        if (areaenv::g_act.on && areaenv::g_act.l == this && areaenv::g_inMove == 0 && object)
            areaenv::onProcessed(object);
        return r;
    }

    void moveAreaObject(GameObject* object, float dx, float dy) {
        ++areaenv::g_inMove;   // it resets its target itself; that is not a new object
        GJBaseGameLayer::moveAreaObject(object, dx, dy);
        --areaenv::g_inMove;
        if (areaenv::g_act.on && areaenv::g_act.l == this && object)
            areaenv::onMove(object, dx, dy);
    }

    void processAreaRotateGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance,
                                      cocos2d::CCPoint position, int outerMin, int outerMax,
                                      int middleMin, int middleMax, int startIndex,
                                      bool targetGroups, bool reset) {
        if (envActive() && instance && objects && objects->count() > 0
            && rotScaleVaries(instance))
            ++areaenv::g_unenveloped;
        GJBaseGameLayer::processAreaRotateGroupAction(objects, instance, position, outerMin,
                                                      outerMax, middleMin, middleMax,
                                                      startIndex, targetGroups, reset);
    }

    void processAreaTransformGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance,
                                         cocos2d::CCPoint position, int outerMin, int outerMax,
                                         int middleMin, int middleMax, int startIndex,
                                         bool targetGroups, bool reset) {
        if (envActive() && instance && objects && objects->count() > 0
            && rotScaleVaries(instance))
            ++areaenv::g_unenveloped;
        GJBaseGameLayer::processAreaTransformGroupAction(objects, instance, position, outerMin,
                                                         outerMax, middleMin, middleMax,
                                                         startIndex, targetGroups, reset);
    }
};
