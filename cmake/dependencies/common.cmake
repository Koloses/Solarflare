# load common dependencies
# this file will also load platform specific dependencies

# Resolve OpenSSL before subprojects run their own find_package(OpenSSL) calls.
# This ensures a user-provided OPENSSL_ROOT_DIR is honored consistently.
find_package(OpenSSL REQUIRED)

# boost, this should be before Simple-Web-Server as it also depends on boost
include(dependencies/Boost_Sunshine)

# submodules
# moonlight common library
set(ENET_NO_INSTALL ON CACHE BOOL "Don't install any libraries built for enet")
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/moonlight-common-c/enet")

# web server
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/Simple-Web-Server")

# libdisplaydevice
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libdisplaydevice")

if(SUNSHINE_ENABLE_TRAY)
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/tray")
endif()

# PyroWave GPU wavelet codec (vendored). Builds a static `pyrowave` target.
if(SUNSHINE_ENABLE_PYROWAVE)
    # CRITICAL: the codec and the host MUST compile against the SAME Vulkan headers.
    # If they differ (different VK_HEADER_VERSION) the vulkan_raii DeviceDispatcher
    # has a different layout per translation unit, so a vk::raii handle created in
    # one TU resolves its function pointers at the wrong offsets in the other ->
    # SIGSEGV in the ICD (release) or "getVkHeaderVersion() == VK_HEADER_VERSION"
    # assertion (debug). The host uses the bundled Vulkan-Headers submodule (unless
    # SUNSHINE_SYSTEM_VULKAN_HEADERS), so hand that exact dir to the codec BEFORE it
    # is configured, and have it use ONLY that (no find_package -> no system headers
    # leaking through Vulkan::Vulkan into our targets).
    if(NOT SUNSHINE_SYSTEM_VULKAN_HEADERS)
        set(PYROWAVE_VULKAN_HEADERS_DIR
            "${CMAKE_SOURCE_DIR}/third-party/build-deps/third-party/FFmpeg/Vulkan-Headers/include")
        # Force the bundled Vulkan headers AHEAD OF any system headers for EVERY
        # target created after this point (Sunshine's main target with the
        # src/pyrowave/*.cpp integration sources AND the pyrowave subdir), so all
        # translation units see the SAME VK_HEADER_VERSION. Without this, the main
        # target picked up system Vulkan headers (different version) while the codec
        # used the bundled ones -> incompatible vulkan_raii DeviceDispatcher layout
        # -> SIGSEGV at vkCreateDescriptorPool (or getVkHeaderVersion assert).
        # BEFORE (and NOT system: plain -I beats -isystem and the default /usr/include
        # in GCC's search order) so the bundled headers are the very first -I and
        # cannot be overridden by a stray system Vulkan include.
        include_directories(BEFORE "${PYROWAVE_VULKAN_HEADERS_DIR}")
    endif()
    add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/pyrowave")

    # CRITICAL (NDEBUG consistency): Sunshine defines NDEBUG for non-Debug builds via
    # add_definitions(-DNDEBUG) in cmake/targets/common.cmake, which is directory-scoped
    # and runs AFTER this point, so it reaches the main target but NOT this already-added
    # subdir. With makepkg's CMAKE_BUILD_TYPE=None, NDEBUG also doesn't come from config
    # flags, so the codec lib would compile WITHOUT NDEBUG while the host compiles WITH it.
    # That changes vulkan_hpp's DispatchLoaderBase layout (the vkHeaderVersion/m_valid
    # members exist only under !NDEBUG), so a vk::raii::Device dispatcher built in the host
    # is misread by the codec -> garbage function-pointer offsets -> SIGSEGV at
    # vkCreateDescriptorPool (or the getVkHeaderVersion assert in debug). Match the host's
    # NDEBUG state on the codec target.
    string(TOUPPER "x${CMAKE_BUILD_TYPE}" PYROWAVE_BUILD_TYPE)
    if(NOT "${PYROWAVE_BUILD_TYPE}" STREQUAL "XDEBUG")
        target_compile_definitions(pyrowave PRIVATE NDEBUG)
    endif()
endif()

# common dependencies
include("${CMAKE_MODULE_PATH}/dependencies/nlohmann_json.cmake")
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED libcurl)

# miniupnp
pkg_check_modules(MINIUPNP miniupnpc REQUIRED)
include_directories(SYSTEM ${MINIUPNP_INCLUDE_DIRS})

# ffmpeg pre-compiled binaries
include("${CMAKE_MODULE_PATH}/dependencies/ffmpeg.cmake")

# Opus
# Homebrew provides opus as a dynamic library only, so disable static linking for Homebrew builds
if(SUNSHINE_BUILD_HOMEBREW)
    set(OPUS_USE_STATIC OFF CACHE BOOL "Static linking for libopus")
else()
    set(OPUS_USE_STATIC ON CACHE BOOL "Static linking for libopus")
endif()
include("${CMAKE_MODULE_PATH}/dependencies/FindOpus.cmake")

# platform specific dependencies
if(WIN32)
    include("${CMAKE_MODULE_PATH}/dependencies/windows.cmake")
elseif(UNIX)
    include("${CMAKE_MODULE_PATH}/dependencies/unix.cmake")

    if(APPLE)
        include("${CMAKE_MODULE_PATH}/dependencies/macos.cmake")
    else()
        include("${CMAKE_MODULE_PATH}/dependencies/linux.cmake")
    endif()
endif()
