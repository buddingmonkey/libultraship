#pragma once

#ifdef ENABLE_DEBUG_TOOLS

namespace Fast::DebugPointer {

// Write "<u> <v> [down] [ms=<n>]" to <appdir>/debug-pointer to put the pointer on the window from
// the host, in the picture's own coordinates from 0 to 1. Nothing else reaches a window in a
// headset: the system routes no pinch to a full-space application, and adb shell input does not
// reach one either. Answers false while no request is live.
bool Poll(float* u, float* v, bool* down);

} // namespace Fast::DebugPointer

#endif
