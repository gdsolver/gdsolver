// GJBaseGameLayer's area effects: where GD's own random numbers can put an object an Area Move
// pushes (solver/areaenv.hpp). Every hook here only watches -- each calls the original with the
// arguments it was given, before or after looking.
#include "mod/playlayer_helpers.hpp"

using namespace p1;

namespace {

// Only while a session records moving geometry: the box goes into that recording and nowhere else.
// Not inside a section search: its steps revisit one frame again and again (a player-snapshot
// restore does not re-stamp the objects), so every revisit read as a second varying action on the
// same object in the same frame. Measured (4002, dpsecauto): compound=1692 and the cold run's check
// called the level NOT CLEAN, on a route whose own flights never ran an Area Move at all.
bool envActive() {
    return g_cfg.areaEnv && grouptrace::g_on && g_started && !g_sessionOver
           && !secsolve::g_active;
}

// Area Rotate / Area Scale draw from the same table (their variances), and nothing here bounds
// them -- so they are counted, and the session line says how many ran.
bool rotScaleVaries(EnterEffectInstance* inst) {
    using namespace areaenv;
    return fld<float>(inst, kLenV) != 0.f || fld<float>(inst, kOffV) != 0.f
           || fld<float>(inst, kOffYV) != 0.f || fld<float>(inst, kRotV) != 0.f
           || fld<float>(inst, kScaleXV) != 0.f || fld<float>(inst, kScaleYV) != 0.f;
}

#ifdef GEODE_IS_WINDOWS
// cfg areadump=1: one row per processAreaMoveGroupAction call -- the centre point GD hands it (`position`: the player's place plus the action's offset, or a point of the screen), both
// players' places and the camera at that moment, the instance's numbers as the call reads them, and the first object's place as the area code reads it (the virtual getAreaObjectValue calls)
// beside its m_positionX. The reader that checks the shadow's centre against the game's instead of inferring it. Observation only.
void areaDumpRow(GJBaseGameLayer* l, cocos2d::CCArray* objects, EnterEffectInstance* inst, cocos2d::CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax,
                 int startIndex, bool targetGroups, bool reset) {
    static std::ofstream f;
    static int s_level = -1;
    const int level = suite::active() ? suite::current() : 0;
    if (!f.is_open() || level != s_level) {
        if (f.is_open()) f.close();
        f.open(std::string(DATA_DIR) + (level ? "/areadump_" + std::to_string(level) + ".csv" : std::string("/areadump.csv")), std::ios::trunc);
        f << "attempt,tick,trig,posX,posY,p1x,p1y,p2x,p2y,layerX,layerY,zoom,camPX,camPY,nObj,outerMin,outerMax,middleMin,middleMax,startIndex,targetGroups,reset,len,off,modFront,modBack,deadzone,moveX,moveY,"
             "obj0uid,obj0realX,obj0realY,obj0x,obj0y\n";
        s_level = level;
    }
    using namespace areaenv;
    void* trig = fld<void*>(inst, kTrigger);
    const int trigUid = trig ? static_cast<GameObject*>(trig)->m_uniqueID : -1;
    const float zoom = *reinterpret_cast<const float*>(reinterpret_cast<const char*>(l) + gdoff::kLayerCameraZoom);
    f.precision(10);
    f << g_attempt << ',' << g_tick << ',' << trigUid << ',' << position.x << ',' << position.y << ','
      << (l->m_player1 ? l->m_player1->getPositionX() : -1.f) << ',' << (l->m_player1 ? l->m_player1->getPositionY() : -1.f) << ','
      << (l->m_player2 ? l->m_player2->getPositionX() : -1.f) << ',' << (l->m_player2 ? l->m_player2->getPositionY() : -1.f) << ','
      << (l->m_objectLayer ? l->m_objectLayer->getPositionX() : 0.f) << ',' << (l->m_objectLayer ? l->m_objectLayer->getPositionY() : 0.f) << ',' << zoom << ',' << *reinterpret_cast<const float*>(reinterpret_cast<const char*>(l) + 0x410) << ',' << *reinterpret_cast<const float*>(reinterpret_cast<const char*>(l) + 0x414) << ','
      << (objects ? (int)objects->count() : -1) << ',' << outerMin << ',' << outerMax << ',' << middleMin << ',' << middleMax << ',' << startIndex << ',' << (int)targetGroups << ',' << (int)reset << ','
      << fld<float>(inst, kLen) << ',' << fld<float>(inst, 0x18) << ',' << fld<float>(inst, kModFront) << ',' << fld<float>(inst, kModBack) << ',' << fld<float>(inst, kDeadzone) << ','
      << fld<float>(inst, kMoveX) << ',' << fld<float>(inst, kMoveY) << ',';
    GameObject* o0 = (objects && objects->count() > 0) ? static_cast<GameObject*>(objects->objectAtIndex(0)) : nullptr;
    if (o0) {
        const cocos2d::CCPoint rp = areaPos(o0);
        f << o0->m_uniqueID << ',' << rp.x << ',' << rp.y << ',' << o0->m_positionX << ',' << o0->m_positionY << '\n';
    } else {
        f << "-1,0,0,0,0\n";
    }
    f.flush();

    // a call that works on parents (targetGroups): who each parent carries along -- the dictionary GD looks the children up in (instance +0xc0 + 1 of the layer's array, keyed by the parent's
    // m_uniqueID), once per (trigger, parent): areakids.csv = trig, parent uid, parent x, parent y, child count, child uids
    if (targetGroups && objects) {
        static std::ofstream g;
        static std::set<std::pair<int, int>> seen;
        static int s_levelK = -1;
        if (!g.is_open() || level != s_levelK) {
            if (g.is_open()) g.close();
            seen.clear();
            g.open(std::string(DATA_DIR) + (level ? "/areakids_" + std::to_string(level) + ".csv" : std::string("/areakids.csv")), std::ios::trunc);
            g << "trig,parent,px,py,nKids,kids\n";
            s_levelK = level;
        }
        auto* dicts = fld<cocos2d::CCArray*>(l, kLayerGroupDicts);
        const int gi = fld<int>(inst, kGroupIndex) + 1;
        auto* dict = (dicts && gi >= 0 && (unsigned)gi < dicts->count()) ? static_cast<cocos2d::CCDictionary*>(dicts->objectAtIndex(gi)) : nullptr;
        g.precision(10);
        for (unsigned i = 0; i < objects->count(); ++i) {
            auto* po = static_cast<GameObject*>(objects->objectAtIndex(i));
            if (!po || !seen.insert({trigUid, po->m_uniqueID}).second) continue;
            auto* kids = dict ? static_cast<cocos2d::CCArray*>(dict->objectForKey((intptr_t)fld<int>(po, kGroupKey))) : nullptr;
            g << trigUid << ',' << po->m_uniqueID << ',' << po->m_positionX << ',' << po->m_positionY << ',' << (kids ? (int)kids->count() : -1) << ',';
            if (kids)
                for (unsigned k = 0; k < kids->count(); ++k) {
                    auto* c = static_cast<GameObject*>(kids->objectAtIndex(k));
                    if (c) g << (k ? " " : "") << c->m_uniqueID;
                }
            g << '\n';
        }
        g.flush();
    }
}
#endif

#ifdef GEODE_IS_WINDOWS
// ---- cfg follow=1 / areaorder=1 / areavar=1 (observation only: plain member reads, no getObjectRect / tryGetObject, nothing the game could notice) --------------------------------------------------
std::ofstream& levelFile(std::ofstream& f, int& sLevel, const char* stem, const char* ext, const char* header) {
    const int level = suite::active() ? suite::current() : 0;
    if (!f.is_open() || level != sLevel) {
        if (f.is_open()) f.close();
        f.open(std::string(DATA_DIR) + "/" + stem + (level ? "_" + std::to_string(level) : std::string()) + ext, std::ios::trunc);
        f << header;
        sLevel = level;
    }
    return f;
}
// follow.csv: after each GJBaseGameLayer::processFollowActions, a row for each Follow node (a CCMoveCNode*, the vector at +0x7b0 / +0x7b8 of GJEffectManager in 22091, which the pinned bindings do not name: read
// raw -- decompile 0x339100) whose (dx, dy) differs from the last row written for its slot, and for a slot's first non-zero reading. The fields are the ones the function reads and writes (all raw, 22091):
// dx, dy = the node's +0x38 / +0x40 (the leader's movement since the node's last frame, times the factors), xmod, ymod = +0x80 / +0x88, centre = the int at +0x74, flag = the byte at +0x78 (the function takes the
// branch on it), cmd = the GroupCommandObject2* at +0xc8 (its m_triggerUniqueID / m_targetGroupID as the bindings name them -- unchecked against the 22091 layout). leader = the object the function takes as the
// leader of `centre` (the group's registered parent, else its only member: the same rule as trigext's `ctr`; -1 none); lx, ly = its +0x3b8 / +0x3c0 (doubles), lpx, lpy = its +0x4d0 / +0x4d4 (floats, the
// position the function subtracts), lframe = its +0x4dc (against the layer's counter at +0x3e0 = param_1[0x7c]).
#if GEODE_COMP_GD_VERSION >= 22091
void followRows(GJBaseGameLayer* l) {
    static std::ofstream f;
    static int s_level = -1;
    GJEffectManager* em = l->m_effectManager;
    if (!em) return;
    levelFile(f, s_level, "follow", ".csv", "attempt,tick,slot,uid,target,centre,leader,dx,dy,xmod,ymod,flag,lx,ly,lpx,lpy,lframe,frame\n");
    f.precision(10);
    static std::map<size_t, std::array<double, 2>> last;
    static int lastLevel = -1;
    if (lastLevel != s_level) { last.clear(); lastLevel = s_level; }
    const char* const eb = reinterpret_cast<const char*>(em);
    const char* const* b = *reinterpret_cast<const char* const* const*>(eb + gdoff::kEffMgrFollowNodes);
    const char* const* e = *reinterpret_cast<const char* const* const*>(eb + gdoff::kEffMgrFollowNodes + 8);
    if (!b || e < b || (size_t)(e - b) > 100000) return;
    auto mainObj = [&](int g) -> GameObject* {   // the registered parent first, else the group's only member (trigext's `ctr`)
        if (g <= 0) return nullptr;
        if (l->m_parentGroupsDict)
            if (auto* pa = static_cast<cocos2d::CCArray*>(l->m_parentGroupsDict->objectForKey(g)); pa && pa->count() > 0) return static_cast<GameObject*>(pa->objectAtIndex(0));
        if (auto* gr = l->getGroup(g); gr && gr->count() == 1) return static_cast<GameObject*>(gr->objectAtIndex(0));
        return nullptr;
    };
    size_t slot = 0;
    for (const char* const* it = b; it != e; ++it, ++slot) {
        const char* n = *it;
        if (!n) continue;
        const double dx = *reinterpret_cast<const double*>(n + gdoff::kNodeDeltaX), dy = *reinterpret_cast<const double*>(n + gdoff::kNodeDeltaY);
        auto lit = last.find(slot);
        if (lit == last.end()) { if (dx == 0.0 && dy == 0.0) continue; last[slot] = {dx, dy}; }
        else if (lit->second[0] == dx && lit->second[1] == dy) continue;
        else lit->second = {dx, dy};
        const int centre = *reinterpret_cast<const int*>(n + gdoff::kNodeCenter);
        GameObject* ld = mainObj(centre);
        auto* cmd = *reinterpret_cast<GroupCommandObject2* const*>(n + gdoff::kNodeCommand);
        const char* lb = reinterpret_cast<const char*>(ld);
        f << g_attempt << ',' << g_tick << ',' << slot << ',' << (cmd ? cmd->m_triggerUniqueID : -1) << ',' << (cmd ? cmd->m_targetGroupID : -1) << ',' << centre << ',' << (ld ? ld->m_uniqueID : -1) << ',' << dx << ',' << dy << ','
          << *reinterpret_cast<const double*>(n + gdoff::kNodeXMod) << ',' << *reinterpret_cast<const double*>(n + gdoff::kNodeYMod) << ',' << (int)*reinterpret_cast<const unsigned char*>(n + gdoff::kNodeFlag) << ','
          << (ld ? *reinterpret_cast<const double*>(lb + gdoff::kObjRealX) : 0.0) << ',' << (ld ? *reinterpret_cast<const double*>(lb + gdoff::kObjRealY) : 0.0) << ','
          << (ld ? *reinterpret_cast<const float*>(lb + gdoff::kObjLastX) : 0.f) << ',' << (ld ? *reinterpret_cast<const float*>(lb + gdoff::kObjLastY) : 0.f) << ',' << (ld ? *reinterpret_cast<const int*>(lb + gdoff::kObjLastFrame) : 0) << ','
          << *reinterpret_cast<const int*>(reinterpret_cast<const char*>(l) + gdoff::kLayerFrameCounter) << '\n';
    }
    f.flush();
}
#else
void followRows(GJBaseGameLayer*) {}   // (the offsets are 22091's: gd_offsets.hpp)
#endif

// areavar.txt: the layer's variance table and the objects' indices, once at the first Area effect of an attempt (the section headed `# attempt N tick T`): `table index value` for the non-zero entries,
// `object uid index` for the objects whose m_varianceIndex is non-zero.
void areaVarSection(GJBaseGameLayer* l) {
    static std::ofstream f;
    static int s_level = -1, s_attempt = -1;
    levelFile(f, s_level, "areavar", ".txt", "");
    if (s_attempt == g_attempt) return;
    s_attempt = g_attempt;
    f.precision(10);
    f << "# attempt " << g_attempt << " tick " << g_tick << '\n';
    for (size_t i = 0; i < l->m_varianceValues.size(); ++i)
        if (l->m_varianceValues[i] != 0.f) f << "table " << i << ' ' << l->m_varianceValues[i] << '\n';
    if (l->m_objects)
        for (auto* o : CCArrayExt<GameObject*>(l->m_objects))
            if (o && o->m_varianceIndex != 0) f << "object " << o->m_uniqueID << ' ' << o->m_varianceIndex << '\n';
    f.flush();
}

// areaorder.txt: `attempt tick type uid uid ...` -- the unique ids of the effect vector's m_gameObject in walk order, after the call (the call sorts the vector), written when the sequence differs from the
// last one written for that vector. A stride check: an element whose object is not an Area trigger (3006..3010) is written as -1 and the row is marked with `!`.
void areaOrderRow(std::vector<EnterEffectInstance>* effects, GJAreaActionType type) {
    static std::ofstream f;
    static int s_level = -1;
    static std::map<const void*, std::vector<int>> last;
    static int s_lastLevel = -1;
    levelFile(f, s_level, "areaorder", ".txt", "");
    if (s_lastLevel != s_level) { last.clear(); s_lastLevel = s_level; }
    std::vector<int> ids;
    bool bad = false;
    for (size_t i = 0; i < effects->size(); ++i) {
        EnterEffectObject* o = (*effects)[i].m_gameObject;
        if (!o) { ids.push_back(-1); continue; }
        if (o->m_objectID < 3006 || o->m_objectID > 3010) { bad = true; ids.push_back(-1); continue; }
        ids.push_back(o->m_uniqueID);
    }
    auto& prev = last[effects];
    if (prev == ids) return;
    prev = ids;
    f << g_attempt << ' ' << g_tick << ' ' << (int)type << (bad ? " !" : "");
    for (int u : ids) f << ' ' << u;
    f << '\n';
    f.flush();
}
#endif
#ifndef GEODE_IS_WINDOWS
// Android: an Area Move whose displacement can vary, which this platform counts instead of
// enveloping (see processAreaMoveGroupAction below).
bool moveVaries(EnterEffectInstance* inst) {
    using namespace areaenv;
    return fld<float>(inst, kLenV) != 0.f || fld<float>(inst, kOffV) != 0.f
           || fld<float>(inst, kOffYV) != 0.f || fld<float>(inst, kDistV) != 0.f
           || fld<float>(inst, kAngleV) != 0.f || fld<float>(inst, kMoveXV) != 0.f
           || fld<float>(inst, kMoveYV) != 0.f;
}
#endif

}  // namespace

