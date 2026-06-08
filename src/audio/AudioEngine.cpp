#include "AudioEngine.h"
#include "AudioSanitize.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// Hard ceiling on backing playback speed. This drives input buffer sizing and runtime clamp.
static constexpr double kMaxBackingSpeed = 4.0;
// Transparent full-speed path — skip the stretcher when rate is effectively 1×.
static constexpr double kBackingSpeedBypassEpsilon = 1.0e-4;

// On Windows, ASIO drivers can crash with access violations.
// We catch C++ exceptions but can't easily catch SEH in functions with dtors.
// The try/catch blocks around device operations are the best we can do
// without restructuring the code into SEH-safe wrapper functions.

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();

    // Construct the full source pool up front so addSource/removeSource never
    // reassign a pointer the audio thread reads — they only flip `active`. Each
    // chain reads the engine's audioRunning / currentSampleRate atomics by
    // reference (both already constructed as members before this body runs).
    // sources[0] is the permanent default input, active from the start; the rest
    // are inactive (no threads — NoteVerifier's worker only starts in prepare()).
    for (int i = 0; i < kMaxSources; ++i)
        sources[(size_t) i] = std::make_unique<SourceChain>(i, audioRunning, currentSampleRate);
    sources[0]->setActive(true);

    auto result = inputDeviceManager.initialiseWithDefaultDevices(2, 2);
    if (result.isNotEmpty())
        std::cerr << "[AudioEngine] input init note: " << result.toStdString() << std::endl;

    auto outResult = outputDeviceManager.initialiseWithDefaultDevices(0, 2);
    if (outResult.isNotEmpty())
        std::cerr << "[AudioEngine] output init note: " << outResult.toStdString() << std::endl;

    // Some backends (WASAPI) bind to a default device on init and would
    // hold it exclusive against the duplex codepath. Idle until split mode.
    outputDeviceManager.closeAudioDevice();

    auto& availableTypes = inputDeviceManager.getAvailableDeviceTypes();
    std::cerr << "[AudioEngine] Available device types: " << availableTypes.size() << std::endl;
    for (auto* type : availableTypes)
    {
        type->scanForDevices();
        std::cerr << "[AudioEngine]   " << type->getTypeName().toStdString()
                  << " - inputs: " << type->getDeviceNames(true).size()
                  << ", outputs: " << type->getDeviceNames(false).size() << std::endl;
    }
    for (auto* type : outputDeviceManager.getAvailableDeviceTypes())
        type->scanForDevices();
}

AudioEngine::~AudioEngine()
{
    stopAudio();
    stopBacking();
}

// ── Device Enumeration ────────────────────────────────────────────────────────

juce::Array<AudioEngine::DeviceTypeInfo> AudioEngine::getDeviceTypes()
{
    juce::Array<DeviceTypeInfo> types;

    for (auto* type : inputDeviceManager.getAvailableDeviceTypes())
    {
        DeviceTypeInfo info;
        info.name = type->getTypeName();

        // Use already-scanned device names (scanForDevices was called during init)
        info.inputDevices = type->getDeviceNames(true);
        info.outputDevices = type->getDeviceNames(false);

        types.add(std::move(info));
    }

    return types;
}

juce::Array<double> AudioEngine::getSampleRates()
{
    juce::Array<double> rates;
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        for (auto rate : device->getAvailableSampleRates())
            rates.add(rate);
    }
    return rates;
}

juce::Array<int> AudioEngine::getBufferSizes()
{
    juce::Array<int> sizes;
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        for (auto size : device->getAvailableBufferSizes())
            sizes.add(size);
    }
    return sizes;
}

AudioEngine::DeviceOptions AudioEngine::probeDeviceOptions(const juce::String& typeName,
                                                           const juce::String& inputName,
                                                           const juce::String& outputName)
{
    return probeDeviceOptionsDual(typeName, inputName, typeName, outputName);
}

AudioEngine::DeviceOptions AudioEngine::probeDeviceOptionsDual(const juce::String& inputTypeName,
                                                               const juce::String& inputName,
                                                               const juce::String& outputTypeName,
                                                               const juce::String& outputName)
{
    DeviceOptions options;
    options.inputType = inputTypeName;
    options.outputType = outputTypeName.isEmpty() ? inputTypeName : outputTypeName;
    options.type = options.inputType;   // legacy alias

    // Resolve each side from its own manager so probe stays consistent with
    // applySplitSetup()/setOutputDeviceType(), which mutate the manager that
    // owns the side they're configuring. Using inputDeviceManager for the
    // output lookup would silently fall back to whatever input has scanned,
    // which can miss output-only backends.
    auto findType = [](juce::AudioDeviceManager& manager,
                       const juce::String& wanted) -> juce::AudioIODeviceType* {
        juce::AudioIODeviceType* match = nullptr;
        for (auto* type : manager.getAvailableDeviceTypes())
        {
            if ((wanted.isNotEmpty() && type->getTypeName() == wanted)
                || (wanted.isEmpty() && match == nullptr))
            {
                match = type;
                if (wanted.isNotEmpty()) break;
            }
        }
        return match;
    };

    auto* inputType  = findType(inputDeviceManager, options.inputType);

    // Match setAudioDevices's resolution: when the caller didn't specify
    // an output type, default it to the SAME type the input side resolved
    // to (using the type's name, looked up in outputDeviceManager). Without
    // this, an empty `options.outputType` would let findType pick whatever
    // outputDeviceManager enumerates first — potentially a different
    // backend than inputDeviceManager picked from the empty string, which
    // then disagrees with the apply path's duplex classification.
    juce::String effectiveOutputTypeName = options.outputType;
    if (effectiveOutputTypeName.isEmpty() && inputType != nullptr)
        effectiveOutputTypeName = inputType->getTypeName();
    auto* outputType = findType(outputDeviceManager, effectiveOutputTypeName);

    if (inputType == nullptr)
    {
        options.error = "Input device type not found";
        options.compatible = false;
        return options;
    }
    if (outputType == nullptr)
    {
        options.error = "Output device type not found";
        options.compatible = false;
        return options;
    }

    try
    {
        options.inputType = inputType->getTypeName();
        options.outputType = outputType->getTypeName();
        options.type = options.inputType;

        options.input = inputName;
        options.output = outputName;

        // userIntendsDuplex matches setAudioDevices's classification:
        // identical names on both sides (typically both empty = OS default)
        // means we'll go duplex regardless of which specific devices the
        // first-enumerated lookup would have produced.
        const bool userIntendsDuplex = (options.inputType == options.outputType
                                        && options.input == options.output);

        // For probing we still need a concrete device to instantiate.
        // Resolve empty names to first-enumerated ONLY for the probe-device
        // creation below — DON'T write back into options.input/options.output;
        // those flow to the UI and the apply path, which treat empty as
        // "OS default" per side.
        auto inputs  = inputType->getDeviceNames(true);
        auto outputs = outputType->getDeviceNames(false);
        const juce::String probeInputName =
            options.input.isEmpty() && inputs.size() > 0 ? inputs[0] : options.input;
        const juce::String probeOutputName =
            options.output.isEmpty() && outputs.size() > 0 ? outputs[0] : options.output;

        const bool isDuplex = userIntendsDuplex
                              || (options.inputType == options.outputType
                                  && options.input == options.output
                                  && options.input.isNotEmpty());

        if (isDuplex)
        {
            std::unique_ptr<juce::AudioIODevice> dev(
                inputType->createDevice(probeOutputName, probeInputName));
            if (!dev) { options.error = "Could not create probe device"; options.compatible = false; return options; }

            options.inputChannels = dev->getInputChannelNames();
            options.outputChannels = dev->getOutputChannelNames();
            for (auto rate : dev->getAvailableSampleRates())
                options.sampleRates.addIfNotAlreadyThere(rate);
            for (auto size : dev->getAvailableBufferSizes())
                options.bufferSizes.addIfNotAlreadyThere(size);
        }
        else
        {
            std::unique_ptr<juce::AudioIODevice> inDev(
                inputType->createDevice({}, probeInputName));
            std::unique_ptr<juce::AudioIODevice> outDev(
                outputType->createDevice(probeOutputName, {}));
            if (!inDev || !outDev)
            {
                options.error = "Could not create dual probe devices";
                options.compatible = false;
                return options;
            }

            options.inputChannels = inDev->getInputChannelNames();
            options.outputChannels = outDev->getOutputChannelNames();

            // Tolerance covers backends that report fractional drift around the nominal rate.
            const auto inRates = inDev->getAvailableSampleRates();
            const auto outRates = outDev->getAvailableSampleRates();
            for (auto r : inRates)
            {
                for (auto r2 : outRates)
                {
                    // <= 0.5 (not <) to match applySplitSetup's rateSupportedBy
                    // check. A backend reporting 47999.5 on both sides has
                    // |r - r2| = 0 (matches anyway) but a backend mixing
                    // 47999.5 in / 48000.0 out has |diff| = 0.5 exactly, which
                    // < 0.5 would reject from the probe even though the
                    // apply-side check accepts it.
                    if (std::abs(r - r2) <= 0.5)
                    {
                        // Round the midpoint to a clean nominal rate
                        // (backends sometimes report fractional near-48000
                        // rates; surfacing the raw value would fail the
                        // apply-side setAudioDeviceSetup, which expects an
                        // exact supported nominal). Re-check the rounded
                        // candidate is within tolerance of BOTH sides — a
                        // matched pair like 48000.4/48000.6 passes the |r-r2|
                        // check but std::round(48000.4)=48000 would fall
                        // outside tolerance of 48000.6 (diff 0.6). Skip
                        // those so the probe stays fail-closed.
                        const double candidate = std::round((r + r2) * 0.5);
                        if (std::abs(r  - candidate) <= 0.5
                         && std::abs(r2 - candidate) <= 0.5)
                        {
                            options.sampleRates.addIfNotAlreadyThere(candidate);
                        }
                        break;
                    }
                }
            }
            if (options.sampleRates.isEmpty())
            {
                options.error = "Input and output devices share no common sample rate";
                options.compatible = false;
            }

            // Split mode opens both sides with the same bufferSize, so the
            // UI should only see sizes the intersection of both devices
            // supports — a union would let the user pick a value that
            // predictably fails at apply time on one side.
            const auto inBufs = inDev->getAvailableBufferSizes();
            const auto outBufs = outDev->getAvailableBufferSizes();
            for (auto b : inBufs)
            {
                for (auto b2 : outBufs)
                {
                    if (b == b2)
                    {
                        options.bufferSizes.addIfNotAlreadyThere(b);
                        break;
                    }
                }
            }
            // An empty intersection means there's no buffer size both sides
            // accept; setting compatible=false stops the UI from re-enabling
            // Apply against a guaranteed-fail config.
            if (options.bufferSizes.isEmpty() && options.error.isEmpty())
            {
                options.error = "Input and output devices share no common buffer size";
                options.compatible = false;
            }
        }

        fprintf(stderr, "[AudioEngine] Probed device options: inType='%s' outType='%s' in='%s' out='%s' "
                "duplex=%d inputs=%d outputs=%d rates=%d buffers=%d compatible=%d\n",
                options.inputType.toRawUTF8(), options.outputType.toRawUTF8(),
                options.input.toRawUTF8(), options.output.toRawUTF8(),
                (int) isDuplex, options.inputChannels.size(), options.outputChannels.size(),
                options.sampleRates.size(), options.bufferSizes.size(), (int) options.compatible);
    }
    catch (const std::exception& e)
    {
        options.error = e.what();
        options.compatible = false;
    }
    catch (...)
    {
        options.error = "Probe failed";
        options.compatible = false;
    }

    return options;
}

