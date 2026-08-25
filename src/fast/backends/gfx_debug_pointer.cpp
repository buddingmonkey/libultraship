#ifdef ENABLE_DEBUG_TOOLS

#include "fast/backends/gfx_debug_pointer.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "ship/Context.h"

namespace Fast::DebugPointer {

static float sU = 0.0f;
static float sV = 0.0f;
static bool sDown = false;
static bool sLive = false;
static bool sHolding = false;
static std::chrono::steady_clock::time_point sExpiry;
static std::filesystem::file_time_type sStamp;
static bool sHasStamp = false;

static std::string RequestPath() {
    return Ship::Context::GetPathRelativeToAppDirectory("debug-pointer");
}

static void Apply(const std::string& line) {
    float u = 0.0f;
    float v = 0.0f;
    bool down = false;
    bool placed = false;
    long holdMs = 0;

    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        long ms = 0;
        if (sscanf(token.c_str(), "ms=%ld", &ms) == 1) {
            holdMs = ms;
            continue;
        }
        if (token == "down") {
            down = true;
            continue;
        }
        if (sscanf(token.c_str(), "%f,%f", &u, &v) == 2) {
            placed = true;
        }
    }

    sLive = placed;
    sU = u;
    sV = v;
    sDown = placed && down;
    sHolding = holdMs > 0;
    if (sHolding) {
        sExpiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
    }
}

bool Poll(float* u, float* v, bool* down) {
    static int framesUntilPoll = 0;

    if (--framesUntilPoll <= 0) {
        framesUntilPoll = 6;
        std::error_code ec;
        const std::string path = RequestPath();
        const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(path, ec);
        if (!ec && (!sHasStamp || stamp != sStamp)) {
            sStamp = stamp;
            sHasStamp = true;
            std::ifstream file(path);
            std::string line;
            std::getline(file, line);
            Apply(line);
        }
    }

    if (sHolding && std::chrono::steady_clock::now() >= sExpiry) {
        sLive = false;
        sDown = false;
        sHolding = false;
    }
    if (!sLive) {
        return false;
    }

    *u = sU;
    *v = sV;
    *down = sDown;
    return true;
}

} // namespace Fast::DebugPointer

#endif
