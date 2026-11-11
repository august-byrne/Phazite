# JUCE workarounds

*JUCE 9.0.5; listed on 2026-11-11.* Phazite builds on JUCE, and most of its JUCE code uses JUCE as JUCE means it to be used. Some of it is there only because JUCE does something wrong, lacks something or keeps something to itself, or it leans on something JUCE doesn't promise. This list keeps the two apart:

- **Workarounds:** code that would go, or be simpler, if JUCE changed: W in the code, B in the build, T in the tests' harness. Each is marked where it is with `JUCE workaround <number> (docs/juce-workarounds.md)`, so `git grep "JUCE workaround"` finds every one. Those marked *JUCE's insides* lean on something JUCE doesn't document (a format of its own, a message's number, a name), which an update can change without a word.
- **Correct patterns:** code shaped by JUCE that looks like a workaround but uses JUCE as documented. It isn't marked, and an update needs nothing of it. Don't "fix" it.
- **Open questions:** code whose reason isn't settled yet, each with what decides it.

How to keep it:
- Code that works around JUCE gets an entry here and the marker in its comment, in the same change; code that stops working around it loses both. A number isn't used again.
- At each JUCE update (CLAUDE.md, "Submodules and third-party code"), go through the workarounds: look at what each names in the new JUCE, remove those it fixed, and check that the insides the code leans on are still as it expects.
- JUCE's files are named by their path in `external/JUCE/modules/`, or in `external/JUCE/` for its build scripts.

## Workarounds

### Threads

**W1. No thread is ended by force.** `juce::Thread::stopThread()` with a time limit ends a thread that hasn't stopped in time, with `TerminateThread()` on Windows (`juce_core/threads/juce_Thread.cpp`, `juce_core/native/juce_Threads_windows.cpp`), which can leave a lock the thread held held for ever: a loader lock left so hung every thread started after it (2026-11-10). `juce::ThreadPool` does it to a thread that hasn't stopped within half a second (`juce_core/threads/juce_ThreadPool.cpp`), and a `TimeSliceThread` as it goes, after 2 s (`juce_TimeSliceThread.cpp`). So Phazite runs its jobs on `phazite::BackgroundJobs` (`src/model/base/BackgroundJobs.h`), stops each `AudioThumbnailCache`'s thread itself first (`src/app/ClipWaveforms.cpp`, `src/ui/browser/SoundView.cpp`), and cancels a plug-in scan before JUCE's plug-in list waits for its pool (`src/app/MainWindow.cpp`, `PluginList`, and `juce_audio_processors/scanning/juce_PluginListComponent.cpp`). `tools/check-architecture.sh` counts the rest (`thread-kills`). `stopThread (-1)` itself is JUCE's documented wait without a limit, a correct pattern.

**W2. The audio threads' priority on Windows.** A real-time thread of JUCE's on Windows puts the whole process in the real-time priority class, or the high one (`Process::setPriority()`, `juce_core/native/juce_Threads_windows.cpp`), every thread of the app above other programs'. Phazite's threads that play audio, the engine's workers and the helpers' threads that play plug-ins, take the highest priority and join MMCSS's "Pro Audio" task, as JUCE's own WASAPI thread does (`src/devices/core/AudioThreads.*`); `tests/EngineTests.cpp` checks that the process's class stays as it was. Elsewhere they are JUCE's real-time threads.

### Plug-ins

**W3. A VST3's restartComponent() isn't taken for a new program.** JUCE's VST3 host reports every `restartComponent()` a plug-in makes as a program change with new parameter info, whatever the plug-in's flags said (`restartComponentOnMessageThread()`, `juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h`), and many make one as they sit there, for their latency or their parameters' names. The helper counts a program change only without new parameter info (`src/host/core/Instance.cpp`); counting them all made an idle project's plug-ins an unsaved change. A plug-in that loads a preset with nothing but `restartComponent()`, no `setDirty()`, counts only as its values change. It goes when JUCE passes the plug-in's own flags on.

