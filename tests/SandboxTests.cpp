// SPDX-License-Identifier: AGPL-3.0-or-later
// The sandbox (src/sandbox/, src/host/): a block channel's round trip, its deadline and
// a block the helper answers late; and the tests' helper program, started for real, hosting the tests' stand-ins: a
// plug-in opened from its description with its parameters by their own IDs, parameters' changes played at their
// samples, values set by hand and the plug-in's own changes and gestures, its state, an instrument's notes, latency, a
// sidechain, the text of a value, another sample rate, two instances side by side, a pipe kept up while quiet, a
// plug-in that hangs and one that crashes, a helper ended, and one that never connects to its pipe as it starts.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "host/SandboxTestHelpers.h"
#include "sandbox/core/BlockChannel.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

// Last, so that its macros can't change the headers above.
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <csignal>
 #include <sys/types.h>
#endif

namespace
{
namespace sandbox = phazite::sandbox;
using phazite::test::playBlock;
using phazite::test::TestHelper;
using Kind = phazite::test::TestPlugins::Kind;

// A name for a channel no other test uses at the same time.
std::string makeChannelName()
{
    static std::atomic<int> count { 0 };
    return "t" + std::to_string (juce::Time::currentTimeMillis() % 1000000) + "-" + std::to_string (++count);
}

// A parameter's index among an instance's, by its own ID.
int indexOf (const sandbox::RemoteInstance& instance, const juce::String& id)
{
    const auto& parameters = instance.getInfo().parameters;

    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].id == id)
            return (int) i;

    FAIL ("no parameter " << id);
    return -1;
}

void addChange (sandbox::RemoteInstance& instance, int index, int sample, float value)
{
    auto& info = instance.getChannel().getInfo();
    instance.getChannel().getChanges()[info.numChanges++] = { index, sample, value };
}

void addNote (sandbox::RemoteInstance& instance, int sample, int note, bool on)
{
    auto& info = instance.getChannel().getInfo();
    auto& event = instance.getChannel().getEvents()[info.numEvents++];
    event.sample = sample;
    event.size = 3;
    event.bytes[0] = on ? 0x90 : 0x80;
    event.bytes[1] = (std::uint8_t) note;
    event.bytes[2] = on ? 100 : 0;
}

// What the plug-in told of itself, as its listener heard it (on the pipe's thread).
struct Told final : sandbox::RemoteInstance::Listener
{
    std::mutex lock;
    std::vector<std::pair<int, float>> changes;
    std::vector<std::pair<int, bool>> gestures;
    std::atomic<int> latency { -1 }, states { 0 };

    void parameterChanged (int index, float value) override
    {
        const std::lock_guard held (lock);
        changes.emplace_back (index, value);
    }

    void gestureChanged (int index, bool starting) override
    {
        const std::lock_guard held (lock);
        gestures.emplace_back (index, starting);
    }

    void latencyChanged (int samples) override { latency = samples; }
    void stateChanged() override { ++states; }
    void parametersChanged (const std::vector<sandbox::ParameterInfo>&) override {}
    void editorResized (int, int) override {}
    void editorKey (bool, int, int, int) override {}
};

bool waitFor (const std::function<bool()>& condition, int milliseconds = phazite::test::hangTimeout)
{
    return phazite::test::dispatchMessagesUntil (condition, milliseconds);
}

// A variable of the tests' environment, which the helpers they start inherit, set while it lasts.
class EnvironmentVariable
{
public:
    EnvironmentVariable (const char* variableName, const char* value)
        : name (variableName)
    {
#if JUCE_WINDOWS
        _putenv_s (name, value);
#else
        setenv (name, value, 1);
#endif
    }

    ~EnvironmentVariable()
    {
#if JUCE_WINDOWS
        _putenv_s (name, "");
#else
        unsetenv (name);
#endif
    }

    EnvironmentVariable (const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator= (const EnvironmentVariable&) = delete;

private:
    const char* name;
};

// A thread of a test's, told to finish and joined however the test ends: a REQUIRE that throws while a std::thread can
// still be joined would end the whole test program (std::terminate) instead of failing the one test. Checks inside the
// thread are CHECKs, which never throw.
class JoinedThread
{
public:
    JoinedThread (std::atomic<bool>& finishFlag, std::function<void()> run)
        : finish (finishFlag),
          thread (std::move (run))
    {
    }

