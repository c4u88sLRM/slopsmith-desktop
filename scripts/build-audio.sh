#!/usr/bin/env bash
set -e

echo "=== Slopsmith Desktop macOS Audio Engine Build ==="

ELECTRON_VERSION="35.7.5"
ARCHITECTURES=("x64" "arm64")

# Re-install clean dependencies once globally
echo "Installing npm dependencies..."
npm install

for ARCH in "${ARCHITECTURES[@]}"; do
  echo "----------------------------------------------------"
  echo "Building audio engine for slice: $ARCH"
  echo "----------------------------------------------------"

  # 1. Define distinct, isolated build directories per architecture
  BUILD_DIR="build-${ARCH}"
  
  # 2. Map standard architecture nomenclature to Apple's compiler target names
  if [ "$ARCH" = "x64" ]; then
    OSX_ARCH="x86_64"
  else
    OSX_ARCH="arm64"
  fi

  # 3. Clean any existing stale paths for this slice
  rm -rf "$BUILD_DIR"

  # 4. Run explicit configuration and build passes using isolated dirs
  npx cmake-js compile \
    --runtime=electron \
    --runtime-version="$ELECTRON_VERSION" \
    --arch="$ARCH" \
    --directory="." \
    --out="$BUILD_DIR" \
    --CDCMAKE_OSX_ARCHITECTURES="$OSX_ARCH" \
    --CDNODE_ARCH="$ARCH" \
    --CDCMAKE_BUILD_TYPE=Release
    
  echo "Successfully built $ARCH slice inside $BUILD_DIR/Release/"
done

echo "----------------------------------------------------"
echo "Stitching architectures together into universal binaries..."
echo "----------------------------------------------------"

# Ensure target directory exists
mkdir -p build/Release

# Stitch your node addon
lipo -create \
  build-x64/Release/slopsmith_audio.node \
  build-arm64/Release/slopsmith_audio.node \
  -output build/Release/slopsmith_audio.node

# Stitch the VST host binary
lipo -create \
  build-x64/Release/slopsmith-vst-host \
  build-arm64/Release/slopsmith-vst-host \
  -output build/Release/slopsmith-vst-host

# Stitch the VST scan binary
lipo -create \
  build-x64/Release/slopsmith-vst-scan \
  build-arm64/Release/slopsmith-vst-scan \
  -output build/Release/slopsmith-vst-scan

echo "Universal binary stitching complete!"
