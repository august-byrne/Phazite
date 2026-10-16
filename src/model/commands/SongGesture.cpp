// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongGesture.h"
#include "internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
struct SongGesture::Impl
{
    std::unique_ptr<Gesture> gesture;
};

SongGesture::SongGesture (ProjectContext& context, const juce::String& name)
    : impl (std::make_unique<Impl>())
{
    if (auto* edit = getEdit (context))
        impl->gesture = std::make_unique<Gesture> (*edit, name);
}

SongGesture::~SongGesture() = default; // the Gesture commits as it goes

void SongGesture::commit()
{
    if (impl->gesture != nullptr)
        impl->gesture->commit();
}

void SongGesture::rollBack()
{
    if (impl->gesture != nullptr)
        impl->gesture->rollBack();
}

bool SongGesture::isOpen() const
{
    return impl->gesture != nullptr && impl->gesture->isOpen();
}
} // namespace phazite