**W4. A VST3's state as JUCE's own block (*JUCE's insides*).** The song keeps a VST3's state as the format's own preset file (`.vstpreset`), but the helper gives it to the plug-in and reads it back through `setStateInformation()` and `getStateInformation()`, turned to and from the block JUCE writes there, a `VST3PluginState` element through `copyXmlToBinary()` (`src/sandbox/core/Vst3State.h`, `src/host/core/Instance.*`): JUCE's `setPreset()` loads a preset without setting the parameters' values again, and `getPreset()` doesn't flush them first, as the state's calls do (`juce_VST3PluginFormatImpl.h`). A plug-in's class ID comes from the header of what `getPreset()` writes, as JUCE has no call for it. `tests/Vst3StateTests.cpp` checks the block's format. It goes when JUCE's preset calls do what its state calls do.

**W5. A helper that crashes is noticed by a thread that watches it.** On Windows, when the helper at the other end goes, JUCE's named pipe waits for another to connect rather than failing (`NamedPipe`, `juce_core/native/juce_Files_windows.cpp`), and `InterprocessConnection` takes a read that times out for a lost connection (`juce_events/interprocess/juce_InterprocessConnection.cpp`). So the pipe has no time limit (JUCE's -1), a thread of Phazite's looks at the helper's process five times a second, and closing the pipe wakes a read or a write waiting on it (`src/sandbox/client/Helper.*`, `src/host/core/Host.cpp`). The watching thread also finds helpers that hang, which stays whatever JUCE does; only the crashes' part would go.

**W6. A plug-in scan keeps when unchanged plug-ins were described.** JUCE's formats stamp every plug-in they describe with the time of the scan, changed or not (`lastInfoUpdateTime`, `juce_VST3PluginFormatImpl.h` and the other formats). The scan gives a plug-in whose file and version are the same the time it had (`keepUpdateTimesOfUnchanged()`, `src/model/engine/PluginScanner.cpp`).

**W28. Phazite's own helper processes, not JUCE's `ChildProcessCoordinator`.** JUCE's coordinator keeps its `ChildProcess` to itself and ends a worker only by asking it to quit (`killWorkerProcess()`, `juce_events/interprocess/juce_ConnectedChildProcess.cpp`), so a helper that hangs couldn't be ended; it notices a crash only when its pings stop, after its time limit (8 s unless told otherwise), not within a fifth of a second; its pings are answered on the pipe's thread, so a helper whose message thread hangs looks alive; its worker ends itself when Phazite's pings stop, as they do while Phazite is paused in a debugger; and it stops its ping thread with a time limit of 10 s (W1). So Phazite starts and talks to its helpers with JUCE's own parts, `ChildProcess` and `InterprocessConnection`, with the coordinator's command line (`--<name>:<pipe>`), and adds what is missing (`src/sandbox/client/Helper.*`; the helper's side, `src/host/core/Host.cpp`). The owner asked for the switch to JUCE's coordinator if nothing would be lost (2026-11-11); this much would be.

### The song

**W8. A property set without an undo step only when its value changes.** Without an `UndoManager`, `ValueTree::setProperty()` compares the value's type too (`NamedValueSet::set()`, `juce_core/containers/juce_NamedValueSet.cpp`; `juce_data_structures/values/juce_ValueTree.cpp`), so the number 5 over the text "5" read from a file is a change its listeners hear; with one, it compares values alone. The two stores that write without one compare first (`storeProperty()`, `src/model/plugins/PluginInstances.cpp`; `store()`, `src/model/mixer/Mixer.cpp`); they could be one function.

### Audio devices and the app

**W9. Windows' low-latency mode by JUCE's name for it (*JUCE's insides*).** Phazite finds Windows' shared mode with small buffers by the name JUCE gives its device type, "Windows Audio (Low Latency Mode)" (`src/model/engine/AudioDevices.h`; `juce_audio_devices/native/juce_WASAPI_windows.cpp`), as JUCE has no other way to name it. Were JUCE to rename it, the first run's low-latency setup and the Low Latency switch would find nothing, without a word.

**W10. The running mutex by JUCE's names for it (*JUCE's insides*).** Phazite holds a `juce::InterProcessLock` named "PhaziteRunning" while it runs, which the installer waits for (`src/app/Main.cpp`). JUCE names the mutex `Global\PhaziteRunning`, or one in the session's own namespace where it can't make that one (`juce_core/native/juce_Threads_windows.cpp`), so the installer looks for both (`RunningMutexes`, `packaging/windows/Phazite.iss`).

### Drawing

**W11. The time line's parts drawn through windows on whole pixels.** While the playlist and the piano roll scroll continuously, the song moves under the view by parts of a pixel, through a transform. JUCE's Direct2D renderer then rounds a component's clip out to whole pixels (`clipToRectangle()`, `juce_graphics/native/juce_Direct2DGraphicsContext_windows.cpp`, with `juce_RenderingHelpers.h`), so a moved part painted into the pixel beside it (a track header's edge flickered); and JUCE leaves a transformed child out when it works out what opaque components cover (`isVisibleToLayer()`, `juce_gui_basics/components/juce_Component.cpp`). Each part that draws the time line shows through an untransformed window on whole pixels, which clips it there and is opaque for it (`TimelinePartWindow`, `src/ui/timeline/TimelineEditor.*`); `tests/ScrollingFrameTests.cpp` guards it. The view's start on whole pixels of the screen and lines rounded from the song (`getViewStep()`, `beatToColumn()`) aren't workarounds: any renderer needs them.

