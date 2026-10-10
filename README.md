# Groove Bank FX

**MIDI-Sequencer als Insert-Effekt** · Spielt den Akkord, den du ihm schickst, im Rhythmus einer Groove-Bibliothek nach Genres. · Hersteller in MPC/Force: Mission Minnow · Lizenz: MIT

Insert-Effekt-Variante von [mpc-vst-groovebank](https://github.com/saustin2010/mpc-vst-groovebank) (Port des Schwung-Moduls
[Groove Bank](https://github.com/mission-minnow/groovebank) v0.1.16). Gleiche Engine, gleiche 97 Grooves in 14 Genres – aber
als Insert-Effekt, belegt also keinen der 8 Instrument-Plugin-Plätze der Force. Das Audio der Spur läuft unverändert durch.

## Was anders ist als beim Instrument

| | Groove Bank (Instrument) | Groove Bank FX (Insert) |
|---|---|---|
| Plugin-Liste | `[SEQ] Groove Bank`, uid `GrvB` | `[SEQ] Groove Bank FX`, uid `GrvF`, Kategorie Effect |
| Akkord-Eingang | Noten der eigenen Spur | **INTERNAL** (Standard): Akkord am Plugin einstellen (ROOT, CHORD, OCTAVE) · **MIDI IN**: externe USB-MIDI-Geräte (verbindet das Plugin selbst) und eigener Port `… MIDI In` |
| Ausgang | Port `… MIDI Out` | Port **`[SEQ] Groove Bank FX MIDI Out`** (unverändert) |
| Audio | Stille | Spur-Audio unverändert durch, keine Latenz |
| Neu | – | **GENRE** (springt zum ersten Groove eines Genres, wie Knob 8 am Move), **Akkordquelle**, **MIDI IN CH** (Omni/1–16), **CC 20–30** |
| Q-Links | VARIANT, SWING, GATE · STRUM, ACCENT, LATCH · GROOVE | Bank 1: GENRE, GROOVE, VARIANT, SWING, GATE, STRUM, ACCENT, MIDI IN CH · Bank 2: ROOT, CHORD, OCTAVE (nur Regler; LATCH und SOURCE per Touch) |

Beide Plugins können nebeneinander installiert sein (eigener Ordner, eigene uid).

## Auf der Force einrichten

Zwei Wege, den Akkord vorzugeben – beide auf der Force nutzbar:

- **Interner Akkord** (Standard, `CHORD: INTERNAL`): ROOT, CHORD, OCTAVE im Plugin, keine MIDI-Verbindung nötig.
- **Von einer MIDI-Spur** (`CHORD: MIDI IN`): eine MIDI-Spur sendet an den Port **`[SEQ] Groove Bank FX MIDI In`**.
  Auf der Force bestätigt am 2026-10-09 mit Oneiroi FX (gleicher Port-Mechanismus): MIDI-Spur → Output Port
  „Oneiroi FX MIDI In“ → die Noten spielen im Insert-Plugin.

### Interner Akkord

1. **Insert setzen:** Groove Bank FX als Insert-Effekt auf die Synth-Spur legen, die den Groove spielen soll (oder auf
   irgendeine Spur mit Audio). Das Audio läuft unverändert durch.
2. **Ausgang freischalten:** *Menu → Preferences → MIDI*: bei **`[SEQ] Groove Bank FX MIDI Out`** (Input Ports) **Track**
   einschalten (mit „Enable MIDI ports when discovered“ geschieht das von selbst).
3. **Zielspur(en):** **MIDI Input** = `[SEQ] Groove Bank FX MIDI Out`, **Monitor** = **In** (nicht Auto).
4. **Akkord wählen:** rechts oben ROOT, CHORD, OCTAVE (Standard: D MIN, D2 F2 A2). Die Zeile darunter zeigt die Noten.
5. **Play drücken:** Der Groove läuft im Force-Tempo und folgt dem Transport.

Akkordwechsel im Song: ROOT/CHORD/OCTAVE sind automatisierbar (Q-Link-Bank 2) oder per CC 28–30 schaltbar.

### Akkord von einer MIDI-Spur (Clip, Pads, Keyboard)

1. Groove Bank FX als Insert setzen und die Zielspur wie oben (Schritte 1–3) einrichten.
2. Am Plugin **SOURCE** auf **`CHORD: MIDI IN`** stellen.
3. Eine **separate MIDI-Spur** anlegen: **Output Port** = **`[SEQ] Groove Bank FX MIDI In`** (Kanal passend zu
   MIDI IN CH; bei IN OMNI egal). Darauf Akkorde spielen oder einen Clip mit Akkordwechseln laufen lassen.
4. Play drücken: Groove Bank spielt die gehaltenen Akkorde im Groove an die Zielspur(en).

Der MIDI-Input-Port der Zielspur bleibt `[SEQ] Groove Bank FX MIDI Out` – nicht die MIDI-Spur selbst.

Externe USB-MIDI-Geräte (z. B. Circuit Tracks) verbindet das Plugin alle 3 s selbst mit seinem MIDI In (nicht die
Force-eigenen Ports); ihre Noten zählen in `CHORD: MIDI IN`, ihre CCs in beiden Stellungen. Die Anzeige zeigt dann
„+ USB DEVICES“.

## Bedienung

- **GENRE** (Stepper oben): HOUSE, TECHNO, GARAGE, DNB, HIPHOP, TRAP, FUNK, SOUL, JAZZ, LATIN, REGGAE, ROCK, AFRO, WORLD –
  springt zum ersten Groove des Genres und zeigt immer das Genre des aktuellen Grooves.
- **GROOVE** (Stepper darunter): alle 97 Grooves der Reihe nach, Anzeige „Name GENRE“.
- **VARIANT, SWING, GATE, STRUM, ACCENT** wie im Original; **LATCH** hält den Akkord nach dem Loslassen.
- **ROOT / CHORD / OCTAVE** (rechts oben): Grundton C…B; Akkord 1 NOTE, 5TH, OCT, MAJ, MIN, SUS2, SUS4, 7, MAJ7, MIN7,
  MIN9, DIM, AUG; OCT 0–5 (Akai-Zählung, Note 60 = C3).
- **SOURCE:** `CHORD: INTERNAL` (Standard, keine Verbindung nötig) oder `CHORD: MIDI IN` (externe USB-Geräte + Port).
- **MIDI IN CH** (unten rechts): `IN OMNI` oder `IN CH 1…16` – filtert Noten **und** CCs. Bei mehreren
  Sequencer-Plugins bekommt so jedes seinen eigenen Kanal.

### CC-Steuerung (auf dem MIDI-IN-CH-Kanal, von externen USB-Geräten oder über den MIDI-In-Port)

| CC | 20 | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 | 29 | 30 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Ziel | GENRE | GROOVE | VARIANT | SWING | GATE | STRUM | ACCENT | LATCH (≥ 64 = an) | ROOT | CHORD | OCTAVE |

CCs von angeschlossenen USB-MIDI-Geräten (z. B. Faderfox, Circuit Tracks) wirken in beiden SOURCE-Stellungen; in
INTERNAL werden nur deren Noten ignoriert.

Der Bildschirm folgt den CC-Änderungen. Alle anderen CCs, Pitchbend und Aftertouch gehen an den Ausgang weiter
(zum Zielsynth), wie im Original.

## Groove-Bibliothek: Abgleich mit dem Schwung-Repo

Geprüft am 2026-10-09 gegen das hochgeladene `groovebank-main` (Schwung-Modul v0.1.16):

- **97 Grooves in 14 Genres, vollständig.** Die Release-Builds holen `src/patterns/*.groove` per `release/fetch-library.py`
  vom Upstream-Commit `4edfa2f` (prüft jede Datei gegen ihren Git-Hash). Dieser Commit ist der aktuelle Upstream-Stand,
  und alle 14 Dateien sind bytegleich mit dem Schwung-Repo. Die Engine (`src/`) ist ebenfalls unverändert.
- Je Genre: HOUSE 7 · TECHNO 6 · GARAGE 6 · DNB 7 · HIPHOP 7 · TRAP 6 · FUNK 8 · SOUL 7 · JAZZ 7 · LATIN 9 · REGGAE 7 ·
  ROCK 8 · AFRO 6 · WORLD 6.
- **Fallstrick im Instrument-Repo:** dessen Ordner `deploy/` enthält nur `groovebank.so` und den Skin, **nicht** den Ordner
  `groovebank/patterns/`. Wer von dort von Hand installiert, bekommt nur die 3 eingebauten Notfall-Grooves (Offbeat, Four
  Floor, Tresillo). Das Release-ZIP aus GitHub Actions enthält die Bibliothek. Dieses Repo hat deshalb kein `deploy/` mehr.
- Nicht mitportiert (gehört zur Move-Oberfläche, nicht zur Engine): das Grid-Bild der Grooves (`canvas.js`) und der
  Jog-Wheel-Browser; das Genre-Springen gibt es jetzt als GENRE-Regler.

## Bauen

Wie das Instrument, über GitHub Actions: *Actions → „Release (draft)“ → Run workflow*, Version eingeben (z. B. `1.0.0`).
Heraus kommt `SEQ-Groove-Bank-FX-<version>-mpc-armv7.zip` im Draft-Release; auf die Force kopieren, entpacken,
`sh install.sh` als root. Das Plugin landet in `/sdcard/Synths/Mission Minnow - VST - [SEQ] Groove Bank FX/`
mit `groovebank_fx.so` und `groovebank/patterns/`.

Lokal (Docker nötig) mit den Werkzeugen aus [saustin2010/mpc-vst-plugins](https://github.com/saustin2010/mpc-vst-plugins) am
Commit aus `.github/workflows/release.yml`:

```
python3 release/fetch-library.py library
bash "$MPC_VST/tools/build_port.sh" vst.json
rm -rf build/groovebank && cp -R library build/groovebank && bash "$MPC_VST/tools/test_port.sh" vst.json
```

## Stand

- Offline-Host-Test (ASan/UBSan, x86): **PASSED** – u. a. Audio läuft durch, NULL-Eingang, Q-Link-Weg, Chunk-Restore.
- Eigener Funktionstest: interner Akkord spielt ohne jede MIDI-Eingabe (D MIN → D2 F2 A2), Akkordwechsel per CC 29,
  Eingangsnoten werden in INTERNAL ignoriert, Umschalten auf MIDI IN, Genre-Sprung, CC 20–27, Kanalfilter, State.
- Skin lokal gebaut, keine Warnungen von `skin_check`.
- **Noch offen auf der Force:** interner Akkord (sollte ohne Routing laufen), automatische Verbindung zu USB-MIDI-Geräten,
  Q-Link-Banken.

## Dateien

| Pfad | Was |
|---|---|
| `mpc/schwung_midi_fx.c` | Adapter: Schwung-MIDI-FX → Engine, jetzt mit `process()` (Audio durch), ALSA-MIDI-In, IN CH, GENRE, CCs |
| `mpc/engine.h` | Engine-Schnittstelle des Frameworks (mit `process` für Effekte) |
| `src/` | die Engine, unverändert vom Upstream |
| `vst.json` | `"effect": true`, uid `GrvF`, `HAS_DISPLAY_REV` |
| `params.json` | 16 Parameter (die ersten 10 wie beim Instrument, dann GENRE, MIDI IN CH und deren Pfeile) |
| `layout.conf` | Bildschirm; `design/fx_bg_edit.py` hat dafür Platz im Hintergrundbild geschaffen |
| `release/` | Bibliothek: Quelle und Commit (`library.json`), Abruf-Skript |

Herkunft: [`UPSTREAM`](UPSTREAM), Framework-Änderungen: [`FRAMEWORK.md`](FRAMEWORK.md).
