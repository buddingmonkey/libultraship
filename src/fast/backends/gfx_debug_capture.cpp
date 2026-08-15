#ifdef ENABLE_DEBUG_TOOLS

#include "fast/backends/gfx_debug_capture.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengles2.h>

#include "ship/Context.h"

namespace Fast::DebugCapture {

static bool sPending = false;

static std::string RequestPath() {
    return Ship::Context::GetPathRelativeToAppDirectory("capture-request");
}

bool Pending() {
    static int framesUntilPoll = 0;

    if (sPending) {
        return true;
    }
    if (--framesUntilPoll > 0) {
        return false;
    }
    framesUntilPoll = 15;

    std::error_code ec;
    sPending = std::filesystem::exists(RequestPath(), ec);
    return sPending;
}

void WriteBoundFramebuffer(const char* label, int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    std::vector<uint8_t> pixels((size_t)width * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    const std::string path = Ship::Context::GetPathRelativeToAppDirectory(std::string("capture-") + label + ".raw");
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return;
    }

    const uint32_t header[2] = { (uint32_t)width, (uint32_t)height };
    fwrite(header, sizeof(header), 1, file);
    fwrite(pixels.data(), pixels.size(), 1, file);
    fclose(file);
}

void Finish() {
    std::error_code ec;
    std::filesystem::remove(RequestPath(), ec);
    sPending = false;
}

} // namespace Fast::DebugCapture

#endif
