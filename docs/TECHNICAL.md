# nanoCDJ technical notes

How nanoCDJ is put together, and the device behaviour it depends on. Most of the
numbers here were measured on an iPod nano 7G (2012, NanoApps firmware, RetailOS 1.1.2
ABI) with the on-device trace; the sections say which.

## Architecture

```
            NanoApps resident (calls hb_raw_frame ~60x/s, blits the framebuffer)
                                   │
port/nanoapps/entry.c      Thumb, soft-float: attaches the FPU, first-frame init
                                   │
port/nanoapps/hal_nanoapps.c   ARM + VFP: touch/buttons → events, audio pump,
                                   │       file streaming, diagnostics, unload hook
src/app.c   UI: browser + deck screens, touch controls          ─┐
src/deck.c  deck engine: transport, cue, loops, pitch, jog        │ portable C,
src/mp3stream.c  streaming MP3 (minimp3) around the playhead      │ only src/hal.h
src/pdb.c, anlz.c  Rekordbox export.pdb and ANLZ readers          │ underneath
src/gfx.c   drawing, built-in 5×8 font                           ─┘
port/nanoapps/audio_ring.c  PCM out through the OS sound-effect voice (Thumb, integer)
```

Everything runs on one thread: each frame handles input, reads a little of the track,
draws, then renders audio up to a short lead ahead of the play cursor.

### Thumb / ARM split

The resident enters apps in Thumb state, and Thumb-state VFP instructions trap on this
firmware. So, as in NanoApps' own floating-point ports:

- `entry.c`, `audio_ring.c` and the SDK are built with the SDK's flags (Thumb, soft float).
  `entry.c` attaches the task's FPU context before anything else runs.
- The core and the HAL are built in ARM state with VFP (`-marm -mfloat-abi=softfp
  -mfpu=vfpv3-d16`) into `libnanocdj.a`.
- `libm.c` provides ARM-mode `__aeabi_d2lz/d2ulz`, so libgcc's Thumb+VFP versions never
  link. Every build runs NanoApps' `check_no_thumb_fp.py` gate.
- The build is freestanding: `libc.c`/`libm.c` supply what the core uses (malloc,
  qsort, vsnprintf, a handful of maths functions). `make test-port` checks them against
  the host's.

## Audio output

NanoApps has no streaming audio API. `audio_ring.c` uses the technique documented in
NanoApps' *entrain* app (`apps/entrain/platform/audio_device.c`). RetailOS sound-effect
descriptors have a NEXT pointer at `+0x54`. When a voice finishes one buffer, it starts
the next in its own completion handler, a sample-exact join. Each descriptor's `+0x64`
byte is 1 while a voice holds it, so finished buffers are known for certain.

What it took on the nano (each found from the trace):

- **Blocks must be long.** 20 ms blocks crackled at every join and rebooted the iPod
  after ~30 s. The mixer pulls in 1024-frame passes, and a buffer shorter than a pass
  breaks its accounting. Blocks are now ~200 ms (8820 frames at 44.1 kHz).
- **Latency comes from where we write, not block size.** Each block is zeroed when
  chained, and every frame renders only up to ~80 ms ahead of the estimated play
  cursor, into blocks the voice is already playing. The voice reads our buffers live.
  The cursor is estimated from the µs clock and re-pinned whenever the "playing" flag
  moves to the next block, so it can't drift (tested against ±120 ppm clock error).
- **No length counter.** entrain sets the voice's sample counter (descriptor `+0x34` in
  ms → voice `+0x48`). On this unit, stating it as `frames` made every 200 ms block take
  ~385 ms: our audio then as much silence, so the deck ran at half speed. `frames / 2`
  didn't time right either. Leaving it at 0 (entrain's fallback) measured 98–101 % for
  minutes. nanoCDJ starts at 0 and keeps timing blocks (`NC_CAL`).
- **Stalls stay in time.** If frames stop (screen lock, a slow load), the voice runs
  into zeroed buffer and stops: silence, never stale audio. When frames resume, the
  deck is moved on by the missed time (`deck_skip`), so the track stays in phase with
  other decks.
- The screen is kept from auto-dimming with the same idle-clock poke as the SDK's
  `hb_wake_lock` (which only exists for LVGL apps).

`make test-ring` runs the ring against a simulated voice that models chaining, mixer
read-ahead, clock drift and the counter behaviours above.

## Streaming MP3

A 320 kbps track is 10–25 MB, the app has 4–7 MB of heap, and the OS file object can
only read forward (no seek). So `mp3stream.c` never loads a track whole:

- **Forward reads, indexed as they go.** Each frame reads a little more, and every MP3
  frame header is parsed into an offset index, so any position can be found exactly
  (CBR or VBR) once the reader has passed it. Reaching a position behind the reader
  means reopening the file and reading forward again.
- **Window around the playhead.** ~1 MB of compressed audio (~6 s behind, ~20 s ahead at
  320 kbps) for loops, jog and scratching.
- **Cue buffer.** ~19 s of compressed audio from the cue point is kept in its own buffer,
  like a CDJ's. CUE is instant from anywhere, and lasts longer than the re-read from the
  start of the file a far jump needs.
