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

### Manual Migration For Existing Mappings

Until desktop ships an automatic one-time rewrite, existing per-song tone
mappings can be moved manually from a filename key to the current playback
settings key:

1. Open the target song in a playback-capable Slopsmith core.
2. In the webview developer console, read the current safe key:

   ```js
   window._slopsmithPlaybackSettingsKey
   ```

3. Inspect the existing mapping store:

   ```js
   JSON.parse(localStorage.getItem('slopsmith-tone-mappings') || '{}')
   ```

4. Copy the old filename-keyed entries to the safe key. Replace
   `/old/raw/song.psarc` with the old key shown in your store:

   ```js
   const store = JSON.parse(localStorage.getItem('slopsmith-tone-mappings') || '{}');
   const oldKey = '/old/raw/song.psarc';
   const newKey = window._slopsmithPlaybackSettingsKey;
   store.songs = store.songs || {};
   store.midiPC = store.midiPC || {};
   if (store.songs[oldKey] && !store.songs[newKey]) store.songs[newKey] = store.songs[oldKey];
   if (store.midiPC[oldKey] && !store.midiPC[newKey]) store.midiPC[newKey] = store.midiPC[oldKey];
   localStorage.setItem('slopsmith-tone-mappings', JSON.stringify(store));
   ```

5. Reload the song and confirm the Tone Switching panel shows the migrated
   mapping.

Do not delete the old bucket until the migrated mapping has been verified. The
old bucket is harmless and remains useful if the same desktop profile is opened
with an older embedded Slopsmith core.

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
