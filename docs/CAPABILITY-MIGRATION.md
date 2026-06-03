# Capability Migration Notes

Slopsmith Desktop is moving first-party renderer integrations away from private
webview globals and legacy `song:*` events toward Slopsmith capability domains.
The goal is to keep desktop behavior aligned with core while avoiding raw local
filenames, device handles, or native transport objects in plugin-visible state.

## Playback Identity For Tone Mappings

Core playback capability v1 emits redaction-safe lifecycle events such as
`playback:loading`, `playback:ready`, `playback:stopped`, and `playback:ended`.
The playback target contains two public identities:

- `targetId`: arrangement-scoped playback target identity.
- `settingsKey`: per-song storage identity shaped like `settings-v1-...`.

Desktop tone switching now uses `target.settingsKey` as the primary song key for
`localStorage.slopsmith-tone-mappings`. Raw filenames from `song:loading` remain
only as a compatibility fallback when the embedded Slopsmith core does not expose
playback capability v1.

### Storage Shape

The store shape does not change:

```json
{
  "global": {},
  "songs": {
    "settings-v1-abc1234": { "Clean": "Clean Preset" }
  },
  "midiPC": {
    "settings-v1-abc1234": { "mode": "midi", "vstSlotId": 0, "mappings": {} }
  }
}
```

Only the per-song bucket key changes. New mappings created on playback-capable
core builds are written under `settingsKey`. Existing filename-keyed buckets are
left in place so older core builds and older desktop releases can still read
them.

### Automatic Migration For Existing Mappings

When playback capability v1 emits `playback:loading`, desktop receives both the
safe `target.settingsKey` and the legacy filename fallback. If
`localStorage.slopsmith-tone-mappings` contains a filename-keyed `songs` or
`midiPC` bucket for the legacy filename and the corresponding `settingsKey`
bucket is missing or empty, desktop copies that bucket to the safe key and leaves
the original bucket untouched.

This migration is intentionally copy-only:

- Existing `settingsKey` buckets win and are not overwritten.
- Filename-keyed buckets remain available to older desktop builds or embedded
  Slopsmith cores without playback capability v1.
- Corrupt or missing mapping stores fall back to the normal empty-store behavior.

For review/debugging, open a song with an existing filename-keyed mapping and
inspect `localStorage.slopsmith-tone-mappings`: the same mapping should appear
under both the old filename key and the new `settings-v1-...` key after the
`playback:loading` event.

### Maintainer Checklist

When migrating more desktop integrations to capabilities:

- Prefer capability events and snapshots over `window.playSong`, `window.stopSong`,
  and raw `song:*` events.
- Use `target.settingsKey` for local per-song plugin settings.
- Use `targetId` only for arrangement/session correlation, not persistent
  per-song settings.
- Keep raw filename fallback code behind a capability-version check.
- Add static migration guards under `tests/` for any removed global wrapper or
  new capability declaration.