- **Reads sized to the frame.** Measured: a 32 KB read blocked for ~44 ms (~0.7 MB/s).
  Small reads mostly run at 1–5 MB/s, with an occasional ~30 ms spike inside the OS. Each
  read is sized from a running cost-per-KB estimate to fit ~5 ms of the frame, or 12 ms
  while less than 4 s is read ahead.
- **Decoding on demand.** minimp3 decodes MP3 frames (~2.5 ms each, ~10 % CPU at 1×)
  into a small cache. After a jump the decoder is primed from far enough back to cover
  the 511-byte bit reservoir: two frames at 320 kbps, three at 128. That makes the
  target frame bit-exact with a straight decode. minimp3's ~16 KB scratch struct was
  moved off the stack (the frame callback's stack size is unknown).
- 48 kHz files are resampled by the deck's interpolator (the source position is scaled
  by `hz / 44100`).
- **Large ID3 tags.** A first frame behind a large ID3 tag (cover art) is searched for
  for up to ~6 s at load.

`make test-mp3 MP3=...` compares the stream against a whole-file decode: straight play
with small reads, random jumps, the cue buffer, scratching backwards, iPod-speed reads,
and the deck's own output.

## Lining up with Rekordbox's beat grid

Where Rekordbox puts 0:00 in an MP3 depends on the encoder. Measured on 80 tracks by
scoring the energy rise at every grid beat against sample offsets (`tools/grid_offset`):

- **LAME-encoded files:** Rekordbox decodes the encoder's Xing/Info frame as 1152
  samples of silence, so beats sit one frame later than in a decoder that skips it.
- **ffmpeg-encoded files** (`Lavc` encoder tag): Rekordbox skips the info frame.
- **Files without one:** no offset.

`mp3stream.c` does the same, so the grids line up within a few ms on almost all
tracks. LAME's stated encoder delay is not trimmed.

## Rekordbox export

- `export.pdb` (DeviceSQL) is read for tracks, artists, keys, playlists and playlist
  entries. On Mac-formatted (HFS+) iPods Rekordbox writes the folder as hidden
  `.PIONEER`, while paths inside still say `/PIONEER/`; `pdb.c` handles both.
- ANLZ `.DAT`/`.EXT`: beat grid (PQTZ), preview and detail waveforms (PWAV, PWV3,
  PWV5), cues (PCOB/PCPT, PCO2/PCP2: the `.EXT` cues win).

## Memory and leaving the app

- The OS allocator reboots the iPod when an allocation fails, so `malloc` refuses
  anything that would leave less than 256 KB in the largest free block, and the stream
  sizes its buffers from what's free.
- NanoApps frees the app image when you leave, but not what it allocated from the OS
  heap, and it zeroes the app's statics on the next launch. Without cleanup, each visit
  leaked ~2.5 MB plus an open OS file object, and the iPod rebooted after a couple of
  visits. `malloc` therefore keeps a list of live blocks. The unload hook
  (`nanocdj_prepare_unload`) waits for the voice to finish, closes the file, and frees
  every block. It runs on the resident's task, which has no FPU, so that path is
  integer-only (checked in the disassembly).
- While audio plays, the OS heap shrinks by ~40–90 bytes/s outside nanoCDJ's
  allocations, probably in the sound player. A restart clears it.

## Buttons

NanoApps can read the hardware buttons but not stop RetailOS from acting on them. Play/
pause switches to the Music app and the volume buttons click, so nanoCDJ uses only Home
(deck ↔ browser). Pitch nudges are on screen.

## On-device diagnostics

Read with `python3 tools/trace.py 100 --only NC_` while nanoCDJ is running.
`tools/trace.py` only uses the read-only memory peek, so it doesn't disturb the iPod.

| Tag | When | v1 | v2 |
|---|---|---|---|
| `NC_INIT`, `NC_FPU`, `NC_READY` | launch | screen w / FPU context | h / enabled |
| `NC_PDB` | launch | heap free | largest free block |
| `NC_LIB` | launch | tracks | playlists |
| `NC_AUDIO` | launch | audio ring started | largest block |
| `NC_RING` | every 5 s | voice starts \| late ticks << 16 | mixer rate \| lead ms << 20 |
| `NC_CAL` | every 5 s | counter divisor (0 = none) \| measurements << 8 | last block time / length, % |
| `NC_HEAP` | every 5 s | heap free | largest free block |
| `NC_GAP` | every 5 s | longest gap between frames, µs | frames > 25 ms apart |
| `NC_COST` | every 5 s | slowest app_tick, µs | slowest audio render, µs |
| `NC_MP3A` | every 5 s, streaming | KB read in the last 5 s | decode misses (silence) |
| `NC_MP3B` | every 5 s, streaming | window KB \| reopens << 16 | slowest read in one frame, µs |
| `NC_MP3C` | every 5 s, streaming | read speed, KB/s | audio read ahead, ms |
| `NC_NOSTR` | every 5 s, not streaming | why: 1 not MP3, 2 file wouldn't open, 3 no memory, 4 no MP3 frames | 2: path length \| non-ASCII bytes << 16; 4: bytes searched |
| `NC_UNLD` | leaving the app | heap blocks freed | heap free after |

If the iPod reboots, NanoApps saves the previous log to `/Library/lasttrace.bin` on the
iPod, but only when the reboot preserved memory. A hard reset (watchdog) loses it.
