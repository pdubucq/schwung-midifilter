# MIDI Filter for Schwung

A MIDI FX for Ableton Move (Schwung) that lets through **one key range**,
optionally transposes it by whole octaves, and sends it to the slot's own synth,
to a **stock Move track**, or to both.

Put one on every slot you want to play from the same keyboard, give each its
own range, and one keyboard that cannot split plays several Schwung slots and
Move tracks at once.

```
                 88-key keyboard, channel 1, USB-A
                                |
        every Schwung slot with Recv Ch = 1 gets every note
          |                |                |                |
       Slot 1           Slot 2           Slot 3           Slot 4
    MIDI Filter      MIDI Filter      MIDI Filter      MIDI Filter
    C-2 .. B0        C1 .. B2         C3 .. G8         C1 .. B2
    own synth        own synth        own synth        Output: move only,
                                                       Move Channel 2
                                                         -> stock Move track
                                                            listening on ch 2
```

Ranges may overlap: two slots with the same range play as a layer.

Based on [schwung-fork](https://github.com/andree182/schwung-fork). Fork's
splitter, pipes and receivers are gone; each slot now filters for itself.

## Setup (one keyboard, several tracks)

1. Set your keyboard's MIDI channel (here: 1).
2. On every Schwung slot you want to play, set **Slot Settings > Recv Ch** to
   that channel.
3. Put **MIDI Filter** in the MIDI FX position of each of those slots and set
   its range (see "Setting the range").
4. For stock Move tracks:
   * **No stock Move track may listen on the keyboard's channel.** Schwung
     does not take the keyboard away from Move, so a Move track on channel 1
     plays the whole keyboard, whatever the filters do. Give the stock tracks
     you want to play other MIDI In channels (e.g. 2 to 5) in Move.
   * On a slot, set **Output** to `move only` (or `both`) and set **Move
     Channel** to that stock track's MIDI In channel. That slot's range now
     plays the stock track: instead of the slot's synth with `move only`, as
     well as it with `both`.
   * The slot may need a synth loaded for the filter to run at all. Schwung
     only wakes a slot for external MIDI when it has a synth. The synth gets
     nothing while Output is `move only`.

## Setting the range

* Press **Learn Low** / **Learn High** (jog click, or turn the knob), then
  press the key on the keyboard. That key sets the note and is not played by
  this slot. Press the button again to cancel; learning also ends by itself
  after 10 seconds. **Status** shows `learn lo` / `learn hi` while waiting.
* Or turn **Lowest Note** / **Highest Note** to set them by hand. Both notes
  are included.
* If the low note ends up above the high note (or the other way round), the
  other end moves to the same note, so the range is that one note.

**Note names follow Move: C3 = MIDI note 60.** Most keyboards call that note
C4, so the names here are one octave lower than on your keyboard's display.

## Parameters

| Parameter | Values | Default | What it does |
|-----------|--------|---------|--------------|
| Learn Low | button | | The next key pressed becomes Lowest Note. |
| Learn High | button | | The next key pressed becomes Highest Note. |
| Lowest Note | C-2 .. G8 | C-2 (0) | Notes below are dropped. |
| Highest Note | C-2 .. G8 | G8 (127) | Notes above are dropped. |
| Output | schwung only / move only / both | schwung only | Where the filtered MIDI goes: this slot's synth, the stock Move track on Move Channel, or both. |
| Move Channel | 1 .. 16 | 2 | MIDI In channel of the stock Move track (a channel, not a track number). Only used when Output sends to Move. |
| Transpose Octave | -10 .. +10 | 0 | Shifts every note by whole octaves, for the slot and for Move. A note that would leave 0..127 is dropped. |
| Sustain | off / on | on | Off: the sustain pedal (CC 64) is not passed on. |
| Status | readout | | `ok`, `learn lo`, `learn hi`, `ch clash` (Output sends to Move and Move Channel equals this slot's Recv Ch, or Recv Ch is All; nothing is sent to Move, the slot still plays), `no move` (the host offers no Move output). |

The range filter looks at the key you **press**, before transposing: with
Transpose Octave +1 and a range of C3 .. B3, those keys play one octave higher.

With the defaults everything passes, so adding a MIDI Filter changes nothing
until you set it.

## What passes where

* **Notes** are decided when the key goes down. The key's release always goes
  where the key went, at the pitch it was played, even if you change the
  range, Output, Transpose Octave or Move Channel while holding it, so nothing
  hangs.
* **Sustain** (CC 64) follows the Sustain switch. If you switch Sustain off,
  or change Output / Move Channel while the pedal is down, the old destination
  gets a pedal-up.
* **Other controllers, pitch bend, aftertouch, program change** are passed to
  the current destination(s). Poly aftertouch follows its note.
* **All Notes Off and the other channel-mode messages** (CC 120-127) always
  pass.
* **Clock and other system messages** go to the slot's chain only, never to
  Move.

## Limits

* **Sending to Move is best effort.** Schwung only sends MIDI into Move when
  no other MIDI is arriving, so notes for a stock track can be a little late
  while you play continuously. Pitch bend, aftertouch and continuous
  controllers sent to Move are thinned (latest value wins) so they do not
  crowd out notes.
* Switching Output to `schwung only` releases every note the slot is holding on
  the stock track. Removing the module from the slot while keys are held does
  not; the stock track can then keep a note hanging until you play it again.
* **Transpose is whole octaves only**, at most 10 either way. Notes pushed
  outside 0..127 are dropped, not folded back.
* **Learning also sounds the key on other slots** whose range contains it:
  every slot hears the keyboard, only this one swallows the key.
* The filter cannot tell where a note comes from: a Move pad or sequencer note
  reaching the slot is filtered (and learned) like a keyboard key.
* Move echoes what it receives back out to USB-A, and to any Schwung slot
  whose Recv Ch equals Move Channel. Keep Move Channel different from every
  slot's Recv Ch unless you want that slot to play it too.

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| A stock Move track plays the whole keyboard | It listens on the keyboard's channel. Move it to another channel. |
| A slot is silent | Recv Ch does not match the keyboard's channel; the key is outside the range; Output is `move only`; Transpose Octave pushes the note outside 0..127. |
| Nothing reaches the Move track | Status shows `ch clash` (Move Channel = Recv Ch, or Recv Ch = All); Move Channel does not match the stock track's MIDI In channel; the slot has no synth loaded. |
| The range is one octave off | Note names: C3 here is C4 on most keyboards. |
| A key does not play right after Learn | That key was learned and swallowed; press it again. |

## Build, test, install

```bash
./scripts/build.sh            # dist/midifilter/, dist/midifilter-module.tar.gz
bash tests/test_midifilter.sh  # needs a host C compiler and ../schwung/src
python3 tests/test_help_json.py
./scripts/install.sh           # copies to modules/midi_fx/midifilter/ on the Move
```

Then add **MIDI Filter** from the MIDI FX position of a Schwung chain.

### On Windows (WSL + Docker)

The toolchain lives in the devcontainer image built from `scripts/Dockerfile`
(VS Code names it `vsc-schwung-midifilter-<hash>`; find yours with
`wsl -d Ubuntu -- docker image ls`). This checks for warnings, builds, and runs
the C tests, with `schwung` checked out next to this repo:

```bash
wsl -d Ubuntu -- bash -lc 'docker run --rm -u $(id -u):$(id -g) -v /mnt/c/Users/Pascal/git:/work -w /work/schwung-midifilter vsc-schwung-midifilter-780a95e76d092429deaa7d65fb705ab299126f69ecb1e4e8acff5fe4a06386f3:latest bash -c "aarch64-linux-gnu-gcc -Wall -Wextra -O2 -fPIC -shared -I src -I ../schwung/src src/dsp/midifilter.c -o /dev/null && echo WARN-CLEAN && ./scripts/build-module.sh && bash tests/test_midifilter.sh"' 2>&1 | tail -25
```

The image only has `python3-minimal` (no `json` module), so run the Python
checks on Windows instead:

```bash
python tests/test_help_json.py
python -c "import json; json.load(open('src/module.json'))"
```

Adjust the `/mnt/c/...` path and the image name for your machine.

## Credits

- Schwung framework and host APIs: Charles Vestal and contributors
- Based on Fork (move-anything-superarp / schwung-fork contributors)
