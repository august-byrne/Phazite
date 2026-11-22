// SPDX-License-Identifier: AGPL-3.0-or-later
// A Phazite project: a folder that holds the project file and everything the project uses.
#pragma once

#include "model/base/BackgroundJobs.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/WholeState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace phazite
{
class TrackRender;

// A new song, as a new project's starts: its tempo map, its master and one empty track, numbered by the song itself
// (Schema.h).
juce::ValueTree makeNewSong();

// A new song, as a new project's starts (Project::create() makes one in a project folder): its tempo map, its track list
// and one empty track, with no undo steps. Its file, if given, is where its audio files' paths start from. For a song
// outside a project: tests' songs, and what is rendered alone.
Song::Ptr createSong (const juce::File& file = {});

// Where new projects go unless the user picks another folder: Documents/Phazite/Projects.
juce::File getDefaultProjectsFolder();

// A song made of a state (Schema.h: PHAZITE, which it takes), with what every song has added (its tempo map, its track list
// with the master, its next ID), as a project opens. Its file, if given, is where its audio files' paths start from, and
// its cache folder where its frozen tracks' files are. No undo steps. Nothing for a state that isn't a song. Once it is
// consistent, its clips with their notes and controllers' values leave the state for values beside it (Song::holdValues()):
// the state given holds no CLIP element then. Message thread.
Song::Ptr makeSong (const juce::ValueTree& state, const juce::File& file = {}, const juce::File& cacheFolder = {});

// The furthest place in beats a song holds anything at, as a file from elsewhere is taken in: a million beats, days of music
// at any tempo (Schema.h: schema::maxBeats, which the song's ranges are described with).
inline constexpr double maxSongBeats = schema::maxBeats;

// What makeSong() does first, for a song whose file anyone could have written (Phazite takes in projects from others): it
// repairs only what no Phazite could have written, and changes nothing in a song Phazite saved, whichever build or
// computer saved it. First, before any number of the song is read, a number whose exponent JUCE's reader would overflow
// made one it reads (makeExponentsReadable()); and each switch and ID another tool wrote in another form ("true", "false",
// "12.0") written as Phazite writes it, as it plays, so that the versions see no change where there is none. Each element
// of a type Phazite knows where Schema.h puts it (one somewhere else goes), and one of a type
// it doesn't know left as it is, with everything in it, to be written back (docs/project-format.md); a device or modulator
// of a type Phazite has; every number Schema.h gives a range brought within it (a channel's volume, a tempo, a note's
// pitch, places in beats up to a million), and every parameter's value set by hand and automation point within its
// parameter's range; every item's ID its own and below 2^62, and in a song whose IDs reach 3 * 2^60, every one at 2^61 or
// past it given anew, what named it following, the song stamped with its new numbering (Schema.h: renumberMark,
// idNumbering); a lane, a curve or a modulation of something the song hasn't
// left out, as a sidechain or a pad's sampler that isn't there, and a sidechain the model would refuse, a device hearing
// its own track's sound round a loop (model/mixer/internal/Sidechain.h: forgetSidechainLoops()); no two elements of a parent with one key (Schema.h:
// getElementKey()); and, for a song with a file, a sound file on a network share, or one that leaves the project's folder
// with "..", looked for by its name in the project's Audio folder (a full path of a computer's stays: Phazite writes one
// where a file couldn't be copied in). No undo steps.
// A song read in part (the versions' song of a commit some of whose pieces couldn't be read, with stand-ins for them:
// model/versions/internal/SongPieces.h, joinSong()) keeps what names an ID it hasn't, which may be in what wasn't read (a
// send to a return, a curve or a modulation of a device in a lost piece): the song it is taken into, which knows, removes
// what names nothing there (model/versions/internal/ApplySong.h: removeBrokenReferences()). For any thread, on a song that
// belongs to that thread alone and to no session: nothing here asks a plug-in's instance (a plug-in's curve is kept within
// 0 to 1 from its element) nor anything else of the message thread's.
enum class ReadAs
{
    whole,
    inPart
};

void makeConsistent (Song&, ReadAs = ReadAs::whole);

// The same, keeping to the ranges, words and hexadecimal ids of the description's tables given (Schema.h:
// schema::getTables()), rather than the description's own: for a test, which changes one in a copy and sees the loader
// follow (tests/SchemaTests.cpp).
void makeConsistent (Song&, ReadAs, const schema::Tables&);

// The properties makeConsistent() takes as switches, by their element's type (Schema.h: "bool", or "true, or none"):
// whether Phazite leaves one that is off out (none), as their setters do, rather than writing it false; and what one that
// is none means, as Schema.h gives its default (a switch that is on unless it says otherwise can't be left out when off).
// A switch written in another form ("true", "false") is written as Phazite writes it, its value the same.
struct SwitchProperty
{
    const schema::Element* type;
    const schema::PropertyDescription* property;
    bool offIsNone, noneIsOn;
};

const std::vector<SwitchProperty>& getSwitchProperties();

// Each number a song's element holds whose exponent JUCE's reader of numbers would overflow as it reads it (an int's worth
// of digits or more: model/base/Json.h, hasExponentTooLongToRead(); known bug 170) made the same number in a text it reads,
// one past every double's or 0, which the ranges then take as no finite number: of the properties Schema.h says hold
// numbers (those it gives ranges, values set by hand, points' values, steps and slices), nothing else, so that a name
// stays as it is. What makeConsistent() does first; and what the versions' pieces get as they are joined into a song.
void makeExponentsReadable (juce::ValueTree song);

// Whether a text is the one Phazite writes for the number it reads as (model/base/internal/SongText.h: writeValue(), the
// number as readValueNumber() reads it, the loader's reading of a number as every reader's), told from the text alone: a
// whole number's digits (up to 15, no leading zero, not "-0"), or a decimal from 0.001 up of up to six significant digits,
// without an exponent or a leading or trailing zero. False for anything else, which the loader compares by writing the
// number (isSameValue()). For the loader and its tests.
bool isNumberWrittenAsPhazite (const juce::var&);

// Whether an entry of a project's folders is a link that leads elsewhere, which a copy of the project leaves out with what
// it leads to (a symbolic link or a junction: anywhere, or round to the project itself), given what its isSymbolicLink() and
// getLinkedTarget() said. On Windows isSymbolicLink() is true for any reparse point, a folder OneDrive or Dropbox keeps
// among them, whose target is itself: that is the user's, and copied. JUCE workaround W35 (docs/juce-workarounds.md).
bool isLinkElsewhere (const juce::File& entry, bool isSymbolicLink, const juce::File& linkedTarget);

// Whether a folder is a copy a first save or Save As left when Phazite stopped in the middle of it, which the next one into
// that folder moves to the system's trash (Project::moveTo()): named "<name> (saving)", with Phazite's marker in it
// (Project::copyMarkerName) from before this run started, and no project file in it, so never a project the user named so.
bool isLeftoverCopy (const juce::File& folder);

// How a leftover copy is removed, for the tests, which delete it so that a test run fills nobody's trash: null, by moving it
// to the system's trash. Set on the message thread; a removal already running keeps the way it began with.
void setLeftoverCopyRemoval (std::function<bool (const juce::File&)> removal);

//==============================================================================
// The project folder:
//   <name>.phazite   the project file: the song (Schema.h)
//   Audio/           recordings and imported audio
//   Generated/       sounds made by Phazite's generators
//   Renders/         exports: the song, and its stems
//   Backups/         earlier versions of the project file, kept by age (backUpProjectFile()), and the autosave
//   Cache/           frozen tracks' files (model/mixer/internal/Freeze.h) and renders of stretched audio clips (the song's
//                    cache folder). Closing the project deletes what nothing in it uses any more. Only the clips' renders
//                    can always be made again: a frozen track's file may be all that is left of a plug-in this computer
//                    hasn't, so a copy of the project never leaves one behind.
//   Versions/        the versions of the song the user saved, and their branches (model/versions/; Schema.h:
//                    schema::versions), once there is one
//
// Create, open and destroy Projects on the message thread.
class Project : private juce::ValueTree::Listener,
                private SongValues::Listener,
                private juce::AsyncUpdater,
                private juce::Timer
{
public:
    static constexpr const char* fileExtension = ".phazite";

    // Creates the folder <parentFolder>/<name> with a new project in it, and saves it. addContent, if given, adds what the
    // project starts with (the starter song, an outline's sections): it goes into that first save, so the project starts
    // without undo steps or unsaved changes. Returns nullptr and sets error if that folder already exists or can't be
    // written.
    static std::unique_ptr<Project> create (const juce::File& parentFolder, const juce::String& name, juce::String& error,
                                            const std::function<void (Song&)>& addContent = {});

    // Opens a saved project. With restoreAutosave it opens the autosaved changes instead (see hasNewerAutosave()); saving
    // then still writes to the project file. An autosave that can't be read gives way to the project file, as it was last
    // saved, and notRestored, if given, says so (known bug 87: the project's own file wasn't even tried).
    static std::unique_ptr<Project> open (const juce::File& projectFile, bool restoreAutosave, juce::String& error,
                                          juce::String* notRestored = nullptr);

    // Opens a project whose file can't be read with a song from elsewhere (its newest backup that reads, or its branch's last
    // commit: model/versions/internal/VersionHistory.h, readLastCommitSong()), as the project's, which a save then writes
    // into its file: unsaved until then. Nothing, with error set, for a state that isn't a song.
    static std::unique_ptr<Project> openWithSong (const juce::File& projectFile, const juce::ValueTree& song, juce::String& error);

    // The newest of the project's backups and its autosave that reads as a song, and the song; none for none. Other files
    // in Backups/ (another name's autosave, a file put there) are not the project's (backUpProjectFile()).
    static std::pair<juce::File, juce::ValueTree> readNewestBackup (const juce::File& projectFile);

    // True if the project has an autosave from a session that ended without saving or closing the
    // project, such as a crash: saving and closing both delete the autosave.
    static bool hasNewerAutosave (const juce::File& projectFile);

    static juce::File getAutosaveFile (const juce::File& projectFile);
    static juce::File getCacheFolder (const juce::File& projectFile) { return projectFile.getSiblingFile ("Cache"); }

    // Closing a project discards its autosave: the user has saved, or chosen not to.
    ~Project() override;

    Song& getSong() const noexcept { return *song; }
    const juce::File& getFile() const noexcept { return file; }
    juce::File getFolder() const { return file.getParentDirectory(); }
    juce::File getAudioFolder() const { return getFolder().getChildFile ("Audio"); }
    juce::File getGeneratedFolder() const { return getFolder().getChildFile ("Generated"); }
    juce::File getRendersFolder() const { return getFolder().getChildFile ("Renders"); }
    juce::File getBackupsFolder() const { return getFolder().getChildFile ("Backups"); }
    juce::File getCacheFolder() const { return getCacheFolder (file); }

    // Whether the project holds anything it didn't hold when it was opened (with Phazite's parts added) or last saved,
    // leaving out what isn't a change (isUncountedProperty()). Compares the project with a copy at most once after each
    // change. Message thread only.
    bool hasUnsavedChanges();

    // As hasUnsavedChanges(), with its plug-ins' settings taken from them as a save takes them and the whole song compared
    // with what was saved, rather than trusting what the plug-ins said: many say their settings changed when nothing did
    // (a VST3's setDirty() as its window opens). What closing the project asks before it asks to save; none found, there
    // are none from then on. Message thread only.
    bool checkUnsavedChanges();

    // Saves to the project file. A copy of the previous version goes into Backups/ first (backUpProjectFile()), and the
    // new file only replaces the old one once it has been written completely. What is being tried in place (Swap) is kept
    // first, as its own undo step (model/commands/internal/Undo.h: Trial).
    juce::Result save();

    // Told, on the message thread, when a save (save(), or Auto Save's) couldn't make its backup, which the save goes on
    // without: once, until a backup is made again (known bug 40).
    std::function<void (const juce::String& message)> onBackupFailed;

    // Where the song stands in its versions (Schema.h: versionsBranch, versionsBase), written into the project file as the
    // versions move them (versionsMoved(), below): told, on the message thread, when that write failed, once until one
    // works; the next save, Auto Save's too, writes them then, and the autosave holds them. And whether the file holds them as
    // the song has them (nothing waits to be written, nor is being written), for tests.
    std::function<void (const juce::String& message)> onVersionsNamesFailed;
    bool hasVersionsNamesWritten() const;

    // Writes the autosave if anything changed since the last save or autosave.
    juce::Result autosave();

    // Whether anything changed since the last save or autosave: what autosave() would write. Message thread only.
    bool hasChangesToAutosave();

    // As autosave(), with the state copied here and its XML made and written on a background thread, so that the window
    // and what is played don't wait for it (docs/archive/performance-audit.md, U2). Nothing while one is being written. Whether the
    // last one failed is told once, by takeAutosaveFailure(), and its changes are written again at the next.
    void autosaveInBackground();
    bool isAutosaving() const;
    bool takeAutosaveFailure();

    // Auto Save (AutosaveTimer::setAutoSave()): as save(), into the project file, with the state copied here and its XML
    // made and written on a background thread as autosaveInBackground() writes, so that the window and what plays don't wait
    // for it; the previous file into Backups/ at most every backupMinutes (the first one at once), so that the backups keep
    // the project as it was then and not each second of it. Nothing while a write is under way, or with nothing to save.
    // Then, once isAutosaving() is false, finishBackgroundSave() takes in what was written: the project is saved, unless it
    // changed since it was copied; false if the write failed, which leaves its changes to save again. And how many changes
    // the project has had, to tell when they stop.
    void saveInBackground();
    bool hasBackgroundSave() const noexcept { return backgroundSave.tree.isValid(); }
    bool finishBackgroundSave();
    juce::int64 getChangeCount() const noexcept { return changeCount; }
    static constexpr int backupMinutes = 10;

    // When the autosave was last written, whole, for the window to say so; none since the project opened or last saved
    // (which deletes it). Message thread only.
    std::optional<juce::Time> getLastAutosave() const;

    // Saves the project as it is now, unsaved changes included, as a new project <parentFolder>/<name>
    // with copies of its Audio, Generated and Renders folders. This project and
    // its file stay as they are. Returns the new project file, or sets error and returns an empty
    // File (if that folder exists, for example).
    juce::File saveCopyAs (const juce::File& parentFolder, const juce::String& name, juce::String& error);

    // Saves the project, unsaved changes included, as <parentFolder>/<name> with copies of its Audio, Generated, Renders and
    // Cache folders and its versions, and goes on there with its undo history: the first save of a project kept with the
    // unsaved ones until then (model/project/internal/UnsavedProjects.h), which asks where it goes. The folders are copied
    // into "<name> (saving)" beside, which takes the project's folder's name once it is whole, and only then does the project
    // move there and save: until then it stays where it was, and nothing that looks like a project is left half made. The
    // folder it was in stays as it is, for its owner to delete once the project has closed (files in it may be open until
    // then). It calls back with what came of it, on the message thread, once: a failure changes nothing (that folder exists,
    // or something can't be written). The folders are copied on a background thread, however few bytes they hold (known
    // bug 90: the message thread copied gigabytes of recordings; and a small project's copy on the message thread waited
    // for the history's writes and failed at the first file another program held a moment), which this returns as a
    // TrackRender, with its progress, whose deletion cancels it, leaving nothing behind and without calling back. Nothing,
    // having called back with why, when the copy couldn't begin (the folder exists, or can't be made). Message thread; the
    // project must outlive what it returns.
    // The copy takes no link inside those folders, nor what it leads to (isLinkElsewhere()); a clip's render in the Cache
    // folder that goes or changes while it copies is left behind, as Phazite renders it again, and nothing else is (a frozen
    // track's file may be all that is left of a plug-in). Its "(saving)" folder holds a marker file (copyMarkerName), which
    // proves it Phazite's: the next first save or Save As into that folder moves one a run that crashed left there (marked
    // before this run started, with no project file in it) to the system's trash, in the background, and nothing else.
    // Its versions are copied last, by what writes them (setVersionsCopier()), so that nothing is written into the folder
    // the project leaves after they were copied (known bug 169); their bytes count with the rest's in the progress, and
    // Cancel stops their copy too. A file another program holds a moment (an antivirus, the indexer) is tried again for a
    // second or so before the save fails, saying which file.
    std::unique_ptr<TrackRender> moveTo (const juce::File& parentFolder, const juce::String& name,
                                         std::function<void (const juce::Result&)> onFinished);

    // How a first save copies the project's Versions folder, given by what writes into it, its song's history
    // (model/versions/internal/VersionHistory.h: makeVersionsCopier()). copy() runs what it is given on the writer's own
    // thread, after every write it was given before, holding every write asked for after until end(), and calls back on the
    // message thread with what came of it. end(), once the move is over, moved or not. Without one, the folder is copied
    // with the rest.
    struct VersionsCopier
    {
        std::function<void (std::function<juce::Result (const BackgroundJobs::ShouldStop&)>, std::function<void (const juce::Result&)>)>
            copy;
        std::function<void()> end;
    };

    void setVersionsCopier (VersionsCopier copier) { versionsCopier = std::move (copier); }
    static constexpr const char* copyMarkerName = ".phazite-saving";

    // For tests: while one is set, called on the copy's thread each time a first save's copy is about to try a file again
    // that couldn't be opened, read or written (another program holds it), with how many tries it made, so that a test can
    // let go of a file it holds after a few. None: nullptr. Any thread.
    static void setCopyRetryHook (std::function<void (int tries)>);

    // For tests: the time backups are made and thinned by, in milliseconds since 1970, which a test moves through days and
    // weeks without waiting (with each saved version's time, its file's modification time, set as it goes); 0: the
    // computer's clock. Message thread.
    static void setBackupTime (juce::int64 now);

private:
    Project (Song::Ptr, const juce::File&);

    Song::Ptr song;
    juce::ValueTree songState; // which this listens to (a listener belongs to the tree object it was added to)
    juce::File file;
    int changesOutsideHistory = 0; // the song's count of them as it was last compared (a plug-in's own changes)

    // While the song shows another (a version open: Song::isShowingOther()), what changes in it is none of the project's: its
    // changes aren't counted (changeCount), and the plug-ins' own changes made meanwhile are left out of the song's count of
    // them (getOwnChangesOutsideHistory()): those made while it showed another, and the song's count as it began to.
    int outsideWhileShowing = 0, outsideAtShowing = 0;
    int getOwnChangesOutsideHistory() const;

    class BackgroundWriter; // one file at a time; its destructor waits for the one under way
    std::unique_ptr<BackgroundWriter> autosaveWriter;
    VersionsCopier versionsCopier; // setVersionsCopier()'s

    // The copies a save that never finished left beside one, moved to the trash in the background as the next first save or
    // Save As goes there (Project.cpp: removeLeftoverCopies()).
    mutable BackgroundJobs leftoverRemovals { "Phazite leftover copies", 1, juce::Thread::Priority::low };
    bool changedSinceSave = false, changedSinceAutosave = false, isWriting = false;
    juce::int64 autosavedAt = 0; // when autosave() last wrote it, in milliseconds since 1970; 0 for none

    // The song as it was at a moment: a copy of its tree, and what it kept beside its tree then (model/base/internal/
    // WholeState.h), which copyWhole() makes the song whole of. Never the whole song itself: the copy's tree is then of the
    // same shape as the song's (holding no more than the song's own tree holds), which the comparisons below find an element
    // in by its place and compare with the song's, its clips and their notes compared as values (WholeState.h:
    // holdSameTrackClips()).
    struct SongCopy
    {
        juce::ValueTree tree;
        ValuesKept values;
    };

    SongCopy copySong() const; // the song as it is now

    // A save in the background: what it writes, and the counts of the changes as it was copied; when the project file was
    // last backed up (milliseconds since 1970); and the changes counted since the project opened.
    SongCopy backgroundSave;
    juce::int64 changesAtBackgroundSave = 0, backedUpAt = 0, changeCount = 0;
    int outsideAtBackgroundSave = 0;

    // The song as it was when the project was opened, with Phazite's parts added, or last saved. JUCE notifies a value
    // written again as another type, which changes nothing. So while the project has no unsaved changes, a notification
    // only sets mayHaveChanged, and hasUnsavedChanges() and autosave() compare the song with this copy, once. Never listen
    // to it or change it. Only what notified is compared (docs/archive/performance-audit.md, U3): a property that notified
    // with its copy's, and an element whose elements were added, removed or moved whole, each found in the copy at the same
    // place among the elements that count; after many notifications, the track (or the song's element) each was in, whole.
    // No song at all while the project file doesn't hold the song that was opened (a restored autosave, or a song from
    // elsewhere: forgetSavedState()): then the song is unsaved, whatever it is compared with, until it is saved.
    SongCopy savedState;
    bool mayHaveChanged = false;

    struct ToCompare
    {
        juce::ValueTree element;
        juce::Identifier property; // none: the element whole
    };

    // An element noted here and then taken out of the song stays alive with what it holds until the next comparison (the
    // song's undo history keeps it anyway, for an undo to put back), and is no longer compared: where it was taken from
    // notified, and is compared whole. Bounded: at most mostToCompare notes, and past them only what is still in the song,
    // each track or song's element once (dropping those taken out is for memory only: they compare as the same).
    std::vector<ToCompare> toCompare;
    bool comparingWhole = false; // past mostToCompare: each one's track, or the song's element, whole
    void clearToCompare();

    // The clips whose notes and controllers' values changed (the song holds them beside its tree: SongValues.h), compared
    // with savedState's values when asked, as an element that notified is (holdSameNotes()). A clip taken out of the song
    // compares as its track does, which notified.
    std::vector<ClipID> notesToCompare;
    void clipNotesChanged (const std::vector<ClipID>&) override;

    // What the values' listener told of the clips (SongValues.h: Listener::clipsChanged()), compared with savedState when
    // asked: each clip changed in place (a property, a warp marker), with the track it was told on, against the saved copy's
    // clip of its ID (holdSameClip()); and each track a clip was added to, taken out of or moved on, and each whose clips came
    // in another order, whole (holdSameTrackClips()). Bounded as toCompare is: past mostToCompare clips, the track each was
    // on, whole. A clip gone compares as its track does; a track gone as what it was taken out of, which notified.
    std::map<ClipID, TrackID> clipsToCompare;
    std::set<TrackID> tracksToCompare;
    bool comparingClipsWhole = false; // past mostToCompare: each one's track whole
    void clipsChanged (const std::vector<SongValues::ClipChange>&, const std::vector<TrackID>& reordered) override;

    // The tempo map changed (SongValues.h: Listener::tempoMapChanged(); the tree's callbacks below leave its elements alone,
    // isHeardAsKept()): compared with savedState's when asked, as one value (WholeState.h: holdSameTempoMap()), so that
    // a tempo changed and changed back is saved again.
    bool tempoToCompare = false;
    void tempoMapChanged() override;

    // The chord lane changed (SongValues.h: Listener::chordLaneChanged(); the tree's callbacks below leave what is in its
    // CHORDLANE alone, isHeardAsKept(), and mark the element's own coming and going as any element's): compared with
    // savedState's when asked, as one value with whether each has a lane (WholeState.h: holdSameChordLane()), so that a chord
    // changed and changed back is saved again.
    bool chordLaneToCompare = false;
    void chordLaneChanged() override;

    // The curves that changed, by their parameters (SongValues.h: Listener::curvesChanged(); the tree's callbacks below leave
    // the curves' elements alone, isHeardAsKept()): compared with savedState's when asked, each as a value, and the curves'
    // order with it (WholeState.h: holdSameCurve(), holdSameCurveOrder()), so that a point moved and moved back is saved
    // again. Bounded as toCompare is: past mostToCompare of them, every curve (holdSameCurves()).
    std::vector<ParameterRef> curvesToCompare;
    bool comparingCurvesWhole = false;
    void curvesChanged (const std::vector<ParameterRef>&) override;
    void clearCurvesToCompare(); // clearToCompare()'s

    // The MIDI links changed (SongValues.h: Listener::midiLinksChanged(); the tree's callbacks below leave what is in its
    // MIDILINKS alone, isHeardAsKept(), and mark the element's own coming and going as any element's): compared with
    // savedState's when asked, as one value with whether each has a MIDILINKS (WholeState.h: holdSameMidiLinks()), so that a
    // link taken away and made again as it was is saved again.
    bool midiLinksToCompare = false;
    void midiLinksChanged() override;

    // The sections changed (SongValues.h: Listener::sectionsChanged(); the tree's callbacks below leave what is in its
    // SECTIONS alone, isHeardAsKept(), and mark the element's own coming and going as any element's): compared with
    // savedState's when asked, as one value with whether each has a SECTIONS (WholeState.h: holdSameSections()), so that a
    // section changed and changed back is saved again.
    bool sectionsToCompare = false;
    void sectionsChanged (const std::vector<SectionID>&) override;

    // What each area marked compared with savedState's values (compareWithSavedState()'s list, a line for each area): true
    // where nothing of it was marked.
    bool areNotesAsSaved() const;
    bool areClipsAsSaved() const;
    bool isTempoMapAsSaved() const;
    bool isChordLaneAsSaved() const;
    bool areCurvesAsSaved() const;
    bool areMidiLinksAsSaved() const;
    bool areSectionsAsSaved() const;

    // Whether a clip of the song is as savedState holds it, all it holds (its properties, its track, its warp markers, its
    // notes and controllers' values), and whether a track holds the clips savedState's does, in the same order: the song's
    // values against savedState.values, by their IDs (WholeState.h: holdSameClip(), holdSameTrackClips()). A clip gone, or on
    // a track compared whole anyway, compares as that track does.
    bool holdSameClip (ClipID, const ValuesKept& now) const;
    bool holdSameTrackClips (TrackID, const ValuesKept& now) const;
    bool isComparedWhole (const juce::ValueTree&) const; // it, or what it is in, is among toCompare whole

    void setSavedState (SongCopy);

    // What a store of the plug-ins' states wrote into the song without notifying (storeForSaving() while isWriting) where
    // the project file doesn't get it (Save a Copy As, an autosave, a save whose write fails): the tracks and the master
    // compared whole at the next check, as a plug-in may have changed its state without saying so. Kept while the project
    // has unsaved changes, for a save in the background that may yet make an older copy the saved one.
    void compareStoredStates();
    bool isAsSaved (const ToCompare&) const;
    juce::ValueTree findSavedCopy (const juce::ValueTree& element) const;

    // Makes the folder <parentFolder>/<name> with copies of the project's Audio, Generated, Renders and Cache folders, and
    // of its Versions if it keeps any, as moveTo() does, and returns the project file that goes in it. If the name is empty,
    // the folder exists or something can't be written, sets error and returns nothing, leaving nothing behind.
    juce::File makeFolderWithFiles (const juce::File& parentFolder, const juce::String& name, juce::String& error) const;
    void setFile (const juce::File&); // the project file from now on, the song's too

    // The end of moveTo(): its copy put in the folder's place, and the project saved there.
    class FolderCopy;
    juce::Result finishMoveTo (const juce::File& copy, const juce::File& folder);

    // The project file into Backups/, unless the newest backup holds the same, named and dated by when that version was
    // saved (its file's modification time): "<project> YYYY-MM-DD HH-MM-SS.phazite", and "_2", "_3"... after the time for
    // more of one second, which come after it. Only files so named are the project's backups, and only they are compared
    // and thinned: anything else there stays. Then the backups thinned by age, as Local History is (model/base/KeptByAge.h:
    // every one of the last two hours, the newest of each hour for a day, the newest of each day for 30 days), and the
    // newest of those older than that kept as well: a version's time is when it was saved, so the file a project opened
    // with after a month away is already older than that as it is backed up, and stays the one the project can go back to.
    // The autosave is no backup and stays.
    void backUpProjectFile();
    bool backupFailed = false; // told until a backup is made again

    // Where the song stands in the project's versions (Schema.h: versionsBranch, versionsBase), which the versions move without
    // an undo step and without an unsaved change (a version saved, a branch made or switched to): written into the project
    // file in the background (namesWriter: the file as it was saved but for them, so no backup is made of it), while the song
    // has no unsaved change, once no other write of the file is under way; while it has one, by the next save, and by the
    // next autosave (marked as a change to autosave). A write that fails leaves them pending (namesPending), which the next
    // save (Auto Save's too) writes, and is told once (onVersionsNamesFailed). So that the file always says which version
    // the song in it grew from, and an untouched project stays saved. Written at once as the project closes, if it still
    // waits. Message thread.
    std::unique_ptr<BackgroundWriter> namesWriter;
    bool namesPending = false, namesWriting = false, namesFailed = false;
    void versionsMoved();
    void writeVersionsNames();
    bool takeNamesWritten(); // a names write under way finished, before another write or copy of the file: pending if it failed
    void tellNamesFailed();  // once, until a write of them works
    void handleAsyncUpdate() override;
    void timerCallback() override;           // a names write under way, or one waiting for another write of the file, looked at
    void rememberSavedState();               // the song as it is now
    void rememberSavedState (SongCopy copy); // or a copy of it taken since its last change (what save() wrote)
    void forgetSavedState();                 // the project file doesn't hold the song: unsaved until a save
    void compareWithSavedState();
    void markChanged (const juce::ValueTree&, const juce::Identifier& property = {});
    void markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child);

    // What isn't a change (Schema.h: schema::isUncountedProperty() and isUncountedElement(): view state, how the song plays,
    // its next ID) is left out of notifications and of the comparison with savedState alike. A plug-in's own change (its
    // settings, which a save stores in the song) counts, as the song counts it (Song::markChangedOutsideHistory()). Each
    // element with what its song kept beside its tree (a track's clips and their notes: holdSameTrackClips()). The tempo map
    // as one value, as compareWithSavedState() compares it (holdSameTempoMap()), not its elements in their order; the chord
    // lane likewise, with whether each has one (holdSameChordLane()); the curves as values, in their order
    // (holdSameCurves()), not their elements; and the MIDI links as one value, with whether each has a MIDILINKS
    // (holdSameMidiLinks()).
    static bool holdTheSameProject (const juce::ValueTree&, const ValuesKept&, const juce::ValueTree&, const ValuesKept&);

public:
    // How many elements the comparisons with the saved copies looked at since the program started (each element compared
    // whole counts, with each of its elements): what the performance tests count (tests/PerformanceGateTests.cpp). Message
    // thread.
    static juce::int64 countElementsCompared();

    // How many notifications since the last comparison are compared one by one; past them, each one's track whole.
    static constexpr size_t mostToCompare = 64;

    // For tests: what the comparison of the whole song with the saved copy says, which hasUnsavedChanges() works out
    // comparing only what notified since. Message thread.
    bool isWholeAsSaved() const { return holdTheSameProject (song->getState(), keepValues (*song), savedState.tree, savedState.values); }

private:
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (! isHeardAsKept (tree)) // the areas': tempoMapChanged(), chordLaneChanged(), curvesChanged(), midiLinksChanged()
            markChanged (tree, property);
    }

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override { markChildChanged (parent, child); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { markChildChanged (parent, child); }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override
    {
        if (! isHeardAsKept (parent)) // the areas': tempoMapChanged(), chordLaneChanged(), curvesChanged(), midiLinksChanged()
            markChanged (parent);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Project)
};

