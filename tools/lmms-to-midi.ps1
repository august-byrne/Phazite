#Requires -Version 5.1
<#
.SYNOPSIS
    Writes the notes of an LMMS project (.mmp or .mmpz) as a standard MIDI file, for an example song that
    Phazite's own instruments play (src/model/project/ExampleSongs.cpp, resources/songs/).

.DESCRIPTION
    Takes the notes only: no sounds, samples, presets or effects. Each instrument track named in -Parts
    becomes a MIDI track of that name, at the pitch it sounds at in LMMS (its key, moved by the track's
    base note, the project's master pitch and, for a TripleOscillator, by its loudest oscillator's coarse
    tuning; an instrument LMMS plays through MIDI, such as ZynAddSubFX, sounds an octave under LMMS's own
    instruments at the same key, and is written there). The beat/bassline
    editor's sample tracks become one "Drums" track of General MIDI's drum notes, each named by its
    sample's file name (kick, snare, clap, hat...) or by -DrumNotes, each pattern repeated where the song
    places it; a track that only drives a controller (a muted peak controller) is left out. Muted tracks
    and clips are left out, and so is everything from bar -Bars on. A note's volume is its velocity
    (LMMS's 100 is 100). The tempo is -Tempo, or else the project's own; tempo automation isn't taken.

    For example, from the repository root:

        powershell -NoProfile -ExecutionPolicy Bypass -File tools\lmms-to-midi.ps1 -Project Exodus.mmpz `
            -Out resources\songs\Exodus.mid -Parts "0=Bass;1=Sub;3=Arp" -Bars 120 -Tempo 111 -Title "Exodus" `
            -Credit "Exodus by XCALIBUR (https://lmms.io/lsp/?action=show&file=12900), CC BY 4.0: its notes, from its LMMS project"

    (Exodus's tracks 2, 4 and 5 play track 3's line again, the same, an octave down and two octaves up, so
    its file holds that line once.) Without -Parts it lists the project's instrument tracks by their
    numbers, to choose from.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Project,
    [string] $Out,
    [string] $Parts = "",      # "number=Name;number=Name", the song's instrument tracks by their order from 0
    [string] $DrumNotes = "",  # "sample file name=note;...", for samples whose names say nothing
    [int] $Bars = 0,           # the song's end, in bars of 4/4; 0 for all of it
    [double] $Tempo = 0.0,     # in beats per minute; 0 for the project's
    [string] $Title = "",
    [string] $Credit = ""
)
$ErrorActionPreference = 'Stop'

Add-Type -ReferencedAssemblies System.Xml -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Xml;

public static class LmmsToMidi
{
    const int TicksPerBeat = 48, TicksPerBar = 192; // LMMS's, which the MIDI file keeps

    public class Note { public int Tick, Length, Pitch, Velocity; }

    public static XmlDocument Load(string path)
    {
        var bytes = File.ReadAllBytes(path);
        string text;
        if (path.EndsWith(".mmpz", StringComparison.OrdinalIgnoreCase))
        {
            // Qt's qCompress: the length, 4 bytes big-endian, then a zlib stream (a 2-byte header, then deflate).
            using (var input = new MemoryStream(bytes, 6, bytes.Length - 6))
            using (var inflate = new DeflateStream(input, CompressionMode.Decompress))
            using (var output = new MemoryStream())
            {
                inflate.CopyTo(output);
                text = Encoding.UTF8.GetString(output.ToArray());
            }
        }
        else
            text = Encoding.UTF8.GetString(bytes);
        var doc = new XmlDocument();
        doc.LoadXml(text);
        return doc;
    }

    static string Attr(XmlNode node, string name, string fallback = "")
    {
        var a = node.Attributes == null ? null : node.Attributes[name];
        return a == null ? fallback : a.Value;
    }

    static int Int(XmlNode node, string name, int fallback = 0)
    {
        double value;
        return double.TryParse(Attr(node, name), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out value)
            ? (int) Math.Round(value) : fallback;
    }

    // LMMS 1.2 names clips "pattern" and the beat/bassline editor's "bbtrack"/"bbtco"; 1.3 "midiclip", "patterntrack", "patternclip".
    static IEnumerable<XmlNode> Clips(XmlNode track) { return track.SelectNodes("pattern|midiclip").Cast<XmlNode>(); }

    public static List<XmlNode> SongTracks(XmlDocument doc)
    {
        return doc.SelectNodes("/lmms-project/song/trackcontainer/track[@type='0']").Cast<XmlNode>().ToList();
    }

