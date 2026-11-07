// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppInfo.h"

#include <PhaziteData.h>

namespace phazite
{
namespace
{
    // The list of notices (resources/licenses/notices.json), compiled into the app.
    juce::var readNoticesList()
    {
        int size = 0;

        if (const auto* data = PhaziteData::getNamedResource ("notices_json", size))
            return juce::JSON::parse (juce::String::fromUTF8 (data, size));

        jassertfalse; // the build compiles it in: CMakeLists.txt
        return {};
    }

    // Whether an entry is this build's: one without platforms is every build's.
    bool isForThisPlatform (const juce::var& entry)
    {
        const auto* platforms = entry["platforms"].getArray();

        if (platforms == nullptr)
            return true;

#if JUCE_WINDOWS
        const juce::String thisPlatform ("windows");
#elif JUCE_MAC
        const juce::String thisPlatform ("macos");
#else
        const juce::String thisPlatform ("linux");
#endif
        return platforms->contains (thisPlatform);
    }
} // namespace

juce::String getVersionString()
{
    return PHAZITE_VERSION_STRING; // defined by CMakeLists.txt
}

juce::String getCopyrightNotice()
{
    return PHAZITE_COPYRIGHT_STRING; // defined by CMakeLists.txt
}

juce::String getSourceCodeAddress()
{
    return PHAZITE_SOURCE_URL; // defined by CMakeLists.txt
}

std::vector<ThirdPartyNotice> getThirdPartyNotices()
{
    std::vector<ThirdPartyNotice> notices;
    const auto list = readNoticesList(); // which the entries are in, so it must last as long as they are read

    if (const auto* entries = list["notices"].getArray())
    {
        for (const auto& entry : *entries)
        {
            if (! isForThisPlatform (entry))
                continue;

            notices.push_back ({ .name = entry["name"].toString(),
                                 .version = entry["version"].toString(),
                                 .licence = entry["licence"].toString(),
                                 .copyright = entry["copyright"].toString(),
                                 .use = entry["use"].toString(),
                                 .url = entry["url"].toString(),
                                 .file = entry["file"].toString() });
        }
    }

    return notices;
}

juce::StringArray getTrademarkNotices()
{
    juce::StringArray sentences;
    const auto list = readNoticesList();

    if (const auto* entries = list["trademarks"].getArray())
        for (const auto& entry : *entries)
            if (isForThisPlatform (entry))
                sentences.add (entry["text"].toString());

    return sentences;
}

juce::File getLicencesFolder()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("licenses");
}
} // namespace phazite