    ~JoinedThread() { join(); }

    void join()
    {
        finish = true;

        if (thread.joinable())
            thread.join();
    }

    JoinedThread (const JoinedThread&) = delete;
    JoinedThread& operator= (const JoinedThread&) = delete;

private:
    std::atomic<bool>& finish;
    std::thread thread;
};
} // namespace

TEST_CASE ("A block channel hands a block over and gets it back played, one round trip")
{
    const auto name = makeChannelName();
    auto engineSide = sandbox::BlockChannel::create (name, phazite::test::makeLimits());
    REQUIRE (engineSide != nullptr);
    auto helperSide = sandbox::BlockChannel::open (name, phazite::test::makeLimits());
    REQUIRE (helperSide != nullptr);

    // The helper doubles what comes in.
    std::atomic<bool> stop { false };
    JoinedThread helper (stop,
                         [&]
                         {
                             while (! stop)
                             {
                                 if (! helperSide->waitForBlock (10'000'000))
                                     continue;

                                 const auto& info = helperSide->getInfo();

                                 for (int channel = 0; channel < info.numChannels; ++channel)
                                     juce::FloatVectorOperations::multiply (helperSide->getChannel (channel), 2.0f, info.numSamples);

                                 helperSide->finish();
                             }
                         });

    for (int block = 0; block < 100; ++block)
    {
        auto& info = engineSide->getInfo();
        info.numSamples = 128;
        info.numChannels = 2;

        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill (engineSide->getChannel (channel), (float) block, 128);

        REQUIRE (engineSide->play (20'000, 1'000'000'000) == sandbox::BlockChannel::Played::done);
        CHECK (engineSide->getChannel (1)[127] == doctest::Approx (2.0f * (float) block));
    }

    helper.join();
}

TEST_CASE ("A block answered late stays the helper's: the next finds the channel busy until it is answered")
{
    const auto name = makeChannelName();
    auto engineSide = sandbox::BlockChannel::create (name, phazite::test::makeLimits());
    auto helperSide = sandbox::BlockChannel::open (name, phazite::test::makeLimits());
    REQUIRE (engineSide != nullptr);
    REQUIRE (helperSide != nullptr);

    std::atomic<bool> release { false };
    JoinedThread helper (release,
                         [&]
                         {
                             const auto got = helperSide->waitForBlock (-1);
                             CHECK (got);

                             while (! release)
                                 std::this_thread::sleep_for (std::chrono::milliseconds (1));

                             if (got)
                                 helperSide->finish();
                         });

    engineSide->getInfo().numSamples = 128;
    CHECK (engineSide->play (0, 2'000'000) == sandbox::BlockChannel::Played::late); // 2 ms
    CHECK (engineSide->isBusy());
    CHECK (engineSide->play (0, 2'000'000) == sandbox::BlockChannel::Played::busy);

    release = true;
    helper.join();
    CHECK_FALSE (engineSide->isBusy());
}

TEST_CASE ("A helper's process ID is taken only as that of the child Phazite started, running its program")
{
    // Known bug 181's review: the ID a helper told in its hello was put in the job that ends its processes with Phazite's,
    // whatever process it named.
    TestHelper helper;
    const auto id = helper.get().getProcessID();
    CHECK (id != 0);
    CHECK (sandbox::Helper::isOwnChild (id, phazite::test::getTestHost()));
    CHECK_FALSE (sandbox::Helper::isOwnChild (id, phazite::test::getTestHost().getSiblingFile ("NotTheHelper.exe")));
    CHECK_FALSE (sandbox::Helper::isOwnChild (0, phazite::test::getTestHost()));
    CHECK_FALSE (sandbox::Helper::isOwnChild (-1, phazite::test::getTestHost()));
    CHECK_FALSE (sandbox::Helper::isOwnChild (std::numeric_limits<std::int64_t>::max(), phazite::test::getTestHost()));
}

