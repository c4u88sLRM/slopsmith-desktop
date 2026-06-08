#pragma once
#include "SourceChain.h"
#include "signalsmith-stretch.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() { return inputDeviceManager; }
    juce::AudioDeviceManager& getInputDeviceManager() { return inputDeviceManager; }
    juce::AudioDeviceManager& getOutputDeviceManager() { return outputDeviceManager; }
    // Per-input DSP now lives on a SourceChain; the engine owns sources[0] (the
    // legacy default input) and forwards the single-source API to it. Multi-source
    // fan-out (sources[1..N]) lands in a later phase; the public surface here is
    // unchanged so NodeAddon and the renderer need no change.
    SignalChain& getSignalChain() { return source0().getSignalChain(); }
    PitchDetector& getPitchDetector() { return source0().getPitchDetector(); }
    MlNoteDetector& getMlNoteDetector() { return source0().getMlNoteDetector(); }

    // Load the Basic Pitch ONNX model for the polyphonic ML detector. When a
    // model is loaded, getActiveDetection() / scoreChord() route through it;
    // otherwise they fall back to the YIN PitchDetector / ChordScorer.
    bool loadNoteModel(const juce::File& modelFile) { return source0().loadNoteModel(modelFile); }
    bool hasMlNoteDetector() const { return source0().hasMlNoteDetector(); }

    // Best current single-note detection: the ML detector's dominant pitch
    // when a model is loaded, else the YIN detector's latest result. Shape is
    // identical either way so the getPitchDetection bridge is detector-agnostic.
    PitchDetector::Detection getActiveDetection() const { return source0().getActiveDetection(); }

    // Raw monophonic YIN detection, always — bypasses the ML preference so the
    // continuous frequency (sub-Hz, parabolically interpolated) and real cents
    // survive even when a Basic Pitch model is loaded. Backs the tuner's
    // getRawPitch bridge endpoint; the YIN detector reads the post-noise-gate
    // signal, so this is silent (frequency -1) when the gate is closed.
    PitchDetector::Detection getRawPitchDetection() const { return source0().getRawPitchDetection(); }

    // Device enumeration
    struct DeviceTypeInfo
    {
        juce::String name;
        juce::StringArray inputDevices;
        juce::StringArray outputDevices;
    };
    struct DeviceOptions
    {
        juce::String type;          // legacy alias = inputType
        juce::String inputType;
        juce::String outputType;
        juce::String input;
        juce::String output;
        juce::StringArray inputChannels;
        juce::StringArray outputChannels;
        juce::Array<double> sampleRates;   // intersection when dual-type
        juce::Array<int> bufferSizes;
        bool compatible = true;     // false when types share no usable sample rate
        juce::String error;
    };

    struct DeviceConfig
    {
        juce::String inputType;
        juce::String inputDevice;
        juce::String outputType;
        juce::String outputDevice;
        double sampleRate = 48000.0;
        int bufferSize = 256;
    };
    struct DeviceConfigResult
    {
        bool ok = false;
        juce::String error;
        double sampleRate = 0.0;
        int inputBlockSize = 0;
        int outputBlockSize = 0;
        bool duplex = true;
    };

    struct DeviceMetrics
    {
        uint64_t inputOverflowCount = 0;
        uint64_t outputUnderflowCount = 0;
        // Counts are in audio frames (stereo pairs), not interleaved-float
        // samples — the ring stores 2 floats per slot but the index math
        // and consumer-facing health metric tick once per frame.
        int outputRingFillFrames = 0;
        int outputRingCapacityFrames = 0;
        bool duplex = true;
    };

    juce::Array<DeviceTypeInfo> getDeviceTypes();
    juce::Array<double> getSampleRates();
    juce::Array<int> getBufferSizes();
    DeviceOptions probeDeviceOptions(const juce::String& typeName,
                                     const juce::String& inputName,
                                     const juce::String& outputName);
    DeviceOptions probeDeviceOptionsDual(const juce::String& inputTypeName,
                                         const juce::String& inputName,
                                         const juce::String& outputTypeName,
                                         const juce::String& outputName);
    juce::String getCurrentDeviceType();    // = getCurrentInputDeviceType
    juce::String getCurrentInputDeviceType();
    juce::String getCurrentOutputDeviceType();
    juce::String getCurrentInputDevice();
    juce::String getCurrentOutputDevice();
    bool isDuplex() const { return duplexMode.load(std::memory_order_relaxed); }
    double getCurrentSampleRate() const { return currentSampleRate.load(std::memory_order_relaxed); }
    int getCurrentBlockSize() const { return inputBlockSize.load(std::memory_order_relaxed); }
    int getCurrentInputBlockSize() const { return inputBlockSize.load(std::memory_order_relaxed); }
    int getCurrentOutputBlockSize() const { return outputBlockSize.load(std::memory_order_relaxed); }
    DeviceMetrics getDeviceMetrics() const;

    bool setDeviceType(const juce::String& typeName);
    bool setInputDeviceType(const juce::String& typeName) { return setDeviceType(typeName); }
    bool setOutputDeviceType(const juce::String& typeName);
    bool setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                        double sampleRate = 48000.0, int bufferSize = 256);
    DeviceConfigResult setAudioDevices(const DeviceConfig& config);

    // Audio start/stop
    void startAudio();
    void stopAudio();
    bool isAudioRunning() const { return audioRunning.load(std::memory_order_relaxed); }

    // Gain controls. Input + chain-output gain are per-source (sources[0]);
    // output gain is the post-mix master and stays engine-global.
    void setInputGain(float gain) { source0().setInputGain(gain); }
    void setOutputGain(float gain) { outputGain.store(gain); }
    float getInputGain() const { return source0().getInputGain(); }
    float getOutputGain() const { return outputGain.load(); }

    // Chain output gain — the amp/tone's output level, applied to the guitar
    // signal before the backing track is mixed. Distinct from outputGain (the
    // post-mix master) so a tone-preset switch doesn't move the song volume.
    void setChainOutputGain(float gain) { source0().setChainOutputGain(gain); }
    float getChainOutputGain() const { return source0().getChainOutputGain(); }

    // Input channel selection (for multi-channel interfaces like Valeton GP-5)
    // 0=left (dry), 1=right (wet), -1=both (mono mix)
    void setInputChannel(int channel) { source0().setInputChannel(channel); }
    int getInputChannel() const { return source0().getInputChannel(); }

    // Monitor mute — when true, input is still processed (pitch detection, metering)
    // but output is silenced unless there are processors in the signal chain
    void setMonitorMute(bool mute) { source0().setMonitorMute(mute); }
    bool isMonitorMuted() const { return source0().isMonitorMuted(); }

    // Monitor-mute suppression — when true, the monitor mute is temporarily
    // overridden so the dry guitar stays audible even with an empty chain.
    // The renderer sets this around a song-load chain rebuild (clear + reload),
    // so the brief empty-chain window doesn't silence the player's guitar.
    void setMonitorMuteSuppressed(bool suppressed) { source0().setMonitorMuteSuppressed(suppressed); }
    bool isMonitorMuteSuppressed() const { return source0().isMonitorMuteSuppressed(); }

    // Number of audio blocks whose signal-chain output had to be scrubbed for
    // non-finite/runaway samples (issue #403). A nonzero value means the chain
    // (NAM/IR/VST) emitted garbage that was contained before it reached the
    // output. Exposed for diagnostics.
    uint32_t getNonFiniteChainBlocks() const { return source0().getNonFiniteChainBlocks(); }

    // Noise gate (post-input-gain, pre FX chain; pitch detector sees ungated signal)
    void setNoiseGate(bool enabled, float thresholdDb, float releaseMs, float depthDb)
    {
        source0().setNoiseGate(enabled, thresholdDb, releaseMs, depthDb);
    }

    // Tone Polish — fixed 3-band mastering EQ (HPF 80 Hz, low shelf -3 dB
    // @ 180 Hz, peak -0.5 dB @ 200 Hz Q=1). Applied on the guitar bus only,
    // between chainOutputGain and the backing-track mix, so the backing
    // track and master output gain stay bit-untouched. Defaults on;
    // renderer exposes a per-preset toggle.
    void setTonePolishEnabled(bool enabled) { source0().setTonePolishEnabled(enabled); }

    // Backing track
    void setBackingVolume(float vol) { backingVolume.store(vol); }
    bool loadBackingTrack(const juce::File& file);
    void setBackingPosition(double seconds);
    void startBacking();
    void stopBacking();
    void setBackingSpeed(double speed);
    // Non-blocking reads — do not acquire backingLock and never block the audio callback
    bool isBackingPlaying() const { return backingPlaying.load(); }
    double getBackingPosition() const { return cachedBackingPosition.load(); }
    double getBackingDuration() const { return cachedBackingDuration.load(); }

    // Metering (read from any thread — atomic). Input level/peak are per-source
    // (sources[0]); output level/peak are the post-mix master, engine-global.
    float getInputLevel() const { return source0().getInputLevel(); }
    float getOutputLevel() const { return currentOutputLevel.load(); }
    float getInputPeak() const { return source0().getInputPeak(); }
    float getOutputPeak() const { return outputPeak.load(); }
    void resetPeaks();

    // Latency
    double getLatencyMs() const;

    // Raw input frame snapshot for renderer-side polyphonic chord scoring in
    // notedetect. Backed by sources[0]'s pre-gate input ring; the rings (and the
    // power-of-two capacity constants) now live on SourceChain. Default snapshot
    // size matches notedetect's _ND_MIN_YIN_SAMPLES (4096 samples).
    std::vector<float> getInputFrame(int numSamples = 4096) const { return source0().getInputFrame(numSamples); }

    // Gapless input-ring consumption for the onset detector — consecutive calls
    // consume each sample exactly once. See SourceChain::getInputSince for the
    // full gap/shortfall contract.
    uint64_t getInputSince(uint64_t fromIndex, std::vector<float>& out) const { return source0().getInputSince(fromIndex, out); }

    // Post-noise-gate raw mono audio snapshot for the external tuner plugin
    // (distinct from getInputFrame's pre-gate ring). Backed by sources[0].
    std::vector<float> getRawAudioFrame(int numSamples = 4096) const { return source0().getRawAudioFrame(numSamples); }

    // Score a chord against the latest input-ring samples. The chord context
    // (notes, arrangement, thresholds) comes from the renderer over IPC; audio
    // data stays inside the engine. Same `{score, hitStrings, totalStrings,
    // isHit, results[]}` shape as the JS implementation.
    ChordScorer::Result scoreChord(const ChordScorer::Request& req) { return source0().scoreChord(req); }

    // Continuous engine-side chart verification (notedetect). The renderer
    // pushes the song's note chart once via setChart(); a background
    // NoteVerifier thread scores each note's timing window against the live
    // playhead and input ring, and the renderer drains finalized verdicts
    // via getNoteVerdicts(). This replaces the renderer's per-tick
    // scoreChord IPC loop, which starved during dense passages.
    void setChart(const NoteVerifier::ChartUpdate& chart) { source0().setChart(chart); }
    void clearChart() { source0().clearChart(); }
    std::vector<NoteVerifier::Verdict> getNoteVerdicts() { return source0().getNoteVerdicts(); }

    // Renderer's unified, already-corrected playhead — the verifier scores
    // against this rather than getBackingPosition(), which is frozen for
    // HTML5-routed (sloppak) songs. Pushed each detect tick via getNoteVerdicts.
    void setPlayhead(double songTime, bool playing) { source0().setPlayhead(songTime, playing); }

    // ── Multi-input source management ─────────────────────────────────────────
    // A "source" is one independent input chain (its own arrangement chart, note
    // detection, scoring, tone, and monitor). sources[0] always exists. Adding a
    // source binds it to an input channel of the current device (multi-channel
    // interface); separate-device binding lands in a later phase.
    struct SourceInfo
    {
        int id = -1;
        int inputChannel = -1;   // -1 = mono mix of first pair
        bool active = false;
    };

    // Activate a pooled chain bound to `inputChannel` and return its id, or -1 if
    // the pool is full. Prepares the chain immediately when audio is running so it
    // starts scoring without a device restart. Control-thread only.
    int addSource(int inputChannel);
    // Deactivate + release a source (id != 0; sources[0] is permanent). Stops its
    // verifier/ML threads; the pooled object is reused by a later addSource.
    bool removeSource(int id);
    // Snapshot of every active source. Control-thread only.
    std::vector<SourceInfo> listSources() const;

    // Per-source accessors for the NodeAddon source-indexed API. Return nullptr
    // for an out-of-range or inactive id (sources[0] always valid).
    SourceChain* getSource(int id);

