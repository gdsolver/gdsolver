// See exe_guard.hpp.
#include "mod/exe_guard.hpp"

#include <algorithm>
#include <cstdio>

#if GDSOLVER_EXE_GUARD && GEODE_COMP_GD_VERSION >= 22090
#include "mod/ui_toast.hpp"
#endif

namespace gdsolver::exeguard {

#ifdef GEODE_IS_WINDOWS
namespace {
std::uint32_t stampOf(const wchar_t* module) {
    HMODULE m = GetModuleHandleW(module);   // nullptr names the executable
    if (!m) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(m) + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->FileHeader.TimeDateStamp : 0;
}
}  // namespace
#endif

const Verdict& verdict() {
    static const Verdict v = [] {
        Verdict r;
#if GDSOLVER_EXE_GUARD && defined(GEODE_IS_WINDOWS)
        // A Geode that supports this GD version checks the mod's version itself; the guard is for the forward compatibility mode.
        r.guarded = geode::Loader::get()->isForwardCompatMode();
        if (r.guarded) {
            r.exe = stampOf(nullptr);
            r.cocos = stampOf(L"libcocos2d.dll");
            r.extensions = stampOf(L"libExtensions.dll");
            r.match = std::any_of(std::begin(kBuilds), std::end(kBuilds), [&](const Build& b) {
                return b.exe == r.exe && b.cocos == r.cocos && b.extensions == r.extensions;
            });
        }
#endif
        return r;
    }();
    return v;
}

std::string message() {
    const Verdict& v = verdict();
    char b[400];
    std::snprintf(b, sizeof(b),
                  "gdsolver: this Geometry Dash build (GD v%u, libcocos2d %08x, libExtensions %08x) is not the one this mod was built for (%s); the mod is not loaded",
                  v.exe, v.cocos, v.extensions, kBuilds[0].name);
    return b;
}

std::string notice() {
    char b[160];
    std::snprintf(b, sizeof(b), "gdsolver is not loaded: this Geometry Dash build (GD v%u) is not the one this mod was built for", verdict().exe);
    return b;
}

}  // namespace gdsolver::exeguard

#if GDSOLVER_EXE_GUARD && GEODE_COMP_GD_VERSION >= 22090 && defined(GDSOLVER_GEODE6)

// Geode 6 builds its notifications on GD 2.2091 itself (and keeps them across scenes): no notice of our own.
$on_mod(Loaded) {
    if (gdsolver::exeguard::allowed()) return;
    geode::log::warn("{}", gdsolver::exeguard::message());
    geode::Notification::create(gdsolver::exeguard::notice(), geode::NotificationIcon::Warning, 12.f)->show();
}

#elif GDSOLVER_EXE_GUARD && GEODE_COMP_GD_VERSION >= 22090

namespace {

// Says it on screen, with nothing from the game: cocos2d's own nodes and the sprite frames the game has loaded. A scene change takes a toast with it, so
// it is put back on the running scene for the first ten seconds after the first scene is up, and the notice goes away by itself.
class GuardNotice : public cocos2d::CCObject {
public:
    static void start() {
        auto* n = new GuardNotice();
        n->autorelease();
        n->retain();
        cocos2d::CCDirector::sharedDirector()->getScheduler()->scheduleSelector(schedule_selector(GuardNotice::tick), n, 0.5f, false);
    }

    void tick(float dt) {
        m_t += dt;
        auto* scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;
        if (!m_toast && m_t >= 3.f) {
            m_toast = gdsolver::ui::Toast::create(gdsolver::exeguard::notice(),geode::NotificationIcon::Warning, 12.f);
            if (!m_toast) { stop(); return; }
            m_toast->retain();
            m_toast->show();
            m_shownAt = m_t;
        } else if (m_toast) {
            m_toast->keepOnScene();
            if (m_t - m_shownAt > 14.f) stop();
        }
    }

private:
    float m_t = 0.f, m_shownAt = 0.f;
    gdsolver::ui::Toast* m_toast = nullptr;

    void stop() {
        cocos2d::CCDirector::sharedDirector()->getScheduler()->unscheduleSelector(schedule_selector(GuardNotice::tick), this);
        if (m_toast) { m_toast->cancel(); m_toast->release(); m_toast = nullptr; }
        this->release();
    }
};

}  // namespace

$on_mod(Loaded) {
    if (gdsolver::exeguard::allowed()) return;
    geode::log::warn("{}", gdsolver::exeguard::message());
    GuardNotice::start();
}

#endif
