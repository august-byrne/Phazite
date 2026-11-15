// SPDX-License-Identifier: AGPL-3.0-or-later
// Apart from the tests that use it, as it needs the operating system's own headers.
#include "HeldFile.h"

#if JUCE_WINDOWS
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

namespace phazite::test
{
#if JUCE_WINDOWS
struct HeldFile::Impl
{
    explicit Impl (const juce::File& f)
        : file (f),
          handle (CreateFileW (f.getFullPathName().toWideCharPointer(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr))
    {
    }

    ~Impl()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle (handle);
    }

    const juce::File file;
    const HANDLE handle;
};
#else
struct HeldFile::Impl
{
    explicit Impl (const juce::File& f)
        : file (f)
    {
        struct stat info {};
        struct stat folderInfo {};
        held = stat (file.getFullPathName().toRawUTF8(), &info) == 0
               && stat (file.getParentDirectory().getFullPathName().toRawUTF8(), &folderInfo) == 0;

        if (held)
        {
            fileMode = info.st_mode & 07777;
            folderMode = folderInfo.st_mode & 07777;
            chmod (file.getFullPathName().toRawUTF8(), 0);
            chmod (file.getParentDirectory().getFullPathName().toRawUTF8(), folderMode & ~(mode_t) (S_IWUSR | S_IWGRP | S_IWOTH));
        }
    }

    ~Impl()
    {
        if (held)
        {
            chmod (file.getParentDirectory().getFullPathName().toRawUTF8(), folderMode);
            chmod (file.getFullPathName().toRawUTF8(), fileMode);
        }
    }

    const juce::File file;
    bool held = false;
    mode_t fileMode = 0, folderMode = 0;
};
#endif

HeldFile::HeldFile (const juce::File& file)
    : impl (std::make_unique<Impl> (file))
{
}

HeldFile::~HeldFile() = default;

bool HeldFile::works() const
{
    // Held, if what anyone may do can't read it.
    return ! juce::FileInputStream (impl->file).openedOk();
}
} // namespace phazite::test