//==============================================================================
// Autosaves a project while it has unsaved changes, on a background thread (Project::autosaveInBackground()): the first
// time soon after the first change, then once changes have waited a few minutes, and at once when a recording ends (what
// a crash would lose most of). Never while recording: the recording isn't in the song until it ends. While the song
// plays, it waits for it to stop, for at most maxWaitWhilePlayingSeconds.
class AutosaveTimer : private juce::Timer
{
public:
    explicit AutosaveTimer (Project&, int intervalSeconds = 120, int firstDelaySeconds = 15);
    ~AutosaveTimer() override;

    // Called once when failuresToReport autosaves (or auto saves) in a row have failed (docs/archive/performance-audit.md, R7); and
    // whether they have, until one works, which the status bar says.
    std::function<void()> onRepeatedFailures;
    static constexpr int failuresToReport = 3;
    bool hasFailedRepeatedly() const noexcept { return failures >= failuresToReport; }
    static constexpr int maxWaitWhilePlayingSeconds = 600;

    // Auto Save, as VS Code's files.autoSave "afterDelay": while on (for a project with a file of its own), the project saves
    // itself autoSaveDelayMs after its last change (Project::saveInBackground()), in place of autosaving for a crash; while
    // the song plays, at most every playingSaveSeconds; never while it records, halfway through a drag (a Gesture) or while
    // something is tried in place (a Trial: model/commands/internal/Undo.h), and at once as a recording ends.
    void setAutoSave (bool);
    bool isAutoSaving() const noexcept { return autoSave; }
    static constexpr int autoSaveDelayMs = 1000, playingSaveSeconds = 10;

