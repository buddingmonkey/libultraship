#pragma once

#ifdef ENABLE_DEBUG_TOOLS

namespace Fast::DebugCapture {

// Touch <appdir>/capture-request; each call adds <appdir>/capture-<label>.raw, a u32 width and
// height then bottom-up RGBA8.
//
// Arm looks for the request, and it is the only thing that does. Call it once at the top of a
// frame: a capture of several images must start at a frame boundary or the images written before
// it are lost, and Finish consumes the request at the end of the same frame.
void Arm();
bool Pending();
void WriteBoundFramebuffer(const char* label, int width, int height);
void Finish();

} // namespace Fast::DebugCapture

#endif
