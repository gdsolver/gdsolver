#pragma once

// Standard-library and model-table includes shared by every dp/ header.
// (Stage A-2 mechanical split of leveldp.cpp; the chain of includes keeps
// the translation unit in its historical order.)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "models/ship_model.hpp"
#include "models/ufo_model.hpp"
#include "models/ship_params.hpp"
