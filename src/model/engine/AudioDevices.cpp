// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioDevices.h"

// Last, so that their macros can't change the headers above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h> // before the others, which use its types

 #include <cfgmgr32.h>
 #include <mmdeviceapi.h>
 #include <propidl.h>
#endif

namespace phazite
{
#if JUCE_WINDOWS
namespace
{
    // Releases a COM object as it goes.
    template <typename Object>
    struct Held
    {
        Object* object = nullptr;

        Held() = default;
        Held (const Held&) = delete;
        Held& operator= (const Held&) = delete;

        ~Held()
        {
            if (object != nullptr)
                object->Release();
        }

        Object* operator->() const noexcept { return object; }
    };

    // Windows' PKEY_Device_FriendlyName, defined here as JUCE's own audio code does, rather than taken from a library.
    const PROPERTYKEY friendlyNameKey { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

    juce::String getFriendlyName (IMMDevice& endpoint)
    {
        Held<IPropertyStore> properties;

        if (FAILED (endpoint.OpenPropertyStore (STGM_READ, &properties.object)))
            return {};

        PROPVARIANT value;
        PropVariantInit (&value);
        juce::String name;

        if (SUCCEEDED (properties->GetValue (friendlyNameKey, &value)) && value.vt == VT_LPWSTR)
            name = juce::String (value.pwszVal);

        PropVariantClear (&value);
        return name;
    }

    // Whether an endpoint's device hangs off a Bluetooth link. Its device node is SWD\MMDEVAPI\<the endpoint's ID>, whose
    // parent is a Bluetooth audio profile's (BTHENUM for A2DP, BTHHFENUM for hands-free, BTHLE... for LE Audio) rather
    // than a sound card's (HDAUDIO, USB).
    bool hangsOffBluetooth (IMMDevice& endpoint)
    {
        LPWSTR id = nullptr;

        if (FAILED (endpoint.GetId (&id)))
            return false;

        const auto path = "SWD\\MMDEVAPI\\" + juce::String (id);
        CoTaskMemFree (id);

        DEVINST node = 0, parent = 0;
        WCHAR parentID[MAX_DEVICE_ID_LEN] {};

        if (CM_Locate_DevNodeW (&node, const_cast<DEVINSTID_W> (path.toWideCharPointer()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS
            || CM_Get_Parent (&parent, node, 0) != CR_SUCCESS || CM_Get_Device_IDW (parent, parentID, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
            return false;

        return juce::String (parentID).startsWithIgnoreCase ("BTH");
    }

    // Whether the active output endpoint of this name is Bluetooth; nothing if Windows lists none by that name (JUCE numbers
    // devices that share a name, for one).
    std::optional<bool> isBluetoothEndpoint (const juce::String& name)
    {
        const auto initialised = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        std::optional<bool> found;

        {
            Held<IMMDeviceEnumerator> enumerator;
            Held<IMMDeviceCollection> endpoints;
            UINT count = 0;

            if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator.object)))
                && SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &endpoints.object))
                && SUCCEEDED (endpoints->GetCount (&count)))
            {
                for (UINT i = 0; i < count && ! found.has_value(); ++i)
                {
                    Held<IMMDevice> endpoint;

                    if (SUCCEEDED (endpoints->Item (i, &endpoint.object)) && getFriendlyName (*endpoint.object) == name)
                        found = hangsOffBluetooth (*endpoint.object);
                }
            }
        }

        if (SUCCEEDED (initialised))
            CoUninitialize();

        return found;
    }
} // namespace
#endif

bool isBluetoothOutput (const juce::String& deviceType, const juce::String& deviceName)
{
#if JUCE_WINDOWS
    // Windows Audio, in any of its modes, and DirectSound name devices as Windows does; ASIO drivers name themselves.
    if (deviceType.startsWith ("Windows Audio") || deviceType == "DirectSound")
        if (const auto bluetooth = isBluetoothEndpoint (deviceName))
            return *bluetooth;
#else
    juce::ignoreUnused (deviceType);
#endif

    return isBluetoothName (deviceName);
}

bool isBluetoothName (const juce::String& deviceName)
{
    // Windows' hands-free endpoints and Linux's BlueZ devices say so, as some devices' own names do.
    for (const auto* word : { "bluetooth", "bluez", "hands-free", "a2dp" })
        if (deviceName.containsIgnoreCase (word))
            return true;

    return false;
}

std::optional<int> chooseLowLatencyBufferSize (const juce::Array<int>& available)
{
    std::optional<int> chosen;

    for (const auto size : available)
        if (size >= 128 && size <= 256 && (! chosen.has_value() || size > *chosen))
            chosen = size;

    return chosen;
}

bool chooseLowLatencySetup (juce::AudioDeviceManager& devices)
{
    if (devices.getCurrentAudioDevice() == nullptr)
        return false; // nothing opened: a later start tries again

    const auto previousType = devices.getCurrentAudioDeviceType();
    const auto previous = devices.getAudioDeviceSetup();
    auto offered = false;

    for (const auto* type : devices.getAvailableDeviceTypes())
        offered = offered || type->getTypeName() == lowLatencyDeviceType;

    if (offered && previousType != lowLatencyDeviceType)
    {
        // The same output and inputs in Low Latency Mode, if it offers a buffer small enough.
        devices.setCurrentAudioDeviceType (lowLatencyDeviceType, true);

        if (auto* device = devices.getCurrentAudioDevice())
        {
            if (const auto size = chooseLowLatencyBufferSize (device->getAvailableBufferSizes()))
            {
                auto setup = previous;
                setup.bufferSize = *size;

                if (devices.setAudioDeviceSetup (setup, true).isEmpty() && devices.getCurrentAudioDevice() != nullptr)
                    return true;
            }
        }

        devices.setCurrentAudioDeviceType (previousType, true);
    }

    // As it was, as the chosen setup.
    devices.setAudioDeviceSetup (previous, true);
    return false;
}
} // namespace phazite
