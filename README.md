# Granny

A granular sampler for the MPC OS standalone hardware (Force, MPC Live/One/X/Key; VST2, armv7), inspired by the Torso
S-4's sculpting sampler. It is built on **[Omni Sampler](https://github.com/B0ss-M/MrHighMan) by MR HighMan**: every
instrument Omni Sampler reads (Akai, E-mu, Roland, Kontakt/Maschine, SF2, SFZ, WAV/AIFF/FLAC/Ogg, and the disk images
they came on) becomes material for grains. It keeps Omni Sampler's pages (PLAY, LAYERS, BROWSE, SETUP, MOD, INFO) and
adds a **GRAIN** page with two things, and a **SLICE** page (below) that finds the breaks and hits in a sample:

- **Grain engine** (ENGINE: GRANULAR): every note plays a cloud of grains from its zone's sample instead of one
  playhead. Keys pitch the grains, and the cloud runs through the voice's envelope, filters, LFOs and mod matrix like a
  sampled note. ENGINE: SAMPLER plays the instrument the normal way.
- **GRAIN FX**: a granular delay on the plugin's output (before drive and reverb), after the S-4's MOSAIC: the sound
  goes into a 6-second ring, grains read it back tempo-synced, free or at their own speed, LOCK freezes the ring.

Granny is a separate plugin (own name, uid `Grny`, own folder and library): it installs next to Omni Sampler.

## GRAIN page

**Grain engine** (left; the page's first 11 Q-Links, GRAIN LIMIT and the switches touch only):

| Control | What it does |
|---|---|
| ENGINE | SAMPLER (classic playback) or GRANULAR (grain clouds) |
| POSITION | where in the sample grains start, 0-100 % (the display marks it in cyan) |
| SCAN | moves that position through the sample (or slice) while a note plays: 100 % (default) = the sample's own speed, 50 % = half speed, 200 % = double, 0 = frozen, negative = backwards: the playback speed, with the pitch left alone |
| SIZE / SIZE SYNC | grain length 5 ms - 2 s, or a tempo division (4/1 ... 1/64, triplets, dotted) |
| DENSITY / RATE SYNC | grains per second 1-200, or one grain per tempo division (tight rhythmic grains) |
| CONTOUR | grain envelope: centre = smooth, left = percussive ramp down, right = swelling ramp up |
| SPRAY / SPRAY MODE | scatters the start position around POSITION (violet on the display); RANDOM per grain, WARP a smooth drift |
| PITCH | grain pitch on top of the key, -24..+24 semitones |
| PATTERN | a fixed pseudo-random pattern of octave and fifth jumps over 16 grains; turning it up adds jumps one by one |
| DETUNE | random pitch per grain, up to +-1 semitone |
| REVERSE | share of grains that play backwards |
| STEREO | random pan per grain |
| GRAIN LIMIT | grains for all voices together (16-256, default 96): the CPU guard. Each voice plays up to 16 |

Release matters: a grain cloud has no end of its own, so notes stop with the envelope (RELEASE on PLAY lengthens the
tail). One-shot zones (drum kits) also follow the note in GRANULAR.

**GRAIN FX** (right; the last 5 Q-Links: MIX, SCAN, SIZE, PITCH, FEEDBACK):

| Control | What it does |
|---|---|
| MODE | DLY SYNC: grains read one RATE behind (a tempo-synced granular delay); DLY FREE: SCAN sets the delay (10 ms - 2 s); STRETCH: SCAN sets the read head's speed (-200..+200 %, 0 = it stands still) |
| RATE | the grain clock (and the delay in DLY SYNC); while MPC plays, grains fall on the song's grid |
| LOCK | stops writing: the ring keeps what it holds and the grains keep playing it (freeze) |
| MIX | dry/wet (0 = off: the ring keeps filling, no grains run) |
| SIZE, PITCH, SPRAY | grain length 10 ms - 1 s, pitch -24..+24, position scatter and stereo spread |
| FEEDBACK | grains written back into the ring (up to 95 %, soft-limited) |
| PATTERN, CONTOUR | as in the grain engine |

**MOD page additions:** destinations Grain Position, Grain Size, Grain Density, Grain Spray, Grain Pitch and FX Mix,
and a seventh LFO wave, DRIFT (smoothed random, glides between random values: good on Grain Position).
Every GRAIN setting is saved with the project and, with SAVE TO LIBRARY, in the instrument (`.omni`).

Tempo: the plugin follows MPC's tempo and play/stop (wrapper `HAS_LFO_BPM`, `HAS_TRANSPORT`). The song position counts
from the moment MPC starts playing (or jumps back in a loop): synced grains line up with bars when playback starts on
a bar.

## SLICE page

Granny analyses every sample when it loads: where the sounds are (silent gaps between them), where the hits are
(drum onsets, with a strength), and the tempo. From that it finds the **breaks** in a file that holds several (a pack of
Amen variations, say) and the **hits** inside them, and KEY MODE decides what a key plays:

| KEY MODE | A key plays |
|---|---|
| SLICES (default) | slice 1, 2, 3 ... from KEY BASE up: the sample is cut into SLICES pieces (1-64) as soon as it loads, and cut again whenever SLICES or SLICE BY changes |
| PITCH | the whole sample (or the break REGION picks), pitched by the key: the normal way |
| POSITIONS | one of 8/16/32/64 positions spread over the sample or REGION, from KEY BASE up (the pads as places in the sound) |
| BREAKS | break 1, 2, 3 ... from KEY BASE up |
| HITS | hit 1, 2, 3 ... from KEY BASE up (like chopping on an MPC) |

In POSITIONS, BREAKS, HITS and SLICES keys play at the sample's own pitch; keys outside the slices stay silent. ENGINE decides how: SAMPLER plays the break or hit
straight through (a classic chop), GRANULAR plays it as grains (POSITION is then where in the break they start, SCAN
moves through it).

| Control | What it does |
|---|---|
| SLICES | how many slices (1-64): turn it and the slices are spread over the keys again |
| SLICE BY | GRID (default): even cuts, each moved onto the strongest hit near it; HITS: cuts at the strongest hits, so slices start on drums but come out uneven |
| BREAKS BY | AUTO (gaps when the file has several sounds with silence between them, else 4-bar chunks), GAPS (silences of 150 ms or more), 1 / 2 / 4 BARS (chunks of the detected tempo from the first hit, each boundary moved onto the nearest hit) |
| HIT SENS | how strong an onset must be to count as a hit |
| REGION | PITCH and POSITIONS modes: 0 = the whole sample, n = only break n |
| KEY BASE | the first key (default C1 = 36, MPC pad A01) |
| SCAN FIT | GRANULAR: SCAN plays once through the break in 1/2 ... 8 bars at MPC's tempo (the break follows the song's tempo, the grains keep its pitch); OFF = the SCAN knob's speed |

