#pragma once
namespace orbtrace {
inline long long g_lastPress = -1; // most recent press tick
inline int g_lines = 0;            // total log lines (cap against flooding)
inline void reset() { g_lastPress = -1; }
}
namespace padtrace {
inline int g_lines = 0;            // total log lines (cap against flooding)
inline void reset() { g_lines = 0; }
}
