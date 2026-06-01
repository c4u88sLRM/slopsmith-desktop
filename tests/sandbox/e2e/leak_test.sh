#!/usr/bin/env bash
# Orphan-cleanup regression test (issue #265, Linux).
#
# A *crashed* host must not leave the slopsmith-vst-host child running and
# holding the audio device + shm. This drives the e2e driver in
# SLOPSMITH_E2E_LEAK_TEST mode — the driver _Exit()s the instant the child is
# alive, skipping its clean shutdown (no `shutdown` op, no SIGTERM ladder) —
# then asserts the child process is gone. Exercises both cleanup paths together:
# PR_SET_PDEATHSIG (installLinuxParentDeathSignal) and the control-socket
# disconnect teardown.
#
#   leak_test.sh <e2e-driver> <vst-host> <plugin.vst3>
set -euo pipefail

E2E="${1:?usage: leak_test.sh <e2e-driver> <vst-host> <plugin.vst3>}"
HOST="${2:?missing vst-host path}"
PLUG="${3:?missing plugin path}"

# Fresh TMPDIR so we only see this run's child log (the child names its log
# $TMPDIR/slopsmith-vst-host-<pid>.log).
TMPDIR_RUN="$(mktemp -d)"
export TMPDIR="$TMPDIR_RUN"
trap 'rm -rf "$TMPDIR_RUN"' EXIT

SLOPSMITH_E2E_LEAK_TEST=1 "$E2E" "$HOST" "$PLUG" >/dev/null 2>&1 || true

# `|| true`: a no-match makes the ls pipeline non-zero, which would trip set -e —
# the empty-LOG case is handled explicitly just below.
LOG=$(ls -t "$TMPDIR_RUN"/slopsmith-vst-host-*.log 2>/dev/null | head -1 || true)
if [[ -z "${LOG:-}" ]]; then
    echo "leak_test: FAIL — no child log produced (driver never spawned the host)"
    exit 1
fi
# Anchored extract; if the name doesn't match, sed echoes it back unchanged, so
# validate the result is a bare pid — otherwise `kill -0` on garbage would fail
# and the test would PASS for the wrong reason.
CPID=$(basename "$LOG" | sed -E 's/^slopsmith-vst-host-([0-9]+)\.log$/\1/')
if [[ ! "$CPID" =~ ^[0-9]+$ ]]; then
    echo "leak_test: FAIL — could not parse a numeric pid from log name '$LOG'"
    exit 1
fi
echo "leak_test: host child pid=$CPID; driver has exited without clean shutdown"

# PDEATHSIG / disconnect are near-instant; poll up to ~5s for CI-runner slack.
for _ in $(seq 1 50); do
    if ! kill -0 "$CPID" 2>/dev/null; then
        echo "leak_test: PASS — child cleaned up after host crash"
        exit 0
    fi
    sleep 0.1
done

echo "leak_test: FAIL — child $CPID still running 5s after host crash (orphan)"
kill -9 "$CPID" 2>/dev/null || true
exit 1