TEST_CASE ("A plug-in opens in a helper from its description, with its parameters by their own IDs, and plays a block")
{
    TestHelper helper;
    auto effect = helper.open (Kind::effect);
    const auto& info = effect->getInfo();
    CHECK (info.name == juce::String ("Test Effect"));
    CHECK_FALSE (info.isInstrument);
    CHECK (info.latencySamples == 0);
    CHECK (info.numInputChannels == 2);
    CHECK (info.numOutputChannels == 2);
    CHECK_FALSE (info.hasSidechain);

    // Its own IDs, in its order, with their names and values.
    REQUIRE (info.parameters.size() == 2);
    CHECK (info.parameters[0].id == juce::String ("1"));
    CHECK (info.parameters[0].name == juce::String ("Gain"));
    CHECK (info.parameters[0].defaultValue == doctest::Approx (1.0f));
    CHECK (info.parameters[1].id == juce::String ("0"));
    CHECK (info.parameters[1].name == juce::String ("Tone"));
    CHECK (info.parameters[1].value == doctest::Approx (0.5f));

    REQUIRE (playBlock (*effect, 128, 0.5f) == sandbox::BlockChannel::Played::done);
    CHECK (effect->getChannel().getChannel (1)[100] == doctest::Approx (0.5f));
    CHECK (effect->getParameterText (0, 0.5f, phazite::test::hangTimeout) == std::optional (juce::String ("50 %")));

    // A plug-in it doesn't have: none, with why.
    juce::String error;
    auto missing = phazite::test::TestPlugins::getDescription (Kind::effect);
    missing.fileOrIdentifier = "PhaziteTestNothing";
    CHECK (sandbox::RemoteInstance::open (helper.get(), phazite::test::toXml (missing), {}, 48000.0, phazite::test::makeLimits(), error,
                                          phazite::test::hangTimeout)
           == nullptr);
    CHECK (error.isNotEmpty());
    helper.forget (*effect);
}

TEST_CASE ("A block's parameter changes play at their samples, and a value set by hand comes in by the next blocks")
{
    TestHelper helper;
    auto effect = helper.open (Kind::effect);
    const auto gain = indexOf (*effect, "1");

    // The gain halved from sample 64, as automation moves it.
    addChange (*effect, gain, 64, 0.5f);
    REQUIRE (playBlock (*effect, 128, 0.5f) == sandbox::BlockChannel::Played::done);
    const auto* played = effect->getChannel().getChannel (0);
    CHECK (played[63] == doctest::Approx (0.5f));
    CHECK (played[64] == doctest::Approx (0.25f));

    // A value set by hand, through the control channel: its blocks take it once the helper has it.
    effect->setParameter (gain, 0.1f);
    CHECK (waitFor (
        [&]
        {
            playBlock (*effect, 128, 1.0f);
            return std::abs (effect->getChannel().getChannel (0)[0] - 0.1f) < 0.001f;
        }));
    helper.forget (*effect);
}

TEST_CASE ("A plug-in's state goes to Phazite and back, and opens a new instance as it was")
{
    TestHelper helper;
    auto effect = helper.open (Kind::effect);
    addChange (*effect, indexOf (*effect, "1"), 0, 0.3f);
    REQUIRE (playBlock (*effect, 64, 1.0f) == sandbox::BlockChannel::Played::done);

    const auto state = effect->getState (phazite::test::hangTimeout);
    REQUIRE (state.has_value());
    CHECK (state->getSize() == 8); // its two values

    auto again = helper.open (Kind::effect, *state);
    CHECK (again->getInfo().parameters[0].value == doctest::Approx (0.3f));
    REQUIRE (playBlock (*again, 64, 1.0f) == sandbox::BlockChannel::Played::done);
    CHECK (again->getChannel().getChannel (0)[10] == doctest::Approx (0.3f));

    // Set from Phazite: the gain back to full.
    juce::MemoryBlock full;
    juce::MemoryOutputStream (full, false).writeFloat (1.0f);
    juce::MemoryOutputStream (full, true).writeFloat (0.5f);
    REQUIRE (again->setState (full, phazite::test::hangTimeout));
    REQUIRE (playBlock (*again, 64, 1.0f) == sandbox::BlockChannel::Played::done);
    CHECK (again->getChannel().getChannel (0)[10] == doctest::Approx (1.0f));
    helper.forget (*effect);
    helper.forget (*again);
}