    public static string Describe(XmlNode track)
    {
        var instrument = track.SelectSingleNode("instrumenttrack/instrument");
        int notes = Clips(track).Sum(clip => clip.SelectNodes("note").Count);
        return string.Format("'{0}' {1}, {2} notes", Attr(track, "name"), instrument == null ? "?" : Attr(instrument, "name"), notes);
    }

    // The instruments LMMS plays through MIDI events rather than by frequency (its "MIDI-based" ones).
    static readonly string[] MidiBased = { "zynaddsubfx", "vestige", "opl2", "opulenz" };

    // How far a track's keys are from the MIDI notes they sound as. LMMS's own instruments play key 57 at 440 Hz
    // (MIDI note 69) when the track's base note is 57. A MIDI-based instrument is handed the key itself as its MIDI
    // note (InstrumentTrack::masterKey), so it sounds an octave lower: Exodus's two ZynAddSubFX layers, measured in
    // LMMS 1.2.2's renders.
    static int PitchShift(XmlNode track, int masterPitch)
    {
        var instrumentTrack = track.SelectSingleNode("instrumenttrack");
        var instrument = instrumentTrack.SelectSingleNode("instrument");
        bool midiBased = instrument != null && MidiBased.Contains(Attr(instrument, "name").ToLowerInvariant());
        int shift = (midiBased ? 0 : 12) + 57 - Int(instrumentTrack, "basenote", 57) + masterPitch;
        var oscillators = instrumentTrack.SelectSingleNode("instrument/tripleoscillator");
        if (oscillators != null)
        {
            int loudest = 0;
            for (int i = 1; i < 3; ++i)
                if (Int(oscillators, "vol" + i) > Int(oscillators, "vol" + loudest))
                    loudest = i;
            shift += Int(oscillators, "coarse" + loudest);
        }
        return shift;
    }

    static Note MakeNote(XmlNode note, int at, int shift, int defaultLength)
    {
        int length = Int(note, "len");
        return new Note {
            Tick = at,
            Length = length > 0 ? length : defaultLength,
            Pitch = Math.Max(0, Math.Min(127, Int(note, "key") + shift)),
            Velocity = Math.Max(1, Math.Min(127, Int(note, "vol", 100)))
        };
    }

    public static List<Note> SongPart(XmlNode track)
    {
        var notes = new List<Note>();
        if (Attr(track, "muted") == "1")
            return notes;
        var head = track.OwnerDocument.SelectSingleNode("/lmms-project/head");
        int shift = PitchShift(track, head == null ? 0 : Int(head, "masterpitch"));
        foreach (var clip in Clips(track))
        {
            if (Attr(clip, "muted") == "1")
                continue;
            int start = Int(clip, "pos"), length = Int(clip, "len");
            foreach (XmlNode note in clip.SelectNodes("note"))
                if (Int(note, "pos") < length)
                    notes.Add(MakeNote(note, start + Int(note, "pos"), shift, TicksPerBeat / 4));
        }
        return notes;
    }

    public static int DrumNoteFor(string sample, Dictionary<string, int> overrides)
    {
        var file = Path.GetFileName(sample.Replace('\\', '/'));
        int given;
        if (overrides.TryGetValue(file, out given))
            return given;
        var name = file.ToLowerInvariant();
        if (name.Contains("kick") || name.Contains("bassdrum") || name.StartsWith("bd")) return 36;
        if (name.Contains("rim") || name.Contains("stick")) return 37;
        if (name.Contains("snare")) return 38;
        if (name.Contains("clap")) return 39;
        if (name.Contains("hat") && (name.Contains("open") || name.Contains("_o"))) return 46;
        if (name.Contains("hat")) return 42;
        if (name.Contains("crash")) return 49;
        if (name.Contains("ride")) return 51;
        if (name.Contains("tom")) return 45;
        if (name.Contains("cowbell")) return 56;
        if (name.Contains("tamb")) return 54;
        if (name.Contains("shaker")) return 70;
        if (name.Contains("perc") || name.Contains("click") || name.Contains("wood")) return 37;
        return -1;
    }

