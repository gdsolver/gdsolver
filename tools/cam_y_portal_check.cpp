// cam_y_portal_check: dp/src/dp/cam_y_portal.hpp against values read from the dumps (t2\portalcam2.py): (mode, portal world y, noSnap) -> the dump's c1ec.
#include <cstdio>
#include "dp/cam_y_portal.hpp"
using namespace dp::gdcam;
int main() {
    struct C { const char* what; int mode; float y; bool nosnap; float want; } cs[] = {
        {"custom A cube->ship (tick 349), portal low", 5, 91.0f + 90.0f, false, 240.0f},
        {"custom B ship, file y 435", 5, 525.0f, false, 510.0f},
        {"custom C ship, file y 190.5", 5, 280.5f, false, 270.0f},
        {"custom C ufo, file y 318.665", 19, 408.665f, false, 390.0f},
        {"custom D wave, file y 885", 26, 975.0f, false, 960.0f},
        {"custom E wave, key 370, file y 360.75", 26, 450.75f, true, 450.75f},
        {"ball low portal (mode 16, h 240; dumps show 210)", 16, 120.0f, false, 210.0f},
    };
    int ok = 0, n = 0;
    for (auto& c : cs) {
        CamYPortalIn in; in.mode = c.mode; in.portalY = c.y; in.noSnap = c.nosnap;
        const CamYPortalOut o = camYPortal(in);
        const bool good = o.enter && std::fabs(o.target - c.want) < 0.01f; ++n; ok += good;
        std::printf("%s: target %.3f want %.3f %s\n", c.what, o.target, c.want, good ? "ok" : "MISS");
    }
    CamYPortalIn r; r.mode = 6; std::printf("cube: enter=%d (want 0)\n", (int)camYPortal(r).enter); ok += !camYPortal(r).enter; ++n;
    CamYPortalIn s; s.mode = 0x21; std::printf("spider: enter=%d h=%g (want 1, 270)\n", (int)camYPortal(s).enter, camYPortal(s).h); ok += camYPortal(s).enter && camYPortal(s).h == 270.0f; ++n;
    CamYPortalIn l; l.mode = 5; l.limitedBefore = true; std::printf("ship while limited: dur %.1f (want 0.4)\n", camYPortal(l).dur); ok += camYPortal(l).dur == 0.4f; ++n;
    std::printf("%d / %d\n", ok, n);
    return ok == n ? 0 : 1;
}
