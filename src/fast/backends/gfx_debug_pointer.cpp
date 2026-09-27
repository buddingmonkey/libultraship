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

struct Channel {
    const char* file;
    float u = 0.0f;
    float v = 0.0f;
    bool down = false;
    bool live = false;
    bool holding = false;
    std::chrono::steady_clock::time_point expiry;
    std::filesystem::file_time_type stamp;
    bool hasStamp = false;
    int framesUntilPoll = 0;
};

static Channel sPointer{ "debug-pointer" };
static Channel sKeyboard{ "debug-keyboard" };

static void Apply(Channel& channel, const std::string& line) {
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

    channel.live = placed;
    channel.u = u;
    channel.v = v;
    channel.down = placed && down;
    channel.holding = holdMs > 0;
    if (channel.holding) {
        channel.expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(holdMs);
    }
}

static bool Poll(Channel& channel, float* u, float* v, bool* down) {
    if (--channel.framesUntilPoll <= 0) {
        channel.framesUntilPoll = 6;
        std::error_code ec;
        const std::string path = Ship::Context::GetPathRelativeToAppDirectory(channel.file);
        const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(path, ec);
        if (!ec && (!channel.hasStamp || stamp != channel.stamp)) {
            channel.stamp = stamp;
            channel.hasStamp = true;
            std::ifstream file(path);
            std::string line;
            std::getline(file, line);
            Apply(channel, line);
        }
    }

    if (channel.holding && std::chrono::steady_clock::now() >= channel.expiry) {
        channel.live = false;
        channel.down = false;
        channel.holding = false;
    }
    if (!channel.live) {
        return false;
    }

    *u = channel.u;
    *v = channel.v;
    *down = channel.down;
    return true;
}

bool Poll(float* u, float* v, bool* down) {
    return Poll(sPointer, u, v, down);
}

bool PollKeyboard(float* u, float* v, bool* down) {
    return Poll(sKeyboard, u, v, down);
}

} // namespace Fast::DebugPointer

#endif
