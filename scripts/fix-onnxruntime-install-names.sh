#!/usr/bin/env bash
# Normalise ONNX Runtime install names on macOS (slopsmith#818).
#
# The prebuilt onnxruntime macOS dylib bakes the *build machine's absolute
# extraction path* (e.g. /Users/runner/work/.../_deps/onnxruntime/.../lib/
# libonnxruntime.1.20.1.dylib) into its LC_ID_DYLIB. At link time the linker
# copies that absolute path into slopsmith_audio.node's LC_LOAD_DYLIB. The
# .node carries the correct `@loader_path` rpath (set in src/audio/
# CMakeLists.txt) and the runtime is staged right beside it — but dyld never
# consults the rpath, because the load command is an absolute path, not an
# `@rpath/...` one. Result: the addon loads fine *only* on the CI runner;
# every other Mac fails to find onnxruntime, MlNoteDetector can't initialise,
# and the engine silently falls back to YIN ("ML detection: OFF").
#
# Fix: rewrite the install names to be `@rpath`-relative so the addon's
# existing `@loader_path` rpath resolves the co-located runtime everywhere.
# Idempotent — re-running is a no-op once the names are already `@rpath/...`.
#
# Usage: fix-onnxruntime-install-names.sh <slopsmith_audio.node> <runtime-lib-name>
#   $1  absolute path to the built slopsmith_audio.node
#   $2  runtime lib filename, e.g. libonnxruntime.1.20.1.dylib
#
# Invoked as a POST_BUILD step on Apple platforms only. install_name_tool
# invalidates any existing (adhoc) code signature, so we re-adhoc-sign the
# touched binaries afterwards; electron-builder overrides this with the real
# Developer ID signature when it packages + signs the .app.
set -euo pipefail

node="${1:?path to slopsmith_audio.node required}"
libname="${2:?onnxruntime runtime lib name required}"
dir="$(cd "$(dirname "$node")" && pwd)"
runtime="$dir/$libname"
providers="$dir/libonnxruntime_providers_shared.dylib"

# Re-adhoc-sign a binary after rewriting its load commands. `install_name_tool`
# strips the lightweight adhoc signature the linker attaches on Apple Silicon;
# without one, dyld refuses to load the addon on a local (unsigned) dev build.
resign() {
    codesign --remove-signature "$1" 2>/dev/null || true
    codesign --force --sign - "$1" 2>/dev/null || true
}

# Discover the addon's current (absolute) reference to the runtime, if any.
# Skip when it is already `@rpath/...` so re-runs / already-correct builds are
# no-ops. Match the exact runtime filename to avoid touching unrelated entries.
old_ref="$(otool -L "$node" | awk -v L="$libname" '$1 ~ L && $1 !~ /^@rpath\// {print $1; exit}')"
if [[ -n "${old_ref:-}" ]]; then
    install_name_tool -change "$old_ref" "@rpath/$libname" "$node"
    resign "$node"
    echo "fix-onnxruntime-install-names: $node -> @rpath/$libname"
fi

# Normalise the runtime's own id so future links (and any consumer that reads
# LC_ID_DYLIB) get `@rpath/...` instead of an absolute build path.
if [[ -f "$runtime" ]]; then
    cur_id="$(otool -D "$runtime" | sed -n '2p')"
    if [[ "$cur_id" != "@rpath/$libname" ]]; then
        install_name_tool -id "@rpath/$libname" "$runtime"
        resign "$runtime"
        echo "fix-onnxruntime-install-names: id $runtime -> @rpath/$libname"
    fi
fi

# providers_shared is dlopen()'d by the runtime at session-init and links the
# main runtime by the same baked absolute path; rewrite it too so a build that
# ships the providers stub doesn't reintroduce the absolute dependency.
if [[ -f "$providers" ]]; then
    prov_name="$(basename "$providers")"
    cur_pid="$(otool -D "$providers" | sed -n '2p')"
    if [[ "$cur_pid" != "@rpath/$prov_name" ]]; then
        install_name_tool -id "@rpath/$prov_name" "$providers"
    fi
    prov_ref="$(otool -L "$providers" | awk -v L="$libname" '$1 ~ L && $1 !~ /^@rpath\// {print $1; exit}')"
    if [[ -n "${prov_ref:-}" ]]; then
        install_name_tool -change "$prov_ref" "@rpath/$libname" "$providers"
    fi
    resign "$providers"
fi