juce::String AudioEngine::getCurrentDeviceType()
{
    return getCurrentInputDeviceType();
}

juce::String AudioEngine::getCurrentInputDeviceType()
{
    if (auto* type = inputDeviceManager.getCurrentDeviceTypeObject())
        return type->getTypeName();
    return {};
}

juce::String AudioEngine::getCurrentOutputDeviceType()
{
    if (duplexMode.load(std::memory_order_relaxed))
        return getCurrentInputDeviceType();
    if (auto* type = outputDeviceManager.getCurrentDeviceTypeObject())
        return type->getTypeName();
    return {};
}

juce::String AudioEngine::getCurrentInputDevice()
{
    if (auto* device = inputDeviceManager.getCurrentAudioDevice())
    {
        auto setup = inputDeviceManager.getAudioDeviceSetup();
        return setup.inputDeviceName;
    }
    return {};
}

juce::String AudioEngine::getCurrentOutputDevice()
{
    auto& mgr = duplexMode.load(std::memory_order_relaxed)
        ? inputDeviceManager : outputDeviceManager;
    if (mgr.getCurrentAudioDevice() == nullptr) return {};
    return mgr.getAudioDeviceSetup().outputDeviceName;
}

AudioEngine::DeviceMetrics AudioEngine::getDeviceMetrics() const
{
    DeviceMetrics m;
    m.duplex = duplexMode.load(std::memory_order_relaxed);
    m.inputOverflowCount = inputOverflowCount.load(std::memory_order_relaxed);
    m.outputUnderflowCount = outputUnderflowCount.load(std::memory_order_relaxed);
    // The output ring is only used in split mode. Report 0/0 in duplex so
    // consumers don't think there's a live ring buffer to monitor when
    // there isn't one. Capacity reads-as-zero in duplex matches the
    // "no ring activity" semantic — the ring is structurally inert.
    if (! m.duplex)
    {
        m.outputRingCapacityFrames = kOutputRingFrames;
        // Output device can stop while the input keeps writing, leaving
        // (w - r) larger than capacity. Clamp uint64 → int via the
        // capacity ceiling so the consumer-facing field never overflows
        // or goes negative.
        const uint64_t w = outputRingWriteIndex.load(std::memory_order_acquire);
        const uint64_t r = outputRingReadIndex.load(std::memory_order_acquire);
        const uint64_t fill = (w >= r) ? (w - r) : 0;
        m.outputRingFillFrames = (int) std::min(fill, (uint64_t) kOutputRingFrames);
    }
    return m;
}

double AudioEngine::getLatencyMs() const
{
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    if (sr <= 0.0) return 0.0;

    if (duplexMode.load(std::memory_order_relaxed))
    {
        if (auto* device = inputDeviceManager.getCurrentAudioDevice())
        {
            int latencySamples = device->getCurrentBufferSizeSamples()
                               + device->getInputLatencyInSamples()
                               + device->getOutputLatencyInSamples();
            return (latencySamples / sr) * 1000.0;
        }
        return 0.0;
    }

    int totalSamples = 0;
    if (auto* in = inputDeviceManager.getCurrentAudioDevice())
        totalSamples += in->getCurrentBufferSizeSamples() + in->getInputLatencyInSamples();
    if (auto* out = outputDeviceManager.getCurrentAudioDevice())
        totalSamples += out->getCurrentBufferSizeSamples() + out->getOutputLatencyInSamples();

    // Steady-state ring residency ≈ half capacity once both clocks stabilize.
    totalSamples += kOutputRingFrames / 2;

    return (totalSamples / sr) * 1000.0;
}

// ── Device Selection ──────────────────────────────────────────────────────────

bool AudioEngine::setDeviceType(const juce::String& typeName)
{
    if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
    {
        if (currentType->getTypeName() == typeName)
        {
            fprintf(stderr, "[AudioEngine] Input device type already selected: %s\n", typeName.toRawUTF8());
            return true;
        }
    }

    for (auto* type : inputDeviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == typeName)
        {
            try {
                fprintf(stderr, "[AudioEngine] Setting input device type: %s\n", typeName.toRawUTF8());
                inputDeviceManager.setCurrentAudioDeviceType(typeName, true);
                // Legacy single-type API contract: callers expect both
                // managers to track the same backend so a subsequent
                // duplex configure or probe sees a consistent dual state.
                // setOutputDeviceType() exists for callers that want to
                // diverge the two sides intentionally. Best-effort — if
                // the output side doesn't expose this backend the input
                // change still stands so duplex on the matched backend
                // keeps working.
                if (auto* currentOutputType = outputDeviceManager.getCurrentDeviceTypeObject())
                {
                    if (currentOutputType->getTypeName() != typeName)
                    {
                        for (auto* outType : outputDeviceManager.getAvailableDeviceTypes())
                        {
                            if (outType->getTypeName() == typeName)
                            {
                                try { outputDeviceManager.setCurrentAudioDeviceType(typeName, true); }
                                catch (...) {
                                    fprintf(stderr, "[AudioEngine] setDeviceType: output sync threw (continuing)\n");
                                }
                                break;
                            }
                        }
                    }
                }
                return true;
            } catch (const std::exception& e) {
                fprintf(stderr, "[AudioEngine] setDeviceType crashed: %s\n", e.what());
                return false;
            } catch (...) {
                fprintf(stderr, "[AudioEngine] setDeviceType crashed (unknown)\n");
                return false;
            }
        }
    }
    return false;
}

bool AudioEngine::setOutputDeviceType(const juce::String& typeName)
{
    if (duplexMode.load(std::memory_order_relaxed))
        return setDeviceType(typeName);

    if (auto* currentType = outputDeviceManager.getCurrentDeviceTypeObject())
    {
        if (currentType->getTypeName() == typeName)
            return true;
    }
    for (auto* type : outputDeviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == typeName)
        {
            try {
                fprintf(stderr, "[AudioEngine] Setting output device type: %s\n", typeName.toRawUTF8());
                outputDeviceManager.setCurrentAudioDeviceType(typeName, true);
                return true;
            } catch (...) {
                fprintf(stderr, "[AudioEngine] setOutputDeviceType crashed\n");
                return false;
            }
        }
    }
    return false;
}

bool AudioEngine::setAudioDevice(const juce::String& inputName, const juce::String& outputName,
                                  double sampleRate, int bufferSize)
{
    DeviceConfig c;
    c.inputType  = getCurrentInputDeviceType();
    c.outputType = c.inputType;
    c.inputDevice = inputName;
    c.outputDevice = outputName;
    c.sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    c.bufferSize = bufferSize > 0 ? bufferSize : 256;
    return setAudioDevices(c).ok;
}