TEST_CASE ("An instrument in a helper plays its notes from their samples, and tells of its own changes and gestures")
{
    TestHelper helper;
    auto synth = helper.open (Kind::synth);
    CHECK (synth->getInfo().isInstrument);
    CHECK (synth->getInfo().numInputChannels == 0);

    // A note from sample 32: silence before it.
    addNote (*synth, 32, 69, true);
    REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
    const auto* played = synth->getChannel().getChannel (0);
    CHECK (played[31] == 0.0f);
    CHECK (std::abs (played[40]) > 0.0f);

    // Note 127 makes the stand-in turn its own Level to 0.75, in a gesture held while the note is, as a control of its
    // window would.
    Told told;
    synth->setListener (&told);
    addNote (*synth, 0, 127, true);
    REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
    addNote (*synth, 0, 127, false);
    REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
    REQUIRE (waitFor (
        [&]
        {
            const std::lock_guard held (told.lock);
            return told.gestures.size() >= 2 && ! told.changes.empty();
        }));

    {
        const std::lock_guard held (told.lock);
        CHECK (told.gestures[0] == std::pair (0, true));
        CHECK (told.changes.back().first == 0);
        CHECK (told.changes.back().second == doctest::Approx (0.75f));
        CHECK (told.gestures[1] == std::pair (0, false));
    }

    synth->setListener (nullptr);
    helper.forget (*synth);
}

TEST_CASE ("A block that asks for a reset plays from silence: what the plug-in still sounded with stops before it")
{
    TestHelper helper;
    auto synth = helper.open (Kind::synth);
    const auto* played = synth->getChannel().getChannel (0);

    // A note held from the first block sounds on in the next.
    addNote (*synth, 0, 69, true);
    REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
    REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
    CHECK (juce::FloatVectorOperations::findMaximum (played, 128) > 0.1f);

    // Reset as the block starts (playback starting from a stop): silent.
    synth->getChannel().getInfo().reset = 1;
    REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
    synth->getChannel().getInfo().reset = 0;
    CHECK (juce::FloatVectorOperations::findMaximum (played, 128) == 0.0f);
    CHECK (juce::FloatVectorOperations::findMinimum (played, 128) == 0.0f);

    helper.forget (*synth);
}

TEST_CASE ("Where every note is to end, the helper ends each note it gave the plug-in, which needn't take the controller for it")
{
    // The stand-in takes no controller, as a VST3 that maps none to a parameter gets none from JUCE's VST3 host: all notes
    // off (123) and all sound off (120) end its notes by the note-offs the helper puts before them.
    TestHelper helper;
    auto synth = helper.open (Kind::synth);
    const auto* played = synth->getChannel().getChannel (0);

    for (const auto controller : { 123, 120 })
    {
        addNote (*synth, 0, 69, true);
        addNote (*synth, 0, 72, true);
        REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
        CHECK (juce::FloatVectorOperations::findMaximum (played, 128) > 0.1f);

        // The controller half way through a block: the notes sound until it, and no further.
        auto& info = synth->getChannel().getInfo();
        auto& event = synth->getChannel().getEvents()[info.numEvents++];
        event.sample = 64;
        event.size = 3;
        event.bytes[0] = 0xb0;
        event.bytes[1] = (std::uint8_t) controller;
        event.bytes[2] = 0;
        REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
        CHECK (juce::FloatVectorOperations::findMaximum (played, 64) > 0.1f);
        CHECK (juce::FloatVectorOperations::findMinAndMax (played + 64, 64) == juce::Range<float>());

        REQUIRE (playBlock (*synth, 128, 0.0f) == sandbox::BlockChannel::Played::done);
        CHECK (juce::FloatVectorOperations::findMinAndMax (played, 128) == juce::Range<float>());
    }

    helper.forget (*synth);
}