private:
    // sources[0] is the legacy default input chain; always present + active.
    SourceChain& source0() { return *sources[0]; }
    const SourceChain& source0() const { return *sources[0]; }
    // Input-device callback. In duplex it writes outputData directly; in split
    // it pushes processed stereo into outputPendingRing for OutputCallback.
    void audioDeviceIOCallbackWithContext(const float* const* inputData,
                                          int numInputChannels,
                                          float* const* outputData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void stopBackingNoLock(); // caller holds backingLock

    // Renders one block of the backing track into backingBuffer (1x bypass or
    // phase-vocoder stretch), advances backingHeardPositionSec /
    // cachedBackingPosition, and clears backingPlaying at EOF. Returns the
    // number of output frames written (== jmin(numSamples, backingBuffer cap)).
    // Shared by the duplex and split output callbacks so the two paths can't
    // drift. Precondition: caller holds backingLock and has verified
    // backingTransport && backingPlaying.
    int renderBackingBlockLocked(int numSamples);

    // Split-mode only: drains outputPendingRing, mixes backing, writes to device.
    void audioOutputCallback(const float* const* inputData,
                             int numInputChannels,
                             float* const* outputData,
                             int numOutputChannels,
                             int numSamples);
    void audioOutputAboutToStart(juce::AudioIODevice* device);
    void audioOutputStopped();

    class OutputCallback : public juce::AudioIODeviceCallback
    {
    public:
        explicit OutputCallback(AudioEngine& e) : engine(e) {}
        void audioDeviceIOCallbackWithContext(const float* const* inputData,
                                              int numInputChannels,
                                              float* const* outputData,
                                              int numOutputChannels,
                                              int numSamples,
                                              const juce::AudioIODeviceCallbackContext&) override
        {
            engine.audioOutputCallback(inputData, numInputChannels, outputData, numOutputChannels, numSamples);
        }
        void audioDeviceAboutToStart(juce::AudioIODevice* device) override { engine.audioOutputAboutToStart(device); }
        void audioDeviceStopped() override { engine.audioOutputStopped(); }
    private:
        AudioEngine& engine;
    };
    OutputCallback outputCallback{ *this };

    juce::String applyDuplexSetup(const juce::String& inputName,
                                  const juce::String& outputName,
                                  double sampleRate,
                                  int bufferSize);
    DeviceConfigResult applySplitSetup(const DeviceConfig& config);
    void teardownSplitMode();

    // Duplex mode: inputDeviceManager owns both directions, outputDeviceManager idle.
    // Split mode: input-only on inputDeviceManager, output-only on outputDeviceManager
    // with an SPSC ring between them.
    juce::AudioDeviceManager inputDeviceManager;
    juce::AudioDeviceManager outputDeviceManager;
    std::atomic<bool> duplexMode{true};

    // Per-input capture+detect+monitor chains. A FIXED pool, all constructed up
    // front, so adding/removing a source never reassigns a pointer the audio
    // thread is reading — addSource/removeSource only flip an atomic `active`
    // flag (and prepare/release the chain). sources[0] is the legacy default,
    // active from construction and bound to the primary input device. The audio
    // callback fans device channels out to each active source and fans their
    // monitor signals into the output mix. SourceChain reads the engine's
    // audioRunning / currentSampleRate atomics through references bound at
    // construction.
    static constexpr int kMaxSources = 8;
    std::array<std::unique_ptr<SourceChain>, kMaxSources> sources;
    // Serialises addSource/removeSource (control threads only — never the audio
    // thread, which just reads each slot's atomic `active`).
    std::mutex sourcesMutex;
    // Audio-thread scratch for the multi-source mix: each active source renders
    // its 2-channel monitor here in turn, then it is summed into the output.
    // Pre-sized in audioDeviceAboutToStart so the hot loop never allocates.
    juce::AudioBuffer<float> sourceMonitorScratch;
    // Bumped once at the top of every input callback. removeSource() flips a
    // source inactive, then waits for this to advance two full callbacks before
    // releasing it — guaranteeing no in-flight callback is still processing it.
    std::atomic<uint64_t> callbackGeneration{0};

    juce::AudioFormatManager formatManager;

    // Master output (post-mix) — engine-global, not per-source.
    std::atomic<float> outputGain{1.0f};
    std::atomic<float> backingVolume{0.7f};
    std::atomic<float> currentOutputLevel{0.0f};
    std::atomic<float> outputPeak{0.0f};

    // Backing track
    std::unique_ptr<juce::AudioFormatReaderSource> backingSource;
    std::unique_ptr<juce::AudioTransportSource> backingTransport;
    signalsmith::stretch::SignalsmithStretch<float> backingStretch;
    juce::AudioBuffer<float> backingInputBuffer; // pulled from transport at device rate
    juce::AudioBuffer<float> backingBuffer; // stretch output, mixed into device buffer
    std::atomic<int> backingStretchLatencySamples{0};
    std::atomic<bool> backingPlaying{false};
    std::atomic<double> cachedBackingPosition{0.0};
    std::atomic<double> cachedBackingDuration{0.0};
    // Heard playhead: accumulates the source frames consumed each block, then
    // clamped to backingTransport->getCurrentPosition() so a short read at EOF
    // can't push it past the real source point. cachedBackingPosition is this
    // value minus the stretcher output latency (zero on the 1x bypass path).
    std::atomic<double> backingHeardPositionSec{0.0};
    // Active playback rate. Mutated ONLY by the audio thread (in
    // renderBackingBlockLocked), coupled with the stretcher reset, so a block
    // is never processed at a new rate with stale stretch state.
    std::atomic<double> backingSpeed{1.0};
    // Lock-free speed hand-off: setBackingSpeed (control thread) publishes the
    // requested rate here and raises backingSpeedChangePending; the audio
    // thread adopts it on the next block. Avoids the control thread blocking on
    // backingLock and starving the RT tryLock (which would drop a backing block
    // mid-slider-drag).
    std::atomic<double> backingPendingSpeed{1.0};
    std::atomic<bool> backingSpeedChangePending{false};
    juce::CriticalSection backingLock;

    // Toggled from startAudio()/stopAudio() (main / device-management
    // threads) and read from isAudioRunning() on the JS thread via the
    // audio-bridge dispatch loop. Plain bool would be a data race;
    // relaxed-atomic is well-defined and compiles to a plain MOV.
    std::atomic<bool> audioRunning{false};
    // Sample rate is written from the JUCE device callbacks (audio
    // thread / device-management thread) and read from arbitrary
    // callers including the JS thread via getCurrentSampleRate(),
    // so a plain double would be a C++ data race. std::atomic<double>
    // is well-defined and lock-free on the platforms we ship; the
    // hot reads use relaxed since the consumer just wants the latest
    // observable value, not a synchronization point.
    std::atomic<double> currentSampleRate{48000.0};
    // Split mode allows different input vs output block sizes; the ring absorbs
    // the asymmetry. DSP prepares against input; backing resampler against output.
    std::atomic<int> inputBlockSize{256};
    std::atomic<int> outputBlockSize{256};

    // The per-input lock-free SPSC rings (pre-gate getInputFrame ring + post-gate
    // getRawAudioFrame ring), the YIN/ML detectors, and the zero-output capture
    // scratch now live on SourceChain — one set per input source. See
    // SourceChain.h for the full lock-free / power-of-two / cold-start rationale.

    // Split-mode SPSC ring (unused in duplex). Each slot packs one stereo frame
    // (L+R floats) into a single 64-bit atomic so the consumer reads both
    // channels in one indivisible load — without packing, the producer's two
    // separate atomic stores could interleave with the consumer's two loads
    // during a drop-oldest wrap, surfacing as L_new+R_old (or vice versa)
    // sample tears. ~85 ms @ 48 kHz — absorbs clock drift over typical sessions.
    static constexpr int kOutputRingFrames = 4096;
    std::array<std::atomic<uint64_t>, kOutputRingFrames> outputPendingRing{};
    static_assert((kOutputRingFrames & (kOutputRingFrames - 1)) == 0,
                  "kOutputRingFrames must be a power of two for mask wraparound");
    // RT-thread reads + writes touch these slots, so a lock-based fallback
    // would risk priority inversion + audible dropouts. On the platforms we
    // ship (x86_64 + arm64 across Linux/macOS/Windows) atomic<uint64_t> is
    // always lock-free; this assert turns a regression into a build error
    // instead of a silent latency degradation if a future platform port
    // breaks the assumption.
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "outputPendingRing requires lock-free atomic<uint64_t> for RT safety");
    static_assert(sizeof(float) == 4,
                  "outputPendingRing pack/unpack assumes 32-bit float");

    // Pack/unpack helpers — std::bit_cast (C++20) is constexpr + alias-safe.
    static inline uint64_t packLR(float l, float r) noexcept
    {
        const uint32_t li = std::bit_cast<uint32_t>(l);
        const uint32_t ri = std::bit_cast<uint32_t>(r);
        return (static_cast<uint64_t>(ri) << 32) | static_cast<uint64_t>(li);
    }
    static inline void unpackLR(uint64_t v, float& l, float& r) noexcept
    {
        l = std::bit_cast<float>(static_cast<uint32_t>(v & 0xFFFFFFFFu));
        r = std::bit_cast<float>(static_cast<uint32_t>(v >> 32));
    }

    std::atomic<uint64_t> outputRingWriteIndex{0};
    std::atomic<uint64_t> outputRingReadIndex{0};
    std::atomic<uint64_t> outputUnderflowCount{0};
    std::atomic<uint64_t> inputOverflowCount{0};

    // Pre-sized to outputBlockSize so the pull loop never allocates.
    std::vector<float> outputPullScratchL;
    std::vector<float> outputPullScratchR;
    juce::AudioBuffer<float> outputBackingBuffer;
    bool outputCallbackRegistered = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