AudioEngine::DeviceConfigResult AudioEngine::setAudioDevices(const DeviceConfig& config)
{
    DeviceConfigResult res;

    fprintf(stderr, "[AudioEngine] setAudioDevices: inType='%s' inDev='%s' outType='%s' outDev='%s' sr=%.0f bs=%d\n",
            config.inputType.toRawUTF8(), config.inputDevice.toRawUTF8(),
            config.outputType.toRawUTF8(), config.outputDevice.toRawUTF8(),
            config.sampleRate, config.bufferSize);

    // Platform-preferred backend when unspecified. Linux prefers ALSA over
    // JACK (jackd may be installed but not running).
    juce::String resolvedInputType = config.inputType;
    if (resolvedInputType.isEmpty())
    {
        if (auto* t = inputDeviceManager.getCurrentDeviceTypeObject())
            resolvedInputType = t->getTypeName();
    }
    if (resolvedInputType.isEmpty())
    {
#if JUCE_LINUX
        const juce::StringArray preferredOrder { "ALSA", "JACK" };
#elif JUCE_MAC
        const juce::StringArray preferredOrder { "CoreAudio" };
#elif JUCE_WINDOWS
        const juce::StringArray preferredOrder { "Windows Audio", "ASIO" };
#else
        const juce::StringArray preferredOrder;
#endif
        const auto& available = inputDeviceManager.getAvailableDeviceTypes();
        for (const auto& want : preferredOrder)
        {
            for (auto* type : available)
            {
                if (type->getTypeName() == want)
                {
                    resolvedInputType = want;
                    break;
                }
            }
            if (resolvedInputType.isNotEmpty()) break;
        }
        if (resolvedInputType.isEmpty() && !available.isEmpty())
            resolvedInputType = available.getFirst()->getTypeName();
    }

    juce::String resolvedOutputType = config.outputType.isEmpty()
        ? resolvedInputType : config.outputType;

    // Stop audio BEFORE mutating device-type or device setup. JUCE can
    // tear down and re-scan devices inside setCurrentAudioDeviceType /
    // setAudioDeviceSetup, and doing that while the audio callback is
    // still attached risks crashes/deadlocks on some backends (ASIO is
    // the usual culprit). stopAudio() detaches both callbacks first;
    // we'll re-start at the end if we were running.
    //
    // Unconditional: audioDeviceStopped() can clear audioRunning during a
    // transient input stop while the output callback intentionally stays
    // registered for JUCE's auto-restart. A reconfigure that races that
    // window would otherwise skip the stopAudio() detach and leave the
    // stale output callback attached. stopAudio() is itself idempotent
    // (R9 fix — removeAudioCallback is a no-op when not registered), so
    // running it unconditionally is safe regardless of audioRunning.
    const bool wasRunning = audioRunning.load(std::memory_order_relaxed);
    stopAudio();

    // setCurrentAudioDeviceType can throw from inside JUCE backends (ASIO
    // is the usual culprit). Catch and propagate as a structured error so
    // the N-API caller doesn't see the exception cross the boundary.
    try
    {
        if (auto* current = inputDeviceManager.getCurrentDeviceTypeObject())
        {
            if (current->getTypeName() != resolvedInputType)
                inputDeviceManager.setCurrentAudioDeviceType(resolvedInputType, true);
        }
        else
        {
            inputDeviceManager.setCurrentAudioDeviceType(resolvedInputType, true);
        }
    }
    catch (...)
    {
        res.error = "setCurrentAudioDeviceType threw for input type '" + resolvedInputType + "'";
        return res;
    }

    // User-intent duplex: both sides came in identical (typically both
    // empty = "system default", or both naming the same explicit device).
    // Capture before we resolve names, otherwise the resolve loop below
    // fills empty-input with first-input-device and empty-output with
    // first-output-device — those usually differ (especially on macOS
    // where defaults are separate input/output devices), and the engine
    // would silently route into split mode with ~85ms of ring-buffer
    // latency for a config the user expected to be duplex. Legacy
    // pre-PR settings commonly use empty names; preserve their behavior.
    const bool userIntendsDuplex = (resolvedInputType == resolvedOutputType
                                    && config.inputDevice == config.outputDevice);

    // Don't resolve empty names to first-device-of-each-type. Pre-PR
    // behavior — and Copilot's fail-closed concern — treat empty names
    // as "OS default" per side. Filling them with inputs[0] / outputs[0]
    // is JUCE-enumeration-order dependent and can pick the wrong device
    // (e.g. an audio interface that isn't the system default). Both
    // applyDuplexSetup and applySplitSetup handle empty names by setting
    // useDefault*Channels=true, letting JUCE select the OS default.
    const juce::String& resolvedInput  = config.inputDevice;
    const juce::String& resolvedOutput = config.outputDevice;

    const bool isDuplex = userIntendsDuplex
                          || (resolvedInputType == resolvedOutputType
                              && resolvedInput == resolvedOutput
                              && resolvedInput.isNotEmpty());

    // Normalize before branching — applyDuplexSetup() only checks `> 0` and
    // would otherwise let Infinity (or NaN slipping past N-API) reach JUCE
    // on the legacy positional setDevice() path. Finite-and-positive is the
    // baseline both modes need.
    const double requestedSampleRate =
        (std::isfinite(config.sampleRate) && config.sampleRate > 0.0)
            ? config.sampleRate
            : 48000.0;
    const int requestedBufferSize = config.bufferSize > 0 ? config.bufferSize : 256;

    if (isDuplex)
    {
        teardownSplitMode();

        const juce::String err = applyDuplexSetup(resolvedInput, resolvedOutput,
                                                  requestedSampleRate, requestedBufferSize);
        if (err.isNotEmpty())
        {
            res.error = err;
            res.duplex = true;
            return res;
        }
        duplexMode.store(true, std::memory_order_relaxed);

        if (auto* dev = inputDeviceManager.getCurrentAudioDevice())
        {
            res.sampleRate = dev->getCurrentSampleRate();
            res.inputBlockSize = dev->getCurrentBufferSizeSamples();
            res.outputBlockSize = res.inputBlockSize;
        }
        res.ok = true;
        res.duplex = true;
    }
    else
    {
        DeviceConfig resolved = config;
        resolved.inputType = resolvedInputType;
        resolved.outputType = resolvedOutputType;
        resolved.inputDevice = resolvedInput;
        resolved.outputDevice = resolvedOutput;
        resolved.sampleRate = requestedSampleRate;
        resolved.bufferSize = requestedBufferSize;

        res = applySplitSetup(resolved);
        if (!res.ok)
            return res;
        duplexMode.store(false, std::memory_order_relaxed);
    }

    if (wasRunning) startAudio();
    return res;
}

