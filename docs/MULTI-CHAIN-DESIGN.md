# Multi-Chain Design

## Motivation

The jam-session plugin assigns a NAM amp profile to each musician (guitar, bass). Currently slopsmith-desktop holds one `SignalChain` per `SourceChain`. All VST stages run in that single chain, so every NAM stage placed after an instrument would process the fully-accumulated mix — drums, keys, and all other instruments included — rather than just that instrument's audio.

Per-instrument amp simulation requires one isolated chain per route key (musician slot).

---

## Current Architecture

```
SourceChain
  └── SignalChain chain          ← one chain, one buffer
        ├── Stage: drums VST
        ├── Stage: bass VST
        ├── Stage: rhythm VST
        └── Stage: lead VST
```

`AudioEngine` sums all `SourceChain` outputs into the final mix. Within a single `SourceChain`, every stage operates on the accumulated buffer from prior stages.

---

## Proposed Architecture

```
SourceChain
  ├── chains["drums"]  → SignalChain  ← isolated buffer per route
  │     └── Stage: drums VST
  ├── chains["bass"]   → SignalChain
  │     ├── Stage: bass VST
  │     └── Stage: NAM (WARWICK GNOME)
  ├── chains["rhythm"] → SignalChain
  │     ├── Stage: rhythm VST
  │     └── Stage: NAM (Blues Junior)
  └── chains["lead"]   → SignalChain
        ├── Stage: lead VST
        └── Stage: NAM (Blues Junior)
```

Each chain processes into its own scratch buffer. `SourceChain::processBlock` sums all scratch buffers at the end, matching what `AudioEngine` already does across `SourceChain` instances.

---

## File-by-File Changes

### `native/SourceChain.h`

```cpp
// Before
SignalChain chain;

// After
std::map<std::string, std::unique_ptr<SignalChain>> chains;
SignalChain& getOrCreateChain(const std::string& routeKey);
```

### `native/SourceChain.cpp`

- `getOrCreateChain(key)` inserts and returns a new `SignalChain` if key is absent.
- `processBlock`: iterate `chains`, call `chain->processBlock(scratchBuffer)` for each, accumulate into output buffer.
- `loadPreset(json)` / `clearChain()`: delegate to `chains["default"]` for backward compat.
- Add `loadPresetForRoute(routeKey, json)` and `clearChainForRoute(routeKey)`.

### `native/AudioEngine.h` / `AudioEngine.cpp`

No structural change needed. `AudioEngine` already sums `SourceChain` outputs; internal routing is opaque to it.

Minor: any direct calls on `sourceChain.chain` must go through the new accessor.

### `native/NodeAddon.cpp`

Add two new N-API methods exposed to JS:

```cpp
// Load a preset into a specific route's chain
Napi::Value LoadPresetForRoute(const Napi::CallbackInfo& info);
// info[0] = sourceId (string), info[1] = routeKey (string), info[2] = presetJson (string)

// Clear a specific route's chain
Napi::Value ClearChainForRoute(const Napi::CallbackInfo& info);
// info[0] = sourceId (string), info[1] = routeKey (string)
```

Register in `Init`:
```cpp
exports.Set("loadPresetForRoute", Napi::Function::New(env, LoadPresetForRoute));
exports.Set("clearChainForRoute", Napi::Function::New(env, ClearChainForRoute));
```

### `src/audio-effects-executor.ts`

Current `applyEffects(sourceId, stages)` calls `native.loadPreset(sourceId, json)`.

Add overload or extend signature:

```ts
applyEffectsForRoute(sourceId: string, routeKey: string, stages: Stage[]): void
// calls native.loadPresetForRoute(sourceId, routeKey, json)
```

The existing `applyEffects` can remain as-is (maps to `routeKey = "default"`), preserving backward compat for any caller that doesn't supply a route.

---

## Backward Compatibility

- Any existing call to `loadPreset` / `clearChain` without a route key operates on `chains["default"]`.
- Hosts with a single-stage chain (no route key concept) continue to work unchanged.
- JS callers that don't use `applyEffectsForRoute` are unaffected.

---

## Integration with jam-session Plugin

The jam-session plugin's `startSession` builds a stage plan per musician. With multi-chain support, it can call `applyEffectsForRoute(sourceId, musician.slot, stages)` where `stages` includes:

1. The instrument VST for that slot
2. The NAM stage (if `musician.nam_rig` is set)

This isolates each musician's signal path and allows NAM amp simulation per instrument without bleed from other slots.

---

## Effort Estimate

| Area | Work |
|---|---|
| `SourceChain` refactor | 1–2 days C++ |
| `NodeAddon` new methods | 0.5 day C++ |
| `audio-effects-executor.ts` | 0.5 day TS |
| jam-session plugin integration | 0.5 day JS |
| Testing / regression | 1 day |
| **Total** | **~3.5–4.5 days** |
