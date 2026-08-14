#pragma once

namespace Fast {

// Where the aim ray meets the quad, in 0..1 of its width and height. False when nothing points at
// it. Needs no hand tracking: the aim pose is part of the interaction profile. Declared apart from
// gfx_openxr.h so the game can read it without the OpenXR headers.
bool GetXrPointer(float* u, float* v, bool* down);

} // namespace Fast