juce::String AudioEngine::applyDuplexSetup(const juce::String& inputName,
                                           const juce::String& outputName,
                                           double sampleRate, int bufferSize)
{
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = inputName;
    setup.outputDeviceName = outputName;
    setup.sampleRate = sampleRate > 0 ? sampleRate : 48000.0;
    setup.bufferSize = bufferSize > 0 ? bufferSize : 256;
    setup.useDefaultInputChannels = inputName.isEmpty();
    setup.useDefaultOutputChannels = outputName.isEmpty();

    // Channel masks must match too — high-numbered selectedInputChannel needs
    // the expanded mask that an older session may not have opened.
    if (auto* currentDevice = inputDeviceManager.getCurrentAudioDevice())
    {
        try
        {
            juce::AudioDeviceManager::AudioDeviceSetup current;
            inputDeviceManager.getAudioDeviceSetup(current);

            const int advertisedInputs = currentDevice->getInputChannelNames().size();
            juce::BigInteger expectedInputs;
            expectedInputs.setRange(0, advertisedInputs > 0 ? advertisedInputs : 2, true);

            const int advertisedOutputs = currentDevice->getOutputChannelNames().size();
            juce::BigInteger expectedOutputs;
            expectedOutputs.setRange(0, juce::jmin(advertisedOutputs > 0 ? advertisedOutputs : 2, 2), true);

            if (current.inputDeviceName == setup.inputDeviceName
                && current.outputDeviceName == setup.outputDeviceName
                && current.sampleRate == setup.sampleRate
                && current.bufferSize == setup.bufferSize
                && current.useDefaultInputChannels == setup.useDefaultInputChannels
                && current.useDefaultOutputChannels == setup.useDefaultOutputChannels
                && current.inputChannels == expectedInputs
                && current.outputChannels == expectedOutputs
                && duplexMode.load(std::memory_order_relaxed))
            {
                fprintf(stderr, "[AudioEngine] Duplex device already configured with same settings, skipping\n");
                return {};
            }
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[AudioEngine] Current device channel check failed: %s\n", e.what());
        }
        catch (...)
        {
            fprintf(stderr, "[AudioEngine] Current device channel check failed (unknown)\n");
        }
    }

    // ALSA deadlocks on reconfigure unless we fully close first. WASAPI
    // reconfigures in place and is much slower if closed.
#if JUCE_LINUX
    juce::String currentTypeName;
    if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
        currentTypeName = currentType->getTypeName();
    if (inputDeviceManager.getCurrentAudioDevice() != nullptr)
    {
        try {
            inputDeviceManager.closeAudioDevice();
            fprintf(stderr, "[AudioEngine] Closed device for reconfiguration\n");
            if (currentTypeName.isNotEmpty())
                inputDeviceManager.setCurrentAudioDeviceType(currentTypeName, true);
        } catch (...) {
            fprintf(stderr, "[AudioEngine] closeAudioDevice crashed, continuing\n");
        }
    }
#endif

    int inputChannelCount = 0;
    int outputChannelCount = 0;
    if (auto* type = inputDeviceManager.getCurrentDeviceTypeObject())
    {
        try
        {
            if (auto probe = std::unique_ptr<juce::AudioIODevice>(type->createDevice(outputName, inputName)))
            {
                inputChannelCount = probe->getInputChannelNames().size();
                outputChannelCount = probe->getOutputChannelNames().size();
            }
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[AudioEngine] Channel probe failed: %s\n", e.what());
        }
        catch (...)
        {
            fprintf(stderr, "[AudioEngine] Channel probe failed (unknown)\n");
        }
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    if (outputChannelCount <= 0) outputChannelCount = 2;

    setup.inputChannels.setRange(0, inputChannelCount, true);
    setup.outputChannels.setRange(0, juce::jmin(outputChannelCount, 2), true);

    juce::String result;
    try {
        result = inputDeviceManager.setAudioDeviceSetup(setup, true);
    } catch (...) {
        return "setAudioDeviceSetup threw";
    }
    if (result.isNotEmpty())
    {
        fprintf(stderr, "[AudioEngine] Device setup error: %s\n", result.toRawUTF8());
        try {
            result = inputDeviceManager.initialiseWithDefaultDevices(2, 2);
        } catch (...) {
            return "fallback initialiseWithDefaultDevices threw";
        }
        if (result.isNotEmpty())
            return "device setup failed: " + result;
    }

    if (auto* configuredDevice = inputDeviceManager.getCurrentAudioDevice())
    {
        const double sr = configuredDevice->getCurrentSampleRate();
        const int bs = configuredDevice->getCurrentBufferSizeSamples();
        currentSampleRate.store(sr, std::memory_order_relaxed);
        inputBlockSize.store(bs, std::memory_order_relaxed);
        outputBlockSize.store(bs, std::memory_order_relaxed);

        fprintf(stderr, "[AudioEngine] Duplex device configured OK. Current device: %s\n",
                configuredDevice->getName().toRawUTF8());
        fprintf(stderr, "[AudioEngine] Actual device setup: sr=%.0f bs=%d (requested bs=%d)\n",
                sr, bs, bufferSize);

        source0().prepareMonitorChain(sr, bs);
        return {};
    }
    currentSampleRate.store(0.0, std::memory_order_relaxed);
    inputBlockSize.store(0, std::memory_order_relaxed);
    outputBlockSize.store(0, std::memory_order_relaxed);
    source0().releaseMonitorChain();
    return "no current device after setup";
}

AudioEngine::DeviceConfigResult AudioEngine::applySplitSetup(const DeviceConfig& config)
{
    DeviceConfigResult res;
    res.duplex = false;

    // The split-mode output ring is fixed at kOutputRingFrames samples
    // (~85ms @ 48kHz). A single callback at bufferSize > kOutputRingFrames
    // would overrun the ring in one go, guaranteeing immediate
    // overwrite/wrap and audible glitches. Reject those configurations up
    // front — duplex still works fine since it bypasses the ring entirely.
    if (config.bufferSize > kOutputRingFrames)
    {
        res.error = "Buffer size " + juce::String(config.bufferSize)
                  + " exceeds split-mode ring capacity ("
                  + juce::String(kOutputRingFrames) + "). Pick a smaller buffer size or use duplex.";
        return res;
    }

    // setCurrentAudioDeviceType can throw from JUCE backends (ASIO).
    // Catch so the failure surfaces as a structured error rather than an
    // exception crossing the N-API boundary.
    try
    {
        if (auto* current = outputDeviceManager.getCurrentDeviceTypeObject())
        {
            if (current->getTypeName() != config.outputType)
                outputDeviceManager.setCurrentAudioDeviceType(config.outputType, true);
        }
        else
        {
            outputDeviceManager.setCurrentAudioDeviceType(config.outputType, true);
        }
    }
    catch (...)
    {
        res.error = "setCurrentAudioDeviceType threw for output type '" + config.outputType + "'";
        return res;
    }

    // v1 forces matching nominal SR — no adaptive resampler yet.
    // Resolve empty name to first-enumerated for the createDevice probe
    // call (matches probeDeviceOptionsDual's strategy). createDevice("")
    // is implementation-defined per backend — some return the default,
    // some return null. Using first-enumerated keeps probe and apply
    // checking the SAME concrete device, so an empty-name config can't
    // pass the UI probe and then fail this check.
    auto rateSupportedBy = [&](juce::AudioIODeviceType* t,
                               const juce::String& dev, bool isInput, double sr) {
        if (!t) return false;
        juce::String resolved = dev;
        if (resolved.isEmpty())
        {
            auto names = t->getDeviceNames(isInput);
            if (names.size() > 0) resolved = names[0];
        }
        std::unique_ptr<juce::AudioIODevice> probe(
            isInput ? t->createDevice({}, resolved) : t->createDevice(resolved, {}));
        if (!probe) return false;
        // Tolerance matches the probe-side rounding: probeDeviceOptionsDual
        // rounds the matched rate to the nearest integer (see :208), so a
        // backend reporting e.g. 47999.5 surfaces 48000 in the UI. If we
        // kept `< 0.5` here, the round-trip would fail at apply time because
        // |47999.5 - 48000.0| is exactly 0.5. Use `<= 0.5` so the boundary
        // case the probe accepted is also accepted at apply.
        for (auto r : probe->getAvailableSampleRates())
            if (std::abs(r - sr) <= 0.5) return true;
        return false;
    };
    juce::AudioIODeviceType* inputType = nullptr;
    juce::AudioIODeviceType* outputType = nullptr;
    for (auto* t : inputDeviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == config.inputType) { inputType = t; break; }
    for (auto* t : outputDeviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == config.outputType) { outputType = t; break; }
    if (!inputType || !outputType)
    {
        res.error = "Device type not found";
        return res;
    }
    if (!rateSupportedBy(inputType, config.inputDevice, true, config.sampleRate)
     || !rateSupportedBy(outputType, config.outputDevice, false, config.sampleRate))
    {
        res.error = "Sample rate not supported by both input and output devices";
        return res;
    }

    juce::AudioDeviceManager::AudioDeviceSetup inSetup;
    // Resolve empty name to first-enumerated input device — matches the
    // rateSupportedBy preflight above AND probeDeviceOptionsDual. Using
    // empty + useDefault*Channels here would make JUCE open the OS
    // default, which can differ from inputs[0] on platforms where the
    // OS-default differs from JUCE's enumeration order. The probe + SR
    // preflight + actual open all need to agree on the same concrete
    // device for the apply path to behave consistently with what the UI
    // showed the user.
    juce::String resolvedInputName = config.inputDevice;
    if (resolvedInputName.isEmpty())
    {
        auto names = inputType->getDeviceNames(true);
        if (names.size() > 0) resolvedInputName = names[0];
    }

    inSetup.inputDeviceName  = resolvedInputName;
    inSetup.outputDeviceName = "";
    inSetup.sampleRate = config.sampleRate;
    inSetup.bufferSize = config.bufferSize;
    inSetup.useDefaultInputChannels = false;
    inSetup.useDefaultOutputChannels = false;

    int inputChannelCount = 0;
    {
        try {
            std::unique_ptr<juce::AudioIODevice> probe(inputType->createDevice({}, resolvedInputName));
            if (probe) inputChannelCount = probe->getInputChannelNames().size();
        } catch (...) {}
    }
    if (inputChannelCount <= 0) inputChannelCount = 2;
    inSetup.inputChannels.setRange(0, inputChannelCount, true);
    inSetup.outputChannels.clear();

    // Rollback helper: on any failure path after a side has been opened,
    // close both managers' devices so we don't leave the OS audio resource
    // held (sometimes exclusively, e.g. ASIO) while setDevice reports a
    // failure. closeAudioDevice is idempotent so unconditional calls are
    // safe even when only the input or neither side opened.
    auto rollbackOpenedDevices = [&]() {
        // Drop any callback we already attached to the output manager —
        // closeAudioDevice() does not invoke removeAudioCallback, and leaving
        // outputCallbackRegistered=true would cause the next startAudio()
        // to skip the re-attach (it gates on !outputCallbackRegistered),
        // leaving split-mode output silent after a partial-open failure.
        if (outputCallbackRegistered)
        {
            try { outputDeviceManager.removeAudioCallback(&outputCallback); } catch (...) {}
            outputCallbackRegistered = false;
        }
        try { inputDeviceManager.closeAudioDevice(); } catch (...) {}
        try { outputDeviceManager.closeAudioDevice(); } catch (...) {}
    };

    // Mirror applyDuplexSetup's JUCE_LINUX close-before-reconfigure pattern:
    // ALSA deadlocks if we let setAudioDeviceSetup mutate a live device. The
    // device type is re-asserted afterwards so the close doesn't drop us back
    // to whatever JUCE picked at startup. closeAudioDevice/setCurrentAudioDeviceType
    // throwing is non-fatal — we still try the setup below and surface its error.
#if JUCE_LINUX
    {
        juce::String currentInputTypeName;
        if (auto* currentType = inputDeviceManager.getCurrentDeviceTypeObject())
            currentInputTypeName = currentType->getTypeName();
        if (inputDeviceManager.getCurrentAudioDevice() != nullptr)
        {
            try {
                inputDeviceManager.closeAudioDevice();
                if (currentInputTypeName.isNotEmpty())
                    inputDeviceManager.setCurrentAudioDeviceType(currentInputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode input close threw, continuing\n");
            }
        }
    }
#endif

    juce::String inErr;
    try { inErr = inputDeviceManager.setAudioDeviceSetup(inSetup, true); }
    catch (...) { res.error = "input setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (inErr.isNotEmpty()) { res.error = "input setup: " + inErr; rollbackOpenedDevices(); return res; }

    auto* inDev = inputDeviceManager.getCurrentAudioDevice();
    if (!inDev) { res.error = "no input device after setup"; rollbackOpenedDevices(); return res; }
    const double inSr = inDev->getCurrentSampleRate();
    const int    inBs = inDev->getCurrentBufferSizeSamples();

    // Same first-enumerated resolution on the output side — see input note
    // above for why this matches the probe + SR preflight strategy.
    juce::String resolvedOutputName = config.outputDevice;
    if (resolvedOutputName.isEmpty())
    {
        auto names = outputType->getDeviceNames(false);
        if (names.size() > 0) resolvedOutputName = names[0];
    }

    juce::AudioDeviceManager::AudioDeviceSetup outSetup;
    outSetup.inputDeviceName  = "";
    outSetup.outputDeviceName = resolvedOutputName;
    outSetup.sampleRate = config.sampleRate;
    outSetup.bufferSize = config.bufferSize;
    outSetup.useDefaultInputChannels = false;
    outSetup.useDefaultOutputChannels = false;

    int outputChannelCount = 0;
    {
        try {
            std::unique_ptr<juce::AudioIODevice> probe(outputType->createDevice(resolvedOutputName, {}));
            if (probe) outputChannelCount = probe->getOutputChannelNames().size();
        } catch (...) {}
    }
    if (outputChannelCount <= 0) outputChannelCount = 2;
    outSetup.inputChannels.clear();
    outSetup.outputChannels.setRange(0, juce::jmin(outputChannelCount, 2), true);

    // Same JUCE_LINUX close-before-reconfigure as the input side above — also
    // protects when split mode is re-applied with a different output device.
#if JUCE_LINUX
    {
        juce::String currentOutputTypeName;
        if (auto* currentType = outputDeviceManager.getCurrentDeviceTypeObject())
            currentOutputTypeName = currentType->getTypeName();
        if (outputDeviceManager.getCurrentAudioDevice() != nullptr)
        {
            try {
                outputDeviceManager.closeAudioDevice();
                if (currentOutputTypeName.isNotEmpty())
                    outputDeviceManager.setCurrentAudioDeviceType(currentOutputTypeName, true);
            } catch (...) {
                fprintf(stderr, "[AudioEngine] split-mode output close threw, continuing\n");
            }
        }
    }
#endif

    juce::String outErr;
    try { outErr = outputDeviceManager.setAudioDeviceSetup(outSetup, true); }
    catch (...) { res.error = "output setAudioDeviceSetup threw"; rollbackOpenedDevices(); return res; }
    if (outErr.isNotEmpty()) { res.error = "output setup: " + outErr; rollbackOpenedDevices(); return res; }

    auto* outDev = outputDeviceManager.getCurrentAudioDevice();
    if (!outDev) { res.error = "no output device after setup"; rollbackOpenedDevices(); return res; }
    const double outSr = outDev->getCurrentSampleRate();
    const int    outBs = outDev->getCurrentBufferSizeSamples();

    if (std::abs(inSr - outSr) > 0.5)
    {
        res.error = "Input and output devices opened at different sample rates";
        rollbackOpenedDevices();
        return res;
    }

    currentSampleRate.store(inSr, std::memory_order_relaxed);
    inputBlockSize.store(inBs, std::memory_order_relaxed);
    outputBlockSize.store(outBs, std::memory_order_relaxed);

    fprintf(stderr, "[AudioEngine] Split mode configured: inSr=%.0f inBs=%d outSr=%.0f outBs=%d\n",
            inSr, inBs, outSr, outBs);

    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    outputUnderflowCount.store(0, std::memory_order_relaxed);
    inputOverflowCount.store(0, std::memory_order_relaxed);
    for (auto& slot : outputPendingRing)
        slot.store(0u, std::memory_order_relaxed);

    source0().prepareMonitorChain(inSr, inBs);

    res.ok = true;
    res.sampleRate = inSr;
    res.inputBlockSize = inBs;
    res.outputBlockSize = outBs;
    return res;
}

void AudioEngine::teardownSplitMode()
{
    // Unconditional remove — JUCE's removeAudioCallback is idempotent
    // (no-op if the callback isn't registered), so we don't need the
    // outputCallbackRegistered guard here. This makes teardown robust
    // against a stale flag left over from a previous failed split setup.
    outputDeviceManager.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
    try { outputDeviceManager.closeAudioDevice(); }
    catch (...) { fprintf(stderr, "[AudioEngine] teardownSplitMode: output close threw\n"); }

    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);
    for (auto& slot : outputPendingRing)
        slot.store(0u, std::memory_order_relaxed);
}

// ── Audio Control ─────────────────────────────────────────────────────────────

void AudioEngine::startAudio()
{
    if (audioRunning.load(std::memory_order_relaxed))
    {
        fprintf(stderr, "[AudioEngine] startAudio: already running\n");
        return;
    }

    // Input first so it has time to prefill the ring before the output
    // callback pulls — otherwise split mode underflows once at start.
    inputDeviceManager.addAudioCallback(this);

    // Guard against double-registration: audioRunning can be cleared by
    // audioDeviceStopped() on a transient input unplug while the output
    // callback intentionally stays registered (JUCE auto-restart relies on
    // that). A later startAudio() would then add the same callback again
    // and JUCE would dispatch it twice per block.
    if (!duplexMode.load(std::memory_order_relaxed) && !outputCallbackRegistered)
    {
        outputDeviceManager.addAudioCallback(&outputCallback);
        outputCallbackRegistered = true;
    }

    audioRunning.store(true, std::memory_order_relaxed);
    fprintf(stderr, "[AudioEngine] startAudio: duplex=%d input='%s' output='%s'\n",
            (int) duplexMode.load(std::memory_order_relaxed),
            inputDeviceManager.getCurrentAudioDevice()
                ? inputDeviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "none",
            outputDeviceManager.getCurrentAudioDevice()
                ? outputDeviceManager.getCurrentAudioDevice()->getName().toRawUTF8() : "(duplex)");
}

void AudioEngine::stopAudio()
{
    // Always attempt to detach both callbacks — removeAudioCallback is
    // idempotent. We don't gate on audioRunning here because that flag can
    // be cleared externally by audioDeviceStopped() (input device
    // hot-unplug); in split mode the output callback may still be registered
    // even after the input side has reported itself stopped, and a guarded
    // stopAudio() would no-op while leaving the output device producing.
    // Output first so it doesn't pull from a stalling ring during detach.
    outputDeviceManager.removeAudioCallback(&outputCallback);
    outputCallbackRegistered = false;
    inputDeviceManager.removeAudioCallback(this);
    audioRunning.store(false, std::memory_order_relaxed);
}

// ── Backing Track ─────────────────────────────────────────────────────────────

bool AudioEngine::loadBackingTrack(const juce::File& file)
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
    backingTransport.reset();
    backingSource.reset();

    const bool exists = file.existsAsFile();
    std::cerr << "[AudioEngine] loadBackingTrack path="
              << file.getFullPathName().toStdString()
              << " exists=" << exists
              << " size=" << (exists ? (long long)file.getSize() : -1)
              << std::endl;

    auto* reader = formatManager.createReaderFor(file);
    if (!reader)
    {
        std::cerr << "[AudioEngine] loadBackingTrack: no reader for ext='"
                  << file.getFileExtension().toStdString()
                  << "' (registered formats=" << formatManager.getNumKnownFormats()
                  << ")" << std::endl;
        // Transport/source already reset above; clear cached state so the renderer
        // doesn't keep displaying the previous track's position/duration.
        cachedBackingPosition.store(0.0);
        cachedBackingDuration.store(0.0);
        return false;
    }

    const double readerSampleRate = reader->sampleRate;
    const juce::int64 readerLengthInSamples = reader->lengthInSamples;
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    // Backing audio plays through the output device in both modes, so size
    // against outputBlockSize. In duplex mode outputBlockSize == inputBlockSize;
    // in split mode the output device's clock drives the backing pull.
    const int    bs = outputBlockSize.load(std::memory_order_relaxed);

    backingSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
    backingTransport = std::make_unique<juce::AudioTransportSource>();
    // The 4th arg makes AudioTransportSource SRC the file to device rate.
    // Stretch always sees device-rate audio so that its presetDefault parameters match.
    backingTransport->setSource(backingSource.get(), 0, nullptr, readerSampleRate);

    // Loading a backing track before the audio device has started leaves
    // sr/bs at zero. presetDefault(2, 0.0f) would seed the stretcher with
    // undefined internal timing, and prepareToPlay(0, 0) is similarly
    // ill-defined. Defer the stretcher + buffer setup; the relevant
    // audio*AboutToStart() re-runs the same block once a real sample
    // rate / block size are known (audioDeviceAboutToStart for duplex,
    // audioOutputAboutToStart for split).
    if (sr > 0.0 && bs > 0)
    {
        // prepareToPlay's first arg is an upper bound on subsequent
        // getNextAudioBlock requests, per the juce::AudioSource contract.
        // The RT callback can pull ceil(bs * kMaxBackingSpeed) frames in a
        // single block when the speed is above 1×, so prepare for that
        // worst case — preparing with just `bs` would risk JUCE internal
        // buffer overruns/asserts on the first faster-than-1× block.
        const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
        backingTransport->prepareToPlay(maxInputFrames, sr);

        backingStretch.presetDefault(2, (float) sr);
        backingStretch.reset();
        backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);

        backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
        backingBuffer.setSize(2, bs, false, false, true);
    }

    cachedBackingDuration.store(backingTransport->getLengthInSeconds());
    cachedBackingPosition.store(0.0);
    backingHeardPositionSec.store(0.0, std::memory_order_relaxed);
    std::cerr << "[AudioEngine] loadBackingTrack OK sr=" << readerSampleRate
              << " len=" << readerLengthInSamples
              << std::endl;
    return true;
}