**W12. A frozen clip's stripes clipped to its outline as a path.** Clipped to a rectangle, which Direct2D rounds out while the clip sits a part of a pixel off the screen's (as in W11), its stripes reached into the line between it and the next clip, more at one frame than the next; a path clips them exactly (`src/ui/playlist/ClipComponent.cpp`).

**W13. Waveforms drawn into a mask by JUCE's software renderer.** JUCE draws a waveform as a list of rectangles a pixel wide (`AudioThumbnail::drawChannel()`, `juce_audio_utils/gui/juce_AudioThumbnail.cpp`), which Direct2D draws sharp or soft as the rounding of their edges falls (`fillRectList()`, `juce_Direct2DGraphicsContext_windows.cpp`, `juce_DirectX_windows.cpp`): a clip blinked as the playlist scrolled (2026-11-09). Each waveform is drawn into a mask by the software renderer, and the mask moves whole (`draw()`, `src/app/ClipWaveforms.cpp`). Keeping the mask is a choice for speed, and stays either way.

**W14. Phazite's own blur.** JUCE's blur radius blurs as far as another on each renderer: on Direct2D its standard deviation is a third of the radius (`juce_graphics/native/juce_Direct2DImage_windows.cpp`), in software it is the radius, with the kernel cut off at one standard deviation (`juce_graphics/images/juce_Image.cpp`, `juce_ImageConvolutionKernel.cpp`), and on macOS it is Core Image's radius. The frosted glass behind popups blurs by three box blurs of Phazite's own, the same on all of them (`blur()`, `src/ui/theme/Popups.*`; docs/vscode-look.md).

**W15. JUCE's standard mouse pointers held all session.** JUCE keeps a standard pointer only while something holds it (`juce_gui_basics/mouse/juce_MouseCursor.cpp`), so a view that switched between two made one again at each switch, the hand's from an image. Phazite's look holds each of them while it lives (`src/ui/theme/LookAndFeel.*`).

### Menus, tooltips and the keyboard focus

**W16. A menu inside the window keeps its keys.** A menu says it didn't use a key it has no use for (`juce_gui_basics/menus/juce_PopupMenu.cpp`), and JUCE then offers the key to the menu's parent, the main window (`juce_gui_basics/windows/juce_ComponentPeer.cpp`), whose shortcuts and computer keyboard would take it. `keepKeysInMenu()` gives every key to the menu (`src/ui/theme/Popups.*`, called from `LookAndFeel.cpp`).

**W17. Tooltips painted by Phazite's tooltip window.** JUCE keeps a tip's text to itself, and its look's `drawTooltip()` isn't told which window it draws (`juce_gui_basics/windows/juce_TooltipWindow.h`), so the look can't tell a tip on frosted glass from a solid one. `Tooltips` keeps what `getTipFor()` returned and paints itself (`src/ui/theme/Popups.*`).

**W18. A slider's value pops up only where there is a display.** On a machine without one (a test machine's), JUCE's popup reads its monitor's area from a null display (`Component::getParentMonitorArea()`, `juce_Component.cpp`; `juce_gui_basics/desktop/juce_Displays.cpp`). `ValueSlider` turns the popup on only when a display exists (`src/ui/components/Controls.cpp`).