    // Before either writes, the plug-ins whose states may have changed are asked for them at once, and the write waits for
    // their answers without keeping the message thread (model/plugins/internal/PluginInstances.h; known bug 89). Called after
    // a write that holds an older state of some plug-ins than theirs (one that didn't answer in time, or whose helper went),
    // with their names, once for each change of that list.
    std::function<void (const juce::StringArray& plugins)> onOlderPluginStates;

    // What it does at each of its ticks (four a second), done now: for tests, which hold the clock it reads its waits by
    // (devices/core/Clock.h) and tick it as they move the clock on.
    void tickNow() { timerCallback(); }

private:
    Project& project;
    const int intervalSeconds, firstDelaySeconds;
    juce::uint32 changedSince = 0; // when the timer first saw a change not yet autosaved (devices::Clock's millisecond counter), or 0
    bool autosavedOnce = false;
    bool wasRecording = false, wasAutosaving = false;
    int failures = 0; // in a row
    bool autoSave = false;
    juce::int64 lastChangeCount = 0;
    juce::uint32 lastChangeAt = 0, savedAt = 0; // devices::Clock's millisecond counter
    bool saveDue = false;                       // Auto Save's, waiting for the plug-ins' states
    juce::StringArray toldOlder;                // the plug-ins onOlderPluginStates last named

    void timerCallback() override;
    bool havePluginStates(); // the plug-ins that may have changed asked, and answered
    void tellOlderPluginStates();
};
} // namespace phazite