The display shows the sample with its breaks (blue, a yellow line where one starts, orange the one playing), the hits
(green ticks) and where grains start; in SLICES mode the top row shows the slices instead of the breaks. The top line reads e.g. `4 breaks (auto) | 154 hits | 137 BPM`.

Breaks back to back without gaps need the tempo: check the BPM on the display (a half or double tempo is possible with
unusual patterns) and pick 1, 2 or 4 BARS to match how long the breaks are.

### Starting points

- **Frozen pad:** GRANULAR, SCAN 0, SIZE ~300 ms, DENSITY ~50/s, SPRAY 30 % WARP, STEREO 70 %, DETUNE 20 %, RELEASE up.
- **Stretched vocal or loop:** POSITION 0, SCAN 25-50 %, SIZE ~120 ms, DENSITY ~40/s, SPRAY 5 %.
- **Rhythmic glitch (psytrance gates):** RATE SYNC 1/16, SIZE SYNC 1/16, CONTOUR -70, PATTERN 50 %, REVERSE 20 %.
- **Octave shimmer:** GRAIN FX DLY SYNC 1/8, PITCH +12, FEEDBACK 60 %, SPRAY 30 %, MIX 35 %, with the reverb after it.
- **Slice and play:** load a break, SLICES 16, play C1 upwards (Force pads in Notes mode, or a keyboard). ENGINE SAMPLER for
  clean chops, GRANULAR for grains inside each slice; turn SLICES on a Q-Link to re-cut on the fly.
- **Amen pack, played by pads:** load the file, SLICE: KEY MODE BREAKS, ENGINE GRANULAR, POSITION 0, SCAN FIT 4 BARS,
  SIZE ~80 ms, DENSITY ~60/s: each pad plays one break locked to MPC's tempo; turn SIZE, SPRAY and PATTERN for glitches.