**W19. The browser's tree laid out at once.** JUCE's `TreeView` lays its rows out a message after they change (`juce_gui_basics/widgets/juce_TreeView.cpp`), and a scroll before then goes by where they were, or is lost. `Browser::layOutTreeNow()` gives the tree its root again, which lays the rows out at once; the root's `itemOpennessChanged()` does nothing, so that its rows stay as the tree closes and opens it there (`src/ui/browser/Browser.cpp`).

**W20. The browser's tree is one stop for Tab.** JUCE's `TreeView` and its `Viewport` both want the keyboard focus (`juce_TreeView.cpp`, `juce_gui_basics/layout/juce_Viewport.cpp`), so Tab stopped on it twice; the viewport is set not to want it (`src/ui/browser/Browser.cpp`).

**W21. A text field lets the keys go when the mouse is pressed elsewhere.** JUCE leaves the keys in a text field when a press lands on what doesn't take them (a panel, a button, a knob), and draws its selection at half strength once they have gone (`juce_gui_basics/widgets/juce_TextEditor.cpp`). `TextFieldRelease` gives the keys back to the window's shortcuts and hides the selection, as other apps do (`src/ui/input/TextFieldRelease.*`).

### Keys and the mouse

**W22. A mouse wheel's notch, by platform.** JUCE reports a wheel's movement in no unit of its own: a notch is 60/256 on Windows, 50/256 on Linux, and 10/256 of the system's own movement on macOS (`juce_gui_basics/native/juce_Windowing_windows.cpp`, `juce_XWindowSystem_linux.cpp`, `juce_NSViewComponentPeer_mac.mm`). Phazite steps, zooms and scrolls by notches, so it says what a notch is on each (`wheelNotch`, `src/ui/components/Controls.h`). It took Windows' everywhere until 2026-11-11, and on Linux a notch went five sixths as far. Set macOS's when Phazite builds there.

**W23. The computer keyboard asks Windows whether a key was down as its message was sent.** On Windows, `KeyPress::isKeyCurrentlyDown()` reads the key as it is now (`GetAsyncKeyState()`, `juce_Windowing_windows.cpp`), so a quick tap was over by the time a busy message thread asked, and played no note (docs/performance-audit.md, U6). The computer keyboard reads `GetKeyState()`, which keeps step with the messages (`src/ui/input/ComputerKeyboard.cpp`); elsewhere JUCE keeps the keys' state from its messages itself.

**W24. Whether a dragged item was dropped, passed in its description.** JUCE tells a drag's source that its drag ended (`DragAndDropContainer::dragOperationEnded()`, `juce_gui_basics/mouse/juce_DragAndDropContainer.h`), not whether it was dropped. What puts a dropped item into the song marks its description, an object every copy of it shares, which the browser reads to note the item as used (`markPlaced()`, `wasPlaced()`, `src/ui/input/LibraryDrag.*`; `Browser::dragOperationEnded()`).

**W25. The order of the main window's key listeners (*JUCE's insides*).** The computer keyboard hears its letters before the shortcuts do because `WindowKeys` adds it after them, and JUCE asks the listener added last first (`ComponentPeer::handleKeyPress()`, `juce_ComponentPeer.cpp`), which `Component::addKeyListener()`'s documentation doesn't promise (`src/ui/input/WindowKeys.*`). To end it: one key listener of `WindowKeys`' own that asks the computer keyboard, then the shortcuts.

**W26. Whether the pointer is over a notice, from the mouse's last events.** `Component::isMouseOver()` also asks where the system's pointer is now (`juce_Component.cpp`), which the tests' mouse events don't move. The device notices ask what the mouse's events last found under it (`isUnderPointer()`, `src/ui/shell/DeviceNotices.cpp`). Only the tests need it.

