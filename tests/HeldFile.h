// SPDX-License-Identifier: AGPL-3.0-or-later
// A file that, while one lives, nobody else can read, write, replace or delete, for tests of what fails when a file can't
// be reached: on Windows held open with no sharing (as another program, a virus scanner or a backup tool, may hold it);
// elsewhere its permissions, and its folder's write permission, taken away (no file can be made beside it either). Where
// they don't bind (a user above permissions, such as root), works() is false and the test has nothing to fail.
#pragma once

#include <juce_core/juce_core.h>

#include <memory>

namespace phazite::test
{
class HeldFile
{
public:
    explicit HeldFile (const juce::File&);
    ~HeldFile();

    bool works() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    HeldFile (const HeldFile&) = delete;
    HeldFile& operator= (const HeldFile&) = delete;
};
} // namespace phazite::test
