include(FetchContent)

#=================== SDL2 ===================
find_package(SDL2 QUIET)
if (NOT ${SDL2_FOUND})
    # SDL's iOS HIDAPI backend links CoreBluetooth, which makes App Store Connect demand an
    # NSBluetoothAlwaysUsageDescription. It only drives the BLE Steam Controller; every other
    # pad comes in through MFi/GameController, so drop the backend and the permission with it.
    set(SDL_HIDAPI OFF)
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.32.10
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(SDL2)
endif()

#=================== nlohmann-json ===================
find_package(nlohmann_json QUIET)
if (NOT ${nlohmann_json_FOUND})
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

#=================== tinyxml2 ===================
find_package(tinyxml2 QUIET)
if (NOT ${tinyxml2_FOUND})
    set(tinyxml2_BUILD_TESTING OFF)
    FetchContent_Declare(
        tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG 11.0.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(tinyxml2)
endif()

#=================== spdlog ===================
find_package(spdlog QUIET)
if (NOT ${spdlog_FOUND})
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.16.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

#=================== libzip ===================
find_package(libzip QUIET)
if (NOT ${libzip_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TOOLS OFF)
    set(BUILD_REGRESS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOC OFF)
    set(BUILD_OSSFUZZ OFF)
    set(BUILD_SHARED_LIBS OFF)
    # libzip enables these codecs by default and goes looking for them on the host. There is
    # no iOS system zstd, so it finds the Homebrew x86_64 build under /usr/local and the app
    # fails to link with undefined _ZSTD_* for arm64. LZMA is off for the same reason (it
    # just happens not to be installed here). bzip2 is left alone -- it resolves to
    # libbz2.tbd in the iOS SDK, which is the right architecture.
    set(ENABLE_ZSTD OFF)
    set(ENABLE_LZMA OFF)
    FetchContent_Declare(
        libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG v1.11.4
        OVERRIDE_FIND_PACKAGE
    )
    # ios.toolchain.cmake defaults ENABLE_STRICT_TRY_COMPILE to OFF, which sets
    # CMAKE_TRY_COMPILE_TARGET_TYPE to STATIC_LIBRARY -- try_compile then compiles but never
    # links. libzip probes for its Annex K and Win32 functions with check_function_exists,
    # which can only fail at link time, so every one of them comes back TRUE. HAVE_MEMCPY_S
    # then suppresses the memcpy fallback in libzip's lib/compat.h and the build dies on an
    # undeclared memcpy_s in zip_winzip_aes.c (HAVE_STRERROR_S, HAVE_STRNCPY_S, HAVE__CLOSE
    # and friends are wrong the same way).
    #
    # Simply flipping CMAKE_TRY_COMPILE_TARGET_TYPE around FetchContent_MakeAvailable does
    # not work: libzip's own project() call re-reads CMAKE_TOOLCHAIN_FILE into libzip's
    # scope, and ios.toolchain.cmake has no include guard, so it sets STATIC_LIBRARY right
    # back before the first check runs. Run the link-based probes here instead, where the
    # override holds -- check_function_exists() is a no-op when its result variable is
    # already in the cache, so libzip picks these up rather than recomputing them.
    #
    # Only the check_function_exists() probes need this. libzip's check_symbol_exists()
    # ones test for a declaration in a header and so are already correct when try_compile
    # only compiles. Every function here has a portable #ifndef fallback in libzip's
    # compat.h, so a probe that fails for an unrelated reason is merely conservative.
    #
    # The probes write to private variables and the HAVE_* entries are then overwritten
    # unconditionally. check_function_exists() skips the probe entirely when its result
    # variable is already cached, so probing straight into HAVE_* would leave a build
    # directory that was configured before this fix stuck with its poisoned values.
    include(CheckFunctionExists)
    include(CheckIncludeFiles)
    set(_lus_libzip_probes
        _close _dup _fdopen _fileno _fseeki64 _fstat64 _setmode _stat64 _strdup
        _strtoi64 _strtoui64 _unlink arc4random clonefile explicit_bzero explicit_memset
        fchmod fileno fseeko ftello getprogname GetSecurityInfo memcpy_s random setmode
        strdup strerror_s strerrorlen_s stricmp strncpy_s strtoll strtoull
    )
    set(_lus_try_compile_type ${CMAKE_TRY_COMPILE_TARGET_TYPE})
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    # Matches libzip: it requests the ISO C Annex K functions before probing for them.
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D__STDC_WANT_LIB_EXT1__=1)
    foreach(_lus_fn IN LISTS _lus_libzip_probes)
        string(TOUPPER "${_lus_fn}" _lus_fn_upper)
        check_function_exists(${_lus_fn} LUS_LIBZIP_HAS_${_lus_fn_upper})
        set(HAVE_${_lus_fn_upper} "${LUS_LIBZIP_HAS_${_lus_fn_upper}}" CACHE INTERNAL "")
    endforeach()
    # fts_open is only probed by libzip when fts.h is present; check_include_files is
    # compile-based, so this agrees with libzip's own result either way.
    check_include_files("sys/types.h;sys/stat.h;fts.h" HAVE_FTS_H)
    if (HAVE_FTS_H)
        check_function_exists(fts_open LUS_LIBZIP_HAS_FTS_OPEN)
        set(HAVE_FTS_OPEN "${LUS_LIBZIP_HAS_FTS_OPEN}" CACHE INTERNAL "")
    endif()
    list(REMOVE_ITEM CMAKE_REQUIRED_DEFINITIONS -D__STDC_WANT_LIB_EXT1__=1)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE ${_lus_try_compile_type})
    FetchContent_MakeAvailable(libzip)
    list(APPEND ADDITIONAL_LIB_INCLUDES ${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})
endif()

#=================== Metal-cpp ===================
FetchContent_Declare(
    metalcpp
    GIT_REPOSITORY https://github.com/briaguya-ai/single-header-metal-cpp.git
    GIT_TAG macOS13_iOS16
)
FetchContent_MakeAvailable(metalcpp)
list(APPEND ADDITIONAL_LIB_INCLUDES ${metalcpp_SOURCE_DIR})

#=================== ImGui ===================
target_sources(ImGui
    PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
)

target_include_directories(ImGui PRIVATE ${metalcpp_SOURCE_DIR})
target_compile_definitions(ImGui PUBLIC IMGUI_IMPL_METAL_CPP)

target_link_libraries(ImGui PUBLIC SDL2::SDL2-static SDL2::SDL2main)