TEST_CASE ("A plug-in's latency is known as it opens and as it changes; a sidechain reaches its bus")
{
    TestHelper helper;
    auto delay = helper.open (Kind::latency);
    CHECK (delay->getLatencySamples() == 64);

    // Its sound 64 samples late.
    auto& channel = delay->getChannel();
    channel.getInfo().numSamples = 128;
    channel.getInfo().numChannels = 2;
    juce::FloatVectorOperations::clear (channel.getChannel (0), 128);
    channel.getChannel (0)[0] = 1.0f;
    REQUIRE (channel.play (0, 5'000'000'000) == sandbox::BlockChannel::Played::done);
    CHECK (channel.getChannel (0)[64] == doctest::Approx (1.0f));

    // Longer: told by the helper.
    addChange (*delay, indexOf (*delay, "40"), 0, 1.0f);
    REQUIRE (playBlock (*delay, 128, 0.0f) == sandbox::BlockChannel::Played::done);
    CHECK (waitFor ([&] { return delay->getLatencySamples() == 128; }));

    auto sidechained = helper.open (Kind::sidechain);
    CHECK (sidechained->getInfo().hasSidechain);
    auto& keyed = sidechained->getChannel();
    keyed.getInfo().numSidechainChannels = 2;
    juce::FloatVectorOperations::fill (keyed.getSidechain (0), 0.25f, 128);
    juce::FloatVectorOperations::fill (keyed.getSidechain (1), -0.25f, 128);
    keyed.getInfo().numSamples = 128;
    keyed.getInfo().numChannels = 2;
    juce::FloatVectorOperations::fill (keyed.getChannel (0), 0.9f, 128);
    juce::FloatVectorOperations::fill (keyed.getChannel (1), 0.9f, 128);
    REQUIRE (keyed.play (0, 5'000'000'000) == sandbox::BlockChannel::Played::done);
    CHECK (keyed.getChannel (0)[50] == doctest::Approx (0.25f));
    CHECK (keyed.getChannel (1)[50] == doctest::Approx (-0.25f));
    helper.forget (*delay);
    helper.forget (*sidechained);
}

TEST_CASE ("A plug-in plays another channel at another sample rate, and two in one helper play side by side")
{
    TestHelper helper;
    auto first = helper.open (Kind::effect);
    auto second = helper.open (Kind::effect);

    REQUIRE (first->prepare (96000.0, phazite::test::makeLimits (1024), phazite::test::hangTimeout));
    REQUIRE (playBlock (*first, 1024, 0.5f) == sandbox::BlockChannel::Played::done);
    CHECK (first->getChannel().getChannel (1)[1000] == doctest::Approx (0.5f));

    // Both handed over, then both waited for.
    for (auto* instance : { first.get(), second.get() })
    {
        auto& info = instance->getChannel().getInfo();
        info.numSamples = 256;
        info.numChannels = 2;
        juce::FloatVectorOperations::fill (instance->getChannel().getChannel (0), 0.75f, 256);
        REQUIRE (instance->getChannel().handOver());
    }

    for (auto* instance : { first.get(), second.get() })
    {
        REQUIRE (instance->getChannel().waitForAnswer (0, 5'000'000'000) == sandbox::BlockChannel::Played::done);
        CHECK (instance->getChannel().getChannel (0)[200] == doctest::Approx (0.75f));
    }

    helper.forget (*first);
    helper.forget (*second);
}

TEST_CASE ("A plug-in that hangs makes its blocks late, not Phazite's: they skip until it answers again")
{
    TestHelper helper;
    auto hanging = helper.open (Kind::hang);
    const auto hang = indexOf (*hanging, "30");

    // It hangs in its block: late by the deadline, then busy, the helper still there.
    addChange (*hanging, hang, 0, 1.0f);
    CHECK (playBlock (*hanging, 128, 0.5f, 20'000'000) == sandbox::BlockChannel::Played::late);
    CHECK (playBlock (*hanging, 128, 0.5f, 20'000'000) == sandbox::BlockChannel::Played::busy);
    CHECK (helper.get().isRunning());

    // Let go of (its parameter set by hand, which its message thread takes): it answers, and plays on.
    hanging->setParameter (hang, 0.0f);
    REQUIRE (waitFor ([&] { return ! hanging->getChannel().isBusy(); }));
    CHECK (playBlock (*hanging, 128, 0.5f) == sandbox::BlockChannel::Played::done);
    helper.forget (*hanging);
}

TEST_CASE ("A plug-in that crashes ends its helper alone, which Phazite hears of once; one ended is lost too")
{
    {
        TestHelper helper;
        auto crashing = helper.open (Kind::crash);
        addChange (*crashing, indexOf (*crashing, "20"), 0, 1.0f);
        CHECK (playBlock (*crashing, 128, 0.5f, 2'000'000'000) == sandbox::BlockChannel::Played::late);
        REQUIRE (waitFor ([&] { return helper.getLosses() > 0; }));
        CHECK_FALSE (helper.get().isRunning());
        CHECK (helper.getLosses() == 1);
        helper.forget (*crashing);
    }

    // A helper ended, as one that hangs is, with an instance in it, after six quiet seconds (the pipe stays up meanwhile:
    // a read that timed out once ended it).
    TestHelper helper;
    auto effect = helper.open (Kind::effect);
    juce::Thread::sleep (6000); // a lower bound only: at least six quiet seconds, however many more
    CHECK (helper.get().isRunning());
    REQUIRE (playBlock (*effect, 128, 0.5f) == sandbox::BlockChannel::Played::done);

    helper.get().kill();
    CHECK_FALSE (helper.get().isRunning());
    REQUIRE (waitFor ([&] { return helper.getLosses() > 0; }));
    CHECK (helper.getLosses() == 1);
    helper.forget (*effect);
}

TEST_CASE ("A helper that doesn't connect to its pipe as it starts is ended once its time is up, and lost at once if it ends")
{
    // Phazite's hello is a write, which waits for the helper to connect to the pipe: neither may leave it waiting for ever.
    // How long start() took, which fails.
    const auto fails = [] (const juce::File& program, int timeoutMilliseconds)
    {
        sandbox::Helper helper (program);
        const auto from = juce::Time::getMillisecondCounterHiRes();
        CHECK_FALSE (helper.start (timeoutMilliseconds));
        const auto took = juce::Time::getMillisecondCounterHiRes() - from;
        CHECK_FALSE (helper.isRunning());
        return took;
    };

    // (A start() that waited for ever would hang here: a hang's limit, however busy the machine.)
    SUBCASE ("It hangs before it connects: ended once the time start() gives it is up")
    {
        const EnvironmentVariable hangs ("PHAZITE_TEST_HOST_HANGS_AT_START", "1");
        // Not before its time (however busy the machine); after it, at a hang's limit at the most (start() says only that it
        // failed, not why: the time is all there is to tell a wait for ever from it).
        const auto took = fails (phazite::test::getTestHost(), 2000);
        CHECK (took >= 2000.0);
        CHECK (took < phazite::test::hangTimeout);
    }

    SUBCASE ("It ends before it connects, as a helper that can't start does: lost at once")
    {
        // A program that ends at once on an argument it doesn't know, as the helper's is.
#if JUCE_WINDOWS
        const auto program = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getChildFile ("whoami.exe");
#else
        const juce::File program ("/bin/false");
#endif
        REQUIRE (program.existsAsFile());
        CHECK (fails (program, phazite::test::hangTimeout) < phazite::test::hangTimeout); // before its time is up
    }

    // The tests' helper starts as before.
    sandbox::Helper helper (phazite::test::getTestHost());
    CHECK (helper.start (phazite::test::hangTimeout));
}

namespace
{
constexpr const char* sideVariable = "PHAZITE_TEST_PHAZITE_SIDE";
constexpr const char* sideLine = "phazite-side";

// Whether a process runs, by its ID; and one ended, by it.
bool isProcessRunning (std::int64_t id)
{
#if JUCE_WINDOWS
    auto* process = OpenProcess (SYNCHRONIZE, FALSE, (DWORD) id);

    if (process == nullptr)
        return false;

    const auto running = WaitForSingleObject (process, 0) == WAIT_TIMEOUT;
    CloseHandle (process);
    return running;
#else
    return ::kill ((pid_t) id, 0) == 0;
#endif
}

void endProcess (std::int64_t id)
{
#if JUCE_WINDOWS
    if (auto* process = OpenProcess (PROCESS_TERMINATE, FALSE, (DWORD) id); process != nullptr)
    {
        TerminateProcess (process, 1);
        CloseHandle (process);
    }
#else
    ::kill ((pid_t) id, SIGKILL);
#endif
}
} // namespace

// Phazite's side of a helper, for the case after this one, which runs it in a process of its own: a helper started, a
// plug-in opened in it (its channel's shared memory made), and the helper's message thread stopped for good (the hang
// stand-in's Stall), so that the pipe's loss can't quit it; then its process's ID and its channel's name written out,
// and nothing more until it is ended. Nothing in any other run.
TEST_CASE ("Phazite's side of a helper in a process of its own (for the case after it)")
{
    if (juce::SystemStats::getEnvironmentVariable (sideVariable, {}).isEmpty())
        return;

    TestHelper helper;
    auto hanging = helper.open (Kind::hang);
    hanging->setParameter (indexOf (*hanging, "31"), 1.0f);

    // Stalled once a request goes unanswered (the Stall is set before the helper's message thread takes it).
    while (hanging->getState (500).has_value())
        hanging->setParameter (indexOf (*hanging, "31"), 1.0f);

    std::printf ("%s %lld %s\n", sideLine, (long long) helper.get().getProcessID(), hanging->getChannel().getName().c_str());
    std::fflush (stdout);

    for (;;)
        juce::Thread::sleep (1000);
}

TEST_CASE ("Phazite's side ended by force leaves neither a helper nor its shared memory behind, whatever the helper does")
{
    // The audit of 2026-11-13: a helper quit only through its message thread as the pipe went, so one that a plug-in kept
    // outlived a Phazite that crashed, and POSIX shared memory was unlinked only by Phazite as it closed.
    juce::ChildProcess side;
    {
        const EnvironmentVariable variable (sideVariable, "1");
        const auto program = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
        REQUIRE (side.start (juce::StringArray { program, "--test-case=Phazite's side of a helper in a process of its own*" },
                             juce::ChildProcess::wantStdOut));
    }

    const juce::ScopeGuard endedAnyway { [&side] { side.kill(); } }; // however the case ends

    // Its line, read a byte at a time (a read of more waits for all of it), within a hang's limit: past that, the side is ended,
    // which ends the read.
    juce::String line;
    std::atomic<bool> read { false };
    std::atomic<bool> timedOut { false };
    JoinedThread minute (read,
                         [&]
                         {
                             const auto until = juce::Time::getMillisecondCounterHiRes() + phazite::test::hangTimeout;

                             while (! read && juce::Time::getMillisecondCounterHiRes() < until)
                                 juce::Thread::sleep (20);

                             if (! read)
                             {
                                 timedOut = true;
                                 side.kill();
                             }
                         });

    for (char byte = 0; side.readProcessOutput (&byte, 1) > 0;)
    {
        if (byte != '\n')
            line += juce::String::charToString ((juce::juce_wchar) (unsigned char) byte);
        else if (line.startsWith (sideLine))
            break;
        else
            line.clear();
    }

    minute.join();
    REQUIRE_MESSAGE (! timedOut, "Phazite's side wrote no line within a minute");
    const auto words = juce::StringArray::fromTokens (line.trim(), " ", "");
    REQUIRE_MESSAGE (words.size() == 3, "Phazite's side wrote: " << line);
    const auto helperID = words[1].getLargeIntValue();
    const auto channelName = words[2].toStdString();
    REQUIRE (helperID > 0);
    REQUIRE (isProcessRunning (helperID));

    // Ended by force, as a crash ends it: its helper goes with it, and nothing is left of its channel.
    REQUIRE (side.kill());
    const auto gone = waitFor ([helperID] { return ! isProcessRunning (helperID); });
    CHECK (gone);

    // Once Phazite's side has gone too: ended by force, it keeps the memory until Windows has taken its process apart, which
    // the helper's end can come before (under MSVC's AddressSanitizer it did, the nightly run of 2026-11-15).
    CHECK (waitFor ([&channelName] { return sandbox::SharedMemory::open (channelName, 64) == nullptr; }));

    if (! gone)
        endProcess (helperID);
}