void AudioEngine::setBackingPosition(double seconds)
{
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingTransport->setPosition(seconds);
        backingStretch.reset();
        // Read back the actual position; the transport may clamp (e.g. negative or past EOF).
        const double pos = backingTransport->getCurrentPosition();
        cachedBackingPosition.store(pos);
        backingHeardPositionSec.store(pos, std::memory_order_relaxed);
    }
}

void AudioEngine::startBacking()
{
    const juce::ScopedLock sl(backingLock);
    if (backingTransport)
    {
        backingTransport->start();
        backingPlaying.store(true);
        backingHeardPositionSec.store(backingTransport->getCurrentPosition(),
                                      std::memory_order_relaxed);
    }
}

void AudioEngine::stopBackingNoLock()
{
    if (backingTransport)
    {
        backingTransport->stop();
        backingStretch.reset();
        backingPlaying.store(false);
    }
}

void AudioEngine::stopBacking()
{
    const juce::ScopedLock sl(backingLock);
    stopBackingNoLock();
}

void AudioEngine::setBackingSpeed(double speed)
{
    if (!std::isfinite(speed) || speed <= 0.0)
    {
        return;
    }

    const double clamped = juce::jlimit(0.01, kMaxBackingSpeed, speed);
    // Dead-zone against the last *requested* rate to coalesce rapid slider
    // ticks — but never skip a change that crosses the 1× bypass boundary, or a
    // request just shy of 1× (e.g. 0.9995 -> 1.0, diff < 0.001) would leave the
    // stretcher path engaged when the caller actually asked for transparent
    // full speed.
    const double prev = backingPendingSpeed.load(std::memory_order_relaxed);
    const bool prevBypass = std::abs(prev    - 1.0) < kBackingSpeedBypassEpsilon;
    const bool newBypass  = std::abs(clamped - 1.0) < kBackingSpeedBypassEpsilon;
    if (std::abs(clamped - prev) < 0.001 && prevBypass == newBypass)
    {
        return;
    }

    // Lock-free hand-off to the audio thread. Publish the requested rate, then
    // raise the pending flag with release so the RT thread is guaranteed to see
    // the new rate once it observes the flag. renderBackingBlockLocked() adopts
    // the rate and resets the stretcher together, on the audio thread, so:
    //   * a control-thread caller (e.g. a speed slider at 30-60 Hz) never takes
    //     backingLock and so never starves the RT tryLock into dropping a block;
    //   * the new rate is never processed with stale stretch state — the reset
    //     and the rate adoption happen in the same RT block (see PR #237).
    // Multiple updates before the RT consumes them coalesce (latest wins), which
    // naturally throttles stretcher resets during a drag.
    backingPendingSpeed.store(clamped, std::memory_order_relaxed);
    backingSpeedChangePending.store(true, std::memory_order_release);
}

