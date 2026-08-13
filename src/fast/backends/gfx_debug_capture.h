#pragma once

#ifdef __ANDROID__

namespace Fast::DebugCapture {

// Touch <appdir>/capture-request; each call adds <appdir>/capture-<label>.raw, a u32 width and
// height then bottom-up RGBA8. A headset presents nothing that adb screencap can read.
bool Pending();
void WriteBoundFramebuffer(const char* label, int width, int height);
void Finish();

} // namespace Fast::DebugCapture

#endif