class $modify(AreaEnvLayer, GJBaseGameLayer) {
    GDSOLVER_HOOK_GUARD()
#ifdef GEODE_IS_WINDOWS
    void processAreaMoveGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance,
                                    cocos2d::CCPoint position, int outerMin, int outerMax,
                                    int middleMin, int middleMax, int startIndex,
                                    bool targetGroups, bool reset) {
        if (g_cfg.areaDump && g_started && !g_sessionOver && instance) {
            areaDumpRow(this, objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
        }
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

    // cfg follow=1 / areaorder=1 / areavar=1 (above): the originals run first (areavar: the table as the call is about to read it), the hooks only read.
    void processFollowActions() {
        GJBaseGameLayer::processFollowActions();
        if (g_cfg.follow && g_started && !g_sessionOver && !secsolve::g_active) followRows(this);
    }
    void processAreaEffects(std::vector<EnterEffectInstance>* effects, GJAreaActionType type, float dt, bool visibleFrame) {
        const bool on = g_started && !g_sessionOver && !secsolve::g_active && effects;
        if (on && g_cfg.areaVar) areaVarSection(this);
        GJBaseGameLayer::processAreaEffects(effects, type, dt, visibleFrame);
        if (on && g_cfg.areaOrder) areaOrderRow(effects, type);
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
#else
    // Android inlines resetAreaObjectValues and moveAreaObject into this function, so the two
    // per-object moments the envelope is built and checked at cannot be hooked. A varying Area
    // Move is counted instead, the way Area Rotate / Scale are on every platform.
    void processAreaMoveGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance,
                                    cocos2d::CCPoint position, int outerMin, int outerMax,
                                    int middleMin, int middleMax, int startIndex,
                                    bool targetGroups, bool reset) {
        if (envActive() && instance && objects && objects->count() > 0 && moveVaries(instance))
            ++areaenv::g_unenveloped;
        GJBaseGameLayer::processAreaMoveGroupAction(objects, instance, position, outerMin,
                                                    outerMax, middleMin, middleMax, startIndex,
                                                    targetGroups, reset);
    }
#endif

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