void AudioEngine::resetPeaks()
{
    source0().resetInputPeak();  // input peak is per-source
    outputPeak.store(0.0f);
}

// ── Multi-input source management (control thread) ───────────────────────────

int AudioEngine::addSource(int inputChannel)
{
    std::lock_guard<std::mutex> lock(sourcesMutex);
    reclaimPendingReleases();  // free up any slot whose release was deferred

    // Find a free pooled slot (slot 0 is the permanent default). Skip a slot whose
    // release is still pending — its chain/worker hasn't been torn down yet, so
    // re-preparing it would double-start the verifier thread.
    int slot = -1;
    for (int i = 1; i < kMaxSources; ++i)
        if (! sources[(size_t) i]->isActive() && ! pendingRelease[(size_t) i]) { slot = i; break; }
    if (slot < 0)
        return -1;  // pool full

    SourceChain& src = *sources[(size_t) slot];
    src.setInputChannel(inputChannel);

    // Prepare fully BEFORE making it visible to the audio thread, so the first
    // callback that observes active==true sees a ready chain + rings. When audio
    // isn't running yet, audioDeviceAboutToStart prepares it later.
    if (audioRunning.load(std::memory_order_relaxed))
    {
        const double sr = currentSampleRate.load(std::memory_order_relaxed);
        const int bs = inputBlockSize.load(std::memory_order_relaxed);
        if (sr > 0.0 && bs > 0)
            src.prepare(sr, bs);
    }
    src.setActive(true);  // release-store: now picked up by the audio callback
    return slot;
}

bool AudioEngine::removeSource(int id)
{
    if (id <= 0 || id >= kMaxSources)
        return false;  // 0 is permanent; out-of-range rejected

    std::lock_guard<std::mutex> lock(sourcesMutex);
    reclaimPendingReleases();  // opportunistically reclaim earlier deferrals

    SourceChain& src = *sources[(size_t) id];
    if (! src.isActive())
        return false;

    // Hide it from the audio callback first; subsequent blocks snapshot active
    // once and skip it. It is logically removed from here on, regardless of when
    // its resources are reclaimed.
    src.setActive(false);

    // Reclaim now if we can confirm no callback body is executing. callbackInFlight
    // is set false BY the input callback at its real exit (release store), so
    // observing false (acquire) proves no callback is inside processBlock right
    // now; any callback that starts afterwards snapshots active and skips this
    // (now-inactive) source — so releasing it cannot race the audio thread. This
    // holds whether audio is running, idle between blocks, or stopped (the last
    // callback left it false). Bounded so a wedged device can't hang this thread.
    for (int spins = 0; spins < 200; ++spins)  // ~200 ms cap
    {
        if (! callbackInFlight.load(std::memory_order_acquire))
        {
            src.releaseResources();  // stops its threads + releases its chain
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // A callback stayed wedged in-flight past the wait (a >200 ms block would be a
    // catastrophic stall). Do NOT force a release that could race it — DEFER it.
    // The source is already inactive so no future callback touches it; reclaim it
    // later (next add/removeSource, or audioDeviceStopped) when the body is quiet.
    pendingRelease[(size_t) id] = true;
    return true;
}

void AudioEngine::reclaimPendingReleases()
{
    // Caller holds sourcesMutex. A deferred source is inactive (future callbacks
    // skip it); releasing it is safe once no callback body is executing. We key
    // strictly on callbackInFlight (set false by the callback at its real exit),
    // which proves the body is not in processBlock — independent of audioRunning.
    // On the audioDeviceStopped path the last callback already left it false.
    if (callbackInFlight.load(std::memory_order_acquire))
        return;  // a callback is mid-body — try again later
    for (int i = 1; i < kMaxSources; ++i)
    {
        if (! pendingRelease[(size_t) i]) continue;
        sources[(size_t) i]->releaseResources();
        pendingRelease[(size_t) i] = false;
    }
}

// Lifetime/threading of getSource() + the NodeAddon *Source* methods:
//  - getSource(), add/removeSource(), and the source-indexed methods all run on
//    the single N-API/JS thread (V8-serialised), so a source can't be removed out
//    from under a getSource() caller on that thread.
//  - getSource() returns a pointer into the FIXED pool. releaseResources() (from
//    removeSource or audioDeviceStopped) only stops the chain's threads + resets
//    its rings — it NEVER frees the SourceChain object — so the pointer never
//    dangles for the life of the engine (no use-after-free).
//  - The only cross-thread overlap is a source-indexed method (N-API) running
//    concurrently with audioDeviceStopped's releaseResources() (device thread).
//    That window is IDENTICAL to the pre-existing legacy methods (scoreChord /
//    getNoteVerdicts / getRawAudioFrame, which all operate on source 0's same
//    chain) and is handled the same way: each component's internals are atomic /
//    individually thread-safe, and the rings are lock-free. This change adds no
//    new race class versus the original single-source engine.
SourceChain* AudioEngine::getSource(int id)
{
    if (id < 0 || id >= kMaxSources)
        return nullptr;
    SourceChain& src = *sources[(size_t) id];
    return (id == 0 || src.isActive()) ? &src : nullptr;
}

std::vector<AudioEngine::SourceInfo> AudioEngine::listSources() const
{
    std::vector<SourceInfo> out;
    for (int i = 0; i < kMaxSources; ++i)
    {
        const SourceChain& src = *sources[(size_t) i];
        if (! src.isActive()) continue;
        SourceInfo info;
        info.id = src.getId();
        info.inputChannel = src.getInputChannel();
        info.active = true;
        out.push_back(info);
    }
    return out;
}

int AudioEngine::renderBackingBlockLocked(int numSamples)
{
    // Adopt any speed change requested since the last block (set lock-free by
    // setBackingSpeed). Common (no-change) path is a plain acquire load — no
    // locked RMW, so the flag's cache line stays shared and isn't bounced to
    // this core every callback. Only the rare block that actually consumes a
    // change does the exchange (clearing the flag atomically so a concurrent
    // setBackingSpeed can't lose an update). The acquire pairs with the
    // release-store in setBackingSpeed so the new rate is visible here. Reset
    // the stretcher and re-anchor the heard position in the SAME block we adopt
    // the rate, so a block is never processed at the new rate with stale stretch
    // state. reset() only clears state (no allocation), so it's audio-thread safe.
    if (backingSpeedChangePending.load(std::memory_order_acquire))
    {
        backingSpeedChangePending.exchange(false, std::memory_order_acquire);
        backingSpeed.store(juce::jlimit(0.01, kMaxBackingSpeed,
                                        backingPendingSpeed.load(std::memory_order_relaxed)),
                           std::memory_order_relaxed);
        backingStretch.reset();
        backingHeardPositionSec.store(backingTransport->getCurrentPosition(),
                                      std::memory_order_relaxed);
    }

    const double rate = juce::jlimit(0.01, kMaxBackingSpeed, backingSpeed.load(std::memory_order_relaxed));

    // Defensive clamp: the buffers are sized in audioDeviceAboutToStart() /
    // audioOutputAboutToStart() from the device's nominal block size, but a
    // callback can deliver a larger numSamples on a device-reconfig race. Drop
    // the excess frames silently rather than reading/writing past the allocated
    // span; the next callback after reconfig arrives at the new nominal size.
    const int outCap = backingBuffer.getNumSamples();
    const int inCap  = backingInputBuffer.getNumSamples();
    const int outSamples = juce::jmin(numSamples, outCap);
    const double sr = currentSampleRate.load(std::memory_order_relaxed);
    const bool bypassStretch = std::abs(rate - 1.0) < kBackingSpeedBypassEpsilon;

    int sourceFramesPulled = 0;

    if (bypassStretch)
    {
        // 1× — direct transport read, no phase-vocoder path. (The transport
        // still sample-rate-converts the file to the device rate, so this is
        // "no time-stretch", not necessarily bit-perfect.)
        backingBuffer.clear(0, outSamples);
        juce::AudioSourceChannelInfo info(&backingBuffer, 0, outSamples);
        backingTransport->getNextAudioBlock(info);
        sourceFramesPulled = outSamples;
    }
    else
    {
        // Slow/fast path — pull only the source frames needed for this output
        // block (output * rate), then stretch in-process to fill outSamples.
        const int inputFrames = juce::jmin((int) std::ceil(outSamples * rate), inCap);

        backingInputBuffer.clear(0, inputFrames);
        juce::AudioSourceChannelInfo info(&backingInputBuffer, 0, inputFrames);
        backingTransport->getNextAudioBlock(info);
        sourceFramesPulled = inputFrames;

        backingBuffer.clear(0, outSamples);

        const float* const* inPtrs  = backingInputBuffer.getArrayOfReadPointers();
        float* const* outPtrs = backingBuffer.getArrayOfWritePointers();
        backingStretch.process(inPtrs, inputFrames, outPtrs, outSamples);
    }

    const double transportPos = backingTransport->getCurrentPosition();
    if (sr > 0.0 && sourceFramesPulled > 0)
    {
        // Accumulate the heard (source) position, but clamp to the transport's
        // actual position. sourceFramesPulled is the requested block size; a
        // short read (e.g. at EOF, where the transport returns fewer real frames
        // and zero-pads) would otherwise advance the playhead past the true
        // source point and report progress beyond the track duration before
        // backingPlaying flips false. getCurrentPosition() stays clamped to the
        // real source position.
        double heard = backingHeardPositionSec.load(std::memory_order_relaxed)
                       + static_cast<double>(sourceFramesPulled) / sr;
        heard = juce::jmin(heard, transportPos);
        backingHeardPositionSec.store(heard, std::memory_order_relaxed);

        // Bypass reads straight from the transport — no phase-vocoder output
        // latency to compensate for. Only the stretch path adds latency.
        const double latencyInputSec = bypassStretch
            ? 0.0
            : (backingStretchLatencySamples.load(std::memory_order_relaxed) * rate) / sr;
        cachedBackingPosition.store(juce::jmax(0.0, heard - latencyInputSec));
    }
    else
    {
        // currentSampleRate is transiently 0 during device teardown/reconfig.
        // We can't accumulate (no Hz to divide by), so anchor both the heard
        // accumulator and the published playhead to the real transport position
        // rather than leaving a stale value visible to the UI.
        backingHeardPositionSec.store(transportPos, std::memory_order_relaxed);
        cachedBackingPosition.store(juce::jmax(0.0, transportPos));
    }

    // Sync the flag if transport stopped at EOF.
    if (!backingTransport->isPlaying())
        backingPlaying.store(false);

    return outSamples;
}

// setNoiseGate / setTonePolishEnabled are now inline forwarders to sources[0]
// (see AudioEngine.h).

// ── Audio Callback ────────────────────────────────────────────────────────────

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    // Fires on the input manager — duplex serves output here too; split has
    // audioOutputAboutToStart on the second manager.
    //
    // Also fires when JUCE auto-restarts the input after a transient stop
    // (hot-replug, OS resume). audioDeviceStopped() cleared audioRunning;
    // restore it here so scoreChord() / getActiveDetection() come back
    // online without requiring a manual stopAudio()/startAudio() cycle,
    // and so a subsequent setAudioDevices() sees the engine as running
    // and runs its defensive stopAudio() before mutating device state.
    audioRunning.store(true, std::memory_order_relaxed);
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    currentSampleRate.store(sr, std::memory_order_relaxed);
    inputBlockSize.store(bs, std::memory_order_relaxed);
    if (duplexMode.load(std::memory_order_relaxed))
        outputBlockSize.store(bs, std::memory_order_relaxed);

    // Input callback uses outputBackingBuffer as DSP scratch in split mode.
    // Pre-size against input block size so the hot path can't allocate when
    // inputBlockSize > outputBlockSize (audioOutputAboutToStart only sizes
    // against output).
    if (!duplexMode.load(std::memory_order_relaxed)
        && outputBackingBuffer.getNumSamples() < bs)
    {
        outputBackingBuffer.setSize(2, bs, false, false, true);
    }

    // Pre-size the multi-source mix scratch (2ch) so the audio thread never
    // allocates when summing active sources.
    if (sourceMonitorScratch.getNumSamples() < bs)
        sourceMonitorScratch.setSize(2, bs, false, false, true);

    // Prepare each ACTIVE source's DSP and reset its rings + zero-output scratch
    // for a clean cold start. Inactive pooled chains stay unprepared (no threads).
    for (auto& src : sources)
        if (src->isActive())
            src->prepare(sr, bs);

    // Split mode preps the backing stretcher in audioOutputAboutToStart
    // instead — that callback owns the device the backing audio actually
    // plays on, and pulls from backingTransport at the output device's
    // block size.
    if (duplexMode.load(std::memory_order_relaxed))
    {
        const juce::ScopedLock sl(backingLock);
        if (backingTransport)
        {
            // See loadBackingTrack() for why prepareToPlay uses maxInputFrames
            // rather than bs: the RT callback can pull ceil(bs * kMaxBackingSpeed)
            // frames in a single block at faster-than-1× speeds.
            const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
            backingTransport->prepareToPlay(maxInputFrames, sr);
            backingStretch.presetDefault(2, (float) sr);
            backingStretch.reset();
            backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);
            backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
            backingBuffer.setSize(2, bs, false, false, true);
        }
    }
}

