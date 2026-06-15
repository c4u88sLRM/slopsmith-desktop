# ONNX Runtime acquisition for the ML note detector (Basic Pitch).
#
# Mirrors the NAM_AVAILABLE / RTNEURAL_AVAILABLE conditional pattern: if ONNX
# Runtime can be obtained, ONNXRUNTIME_AVAILABLE is set ON and the audio addon
# compiles with SLOPSMITH_ONNX_SUPPORT=1; otherwise the build still succeeds and
# the engine falls back to the YIN PitchDetector / ChordScorer (Constitution VII).
#
# Delivery: a pinned, prebuilt CPU release fetched at configure time with a
# pinned URL + SHA-256 (Constitution V — reproducible builds). For fully offline
# / pre-seeded builds, set -DSLOPSMITH_ONNXRUNTIME_ROOT=/path/to/extracted/onnxruntime
# (the directory containing include/ and lib/) and the fetch is skipped.
#
# Exports (cache/parent scope):
#   ONNXRUNTIME_AVAILABLE     ON/OFF
#   ONNXRUNTIME_INCLUDE_DIR   header directory
#   ONNXRUNTIME_IMPORT_LIB     library to link against
#   ONNXRUNTIME_RUNTIME_LIB   shared lib that must sit next to slopsmith_audio.node

# Plain variable, not a cache entry: the version is pinned in lock-step with
# the per-asset SHA-256 table below, so it must always be the value in this
# file. A CACHE STRING would keep a stale version from a reused build dir
# (a branch on a different version), making the hash check fail and silently
# disable ONNX.
set(ONNXRUNTIME_VERSION "1.20.1")

# --- Resolve the prebuilt asset for this OS/arch --------------------------
set(_ort_base "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}")
set(_ort_ok ON)

# Only x64 / arm64 prebuilt assets exist. An unrecognised processor must
# disable ONNX (clean YIN fallback) — never default to an x64 asset, which
# would only fail later at link/load on e.g. Windows ARM64 or 32-bit ARM.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x64")
        set(_ort_asset "onnxruntime-win-x64-${ONNXRUNTIME_VERSION}")
        set(_ort_ext "zip")
        set(_ort_sha "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581")
        set(_ort_import "onnxruntime.lib")
        set(_ort_runtime "onnxruntime.dll")
        set(_ort_providers "onnxruntime_providers_shared.dll")
    else()
        set(_ort_ok OFF)
        message(STATUS "ONNX Runtime: unsupported Windows arch '${CMAKE_SYSTEM_PROCESSOR}' — ML note detection disabled")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_ort_ext "tgz")
    
    # Resolve cross-compilation target on Apple Silicon/Intel environments.
    # CMAKE_SYSTEM_PROCESSOR reflects the host runner hardware, whereas
    # CMAKE_OSX_ARCHITECTURES tells us what target binary slice we are building.
    if(DEFINED CMAKE_OSX_ARCHITECTURES AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
        set(_mac_target_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(_mac_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    if(_mac_target_arch MATCHES "arm64|aarch64")
        set(_ort_asset "onnxruntime-osx-arm64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "b678fc3c2354c771fea4fba420edeccfba205140088334df801e7fc40e83a57a")
    elseif(_mac_target_arch MATCHES "x86_64|AMD64|x64")
        set(_ort_asset "onnxruntime-osx-x86_64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "0f73006813af2a1a5d1723ed7dfb694fc629d15037124081bb61b7bf7d99fc78")
    else()
        set(_ort_ok OFF)
        message(STATUS "ONNX Runtime: unsupported macOS arch '${_mac_target_arch}' — ML note detection disabled")
    endif()
    
    set(_ort_import "libonnxruntime.dylib")
    set(_ort_runtime "libonnxruntime.${ONNXRUNTIME_VERSION}.dylib")
    set(_ort_providers "libonnxruntime_providers_shared.dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ort_ext "tgz")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_ort_asset "onnxruntime-linux-aarch64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "ae4fedbdc8c18d688c01306b4b50c63de3445cdf2dbd720e01a2fa3810b8106a")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x64")
        set(_ort_asset "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}")
        set(_ort_sha "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105")
    else()
        set(_ort_ok OFF)
        message(STATUS "ONNX Runtime: unsupported Linux arch '${CMAKE_SYSTEM_PROCESSOR}' — ML note detection disabled")
    endif()
    set(_ort_import "libonnxruntime.so")
    # SONAME is libonnxruntime.so.1 — that exact name must sit next to the addon.
    set(_ort_runtime "libonnxruntime.so.1")
    set(_ort_providers "libonnxruntime_providers_shared.so")
else()
    set(_ort_ok OFF)
    message(STATUS "ONNX Runtime: unsupported platform '${CMAKE_SYSTEM_NAME}' — ML note detection disabled")
endif()

# --- Obtain the runtime: explicit root override, or pinned fetch ----------
set(_ort_root "")

if(_ort_ok AND DEFINED SLOPSMITH_ONNXRUNTIME_ROOT)
    if(EXISTS "${SLOPSMITH_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
        set(_ort_root "${SLOPSMITH_ONNXRUNTIME_ROOT}")
        message(STATUS "ONNX Runtime: using prepopulated root ${_ort_root}")
    else()
        message(WARNING "SLOPSMITH_ONNXRUNTIME_ROOT='${SLOPSMITH_ONNXRUNTIME_ROOT}' "
                        "has no include/onnxruntime_cxx_api.h — ignoring")
    endif()
endif()

# Download + extract the prebuilt archive. Every failure mode here is SOFT —
# offline machine, download error, hash mismatch, bad layout all just disable
# ML detection (YIN fallback). Configure must never abort on this, so we use
# file(DOWNLOAD) with a status check rather than FetchContent, which raises a
# FATAL_ERROR when the archive can't be fetched.
if(_ort_ok AND _ort_root STREQUAL "")
    set(_ort_url     "${_ort_base}/${_ort_asset}.${_ort_ext}")
    set(_ort_archive "${CMAKE_BINARY_DIR}/_deps/${_ort_asset}.${_ort_ext}")
    set(_ort_extract "${CMAKE_BINARY_DIR}/_deps/onnxruntime")
    set(_ort_header  "${_ort_extract}/${_ort_asset}/include/onnxruntime_cxx_api.h")

    # On a clean build tree _deps does not exist yet — create it so the
    # file(DOWNLOAD) below can open its destination file instead of failing
    # and silently disabling ONNX support on every fresh configure.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")

    # Download — cached: a prior configure's archive with a matching
