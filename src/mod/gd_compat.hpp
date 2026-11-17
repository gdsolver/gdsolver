#pragma once
// Differences in GD's own functions between the versions the mod is built for (the build's GEODE_COMP_GD_VERSION).
#include <Geode/Geode.hpp>

#include <string>

#if GEODE_COMP_GD_VERSION >= 22090
#include <Windows.h>   // TerminateProcess (exitGame)
#endif

namespace gdcompat {

// GD's level-string compression. 2.209 put one more int in front of the key; GD's own callers pass -1 there. The key stays 0: no XOR
// layer, which is what the mod has always asked for. (The plain base64+gzip the mod could write by hand does not load: level_entry.hpp.)
inline std::string compressLevelString(std::string const& raw) {
#if GEODE_COMP_GD_VERSION >= 22090
    return cocos2d::ZipUtils::compressString(raw, false, -1, 0);
#else
    return cocos2d::ZipUtils::compressString(raw, false, 0);
#endif
}

// A level string in whichever of its three forms the game keeps it, as the raw text (the header's key,value pairs, then the objects, ';'-separated):
// raw already, gzip + base64 ("H4sI", what 2.0 and later levels are served as), or zlib + base64 ("eJ", what a level made before 2.0 is served as --
// game version 18 and below, which is 425 of the 683 levels the lab has fetched). The play menu's notes, the two-player flag, the attempt check and the
// slice all read the raw text and used to unpack only "H4sI": a level in the other form read as a base64 line with no header, and every note on it said
// "none" (a two-player level among them). Base64 has no ';' and every raw level has one, so a string without one is the stored form and GD's own
// decompressString (which reads both) is asked; an empty answer leaves the string as it was.
inline std::string rawLevelString(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    s = s.substr(b);
    if (s.find(';') != std::string::npos) return s;
    std::string d(cocos2d::ZipUtils::decompressString(s, false, 0));
    return d.empty() ? s : d;
}

// Ends the game, for a session that quits it. Geode's utils::game::exit first joins a thread of libcocos2d's CCEGLView, through a global it finds at
// the offset 2.2081 has (libcocos2d +0x1a8530); on 2.209 that global is elsewhere, the join fails and Geode throws a std::system_error out of the exit, so every
// session ended in an unhandled C++ exception and exit status 1.
//
// On 2.209 the process is ended here, with nothing after it. The director's end() and the CRT's exit() that follows ran Geode's shutdown inside ExitProcess:
// Geode's async runtime reports the workers it finds hung (ExitProcess has killed every thread but the caller by then, its stdout reader among them), the
// report is a write to stdout, stdout is a pipe nothing reads any more, and after a long session the pipe is full: the write blocks for good. The process
// stays in the table with one thread, ignores TerminateProcess, holds ~650 MB and keeps the worker from starting again (a full 22-level suite did it three
// times out of three). The session's own files are flushed before this is called (endSession), and a worker owes the game nothing else: its profile is not
// saved while it drives.
inline void exitGame() {
#if GEODE_COMP_GD_VERSION >= 22090
    ::TerminateProcess(::GetCurrentProcess(), 0);
#else
    geode::utils::game::exit(false);
#endif
}

}  // namespace gdcompat