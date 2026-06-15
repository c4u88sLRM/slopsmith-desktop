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
if [[ -z "${APPLE_SIGNING_IDENTITY:-}" && -z "${CSC_NAME:-}\" && -z \"${CSC_LINK:-}" ]]; then
    export CSC_IDENTITY_AUTO_DISCOVERY=false
fi

# Derive CSC_NAME (electron-builder's identity name) from
# APPLE_SIGNING_IDENTITY. codesign accepts the full identity string with
# "Developer ID Application:" prefix; electron-builder rejects that
# prefix and wants the bare team-name + team-id form. Strip the prefix
# once here so the rest of the build (sign-macos-binaries.sh and the
# electron-builder call itself) doesn't have to keep switching modes.
if [[ -n "${APPLE_SIGNING_IDENTITY:-}" && -z "${CSC_NAME:-}" ]]; then
    if [[ "$APPLE_SIGNING_IDENTITY" =~ ^"Developer ID Application:"[[:space:]]*(.*) ]]; then
        export CSC_NAME="${BASH_REMATCH[1]}"
        echo "Derived electron-builder CSC_NAME: $CSC_NAME"
    else
        export CSC_NAME="$APPLE_SIGNING_IDENTITY"
    fi
fi

# Define implementation functions required by build-common.sh
install_system_deps() {
    echo "Checking system dependencies via Homebrew..."
    if ! command -v brew &>/dev/null; then
        echo "Error: Homebrew is required for dependency installation" >&2
        exit 1
    fi
    
    # Read packages from brew.txt (ignoring comments/empty lines)
    local packages=()
    while IFS= read -r line || [[ -n "$line" ]]; do
        # Strip comments and whitespace
        line=$(echo "$line" | sed 's/#.*//' | xargs)
        [[ -n "$line" ]] && packages+=("$line")
    done < "$PROJECT_DIR/.packages/brew.txt"
    
    if [[ ${#packages[@]} -gt 0 ]]; then
        echo "Installing Homebrew packages: ${packages[*]}"
        brew install "${packages[@]}"
    fi
}

bundle_python_impl() {
    local target_dir="$1"
    echo "Bundling macOS system Python runtime..."
    
    # Create target directory map layout structures
    mkdir -p "$target_dir"
    
    # Find active python3 installation path
    local python_exe=$(command -v python3)
    if [[ -z "$python_exe" ]]; then
        echo "Error: python3 executable not found" >&2
        exit 1
    fi
    
    # Resolve physical path (handling homebrew symlinks correctly)
    local real_python=$(python3 -c "import sys; import os; print(os.path.realpath(sys.executable))")
    local python_base=$(dirname "$(dirname "$real_python")")
    
    echo "Copying Python home framework from $python_base..."
    cp -R "$python_base/" "$target_dir/"
    
    # Ensure correct internal executable maps resolve cleanly
    chmod +w "$target_dir/bin/python3" || true
}

bundle_binaries_impl() {
    local target_dir="$1"
    echo "Bundling system binaries for macOS package..."
    
    # Copy ffmpeg binary from Homebrew location path
    local ffmpeg_path=$(command -v ffmpeg)
    if [[ -n "$ffmpeg_path" ]]; then
        # Resolve real file paths
        local real_ffmpeg=$(python3 -c "import os; print(os.path.realpath('$ffmpeg_path'))")
        cp "$real_ffmpeg" "$target_dir/"
        echo "  Bundled ffmpeg -> $target_dir/"
    else
        echo "Warning: ffmpeg not found in PATH"
    fi
}

get_expected_artifacts() {
    printf "%s\n" "$PROJECT_DIR/release/*.dmg" "$PROJECT_DIR/release/*.zip"
}

# Override package_application phase to implement true Universal cross-compilation loops
package_application() {
    echo "============================================="
    echo "Compiling native addons for multiple targets..."
    
    echo "-> Compiling x64 (Intel) native audio slice..."
    FORCE_ARCH="x64" bash "$SCRIPT_DIR/build-audio.sh" Release
    
    echo "-> Compiling arm64 (Apple Silicon) native audio slice..."
    FORCE_ARCH="arm64" bash "$SCRIPT_DIR/build-audio.sh" Release
    echo "============================================="
    
    echo "Packaging app bundle via electron-builder for universal targets..."
    npx electron-builder --mac --universal
}

# Source common build logic orchestrator to parse the implementation stack
source "$SCRIPT_DIR/build-common.sh"

# Run common build setup pipeline sequence explicitly
main

# Post-processing notarization loop checks if certificates are available
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
        # `notarytool submit --wait` returns when Apple's notary service
        # accepts the submission, but the ticket can take an extra
        # 30-60 s to propagate to CloudKit (where `stapler` reads from).
        # Stapling immediately fails with `Error 65: Record not found`
        # on CI roughly half the time. Retry with backoff.
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
            echo -e "${RED}Error: Stapling failed for $dmg${NC}" >&2
            exit 1
        fi
    done
    shopt -u nullglob
fi
