#pragma once

#ifdef ENABLE_DEBUG_TOOLS

namespace Fast::DebugCapture {

// Touch <appdir>/capture-request; each call writes <appdir>/capture-<label>.raw: u32 width, u32 height, RGBA8.
void Arm();
bool Pending();
void WriteBoundFramebuffer(const char* label, int width, int height);
void Finish();

} // namespace Fast::DebugCapture

#endif
