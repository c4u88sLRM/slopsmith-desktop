// slopsmith-vst-scan — out-of-process VST3 probe (macOS).
//
// One-shot CLI: --scan-plugin <path> --scan-out <xml>
// Spawns from VSTHost::scanDirectories so a crashy plugin only kills this
// helper, not the Electron main process.

#include "VSTHost.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace {

int runScanMode(const juce::String& pluginPath, const juce::String& outPath)
{
    if (pluginPath.isEmpty() || outPath.isEmpty())
        return 2;

    juce::ScopedJuceInitialiser_GUI juceInit;
    VSTHost host;
    const juce::String xml = host.scanPluginFileToXml(pluginPath);
    if (! juce::File(outPath).replaceWithText(xml))
        return 11;

    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    juce::String scanPlugin, scanOut;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg(argv[i]);
        if (arg == "--scan-plugin" && i + 1 < argc)
            scanPlugin = argv[++i];
        else if (arg == "--scan-out" && i + 1 < argc)
            scanOut = argv[++i];
    }

    if (scanPlugin.isEmpty() || scanOut.isEmpty())
        return 2;

    return runScanMode(scanPlugin, scanOut);
}
