#pragma once

#include <cstdint>
#include <string>

namespace Fast {

struct XrViewGeometry {
    float eyeOffset[3];
    float windowDistance;
};

bool IsXrPresenting();

bool IsXrInputFocused();

bool GetXrViewGeometry(XrViewGeometry* geometry);

bool GetXrViewGeometryOf(int view, XrViewGeometry* geometry);

void SetXrSceneNear(float units);

void SetXrViewTangents(float tanHalfWidth, float tanHalfHeight);

void SetXrWindowDistance(float meters);
float GetXrWindowDistance();

void SetXrDioramaDepth(float meters);

void SetXrDepthLimit(float degrees);

void SetXrSteadyDepth(bool enabled);

void SetXrSubjectDistance(float units);

void SetXrWindowScale(float scale);
float GetXrWindowScale();

void RecenterXrWindow();

void SetXrStereo(bool enabled);

void SetXrEdgeSoftness(float softness);

void SetXrEdgeFloat(float fraction);

int GetXrViewIndex();

void SetXrFlatProjection(bool flat);

float GetXrWindowAngularWidth();

float GetXrRenderScale();

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
    float stick[2][2];
    float trigger[2];
    float squeeze[2];
    uint32_t buttons;
    bool thumbsticks;
};

bool GetXrPad(XrPadState* pad);

#ifdef ENABLE_OPENXR
struct XrKeyboardInput {
    std::string text;
    int backspaces;
    bool enter;
    bool closed;
};

bool IsXrVirtualKeyboardAvailable();
void ShowXrVirtualKeyboard(bool shown, const char* textContext);
bool TakeXrVirtualKeyboardInput(XrKeyboardInput* input);

void SetXrMenuButtonShown(bool shown);
#endif

} // namespace Fast
