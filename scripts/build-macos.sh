#!/bin/bash
# Native macOS build script
# Uses Homebrew for dependencies and system Python

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$PROJECT_DIR/.build-config.json"

# Platform identifier
export PLATFORM="macos"

# Check we're on macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "Error: This script is for macOS only" >&2
    exit 1
fi

echo "=== Slopsmith Desktop macOS Build ==="
echo ""

# Disable electron-builder's keychain-identity auto-discovery on unsigned
# builds. Without this, electron-builder picks the first codesigning
# identity it finds (often an "Apple Development" cert from Xcode) and
# tries to sign with it — which both produces unusable artifacts AND
# fails when Slopsmith.app contains paths the Apple Development cert
# can't sign. Signed CI builds set APPLE_SIGNING_IDENTITY / CSC_NAME, so
# this guard only triggers for local unsigned dev builds.
if [[ -z "${APPLE_SIGNING_IDENTITY:-}" && -z "${CSC_NAME:-}" && -z "${CSC_LINK:-}" ]]; then
    export CSC_IDENTITY_AUTO_DISCOVERY=false
fi

# Derive CSC_NAME (electron-builder's identity name) from
# APPLE_SIGNING_IDENTITY. codesign accepts the full identity string with
# "Developer ID Application:" prefix; electron-builder rejects that
# prefix and wants the bare team-name + team-id form. Strip the prefix
# once here so the rest of the build (sign-macos-binaries.sh and
# electron-builder) can each consume the form they expect.
if [[ -z "${CSC_NAME:-}" && -n "${APPLE_SIGNING_IDENTITY:-}" ]]; then
    export CSC_NAME="${APPLE_SIGNING_IDENTITY#Developer ID Application: }"
fi

# Color setup
export RED='\033[0;31m'
export GREEN='\033[0;32m'
export YELLOW='\033[1;33m'
export BLUE='\033[0;34m'
export NC='\033[0m'

# Source common build logic
source "$SCRIPT_DIR/build-common.sh"

# Platform-specific: Install system dependencies
install_system_deps() {
    if command -v brew &>/dev/null; then
        PACKAGES=$(grep -v '^[[:space:]]*#' "$PROJECT_DIR/.packages/brew.txt" | grep -v '^[[:space:]]*$' | tr '\n' ' ')
        if [[ -n "$PACKAGES" ]]; then
            brew install $PACKAGES
        fi
    else
        echo "Error: Homebrew not found. Install from https://brew.sh" >&2
        exit 1
    fi
}

# Platform-specific: Bundle Python runtime
bundle_python_impl() {
    local config_py
    config_py=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$PROJECT_DIR/.build-config.json" .versions.python)

    # For a true Universal build, we fetch and bundle the x64 standalone Python runtime
    # as the baseline to ensure full compatibility across both Intel and Apple Silicon (via Rosetta 2).
    local target_arch="x86_64"
    local config_key="python_standalone_macos_x64"

    local pbs_url pbs_sha
    pbs_url=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$PROJECT_DIR/.build-config.json" ".external.${config_key}.url")
    pbs_sha=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$PROJECT_DIR/.build-config.json" ".external.${config_key}.sha256")

    local runtime="$PROJECT_DIR/resources/python/runtime"
    local tarball="/tmp/cpython-${config_py}-macos-${target_arch}.tar.gz"

    mkdir -p "$PROJECT_DIR/resources/python"
    rm -rf "$runtime"

    if [[ ! -f "$tarball" ]] || ! shasum -a 256 "$tarball" | awk '{print $1}' | grep -qx "$pbs_sha"; then
        echo "  Downloading python-build-standalone ${config_py} (${target_arch})"
        curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$pbs_url" -o "$tarball"
    fi
    local actual_sha
    actual_sha=$(shasum -a 256 "$tarball" | awk '{print $1}')
    if [[ "$actual_sha" != "$pbs_sha" ]]; then
        echo "Error: python-build-standalone tarball SHA256 mismatch" >&2
        exit 1
    fi

    local extract_dir="/tmp/pbs-extract-$$"
    rm -rf "$extract_dir"
    mkdir -p "$extract_dir"
    tar -xzf "$tarball" -C "$extract_dir"
    mv "$extract_dir/python" "$runtime"
    rm -rf "$extract_dir"

    if [[ -z "${SLOPSMITH_DIR:-}" ]]; then
        if [[ -d "$PROJECT_DIR/../slopsmith" ]]; then
            SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
        elif [[ -d "$HOME/Repositories/slopsmith" ]]; then
            SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
        fi
    fi
    if [[ -z "${SLOPSMITH_DIR:-}" ]] || [[ ! -f "$SLOPSMITH_DIR/requirements.txt" ]]; then
        echo "ERROR: slopsmith requirements.txt not found (SLOPSMITH_DIR=${SLOPSMITH_DIR:-<unset>})." >&2
        exit 1
    fi
    "$runtime/bin/python3" -m pip install --quiet --no-cache-dir \
        -r "$SLOPSMITH_DIR/requirements.txt" 2>&1 | tail -5
    "$runtime/bin/python3" -m pip install --quiet --no-cache-dir \
        -r "$PROJECT_DIR/.packages/python.txt" 2>&1 | tail -5
}

