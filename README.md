# nanoCDJ

A pocket CDJ for the **iPod nano 7th generation**. It reads your **Rekordbox USB export**
(playlists, beat grids, waveforms, cues) and plays your MP3s with CDJ-style controls, so
you can beatmatch against vinyl or other decks from a 2012 iPod.

It runs as a [NanoApps](https://github.com/nfzerox/NanoApps) app: NanoApps is homebrew
firmware for the nano 7G that adds an app launcher, and nanoCDJ is one of its apps.

> **Unofficial and experimental.** Not affiliated with Pioneer DJ / AlphaTheta or Apple.
> Installing NanoApps modifies your iPod's firmware; follow its instructions and use it at
> your own risk. nanoCDJ itself only ever reads your files.

## Features

- **Rekordbox library**: browse playlists from a normal Rekordbox USB export, sort by
  playlist order, BPM, artist or title.
- **Deck**: play/pause, CDJ-style CUE (back to cue, hold to preview, set when paused),
  manual loops (in/out, reloop/exit, ½× / 2×), instant 4-beat loop, quantize.
- **Tempo**: pitch fader with ±6 / 10 / 16 % and WIDE (±50 %) ranges, relative drag,
  double-tap to reset, fine nudge buttons (0.02 %, hold to repeat).
- **Jog**: circle a finger to bend the tempo, or switch to VINYL mode to scratch.
- **Display**: colour overview and scrolling detail waveform with beat grid and cues,
  from Rekordbox's analysis.
- **Audio**: MP3 at 44.1 or 48 kHz, any bitrate (CBR or VBR), streamed from the iPod's
  flash so track length doesn't matter. Audio stays on Rekordbox's beat grid.

## Requirements

- An **iPod nano 7th generation**. Developed and tested on the 2012 model (A1446,
  firmware 1.0.4) with NanoApps' firmware installed. Other 7G units supported by
  NanoApps should work but are untested.
- **NanoApps** installed on the iPod: see the
  [NanoApps README](https://github.com/nfzerox/NanoApps#readme).
- **Rekordbox** (any recent version) to export your playlists to the iPod.
- Your tracks as **MP3**. Other formats load, but play a click on every beat instead of
  audio (see [Limitations](#limitations)).

## Install

1. Get `nanoCDJ.zip` from the [Releases](../../releases) page, or [build it](#building).
2. Connect the iPod with **disk use enabled** (Finder → the iPod → "Enable disk use"), so
   it mounts as a drive.
3. Unzip and copy the `nanoCDJ.nanoapp` folder into the `Apps` folder on the iPod.
4. In Rekordbox, export your playlists to the iPod like to any USB drive. This creates a
   `PIONEER` folder (hidden as `.PIONEER` on Mac-formatted iPods; both work) and copies
   the tracks into `Contents`.
5. Eject the iPod and open **nanoCDJ** from the NanoApps home screen.

The Rekordbox export is separate from the iPod's own Music library, so tracks you have
in both are stored twice.

## Controls

| Control | What it does |
|---|---|
| `<` (top left) | deck → browser; in the browser, up one level |
| `>` (top right, browser) | back to the deck while a track is loaded |
| Home button | switch between the browser and the deck |
| Sort bar | **#** playlist order, **BPM**, **ARTIST**, **NAME**. Tap the active one again to reverse. Tracks missing that info go last |
| Overview strip | needle search (while paused) |
| Detail waveform | tap to zoom ×1 / ×2 / ×4 |
| **PLAY** | play / pause |
| **CUE** | playing: back to the cue point. Paused at the cue: hold to preview, release to return. Paused elsewhere: set the cue (quantized). Hold CUE + PLAY to keep playing |
| **IN / OUT / RELOOP** | manual loop; while looping they become ½× / 2× / EXIT |
| **4 BEAT** | 4-beat loop on the beat grid |
| Pitch fader | drag (relative: touching it never jumps the tempo). Top = slower, bottom = faster, like a CDJ. **Double-tap to reset to 0 %** |
| `−` / `+` by the fader | fine pitch: 0.02 % (0.1 % in WIDE). Hold to keep nudging |
| Range chip | ±6 / 10 / 16 % / WIDE (±50 %) |
| **Q** chip | quantize on/off |
| Jog | circle your finger. **BEND**: nudges the speed while playing. **VINYL**: stops and scratches. Paused: search in either mode |

The side **volume** and **play/pause** buttons aren't used by nanoCDJ: the iPod's own
system still handles them (volume clicks; play/pause switches to the Music app), and
NanoApps can't stop that. Set your volume before you start.

## Limitations

- **MP3 only.** Other formats (WAV, AIFF, AAC/M4A, FLAC) play a click on every beat of
  the Rekordbox grid instead of audio. The deck, loops and jog still work, which is
  handy for checking a grid, but there's no decoder for them yet.
- **One deck, one finger.** NanoApps' raw drawing surface reports a single touch point.
- **Latency** from touch to sound is about 80–100 ms. Fine for beatmatching by ear,
  noticeable for fast scratching.
- **Loading** takes 0.1–0.3 s. Tracks with very large embedded cover art can take a few
  seconds.
- A far-away **CUE** is instant (a ~19 s cue buffer), but the deck needs a few seconds to
  re-read the file to get there, since the iPod can't seek within a file.
- Hot cues and memory cues show on the waveforms (the first memory cue is loaded as the
  cue point), but there are no hot cue buttons, cue lists, sync or key lock yet.
- The iPod's system loses a little memory while audio plays (~90 bytes/s, roughly 12
  hours before it matters). Restart the iPod before a long set to be safe: hold
  Sleep/Wake + Home until the Apple logo appears.

## Possible future upgrades

Ideas, roughly from most to least requested. Contributions welcome.

- **Other file formats.** WAV and AIFF are the easy ones: no decoding, just reading and
  converting. AAC/M4A and FLAC need decoders small enough for the iPod and a way to find
  positions in the file without seeking.
- **Shared library with the Music app.** Play the copies the iPod's Music library
  already has, instead of Rekordbox's second copy of every track. A script run after
  each Rekordbox export would match each exported track to its file in
  `iPod_Control/Music`, write a lookup table, and remove the duplicates. That would
  roughly halve the space your DJ tracks take.
- **Hot cues.** Rekordbox's hot cues are already read and drawn; they need buttons
  (e.g. a row of A–H pads) to jump to them, with the same cue-buffer trick so jumps are
  instant.
- **Multi-touch.** NanoApps keeps a snapshot of every finger the OS reports; using it
  would allow holding CUE while touching PLAY, or riding the fader while nudging.
- **Beat jump and loop moves.** Jump or shift a loop by 1–32 beats on the grid.
- **Key lock (master tempo).** Change tempo without changing pitch. Needs a
  time-stretcher cheap enough for the iPod's CPU.
- **Lower latency.** Writing closer to the play cursor (now ~80 ms ahead) would make the
  jog feel tighter. It needs measuring how far ahead the iPod's mixer really reads.
- **Search and memory cues.** Search the library by name, and add or remove memory
  cues on the device.
- **Using the side buttons.** Stop the iPod's system from also acting on the play/pause
  and volume buttons, so they can be used as deck controls. It would mean finding and
  patching RetailOS's button handling, which is uncertain work.
- **More Rekordbox metadata.** Show track colours, ratings, comments and key in the
  browser, and sort or filter by them.

## Building

You need macOS or Linux with:

- `arm-none-eabi-gcc` (`brew install arm-none-eabi-gcc`, or your distro's package)
- Python 3 with Pillow and pyelftools: `pip3 install pillow pyelftools`
- a C compiler for the host tests

```sh
git clone --recursive https://github.com/<you>/nanoCDJ.git
cd nanoCDJ
make ipod      # -> port/nanoapps/build/nanoCDJ.nanoapp and nanoCDJ.zip
make test      # host unit tests, no iPod needed
```

If you cloned without `--recursive`, run `git submodule update --init` first: NanoApps
(the SDK) is a submodule pinned to the commit nanoCDJ is built and tested against.

Optional checks with your own music (files never leave your machine):

```sh
make test-mp3 MP3="path/to/track.mp3"   # streaming decoder vs a whole-file decode
make tools && tools/.build/grid_offset a.mp3 b.mp3   # audio vs Rekordbox beat grid (macOS)
```

## Debugging on the device

nanoCDJ writes breadcrumbs (memory, audio timing, file reads) into NanoApps' trace log
every 5 seconds. With the iPod plugged in **and nanoCDJ running** (not in disk mode):

```sh
python3 tools/trace.py 100 --only NC_
```

Use `tools/trace.py` rather than NanoApps' `./start trace`: on macOS the latter switches
a running iPod into sync mode, which closes the app (and reboots the iPod soon after).
The tags are described in [docs/TECHNICAL.md](docs/TECHNICAL.md#on-device-diagnostics).

## How it works

[docs/TECHNICAL.md](docs/TECHNICAL.md) covers the architecture and the parts that took
measuring on the device: audio output through the OS sound-effect player, streaming MP3
from a filesystem that can't seek, lining the audio up with Rekordbox's beat grid, and
running in a few MB of memory.

## Project layout

```
src/              portable core: Rekordbox export.pdb + ANLZ readers, deck engine,
                  UI, drawing, streaming MP3 decoder (no platform code)
src/hal.h         everything the core needs from the device
port/nanoapps/    the iPod implementation: NanoApps glue, audio output, file streaming,
                  freestanding libc/libm, build and packaging scripts
tests/            host unit tests (deck, UI, audio output, streaming decoder, libc)
tools/            trace reader, beat-grid checker, font generator
third_party/      minimp3 (vendored) and NanoApps (git submodule)
docs/             technical notes
```

## Credits

- [NanoApps](https://github.com/nfzerox/NanoApps) (MIT) by nfzerox, Merquice, Raphmatt
  and Nikita Shimkus: the firmware, SDK and app format nanoCDJ is built on. Its
  *entrain* app documented how to stream audio through RetailOS's sound-effect voice.
  `port/nanoapps/hb_silverdb.bin` is NanoApps' compiled screen database, vendored so
  packaging needs no Rust toolchain.
- [minimp3](https://github.com/lieff/minimp3) (CC0) by lieff: the MP3 decoder. See
  [third_party/minimp3/README.md](third_party/minimp3/README.md) for the one local change.
- The Rekordbox file formats are read from the community's reverse-engineering work,
  notably [Deep Symmetry's crate-digger](https://github.com/Deep-Symmetry/crate-digger)
  documentation.

Rekordbox, CDJ and Pioneer DJ are trademarks of AlphaTheta Corporation; iPod is a
trademark of Apple Inc. They're used here only to describe compatibility.

## License

MIT, © 2026 Mateo Knox: see [LICENSE](LICENSE). minimp3 is CC0 and NanoApps is MIT.
