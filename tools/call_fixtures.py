"""Fixtures for gd_call's guards (mod src/mod/callverify.hpp): a hazard slope is refused and not called, +0x7ed is forced to 1 after the patches, a function outside the allow-list is refused.
   python tools/call_fixtures.py [<.geode> [<worker>]]   -- needs a GD worker; no dangerous call is made, the point is that none reaches the game."""
import sys, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "mcp")); sys.path.insert(0, os.path.join(ROOT, "py"))
from gdmcp import tools as T
MOD = sys.argv[1] if len(sys.argv) > 1 else None      # the .geode to test (default: the repository build)
T.gd_session_open(1, int(sys.argv[2]) if len(sys.argv) > 2 else 100, {"callverify": "1"}, MOD)
T.gd_run(plan=[[0,0]], stop_at=60, timeout_s=120)
ok = True
def check(name, cond, extra=""):
    global ok
    print(("PASS " if cond else "FAIL ") + name, extra); ok = ok and cond
# 1. a slope with no hazard runs, and says where the hazard byte is
r = T.gd_call("collidedWithSlopeInternal", [], {"dt": 0.004})
check("plain slope call runs", r.get("ok") is True, str(r.get("error")))
off = r.get("slope_hazard_off")
check("slope_hazard_off reported", isinstance(off, int) and off > 0, str(off))
# 2. the hazard byte set by a patch: refused, not called (no player bytes come back)
r2 = T.gd_call("collidedWithSlopeInternal", [{"who": "s", "offset": off, "type": "u8", "value": 1}], {"dt": 0.004})
check("hazard slope refused", r2.get("ok") is False and "m_slopeIsHazard" in str(r2.get("error")), str(r2.get("error")))
check("refused call returned no bytes", "player_hex" not in r2)
# 3. the session still answers afterwards
r3 = T.gd_call("collidedWithSlopeInternal", [], {"dt": 0.004})
check("next call runs", r3.get("ok") is True)
# 4. +0x7ed patched to 0 for collidedWithObjectInternal: entered with 1 anyway
r4 = T.gd_call("collidedWithObjectInternal", [{"who": "p", "offset": 0x7ed, "type": "u8", "value": 0}], {"dt": 0.004, "noobj": 1, "rect": [0, 0, 30, 30]})
check("object call runs", r4.get("ok") is True, str(r4.get("error")))
pin = bytes.fromhex(r4["player_in_hex"]) if r4.get("ok") else b""
check("+0x7ed entered as 1 despite patch 0", len(pin) > 0x7ed and pin[0x7ed] == 1)
# 5. a not-allowed function is refused
r5 = T.gd_call("destroyPlayer", [], {})
check("function outside allow-list refused", r5.get("ok") is False, str(r5.get("error")))
# 6. pointer slots: a patch over one of the scratch object's heap pointers (the vtable at +0, or any listed slot) is refused and not called
slots = r4.get("ptr_slots_p") or []
check("ptr_slots_p reported (the vtable at 0 among them)", 0 in slots and len(slots) > 5, str(slots[:6]))
r6 = T.gd_call("updateSlopeRotation", [{"who": "p", "offset": 0, "type": "u64", "value": 0x1BF5220B440}], {"dt": 0.25})
check("patch over the vtable refused", r6.get("ok") is False and "heap pointer" in str(r6.get("error")), str(r6.get("error")))
slot = next((s for s in slots if s > 0x100), None)
r7 = T.gd_call("updateSlopeRotation", [{"who": "p", "offset": slot, "type": "u64", "value": 0}], {"dt": 0.25})
check("patch zeroing a pointer slot refused", r7.get("ok") is False and "holds" in str(r7.get("error")), "slot %s: %s" % (slot, r7.get("error")))
r8 = T.gd_call("updateSlopeRotation", [{"who": "p", "offset": 0x9c5, "type": "u8", "value": 1}, {"who": "p", "offset": 0x9c7, "type": "u8", "value": 1}], {"dt": 0.25})
check("flag bytes sharing an 8-byte word with values above 2^32 are NOT refused (judged by slot, not by shape)", r8.get("ok") is True, str(r8.get("error")))
vt = int.from_bytes(bytes.fromhex(r4["player_in_hex"])[0:8], "little")   # the scratch player's own vtable: an address of committed memory
r10 = T.gd_call("updateSlopeRotation", [{"who": "p", "offset": 0x9d0, "type": "u64", "value": vt}], {"dt": 0.25})
check("an address of committed memory written into a plain word is refused", r10.get("ok") is False and "committed memory" in str(r10.get("error")), str(r10.get("error")))
r9 = T.gd_call("updateSlopeRotation", [], {"dt": 0.25})
check("next call runs after the refusals", r9.get("ok") is True)
T.gd_session_close()
print("ALL PASS" if ok else "SOME FAILED")