**W27. The display's frames on JUCE's millisecond clock (*JUCE's insides*).** The playhead goes where the song is at the time of the display's frame, which `VBlankAttachment` gives in seconds without saying from when. Phazite reads it as `Time::getMillisecondCounterHiRes()`'s clock (`showFrame()`, `src/ui/timeline/TimelineEditor.*`), which it is on Windows and Linux (`juce_gui_basics/native/juce_VBlank_windows.cpp`, `juce_Windowing_linux.cpp`) but not on macOS, where it is the display's own. Check it when Phazite builds on macOS.

## The build

**B1. GCC's dangling-reference warning off for one unit of JUCE's.** GCC 13 and later read `opt.getButtons().front()` in `juce_gui_basics/lookandfeel/juce_LookAndFeel_V2.cpp` as a reference to a temporary; nothing dangles. The warning is off for `juce_gui_basics_2.cpp` alone (`CMakeLists.txt`).

**B2. JUCE's recommended config flags with GCC and Clang only.** On MSVC they add `/MP` (`extras/Build/CMake/JUCEHelperTargets.cmake`), which MSVC refuses beside the `/showIncludes` that Ninja needs (warning D9030 for every file); CMake's own defaults give MSVC the rest (`CMakeLists.txt`).

**B3. juceaide built with Phazite's toolchain.** JUCE configures its `juceaide` helper in a CMake run of its own, without the toolchain file, and keeps that folder even after `--fresh` (`extras/Build/juceaide/CMakeLists.txt`). Phazite hands the toolchain on through the environment (`CMakeLists.txt`, `CMakePresets.json`, `cmake/toolchains/windows-msvc.cmake`), deletes the folder when the compiler or the toolchain changes, and stops if it was configured without the toolchain (`cmake/PhaziteHelpers.cmake`).

**B4. The installer takes the app's icon from juceaide's folder (*JUCE's insides*).** juceaide makes the app's icon as CMake configures (`extras/Build/CMake/JUCEUtils.cmake`); the installer target reads it from the app's generated sources by its name, `icon.ico`, and the logo is a configure dependency, so that a change to it makes the icon again (`CMakeLists.txt`).

**B5. linux-tsan in RelWithDebInfo, with -Wno-tsan.** ThreadSanitizer needs optimisation, and JUCE forces `-O0` in Debug (`JUCEHelperTargets.cmake`); GCC warns that TSan doesn't model `atomic_thread_fence()`, which JUCE (`juce_core/memory/juce_Atomic.h`) and Phazite's engine use. In `CMakePresets.json`, which can't hold a marker.

Build choices that aren't workarounds: JUCE compiled once, in `phazite_core` (`phazite_use_core()`), because JUCE's modules are interface libraries that compile into every target linking them, as JUCE advises for code shared between targets; JUCE's options on `phazite_core`, among them `JUCE_USE_CURL=0`, which leaves Linux without HTTPS, so that Phazite Sounds' download and the update check refuse there until Phazite builds with curl (docs/design.md); `using` declarations where GCC's `-Woverloaded-virtual` meets JUCE's overloaded virtual functions; and parallel compiles limited by the memory JUCE's large units take (`cmake/PhaziteHelpers.cmake`).

## The tests' harness

**T1. Messages delivered at once (*JUCE's insides*).** The tests deliver what was posted to the message thread through `juce::detail::dispatchNextMessageOnSystemQueue()`, a function of JUCE's that its headers don't declare (`juce_events/messages/juce_MessageManager.cpp`). JUCE posts its one wake-up only into an empty queue, so `queueWithoutWakeUp()` posts with JUCE's number for it, `WM_USER + 123` (`juce_events/native/juce_Messaging_windows.cpp`), and fails loudly if that changes (`tests/UiTestHelpers.cpp`).

**T2. A window without a native one.** JUCE has no window peer that is on no screen, so the tests' windows have one of their own, `HeadlessPeer` (`tests/UiTestHelpers.cpp`). It implements `getFrameSize()`, which JUCE deprecates on Linux but keeps pure virtual, with the deprecation warning off.

**T3. A display where the machine has none.** JUCE's `ComponentAnimator` uses the display it finds for a component without checking that it found one (`juce_gui_basics/layout/juce_ComponentAnimator.cpp`), and a machine without a screen has none: a drag's image let go crashed Linux CI (2026-11-01). `ScopedDisplay` adds one while a test needs it (`tests/UiTestHelpers.*`).

**T4. The tests' mouse.** `Mouse::send()` sets `ModifierKeys::currentModifiers` as JUCE's platform code does before it passes an event on, and ends JUCE's drag auto-repeat, which repeats a drag at the real pointer's place on the screen (`juce_gui_basics/detail/juce_MouseInputSourceList.h`) (`tests/UiTestHelpers.cpp`).

**T5. A temporary folder of the tests' own (*JUCE's insides*).** The tests set TMP and TEMP (TMPDIR elsewhere) to a folder of their own, which works because JUCE asks the system for the temporary folder at each call (`juce_core/native/juce_Files_windows.cpp`) (`tests/TestMain.cpp`).

**T6. The tests' audio device type first (*JUCE's insides*).** JUCE's device manager makes its own device types only while it has none (`juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp`), so the tests add theirs first and JUCE never looks for the computer's devices (`tests/TestAudioDevice.cpp`).

Some tests also say where they meet a JUCE behaviour, such as tooltips shown only while the app is in front (`tests/PopupsTests.cpp`) or what Direct2D's images cost (`tests/Benchmarks.cpp`); those notes are where they apply.

## Correct patterns

Code shaped by JUCE that uses it as documented. It needs no marker, and nothing at an update.

- **Threads:** `stopThread (-1)` once what the thread waits on is woken, JUCE's documented wait without a limit; where the system allows no real-time threads, the highest priority when `startRealtimeThread()` returns false (`src/devices/core/AudioThreads.cpp`).
- **Audio buffers:** a part's output buffer is cleared through its channels' pointers, never by `AudioBuffer::clear()`, which JUCE documents as skipping a buffer it marked clear: writing through the pointers doesn't unmark it (`src/engine/nodes/PartOutput.h`).
- **Child processes:** `juce::ChildProcess` isn't safe from several threads (on Linux `isRunning()` reaps the child), so a helper's process is looked at by one thread at a time (`src/sandbox/client/Helper.h`).
- **Downloads:** the update check follows redirects itself, as JUCE would follow one to `http://` (`src/services/Updates.cpp`).
- **Keys over plug-ins in their helper:** a `WH_GETMESSAGE` hook catches the keys on their way to a plug-in's own native windows, as JUCE's own plug-in client does (`src/host/core/Editor.cpp`).
- **What JUCE leaves to the app:** a Bluetooth output found by its device's name (`src/model/engine/AudioDevices.cpp`), devices plugged in found by comparing lists (`AudioDeviceWatch.cpp`), oversampling a sample at a time with JUCE's half-band design (`src/devices/dsp/Oversampling.cpp`), memory shared between processes (`src/sandbox/core/SharedMemory.h`) and a queue that several threads write into (`src/engine/core/EventQueue.h`).
- **Choices over JUCE's own:** r8brain-free-src in place of JUCE's Lagrange interpolator (docs/dsp-audit.md), and the song as Phazite's own text with standard base 64, not JUCE's XML writer and its own base 64 (`src/model/base/internal/SongText.h`).
- **Devices and files:** inputs opened only as the tracks need them (`useDefaultInputChannels = false`, `src/model/transport/AudioRecorder.cpp`); a device set up without taking it for the user's choice (`setAudioDeviceSetup (setup, false)`, `PlaybackHost.cpp`); a file deleted before a `FileOutputStream` writes it, as a stream adds to what is there (`Export.cpp`, `OfflineRender.cpp`).
- **The audio thread:** what JUCE's device manager and plug-in hosting do there is in docs/realtime-audit.md (findings 3 and 9).
- **Devices' windows:** each floats over the main window, above it but not above other apps, minimised and moved with it, through `TopLevelWindow::setAssociatedComponent()` (`src/app/PluginWindow.cpp`).
- **Windows and the focus:** the title bar's hit test by bounds alone, as JUCE warns that `contains()` there can recurse, with the style flags JUCE gives its own title bar's buttons added back for Phazite's (`findControlAtPoint()`, `src/app/MainWindow.cpp`); the shell's focus traverser, which JUCE asks where the focus goes when a focused part hides (`AppShell.cpp`); a value field in the window's content component, as JUCE asserts when children are added to the window itself, or in the dialog or popup that has the keys (`Controls.cpp`); and a value field that finishes on Return, Escape or a lost focus at once, by overriding `keyPressed()` and `focusLost()`, as JUCE's `onReturnKey` and the like come a message later on purpose (`Controls.cpp`); and the search field of the command palette, the plug-in picker and the branch picker taking the keys a message after its call-out opens (`CommandPalette.cpp`, `PluginPicker.cpp`, `BranchPicker.cpp`): JUCE's call-out already gives them to its first control as it goes modal (`Component::enterModalState()`), so this only makes sure of it.
- **The mouse and the keys:** an editor's `UseListener`, not the editor listening to itself, as JUCE's listeners hear a component's events besides it (`TimelineEditor.cpp`); `onUsed` on a press, as JUCE tells focus changes a message later (`TimelineEditor.h`); held keys by `keyStateChanged()` and `isKeyCurrentlyDown()` (`KeyDispatcher.cpp`); keymap changes saved by command ID, as JUCE's command numbers follow the order commands were registered in (`KeymapStore.cpp`).
- **Menus, tabs, labels and buttons:** a menu's result checked to be over 0, as 0 is a menu dismissed; one tab holder for each area, as `clearTabs()` hides the view it showed (`Panels.cpp`); JUCE's documented order of a label's callbacks (`SectionsStrip.cpp`) and a radio button's `onClick` for the button turned off (`Welcome.cpp`).
- **Drawing:** the look's overrides, JUCE's hooks for its controls' look (`LookAndFeel.cpp`); a theme switch through `applyColourToAllText()` and the colours set again (`LookAndFeel.cpp`, `MainWindow.cpp`); `Path::isEmpty()` read as JUCE documents it, true for a path of starts alone (`DeviceGraph.cpp`); `setBufferedToImage()` called only when the choice changes, as each call with true makes a new picture (`PlaylistRows.cpp`); a drag's image made as a software image, quick to make (`StripParts.cpp`); the playhead moved at the display's frames by a `VBlankAttachment` (`TimelineEditor.cpp`); Lucide's `currentColor`, which JUCE reads as black (`Icons.cpp`; until 2026-11-11 the icons' text was changed to black before JUCE read it, which JUCE 9 doesn't need).
- **Typed values:** text without a digit, ignored by a knob, as JUCE reads what isn't a number as 0 and checking what was typed is the app's job (`Controls.cpp`); and the field a value is typed in without a right-click menu, as it lives only while it has the keys, which a menu would take, with Ctrl+C and Ctrl+V working in it (`setPopupMenuEnabled (false)`, `Controls.cpp`; the owner's choice, 2026-11-11).
- **Late notifications:** a change a slider sends asynchronously, as JUCE documents `sendNotificationAsync`, ignored once the piano roll's transform bar has closed (`PianoRollLanes.cpp`).
- **The song against its saved copy** (`src/model/project/internal/Project.h`), which also finds edits undone by hand.
- **Memory:** a helper's private working set, measured for the benchmarks (`src/host/core/Platform.cpp`); and DbgHelp set up once for the process, as JUCE sets it up too (`tests/AudioThreadMonitor.cpp`).

Some code works around the operating system, not JUCE: the tests' timers, which Windows 11 slows in a program none of whose windows can be seen (`keepTimerResolution()`, `tests/TestMain.cpp`), and waits that spin rather than yield on the audio thread, where Windows' scheduler can lose 16 ms (docs/realtime-audit.md, finding 24). PhaziteHost's timers are probably slowed the same way while none of its plug-ins' editors shows, which harms nothing: its audio threads wait on the bells Phazite rings, not on timers (not measured).

## Open questions

None now.

## Removed and settled

Numbers that no longer name an entry above, and what became of each (2026-11-11):
- **W7**, devices' windows kept above the main window by a timer that switched `setAlwaysOnTop()`: they float over it through JUCE 9's `TopLevelWindow::setAssociatedComponent()` (a correct pattern above).
- **Q1**, the app's key hook for plug-ins' own windows (`PluginKeys`' `NativeHook`): removed. Since every plug-in plays in a helper (roadmap A6), a plug-in's windows belong to the helper's thread, whose messages it never saw; the helper's own hook catches those keys and hands them back (`src/host/core/Editor.cpp`, `src/app/SandboxedEditor.*`).
- **Q2**, why the helpers aren't JUCE's `ChildProcessCoordinator`: W28.
- **Q3**, the keys given to a call-out's search field a message after it opens: a correct pattern above.
- **Q4**, the field a value is typed in without a right-click menu: the owner kept it so (a correct pattern above).
- **Q5**, the piano roll's transform bar ignoring a slider's late change: a correct pattern above.
- **Q6**, PhaziteHost's timers: Windows', not JUCE's, and harmless (the operating system's paragraph above).