    // The beat/bassline editor's sample tracks as General MIDI's drums, each pattern where the song places it.
    public static List<Note> Drums(XmlDocument doc, Dictionary<string, int> overrides, List<string> report)
    {
        var notes = new List<Note>();
        var beats = doc.SelectNodes("/lmms-project/song/trackcontainer/track[@type='1']").Cast<XmlNode>().ToList();
        if (beats.Count == 0)
            return notes;
        var container = beats[0].SelectSingleNode("bbtrack/trackcontainer|patterntrack/trackcontainer");
        if (container == null)
            return notes;
        var instruments = container.SelectNodes("track[@type='0']").Cast<XmlNode>().ToList();

        // Each beat's length: its longest pattern's, a bar at least.
        var lengths = new int[beats.Count];
        for (int b = 0; b < beats.Count; ++b)
            lengths[b] = TicksPerBar;
        foreach (var instrument in instruments)
            foreach (var clip in Clips(instrument))
            {
                int index = Int(clip, "pos") / TicksPerBar;
                if (index < beats.Count)
                    lengths[index] = Math.Max(lengths[index], Int(clip, "len"));
            }

        foreach (var instrument in instruments)
        {
            var name = Attr(instrument, "name");
            var sample = instrument.SelectSingleNode("instrumenttrack/instrument/audiofileprocessor");
            var controller = instrument.SelectSingleNode(".//peakcontrollereffectcontrols");
            if (Attr(instrument, "muted") == "1" || (controller != null && Attr(controller, "mute") == "1"))
            {
                report.Add("  drums: left out '" + name + "' (muted, or it drives a controller)");
                continue;
            }
            if (!Clips(instrument).Any(clip => clip.SelectNodes("note").Count > 0))
                continue;
            int drum = sample == null ? -1 : DrumNoteFor(Attr(sample, "src"), overrides);
            if (drum < 0)
            {
                report.Add("  drums: left out '" + name + "': no drum note for it (give one with -DrumNotes)");
                continue;
            }
            report.Add("  drums: '" + name + "' as note " + drum);
            foreach (var clip in Clips(instrument))
            {
                int index = Int(clip, "pos") / TicksPerBar;
                if (index >= beats.Count || Attr(beats[index], "muted") == "1")
                    continue;
                foreach (XmlNode place in beats[index].SelectNodes("bbtco|patternclip"))
                {
                    if (Attr(place, "muted") == "1")
                        continue;
                    int start = Int(place, "pos"), end = start + Int(place, "len");
                    for (int repeat = start; repeat < end; repeat += lengths[index])
                        foreach (XmlNode note in clip.SelectNodes("note"))
                        {
                            int at = repeat + Int(note, "pos");
                            if (at < end)
                            {
                                var made = MakeNote(note, at, 0, TicksPerBeat / 4);
                                made.Pitch = drum;
                                made.Length = TicksPerBeat / 4;
                                notes.Add(made);
                            }
                        }
                }
            }
        }
        return notes;
    }

    public static List<Note> Cut(List<Note> notes, int bars)
    {
        if (bars <= 0)
            return notes.OrderBy(n => n.Tick).ThenBy(n => n.Pitch).ToList();
        int end = bars * TicksPerBar;
        foreach (var note in notes)
            note.Length = Math.Min(note.Length, end - note.Tick);
        return notes.Where(n => n.Tick < end && n.Length > 0).OrderBy(n => n.Tick).ThenBy(n => n.Pitch).ToList();
    }

    //==============================================================================
    static void WriteVarLen(List<byte> data, int value)
    {
        var bytes = new List<byte> { (byte) (value & 0x7f) };
        while ((value >>= 7) > 0)
            bytes.Insert(0, (byte) ((value & 0x7f) | 0x80));
        data.AddRange(bytes);
    }

    static void WriteMeta(List<byte> data, int delta, byte type, byte[] content)
    {
        WriteVarLen(data, delta);
        data.Add(0xff);
        data.Add(type);
        WriteVarLen(data, content.Length);
        data.AddRange(content);
    }

    static byte[] Chunk(string id, List<byte> data)
    {
        var chunk = new List<byte>(Encoding.ASCII.GetBytes(id));
        chunk.Add((byte) (data.Count >> 24)); chunk.Add((byte) (data.Count >> 16)); chunk.Add((byte) (data.Count >> 8)); chunk.Add((byte) data.Count);
        chunk.AddRange(data);
        return chunk.ToArray();
    }

    static byte[] Track(string name, List<Note> notes, int channel)
    {
        var data = new List<byte>();
        WriteMeta(data, 0, 0x03, Encoding.UTF8.GetBytes(name));
        // Each note's start and end, ends before starts at the same tick.
        var events = new List<Tuple<int, int, Note>>();
        foreach (var note in notes)
        {
            events.Add(Tuple.Create(note.Tick, 1, note));
            events.Add(Tuple.Create(note.Tick + note.Length, 0, note));
        }
        int now = 0;
        foreach (var e in events.OrderBy(e => e.Item1).ThenBy(e => e.Item2).ThenBy(e => e.Item3.Pitch))
        {
            WriteVarLen(data, e.Item1 - now);
            now = e.Item1;
            data.Add((byte) ((e.Item2 == 1 ? 0x90 : 0x80) | channel));
            data.Add((byte) e.Item3.Pitch);
            data.Add((byte) (e.Item2 == 1 ? e.Item3.Velocity : 0));
        }
        WriteMeta(data, 0, 0x2f, new byte[0]);
        return Chunk("MTrk", data);
    }

