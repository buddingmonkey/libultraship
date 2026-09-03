#ifdef ENABLE_XR_WINDOW

#include "fast/backends/gfx_xr_view.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>

namespace Fast {

namespace {
// The game thread walks the display list, the shell copies the picture, and the GPU reports its own
// buffers on a third thread, so every part arrives from somewhere else.
std::mutex gCostMutex;
double gCost[7] = {};
int gFrames = 0;
int gPresents = 0;
double gNextReport = 0.0;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void AddXrCost(XrCost part, double seconds) {
    // A command buffer the GPU did no work in reports no time at all, and one that is still open
    // reports the two ends the wrong way round. Neither is a measurement.
    if (!(seconds > 0.0) || seconds > 10.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(gCostMutex);
    gCost[static_cast<int>(part)] += seconds;
}

void CountXrFrame() {
    std::lock_guard<std::mutex> lock(gCostMutex);
    ++gFrames;
}

void CountXrPresent() {
    std::lock_guard<std::mutex> lock(gCostMutex);
    ++gPresents;
}

void ReportXrCost() {
    double cost[7];
    int frames;
    int presents;
    {
        std::lock_guard<std::mutex> lock(gCostMutex);
        const double now = Now();
        if (now < gNextReport) {
            return;
        }
        const bool first = gNextReport <= 0.0;
        gNextReport = now + 1.0;
        frames = gFrames;
        presents = gPresents;
        for (int part = 0; part < 7; ++part) {
            cost[part] = gCost[part];
            gCost[part] = 0.0;
        }
        gFrames = 0;
        gPresents = 0;
        if (first) {
            return;
        }
        // A second with no frame in it is the one worth reporting, so it is said once. Saying it
        // every second would fill the log of an app that is parked and drawing nothing.
        static bool sIdle = false;
        if (frames < 1 && sIdle) {
            return;
        }
        sIdle = frames < 1;
    }

    const double perFrame = frames > 0 ? 1000.0 / frames : 0.0;
    SPDLOG_INFO("xr cost: frames {}/s, presents {}/s, interpreter {:.2f} ms, draw gpu {:.2f} ms, copy {:.2f} ms, "
                "copy gpu {:.2f} ms, prepare {:.2f} ms, wait {:.2f} ms, throttle {:.2f} ms",
                frames, presents, cost[static_cast<int>(XrCost::Interpreter)] * perFrame,
                cost[static_cast<int>(XrCost::DrawGpu)] * perFrame, cost[static_cast<int>(XrCost::Copy)] * perFrame,
                cost[static_cast<int>(XrCost::CopyGpu)] * perFrame,
                cost[static_cast<int>(XrCost::Prepare)] * perFrame, cost[static_cast<int>(XrCost::Wait)] * perFrame,
                cost[static_cast<int>(XrCost::Throttle)] * perFrame);
}

} // namespace Fast

#endif