- **Chop and play hits:** KEY MODE HITS, ENGINE SAMPLER, Voice Mode MONO for choked hits.
- **Freeze a moment:** GRAIN FX STRETCH, SCAN at 0 %, play something, press LOCK; move SCAN to scan through it.

### Not like the S-4

Granny is an instrument plugin: MPC gives instrument plugins no audio input, so it cannot record or process live
audio. For a granular effect on a track's audio, the catalog has effect plugins (Boris Granular, Overcast). The grain
engine reads the loaded instrument; GRAIN FX processes what the plugin itself plays.

## Formats

| Family | Files |
|---|---|
| Akai S900 / S1000 / S3000 | `.p .p1 .p3 .s .s1 .s3 .p9 .s9 .s9c`, MESA `.s3p`; Akai hard-disk, CD and floppy images |
| Akai S5000 / S6000 / Z4 / Z8 | `.akp`, `.akm` |
| Akai MPC | `.xpm` (XML, MPC 2/3 JSON, and the `<Program>`/`<Keygroups>` layout preset generators write), `.xpj` / `.xty` projects, MPC1000 `.pgm`, MPC2000/3000 `.pgm` / `.snd`, MPC60 `.set` |
| E-mu | EOS / E4 `.e4b`, Emulator III `.e3b .e3x .esi`, Emax / Emax II, Emulator I / II, Emulator X `.exb` + `.ebl`; EOS and EIII CD images |
| Native Instruments | Kontakt `.nki` / `.nkm` (Kontakt 1, 2-4.1, 4.2, 5-8 and both monolith kinds), NCW samples, Maschine 1 `.msnd`, Maschine 2/3 `.mxsnd` |
| Roland | S-700 series (S-750 / S-760 / S-770, SP-700, DJ-70) CD-ROM, hard-disk and floppy images: volumes (banks), performances and patches; S-50 / S-550 / S-330 / W-30 floppy images and S-500 "LAND" CD-ROMs |
| Open formats | SoundFont 2 `.sf2`, SFZ `.sfz`; samples in WAV (PCM, float, ADPCM, RF64), AIFF/AIFC, FLAC, Ogg Vorbis |
| Disk images | ISO 9660 (Joliet, Rock Ridge; `.iso`, raw `.bin`), FAT12/16/32 (floppies, partitioned cards and ZIP disks), Akai and E-mu sampler file systems, HFE and IMD floppy images. Images open like folders, also nested (an Akai CD image on a FAT card) |

Roland images mount like folders: S-700 volumes (banks) hold their patches and a Performances folder, "All Patches"
lists every patch; S-500 disks list their patches (P11-P28), a LAND CD one folder per disk. Multi-floppy S-700 sets
are not read yet. S-500 patches play with their loop tune (the per-tone correction for short loops). Checked
against a real S-50 CD (L-CD1, all disks); the S-700 readers follow ConvertWithMoss's notes and were checked with
generated images only: please report how real S-700 discs load.

Not possible: encrypted commercial Kontakt libraries (Kontakt Player / NKS protected). They are detected and
reported as such.

Checked so far against test files made with ConvertWithMoss, real Kontakt 6.8 and Maschine files and generated
disk images: SFZ, SF2, XPM, Akai S1000/S3000 CD images, E4B and EOS CDs, Emulator III / X, Emax / Emax II,
Emulator I / II floppies, Kontakt 1 and 5+, Maschine 1-3, ISO and FAT images. Written but not yet checked against
real files: S900, AKP/AKM, MPC JSON programs and projects, MPC1000/2000/60, MESA, Kontakt 2-4 and monoliths, encrypted-library detection, Akai
floppies, IMD. Reports and sample files welcome.

## Pages (inherited from Omni Sampler)

- **PLAY**: the front panel. Top: instrument stepper and name, format. PITCH (transpose, bend range, fine tune),
  VOICE (glide, voices, drive), REVERB (mix, size, damping); filter type, voice mode and interpolation buttons; the
  display shows the zone the last note played (name, root, keys, velocity range, hit velocity) and the instrument;
  volume, pan, status; FILTER (cutoff, resonance), ENVELOPE faders, velocity sensitivity and filter velocity.
  Along the bottom (where Omni Sampler has 16 touch pads) a strip shows the sample's slices (or breaks) and hits,
  the one playing in orange; PLAYING above it names it. The envelope and filter
  controls adjust the instrument's own settings: attack and release add time, decay and sustain scale them.
  The display also draws the loaded sound: the waveform of the zone last played, and a keyboard strip showing which
  keys each layer covers (lit while a key plays).