    public static void Write(string path, double bpm, string title, string credit, List<KeyValuePair<string, List<Note>>> parts)
    {
        var header = new List<byte> { 0, 1, 0, (byte) (parts.Count + 1), 0, (byte) TicksPerBeat };
        var conductor = new List<byte>();
        if (title.Length > 0) WriteMeta(conductor, 0, 0x03, Encoding.UTF8.GetBytes(title));
        if (credit.Length > 0) WriteMeta(conductor, 0, 0x02, Encoding.UTF8.GetBytes(credit));
        int microseconds = (int) Math.Round(60000000.0 / bpm);
        WriteMeta(conductor, 0, 0x51, new[] { (byte) (microseconds >> 16), (byte) (microseconds >> 8), (byte) microseconds });
        WriteMeta(conductor, 0, 0x58, new byte[] { 4, 2, 24, 8 });
        WriteMeta(conductor, 0, 0x2f, new byte[0]);

        using (var file = File.Create(path))
        {
            var chunk = Chunk("MThd", header);
            file.Write(chunk, 0, chunk.Length);
            chunk = Chunk("MTrk", conductor);
            file.Write(chunk, 0, chunk.Length);
            int channel = 0;
            foreach (var part in parts)
            {
                bool drums = part.Key == "Drums";
                chunk = Track(part.Key, part.Value, drums ? 9 : channel);
                file.Write(chunk, 0, chunk.Length);
                if (!drums && ++channel == 9)
                    ++channel;
            }
        }
    }

    public static string Summary(string name, List<Note> notes)
    {
        if (notes.Count == 0)
            return string.Format("  {0}: no notes", name);
        return string.Format("  {0}: {1} notes, MIDI {2} to {3}, bars {4} to {5}", name, notes.Count, notes.Min(n => n.Pitch),
                             notes.Max(n => n.Pitch), notes.Min(n => n.Tick) / TicksPerBar, (notes.Max(n => n.Tick + n.Length) - 1) / TicksPerBar + 1);
    }
}
'@

$doc = [LmmsToMidi]::Load((Resolve-Path -LiteralPath $Project).Path)
$tracks = [LmmsToMidi]::SongTracks($doc)

if ($Parts -eq "") {
    "The song's instrument tracks:"
    for ($i = 0; $i -lt $tracks.Count; ++$i) { "  $i " + [LmmsToMidi]::Describe($tracks[$i]) }
    return
}

if (-not $Out) { throw "Give -Out, the MIDI file to write." }

if ($Tempo -le 0) {
    $head = $doc.SelectSingleNode("/lmms-project/head")
    $Tempo = if ($head.HasAttribute("bpm")) { [double]$head.bpm } else { [double]$head.SelectSingleNode("bpm").value }
}

$overrides = New-Object 'System.Collections.Generic.Dictionary[string,int]'
foreach ($pair in ($DrumNotes -split ';' | Where-Object { $_ -ne "" })) {
    $name, $note = $pair -split '=', 2
    $overrides[$name.Trim()] = [int]$note
}

$report = New-Object 'System.Collections.Generic.List[string]'
$written = New-Object 'System.Collections.Generic.List[System.Collections.Generic.KeyValuePair[string,System.Collections.Generic.List[LmmsToMidi+Note]]]'

foreach ($pair in ($Parts -split ';' | Where-Object { $_ -ne "" })) {
    $number, $name = $pair -split '=', 2
    $track = $tracks[[int]$number]
    $notes = [LmmsToMidi]::Cut([LmmsToMidi]::SongPart($track), $Bars)
    $written.Add((New-Object 'System.Collections.Generic.KeyValuePair[string,System.Collections.Generic.List[LmmsToMidi+Note]]' -ArgumentList $name.Trim(), $notes))
    $report.Add("  " + $name.Trim() + " from track " + $number + " " + [LmmsToMidi]::Describe($track))
}

$drums = [LmmsToMidi]::Cut([LmmsToMidi]::Drums($doc, $overrides, $report), $Bars)
if ($drums.Count -gt 0) {
    $written.Add((New-Object 'System.Collections.Generic.KeyValuePair[string,System.Collections.Generic.List[LmmsToMidi+Note]]' -ArgumentList "Drums", $drums))
}

[LmmsToMidi]::Write([System.IO.Path]::GetFullPath($Out), $Tempo, $Title, $Credit, $written)
"Wrote $Out at $Tempo BPM:"
$report
foreach ($part in $written) { [LmmsToMidi]::Summary($part.Key, $part.Value) }
