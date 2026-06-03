const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');

const ROOT = path.join(__dirname, '..');

function readJson(relpath) {
    return JSON.parse(fs.readFileSync(path.join(ROOT, relpath), 'utf8'));
}

function readText(relpath) {
    return fs.readFileSync(path.join(ROOT, relpath), 'utf8');
}

test('renderer manifest declares active playback observer', () => {
    const manifest = readJson('src/renderer/plugin.json');
    const playback = manifest.capabilities.playback;

    assert.deepEqual(playback.roles, ['observer']);
    assert.equal(playback.kind, 'lifecycle');
    assert.deepEqual(playback.observes, ['loading', 'ready', 'stopped', 'ended']);
    assert.equal(playback.mode, 'active');
    assert.equal(playback.compatibility, 'shim-allowed');
    assert.equal(playback.ownership, 'observer-only');
    assert.equal(playback.safety, 'safe');
    assert.equal(playback.version, 1);
});

test('renderer uses playback lifecycle instead of global transport wrappers', () => {
    const source = readText('src/renderer/screen.js');

    assert.equal(source.includes('window.playSong ='), false);
    assert.equal(source.includes('window.stopSong ='), false);
    assert.equal(source.includes('registerObserver'), true);
    assert.equal(source.includes("playback:loading"), true);
    assert.equal(source.includes("playback:ready"), true);
    assert.equal(source.includes("playback:stopped"), true);
    assert.equal(source.includes('settingsKey'), true);
    assert.equal(source.includes('_slopsmithPlaybackSettingsKey'), true);
});

test('renderer migrates legacy filename tone mappings to playback settings key', () => {
    const source = readText('src/renderer/screen.js');
    const docs = readText('docs/CAPABILITY-MIGRATION.md');

    assert.equal(source.includes('migrateToneMappingsToPlaybackSettingsKey'), true);
    assert.equal(source.includes('window._aeMigrateToneMappingsToPlaybackSettingsKey'), true);
    assert.equal(source.includes("migrateBucket('songs')"), true);
    assert.equal(source.includes("migrateBucket('midiPC')"), true);

    assert.equal(docs.includes('Automatic Migration For Existing Mappings'), true);
    assert.equal(docs.includes('Existing `settingsKey` buckets win and are not overwritten.'), true);
});

test('renderer registers native audio-mix fader participants', () => {
    const source = readText('src/renderer/screen.js');

    assert.equal(source.includes('registerMixParticipant'), true);
    assert.equal(source.includes('audio_engine.input_gain'), true);
    assert.equal(source.includes('audio_engine.chain_gain'), true);
    assert.equal(source.includes("'fader.get-value'"), true);
    assert.equal(source.includes("'fader.set-value'"), true);
});