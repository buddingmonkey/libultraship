#pragma once

#include <cstdint>

namespace Fast {

// Where the head is, measured from the viewpoint the game's own projection was made for, and how
// far that viewpoint sits from the window plane. Both in game units, in the window's axes: x
// right, y up, z towards the viewer. The interpreter turns the two into an off-axis frustum.
// Declared apart from gfx_openxr.h so the interpreter needs none of the OpenXR headers, and so a
// second headset backend can fill the same numbers.
struct XrViewGeometry {
    float eyeOffset[3];
    float windowDistance;
};

bool GetXrViewGeometry(XrViewGeometry* geometry);

// How near the game drew, in game units from the viewpoint. The glass goes at the nearest of
// these, so nothing stands in front of the window. It leaves the picture alone and only scales
// the world behind the glass.
void SetXrSceneNear(float units);

// The half-angle tangents the game asks for, after the widescreen adjustment. The backend sizes
// the window to match, so the framing does not change when the off-axis frustum replaces the
// game's own projection.
void SetXrViewTangents(float tanHalfWidth, float tanHalfHeight);

// How far the window hangs from the viewer, in meters. The window keeps the angular size the
// game's field of view gives it, so this does not change the framing: it scales the whole diorama
// towards the viewer, and a smaller world at a shorter range has more depth in it. Parallax on the
// glass never exceeds one eye separation whatever the distance, so no setting here can make the
// eyes diverge. The move bar writes the same number, so read it back before writing it again: the
// one that moved last owns the range.
void SetXrWindowDistance(float meters);
float GetXrWindowDistance();

// How large the window is drawn, as a multiple of the size the game's field of view gives it at
// that range. It holds the range and widens the angle, so a larger window shows a larger diorama
// where a shorter range shows a deeper one. The corner handles write the same number.
void SetXrWindowScale(float scale);
float GetXrWindowScale();

// Puts the window in front of the viewer, where they are looking now.
void RecenterXrWindow();

// Draws one image for both eyes instead of one per eye. There is no parallax then, but the frame
// costs half as much and only one layer reaches the compositor.
void SetXrStereo(bool enabled);

// How far the picture fades out at its edge, against the width the backend holds. Each eye looks
// through the window at its own angle, so the edge cuts a different sliver of the world out of each
// one. A hard cut is two edges the eyes cannot fuse; a ramp gives them nothing sharp to disagree
// about. Zero leaves a hard edge, with the rounded corners still clean.
void SetXrEdgeSoftness(float softness);

// How much of the eye separation each eye gives up at its own side edge, from none to all of it.
// At a side edge one eye sees a sliver of the world the other cannot, which is what a window does
// and no mask can remove. The crop puts crossed parallax on the edge, so the frame floats towards
// the viewer and the sliver reads as ordinary occlusion behind a near edge instead of as one eye
// covered. It costs that much width off one side of each eye. Nothing happens without stereo.
void SetXrEdgeFloat(float fraction);

// Which eye's pass is rendering now: 0 for the first or only view, 1 for the second. The whole
// display list runs once per eye, so a capture that must differ per eye keys off this.
int GetXrViewIndex();

// Holds the next draws on the glass. A HUD element is placed in front of the camera by the game
// and is not part of the world behind the window, so it must not take the off-axis frustum: with
// this set the game's own projection stands, both eyes draw the same picture, and the element
// lands on the window plane with no parallax.
void SetXrFlatProjection(bool flat);

// Radians the window covers, for a caller that scales a menu drawn on it. Zero without a session.
float GetXrWindowAngularWidth();

// What fraction of the window's own width the game has to draw for one game pixel to be about one
// eye pixel. The frame is drawn once per eye and then sampled onto a window that covers part of the
// view, so every pixel beyond that is thrown away. It matters most where the headset hands the app
// its whole binocular panel: the window then covers about a quarter of the width the game is given,
// which is sixteen times the pixels. 1 without a session, and never above 1.
float GetXrRenderScale();

// The headset's own controllers. Horizon OS routes a Touch controller through OpenXR alone, so
// Android reports no input device for one and SDL never sees it. The state is read from the action
// set each frame and left in the runtime's own units: sticks and triggers from 0 to 1, buttons as
// XrPadButton bits. What each control does in the game is decided by the caller.
enum XrPadButton {
    XR_PAD_A = 1 << 0,
    XR_PAD_B = 1 << 1,
    XR_PAD_X = 1 << 2,
    XR_PAD_Y = 1 << 3,
    XR_PAD_MENU = 1 << 4,
    XR_PAD_LEFT_STICK = 1 << 5,
    XR_PAD_RIGHT_STICK = 1 << 6,
};

struct XrPadState {
    float stick[2][2]; // left then right hand, x right and y up
    float trigger[2];  // index trigger, left then right
    float squeeze[2];  // grip, left then right
    uint32_t buttons;  // XrPadButton bits
};

bool GetXrPad(XrPadState* pad);

} // namespace Fast