# Platform-specific: Return expected artifact patterns
get_expected_artifacts() {
    printf "%s\n" "$PROJECT_DIR/release/mac*/Slopsmith.app"
}

# Platform-specific: Bundle system binaries
bundle_binaries_impl() {
    local bin_dir="$PROJECT_DIR/resources/bin"
    mkdir -p "$bin_dir"

    # Helper helper function to download a specific architecture file cleanly
    download_tool_slice() {
        local tool="$1" key="$2" arch="$3"
        local url sha tarball
        url=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" ".external.${key}.url")
        sha=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" ".external.${key}.sha256")
        tarball="/tmp/${tool}-macos-${arch}.zip"

        if [[ ! -f "$tarball" ]] || ! shasum -a 256 "$tarball" | awk '{print $1}' | grep -qx "$sha"; then
            echo "  Downloading $tool ($arch) from $url"
            curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$url" -o "$tarball"
        fi

        local extract_dir="/tmp/${tool}-${arch}-extract-$$"
        rm -rf "$extract_dir" && mkdir -p "$extract_dir"
        unzip -q -o "$tarball" -d "$extract_dir"
        local found
        found=$(find "$extract_dir" -type f -name "$tool" -not -path '*/__MACOSX/*' | head -1)
        if [[ -z "$found" ]]; then
            echo "Error: '$tool' binary not found after unzipping $tarball" >&2
            exit 1
        fi
        mkdir -p "/tmp/slices"
        cp "$found" "/tmp/slices/${tool}.${arch}"
        rm -rf "$extract_dir"
    }

    echo "--> Downloading Intel (x64) and Apple Silicon (arm64) binary slices..."
    download_tool_slice ffmpeg "ffmpeg_macos_x64" "x64"
    download_tool_slice ffmpeg "ffmpeg_macos_arm64" "arm64"
    download_tool_slice ffprobe "ffprobe_macos_x64" "x64"
    download_tool_slice ffprobe "ffprobe_macos_arm64" "arm64"

    echo "--> Stitching binary layers together using lipo..."
    lipo -create /tmp/slices/ffmpeg.x64 /tmp/slices/ffmpeg.arm64 -output "$bin_dir/ffmpeg"
    lipo -create /tmp/slices/ffprobe.x64 /tmp/slices/ffprobe.arm64 -output "$bin_dir/ffprobe"
    chmod +x "$bin_dir/ffmpeg" "$bin_dir/ffprobe"
    xattr -d com.apple.quarantine "$bin_dir/ffmpeg" "$bin_dir/ffprobe" 2>/dev/null || true
    rm -rf /tmp/slices

    if ! "$bin_dir/ffmpeg" -hide_banner -encoders 2>/dev/null | grep -wq libvorbis; then
        echo "Error: bundled universal ffmpeg lacks libvorbis encoder." >&2
        exit 1
    fi

    # Bundle the alternative rubberband processing fallback binary
    local rb_url rb_sha rb_zip rb_extract rb_found rb_actual
    rb_url=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" ".external.ffmpeg_macos_rubberband.url")
    rb_sha=$(python3 "$SCRIPT_DIR/parse-build-config.py" "$CONFIG" ".external.ffmpeg_macos_rubberband.sha256")
    rb_zip="/tmp/ffmpeg-rubberband-macos.zip"
    if [[ ! -f "$rb_zip" ]] || ! shasum -a 256 "$rb_zip" | awk '{print $1}' | grep -qx "$rb_sha"; then
        echo "  Downloading ffmpeg-rubberband (Intel) from $rb_url"
        curl -sL --fail --retry 5 --retry-delay 5 --retry-all-errors "$rb_url" -o "$rb_zip"
    fi
    rb_extract="/tmp/ffmpeg-rubberband-extract-$$"
    rm -rf "$rb_extract" && mkdir -p "$rb_extract"
    unzip -q -o "$rb_zip" -d "$rb_extract"
    rb_found=$(find "$rb_extract" -type f -name ffmpeg -not -path '*/__MACOSX/*' | head -1)
    cp "$rb_found" "$bin_dir/ffmpeg-rubberband"
    chmod +x "$bin_dir/ffmpeg-rubberband"
    xattr -d com.apple.quarantine "$bin_dir/ffmpeg-rubberband" 2>/dev/null || true
    rm -rf "$rb_extract"

    local fluidsynth_bin
    fluidsynth_bin="$(command -v fluidsynth || true)"
    if [[ -z "$fluidsynth_bin" ]]; then
        echo "Error: fluidsynth not found on PATH." >&2
        exit 1
    fi
    cp "$fluidsynth_bin" "$PROJECT_DIR/resources/bin/"

    echo -e "${BLUE}=== Using Homebrew vgmstream-cli ===${NC}"
    VGM_BIN="$(command -v vgmstream-cli || true)"
    if [[ -z "$VGM_BIN" ]]; then
        echo -e "${RED}ERROR: vgmstream-cli not found. Install it with: brew install vgmstream${NC}" >&2
        exit 1
    fi
    cp "$VGM_BIN" "$PROJECT_DIR/resources/bin/vgmstream-cli"
    chmod +x "$PROJECT_DIR/resources/bin/vgmstream-cli"
    xattr -d com.apple.quarantine "$PROJECT_DIR/resources/bin/vgmstream-cli" 2>/dev/null || true

    if command -v dylibbundler &>/dev/null; then
        for bin in fluidsynth ffmpeg ffprobe vgmstream-cli; do
            local target="$PROJECT_DIR/resources/bin/$bin"
            [[ -f "$target" ]] || continue
            echo -e "${BLUE}Bundling ${bin} dependencies...${NC}"
            dylibbundler -cd -b -of -x "$target" \
                -d "$PROJECT_DIR/resources/bin" \
                -p '@executable_path/'
        done
    fi

    "$SCRIPT_DIR/sign-macos-binaries.sh"
}