void AudioEngine::audioDeviceStopped()
{
    // JUCE calls audioDeviceStopped() only AFTER the input device has stopped
    // invoking the IO callback (stop() blocks for the callback thread to finish),
    // so the body is guaranteed quiescent here: callbackInFlight is already false
    // and no callback can touch a source while we release it below.
    audioRunning.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        // Release each ACTIVE source's chain + detectors + verifier and zero its
        // rings. Inactive pooled chains were never prepared.
        for (auto& src : sources)
            if (src->isActive())
                src->releaseResources();
        // Reclaim any source whose removal was deferred (handshake timed out) —
        // audioRunning is now false so reclaimPendingReleases() frees them all.
        reclaimPendingReleases();
    }
    outputRingWriteIndex.store(0, std::memory_order_relaxed);
    outputRingReadIndex.store(0, std::memory_order_relaxed);

    // Note on split-mode lifecycle: we deliberately do NOT detach the
    // output callback here. JUCE auto-restarts a transiently-stopped input
    // device by re-firing audioDeviceAboutToStart() on its own; that path
    // doesn't re-add the output callback, so detaching would break
    // automatic recovery (output stays silent until a manual reconfigure).
    // While input is down, the guitar/DSP side of the output goes silent
    // (no producer feeding outputPendingRing, so the consumer's underflow
    // branch zero-fills), but the backing track keeps playing — the output
    // callback mixes backingTransport independently of ring state. That's
    // intentional UX: a user unplugging their interface mid-song doesn't
    // lose their place. Real teardown belongs to stopAudio() / teardownSplitMode().
}

void AudioEngine::audioOutputAboutToStart(juce::AudioIODevice* device)
{
    const int bs = device->getCurrentBufferSizeSamples();
    outputBlockSize.store(bs, std::memory_order_relaxed);

    if ((int) outputPullScratchL.size() < bs) outputPullScratchL.assign((size_t) bs, 0.0f);
    if ((int) outputPullScratchR.size() < bs) outputPullScratchR.assign((size_t) bs, 0.0f);
    // NOTE: outputBackingBuffer is sized by audioDeviceAboutToStart() from the
    // INPUT device's block size — it's the split-input DSP scratch, not an
    // output-side buffer. Don't touch it here: resizing from the output
    // thread races with the input callback and can shrink the scratch below
    // its expected size. The output side uses backingBuffer / backingInputBuffer,
    // sized below.

    // Prefer the output device's reported rate as authoritative. The
    // stored currentSampleRate (set from the input side) was the right
    // fallback during initial setup, but if the OS or driver reopened
    // the output device at a different rate (format change, sleep/resume,
    // user-changed default), trusting the stored value would seed the
    // stretcher with a mismatched rate. Compare and warn so the
    // discrepancy is visible in logs; the device-reported rate wins.
    const double srStored = currentSampleRate.load(std::memory_order_relaxed);
    const double srDev    = device->getCurrentSampleRate();
    const double sr = srDev > 0.0 ? srDev : srStored;
    if (srStored > 0.0 && srDev > 0.0 && std::abs(srStored - srDev) > 0.5)
    {
        fprintf(stderr, "[AudioEngine] audioOutputAboutToStart: stored sr=%.0f differs from device sr=%.0f — using device\n",
                srStored, srDev);
    }
    // Propagate the authoritative rate to currentSampleRate so downstream
    // consumers (audioOutputCallback's latency comp, getLatencyMs(),
    // scoreChord's fallback) all see the same value. Without this, a
    // device-side rate change (sleep/resume, format change) would leave
    // currentSampleRate stuck at the input-side seed value.
    if (sr > 0.0) currentSampleRate.store(sr, std::memory_order_relaxed);
    {
        const juce::ScopedLock sl(backingLock);
        if (backingTransport && sr > 0.0 && bs > 0)
        {
            // Mirror loadBackingTrack() / audioDeviceAboutToStart() — the
            // output device drives backing playback in split mode, so this
            // is where the stretcher gets sized for that side. prepareToPlay
            // upper-bounds future getNextAudioBlock requests, and the
            // RT callback can pull ceil(bs * kMaxBackingSpeed) at faster
            // speeds.
            const int maxInputFrames = (int) std::ceil(bs * kMaxBackingSpeed) + 64;
            backingTransport->prepareToPlay(maxInputFrames, sr);
            backingStretch.presetDefault(2, (float) sr);
            backingStretch.reset();
            backingStretchLatencySamples.store(backingStretch.outputLatency(), std::memory_order_relaxed);
            backingInputBuffer.setSize(2, maxInputFrames, false, false, true);
            backingBuffer.setSize(2, bs, false, false, true);
        }
    }
}

void AudioEngine::audioOutputStopped()
{
    // No-op by design. The consumer's catch-up branch in audioOutputCallback
    // handles both (w - r) > cap (producer lapped during the stop) and
    // w < r (a future reset race) on the next output start, so we don't
    // need to reset readIndex here. Resetting r to 0 while the producer
    // keeps advancing w is equivalent — both end up in the catch-up branch
    // — but it leaves a transient window where r appears reset without the
    // ring invariants being re-established, which is harder to reason about.
}

void AudioEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputData, int numInputChannels,
    float* const* outputData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Publish that the callback body is executing so removeSource() and deferred-
    // release reclamation know when no source is being processed (the body is
    // quiescent) and a removed source can be safely released.
    callbackInFlight.store(true, std::memory_order_release);

    const bool duplex = duplexMode.load(std::memory_order_relaxed);

    // Duplex writes outputData directly. Split runs DSP into a private 2-channel
    // scratch and pushes the result to outputPendingRing for OutputCallback.
    juce::AudioBuffer<float> buffer;
    if (duplex)
    {
        buffer.setDataToReferTo(outputData, numOutputChannels, numSamples);
    }
    else
    {
        // outputBackingBuffer is pre-sized to inputBlockSize in
        // audioDeviceAboutToStart(); never realloc on the RT thread. A
        // reconfig race could transiently deliver a larger numSamples —
        // clamp it (drop the tail) so the rest of the callback operates
        // strictly within the allocated scratch.
        const int scratchCap = outputBackingBuffer.getNumSamples();
        if (numSamples > scratchCap)
            numSamples = scratchCap;
        outputBackingBuffer.clear(0, 0, numSamples);
        outputBackingBuffer.clear(1, 0, numSamples);
        buffer.setDataToReferTo(outputBackingBuffer.getArrayOfWritePointers(), 2, numSamples);
    }

    const int effectiveOutputChannels = duplex ? numOutputChannels : 2;

    // Per-source capture + detect + monitor.
    //
    // Fast path — exactly one active source: process it in place on `buffer`,
    // byte-identical to the single-pipeline engine (channel select / mono mix +
    // input gain, input metering, ML + input-ring feed, noise gate, YIN + raw-ring
    // feed, tone chain + NaN scrub, monitor mute, chain gain, tone polish).
    //
    // Multi-source path: each active source renders its own 2-channel monitor into
    // sourceMonitorScratch (each builds its mono from its bound input channel +
    // feeds its own rings/detectors/verifier), and the monitors are summed into the
    // output. Each source scores its OWN arrangement independently.
    // Snapshot each source's active flag ONCE so the count and the process/mix
    // passes below are consistent within this block — a concurrent add/
    // removeSource flipping a flag between two reads must not change which branch
    // runs mid-callback. removeSource waits for this callback to drain before
    // releasing a source, so a source snapshotted active here is safe to process
    // even if it is deactivated an instant later.
    bool act[kMaxSources];
    int firstActive = -1, activeCount = 0;
    for (int i = 0; i < kMaxSources; ++i)
    {
        act[i] = sources[(size_t) i]->isActive();
        if (act[i]) { ++activeCount; if (firstActive < 0) firstActive = i; }
    }

    if (activeCount <= 1)
    {
        // Single active source: in-place on `buffer` — identical to the original
        // single-pipeline engine for the steady single-input case (the only state
        // the legacy renderer ever reaches). Output channel count is whatever the
        // device exposes (broadcast/pass-through handled inside processBlock).
        sources[(size_t) (firstActive < 0 ? 0 : firstActive)]
            ->processBlock(inputData, numInputChannels, buffer, effectiveOutputChannels, numSamples);
    }
    else
    {
        for (int ch = 0; ch < effectiveOutputChannels; ++ch)
            buffer.clear(ch, 0, numSamples);
        // Multi-source monitoring mixes to STEREO (channels 0/1). On a >2-channel
        // output device, channels 2+ stay silent in multi-source mode (acceptable:
        // the single-source fast path above still broadcasts to all channels).
        const int mixCh = juce::jmin(effectiveOutputChannels, 2);
        // sourceMonitorScratch is sized to the device block in audioDeviceAboutTo-
        // Start, so n == numSamples on every steady-state block (no truncation).
        // The jmin is purely an overrun backstop for the same transient device-
        // reconfig window the split path guards (a block briefly larger than the
        // prepared size); dropping that one transient tail is RT-safe and matches
        // the split path. Resizing here would allocate on the audio thread.
        const int n = juce::jmin(numSamples, sourceMonitorScratch.getNumSamples());
        for (int i = 0; i < kMaxSources; ++i)
        {
            if (! act[i]) continue;
            // Render this source's monitor into the 2-channel scratch, then sum.
            sources[(size_t) i]->processBlock(inputData, numInputChannels,
                                              sourceMonitorScratch, 2, n);
            for (int ch = 0; ch < mixCh; ++ch)
                buffer.addFrom(ch, 0, sourceMonitorScratch, ch, 0, n);
        }
    }

    // Duplex mixes backing + applies output gain + meters here.
    // Split defers all three to OutputCallback (output device's clock).
    if (duplex)
    {
        const juce::ScopedTryLock sl(backingLock);
        if (sl.isLocked() && backingTransport && backingPlaying.load())
        {
            const int outSamples = renderBackingBlockLocked(numSamples);
            const float bVol = backingVolume.load();
            const int mixChannels = juce::jmin(numOutputChannels, 2);
            for (int ch = 0; ch < mixChannels; ++ch)
                buffer.addFrom(ch, 0, backingBuffer, ch, 0, outSamples, bVol);
        }

        // Apply output gain
        buffer.applyGain(outputGain.load());

        // Output metering
        float peak = 0.0f;
        for (int ch = 0; ch < numOutputChannels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
        currentOutputLevel.store(peak);
        float prevPeak = outputPeak.load();
        if (peak > prevPeak) outputPeak.store(peak);
    }
    else
    {
        // Split: push processed stereo (pre-backing, pre-output-gain) into the
        // ring. OutputCallback adds backing + output gain on its own clock.
        //
        // Strict SPSC: producer (this callback) only ever writes
        // outputRingWriteIndex; consumer (audioOutputCallback) is the sole
        // writer of outputRingReadIndex. Drop-oldest is achieved by letting
        // writeIndex lap the buffer — old slots get overwritten in place,
        // and the consumer catches up by advancing its own readIndex when
        // it observes (w - r) > cap. Counting the overflow at the consumer
        // side is what surfaces it in DeviceMetrics.
        constexpr uint64_t kMask = (uint64_t) kOutputRingFrames - 1;
        const uint64_t w = outputRingWriteIndex.load(std::memory_order_relaxed);

        const float* L = buffer.getReadPointer(0);
        const float* R = buffer.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            const uint64_t slot = (w + (uint64_t) i) & kMask;
            // Single atomic store packs both channels — prevents the L/R tear
            // a consumer could otherwise observe when the producer wraps
            // mid-callback (relaxed because ordering is established by the
            // release on outputRingWriteIndex below).
            outputPendingRing[slot].store(packLR(L[i], R[i]), std::memory_order_relaxed);
        }
        outputRingWriteIndex.store(w + (uint64_t) numSamples, std::memory_order_release);
    }

    // Body done — no source is being processed past this point until the next
    // callback. Pairs with removeSource()/reclaimPendingReleases() acquire loads.
    callbackInFlight.store(false, std::memory_order_release);
}

void AudioEngine::audioOutputCallback(const float* const* /*inputData*/,
                                      int /*numInputChannels*/,
                                      float* const* outputData,
                                      int numOutputChannels,
                                      int numSamples)
{
    juce::AudioBuffer<float> buffer(outputData, numOutputChannels, numSamples);
    if (numOutputChannels <= 0)
        return;

    constexpr uint64_t kMask = (uint64_t) kOutputRingFrames - 1;
    constexpr uint64_t kCap  = (uint64_t) kOutputRingFrames;

    // Clamp the working size to the scratch capacity pre-allocated in
    // audioOutputAboutToStart() so the .assign() calls below never realloc
    // on the RT thread when a transient oversized block arrives (mirrors
    // the backing path's clamp in audioDeviceIOCallbackWithContext).
    const int scratchCap = (int) outputPullScratchL.size();
    const int outSamples = juce::jmin(numSamples, scratchCap);

    uint64_t r = outputRingReadIndex.load(std::memory_order_relaxed);
    const uint64_t w = outputRingWriteIndex.load(std::memory_order_acquire);

    // If audioDeviceStopped() raced between our two loads and reset both
    // indices to 0, we can observe w < r. Treat that as an empty ring
    // and resync — without this, the unsigned (w - r) wraps into a huge
    // positive value and falls into the catch-up branch reading stale slots.
    if (w < r)
    {
        r = w;
        outputRingReadIndex.store(r, std::memory_order_relaxed);
    }

    // Catch up if the producer has lapped (drop-oldest is achieved via this
    // single-writer consumer-side advance, not a producer-side write to r).
    if ((w - r) > kCap)
    {
        r = w - kCap;
        outputRingReadIndex.store(r, std::memory_order_relaxed);
        inputOverflowCount.fetch_add(1, std::memory_order_relaxed);
    }

    const uint64_t available = w - r;
    const int      pullCount = juce::jmin(outSamples, (int) available);

    // When scratchCap clamped outSamples below numSamples (device-reconfig
    // race), we still need to consume the ring frames that match the
    // device callback's full block size — otherwise those extras stay
    // queued and play back late, accumulating ring/output-clock skew
    // until the latency is audible. Drop them from the ring without
    // copying into scratch.
    const int      consumeCount = juce::jmin(numSamples, (int) available);

    for (int i = 0; i < pullCount; ++i)
    {
        const uint64_t slot = (r + (uint64_t) i) & kMask;
        // Single atomic load → atomic unpack of both channels (matches the
        // producer's packed store) so L and R always belong to the same frame.
        float l, rr;
        unpackLR(outputPendingRing[slot].load(std::memory_order_relaxed), l, rr);
        outputPullScratchL[(size_t) i] = l;
        outputPullScratchR[(size_t) i] = rr;
    }
    if (pullCount < outSamples)
    {
        for (int i = pullCount; i < outSamples; ++i)
        {
            outputPullScratchL[(size_t) i] = 0.0f;
            outputPullScratchR[(size_t) i] = 0.0f;
        }
        outputUnderflowCount.fetch_add(1, std::memory_order_relaxed);
    }
    outputRingReadIndex.store(r + (uint64_t) consumeCount, std::memory_order_release);

    buffer.clear();
    const int copyChannels = juce::jmin(numOutputChannels, 2);
    for (int i = 0; i < outSamples; ++i)
    {
        buffer.setSample(0, i, outputPullScratchL[(size_t) i]);
        if (copyChannels > 1)
            buffer.setSample(1, i, outputPullScratchR[(size_t) i]);
    }

    {
        const juce::ScopedTryLock sl(backingLock);
        if (sl.isLocked() && backingTransport && backingPlaying.load())
        {
            // Shared with the duplex path so the two callbacks can't drift.
            const int backingOut = renderBackingBlockLocked(numSamples);
            const float bVol = backingVolume.load();
            for (int ch = 0; ch < copyChannels; ++ch)
                buffer.addFrom(ch, 0, backingBuffer, ch, 0, backingOut, bVol);
        }
    }

    buffer.applyGain(outputGain.load());

    float peak = 0.0f;
    for (int ch = 0; ch < numOutputChannels; ++ch)
        peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, numSamples));
    currentOutputLevel.store(peak);
    float prevPeak = outputPeak.load();
    if (peak > prevPeak) outputPeak.store(peak);
}