- **LAYERS**: up to four instruments (slots A-D) at once. Load into a slot by choosing it on BROWSE (SLOT A-D)
  before tapping a preset. Each slot has a key range (low / high), volume, tune and mute; the keyboard map shows the
  ranges in the slot colours. **Layer Mode** Layer plays every slot whose range holds the key; Keyswitch plays only
  the slot last selected with the four keyswitch notes from **Keyswitch Base** (default C0, 24-27). **AUTO SPLIT**
  divides the keyboard evenly between the loaded slots; **CLEAR SLOT** empties the selected slot.
- **BROWSE**: drives (Plugin Library, Internal, every USB/SD volume under `/media`), folders, files and disk
  images. Tap a file with several instruments (an SF2 bank, a multi, a disk partition) to list them. The arrows
  next to the instrument name step through the presets of a file, or the files of a folder.
- **SETUP**: polyphony, voice mode (poly / mono / legato) and glide, bend range ("Inst" = the instrument's own),
  volume, pan, transpose, fine tune, velocity sensitivity, filter velocity, interpolation, reverb damping,
  memory limit, the first slice key and MIDI program change (selects the n-th preset of the loaded file). **Auto Loop**
  finds a sustain loop for samples that sustain but have none (a zero-crossing match near the end of the sample;
  drums, one-shots and decaying sounds are left alone); applies to the next load.
- **MOD**: two LFOs (sine, triangle, saw up / down, square, sample & hold; rate 0.02-20 Hz or synced to MPC's tempo,
  4 bars to 1/16 with triplets and dotted; free-running or restarted by every note) and an 8-slot modulation matrix:
  source (LFO 1/2, mod wheel, aftertouch, pitch bend, velocity, key, per-note random, amp envelope) to destination
  (pitch +-12 semitones, cutoff +-4 octaves, resonance, volume (silent at -100%, +6 dB at +100%), pan, sample start,
  drive, reverb mix, LFO 1/2 rate +-3 octaves), amount -100..100%. The slots come routed (LFO 1 to pitch, LFO 2 to
  cutoff, mod wheel to cutoff, ...) at amount 0: turn an amount up to use one. Drive, reverb mix and LFO rates follow
  only the global sources (LFOs, wheel, aftertouch, bend).
- **INFO**: every supported format with its file extensions; a green dot marks formats checked against test
  files, an amber one formats supported but not yet tested on real files.

Q-Links follow the screen sections: each Q-Link bank (the Q-Link button on 4-knob MPCs) is one section of the
page, in this order. PLAY: envelope (attack, decay, sustain, release) / filter (cutoff, resonance, filter velocity,
velocity) / pitch (transpose, bend, fine tune, glide) / drive, reverb mix, volume, pan. SETUP: voice / output /
playback / effects and system. MOD: amounts 1-4 / amounts 5-8 / LFO 1 / LFO 2. LAYERS: one bank per slot A-D (low
key, high key, volume, tune). GRAIN: position, scan, size, density / contour, spray, pitch, pattern / detune, reverse,
stereo, FX mix / FX scan, size, pitch, feedback.

## Disk images, extraction and projects

Presets inside a disk image play straight from it: tap the image, open a bank (folder), tap a patch, play. With
**Auto Extract** on (BROWSE, default Disk Images) the plugin then writes that one preset into the Plugin Library,
`Extracted/<image>/<preset>.omni` with its samples in `<preset> Samples/`, and the loaded instrument refers to that
file. A project saves only that, so reopening it loads the one preset without the image (which may be gone). The
instrument stepper and MIDI program changes still walk the image's presets while the image is there; a preset
extracted before is reused, not written again. **SAVE TO LIBRARY** does the same for anything loaded (e.g. one
preset of a big SF2 bank) and also stores the current sound settings (envelope, filter, pitch, MOD page, ...) in
the file: loading that instrument later brings them back, so a customised sound can be kept. Instruments extracted
by an older version from a format since read better (Roland S-500 loops before 1.4) are re-read from their image
when it is still there and rewritten in place, keeping their settings. `.omni` is the plugin's own lossless format (JSON: every zone, envelope, filter and LFO
setting; 16-bit WAV samples) and loads like any other instrument.