# Before running main, hijack native extension builds to build unified x64 and arm64 libraries
echo "============================================="
echo "Compiling native addon binary fragments for universal stitching..."
rm -rf "$PROJECT_DIR/build/Release"
FORCE_ARCH="x64" bash "$SCRIPT_DIR/build-audio.sh" Release
mkdir -p "$PROJECT_DIR/build/Release-x64"
cp "$PROJECT_DIR/build/Release/slopsmith_audio.node" "$PROJECT_DIR/build/Release-x64/"

rm -rf "$PROJECT_DIR/build/Release"
FORCE_ARCH="arm64" bash "$SCRIPT_DIR/build-audio.sh" Release
mkdir -p "$PROJECT_DIR/build/Release-arm64"
cp "$PROJECT_DIR/build/Release/slopsmith_audio.node" "$PROJECT_DIR/build/Release-arm64/"

rm -rf "$PROJECT_DIR/build/Release" && mkdir -p "$PROJECT_DIR/build/Release"
lipo -create \
    "$PROJECT_DIR/build/Release-x64/slopsmith_audio.node" \
    "$PROJECT_DIR/build/Release-arm64/slopsmith_audio.node" \
    -output "$PROJECT_DIR/build/Release/slopsmith_audio.node"
echo "============================================="

# Overwrite electron-builder configuration target parameters dynamically before execution
export ELECTRON_BUILDER_EXTRA_ARGS="--mac --universal"

# Run the build orchestra
main "$@"

# Post-build: notarize and staple the DMG.
if [[ -n "${APPLE_SIGNING_IDENTITY:-}" && -n "${APPLE_ID:-}" \
        && -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" \
        && -n "${APPLE_TEAM_ID:-}" ]]; then
    shopt -s nullglob
    for dmg in "$PROJECT_DIR"/release/*.dmg; do
        echo -e "${BLUE}Notarizing $(basename "$dmg") (wait for Apple)...${NC}"
        xcrun notarytool submit "$dmg" \
            --apple-id "$APPLE_ID" \
            --password "$APPLE_APP_SPECIFIC_PASSWORD" \
            --team-id "$APPLE_TEAM_ID" \
            --wait
        echo -e "${BLUE}Stapling notarization ticket to $(basename "$dmg")...${NC}"
        staple_ok=0
        for attempt in 1 2 3 4 5; do
            if xcrun stapler staple "$dmg"; then
                staple_ok=1
                break
            fi
            echo "  staple attempt $attempt failed; waiting before retry..."
            sleep $((attempt * 15))
        done
        if [[ "$staple_ok" -ne 1 ]]; then
            echo -e "${RED}Failed to staple $(basename "$dmg") after 5 attempts${NC}" >&2
            exit 1
        fi
        xcrun stapler validate "$dmg"
    done
    shopt -u nullglob
fi
