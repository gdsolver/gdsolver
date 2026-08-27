#pragma once

// Phase 1: measured trace of the physics tick structure + state dump + autorun + input injection
//
// Output (the session data dir, resolved at startup by resolveDataDir):
//   trace.csv  — hook-call event stream (frame, attempt, step, event, a, b, c)
//   dump.csv   — player state after every PlayerObject::update
//   result.txt — session result
// Control (autorun.cfg, in that same dir):
//   enabled=1 / level=1 / attempts=2 / delay=2.0 / quitwhendone=1
//   input=<step>,<1|0>   (1=press, 0=release; multiple lines allowed; the same sequence is
//                         injected in every attempt)
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/GameStatsManager.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/AppDelegate.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <coroutine>
#include <thread>
#include <atomic>
#include <cmath>
#include <exception>
#include <new>
#include <climits>
#include <deque>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using namespace geode::prelude;
