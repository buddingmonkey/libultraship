set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
set(SDL_MISC OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
include(${CMAKE_CURRENT_LIST_DIR}/ios.cmake)

# ItemAdd is the only place ImGui reports every item rectangle, and the test engine hook is the
# only supported way to read it. Tracking areas need those rectangles. libultraship supplies the
# hooks; see gfx_visionos.cpp.
target_compile_definitions(ImGui PUBLIC IMGUI_ENABLE_TEST_ENGINE)