## Where to put sound files

Anywhere the browser reaches: the **Plugin Library**, the internal storage, or a USB stick / SD card. The Plugin
Library defaults to `<plugin folder>/granny/library`; internal storage is small, so move it to an SD card or SSD:
browse to a folder there and tap **SET LIBRARY HERE** (BROWSE). Extracted instruments go to its `Extracted` folder.
If that drive is missing later the plugin falls back to the internal library. Keep sample folders next to the instrument files that use them (Kontakt and Maschine
paths are also searched two folders up, as libraries are laid out). Samples are loaded into memory up to the
**Memory Limit** (SETUP, default 512 MB); samples over it are skipped and reported.

## CPU

Not measured on hardware yet. Desktop numbers (x86, -O2, one 128-frame block): one sampled note 3 us; one grain cloud
of 16 grains 10 us; 8 notes with 96 grains (the default limit) 43 us. Expect several times that on a Force / MPC Live
(Cortex-A17): lower GRAIN LIMIT on the GRAIN page or Polyphony on SETUP if a project gets close. Measure with
`$MPC_VST/tools/bench.sh build/granny.so <ip> -j > bench.txt` and commit `bench.txt` (the release workflow puts it
into INSTALL.md).

## Install

Download `Granny-<version>-mpc-armv7.zip` from Releases, unzip it, copy the folder to the device and run
`sh install.sh` as root (it stops MPC, backs up `MPC.settings`, installs and restarts MPC). The zip's `INSTALL.md` has
the full steps. `uninstall.sh` removes it again.

## Build and release

GitHub Actions: **Actions -> Granny release -> Run workflow** (version, optional dry run). It builds the armhf
plugin and skin with [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)' reusable workflow, runs the host
test, packages the zip and attaches it to a draft release. Test that zip on the device, then publish the draft.

Locally (needs Docker, or `arm-linux-gnueabihf-g++-12` and a Python with Playwright):

```
git clone https://github.com/sd88me/mpc-vst-plugins
export MPC_VST=$PWD/mpc-vst-plugins
python3 design.py                       # params.json, layout.conf, skin.css, art/ (the skin design)
./build.sh                              # build/granny.so (armhf), build/skin/, pluginlist-entry.xml
"$MPC_VST/tools/test_port.sh" vst.json  # x86 host test (ASan)
tests/build_x86.sh                      # build/x86/probe, test_engine and grain_render (ASan; --fast for -O2)
build/x86/test_engine <file> [preset] [notes]
build/x86/grain_render <file> out.wav engine=1 g_scan=50 fx_mix=40 notes=48,55 secs=6 hold=4   # offline render
build/x86/slice_probe <file> [hit_sens]  # breaks, hits and tempo the analysis finds
```

Omni Sampler 1.4's source passes the host transport through an engine callback that the public mpc-vst-plugins
wrapper does not have; Granny takes tempo and play state through the wrapper's `HAS_LFO_BPM` / `HAS_TRANSPORT`
parameters instead, so it builds against the public wrapper as it is.

## License and credits

LGPL-3.0-or-later, like Omni Sampler. Granny is a modified version of Omni Sampler 1.4.0 by MR HighMan
(https://github.com/B0ss-M/MrHighMan): the grain engine, GRAIN FX, the GRAIN page, the new modulation destinations and
the DRIFT wave, the SLICE page and its analysis are additions; the format readers, browser, layers and the rest of the engine are Omni Sampler's. The
format readers are C++ translations of [ConvertWithMoss](https://github.com/git-moss/ConvertWithMoss) by Jürgen
Moßgraber (LGPL-3.0); keep this attribution and the included [LGPL-3.0.txt](LGPL-3.0.txt) and [GPL-3.0.txt](GPL-3.0.txt)
when distributing it. Bundled libraries keep their own licences: dr_flac (public domain / MIT-0), stb_vorbis (public
domain / MIT), tinf (zlib). See [src/third_party/VENDORED.md](src/third_party/VENDORED.md). The GRAIN page's ideas
(SCAN, SPRAY, PATTERN, CONTOUR, LOCK) follow the published description of the Torso Electronics S-4; Granny is not
affiliated with Torso Electronics.
