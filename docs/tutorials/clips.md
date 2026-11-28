# Clip loops

This tutorial builds a band that loops MIDI clips on the beat clock: a drum machine that plays the clips of one bank, a loop matrix on the project's page that chooses them, and a bass and a pad with banks, rows, effect chains, and tabs of their own. It assumes the project and page of [A first piece](../guide/first-piece.md).

## A project and its clips

A clip is a MIDI file that an instrument loops. A bank is a folder of clips, `clips/<bank>/` under the project root, and each of its files is a slot: `1 four on the floor.mid` is slot 1, `2 backbeat.mid` slot 2, up to slot 64. The title after the number is optional; `3.mid` is slot 3.

Create a project folder `band` holding a `project.lisp`:

```lisp
(project :name "band" :page-port 8000)
```

and copy the `clips/` folder of `examples/autoband/` into it. Its banks are `bass`, `pad`, `lead`, and `drums`: clips of four bars of 4/4, the pitched ones in A minor.

A clip is any Standard MIDI File of format 0 or 1. The player merges its tracks, reads a quarter note as a beat, and ignores its tempo and meter events, so a clip follows the piece's tempo. `examples/make-clips.py` writes autoband's clips: run from `examples/`, it rewrites `autoband/clips/` byte for byte, as format 0 files of 96 ticks a quarter note, the drums on channel 10.

## A drum machine

Save as `main.lisp`:

```lisp
;; A drum machine that loops the first clip of the bank clips/drums/ at 112 BPM.

(import "std/instruments")

(defpiece main ()
  (tempo 112bpm)
  (let ((notes (pipe :event))
        (out   (pipe :audio)))
    (clip-player :bank "drums" :select 0 :grid 16beat :out notes)
    (play (make drum-kit) notes out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
```

- `(tempo 112bpm)` sets the beat clock's tempo when the piece starts. Clips play in beats, so every clip follows it.
- `clip-player` plays the clips of bank `drums`, `clips/drums/`, as notes on the event pipe `notes`.
- `:select` chooses the slot that plays, counting from 0: `:select 0` plays the file of slot 1, `1 four on the floor.mid`. `-1` plays nothing, and so does a slot without a file.
- `:grid` is the grid on which the slot changes: lines a multiple of 16 beats apart on the beat clock, four bars of four. A change of `:select` waits for the next grid line; the clip playing plays until then, its held notes end there, and the chosen clip starts on the line.
- `drum-kit`, from [std/instruments](../library.md#stdinstruments), plays the General MIDI drum notes: kick (36), snare (38), clap (39), closed hi-hat (42), and open hi-hat (46), on any channel.

Play the piece. A clip loops at its length rounded up to whole bars, from the grid line it started on. A `clip-player` that the running plan does not hold yet starts its slot at once, at the place the clip would have reached had it started on the last grid line, so a player added by an update keeps time with the clips already playing.

`:select` takes a constant or a control pipe. Change `0` to `1` and update the piece: the backbeat starts on the next grid line, about 8.6 s apart at 112 BPM. A page control chooses the slot while the piece plays: `:select (select "Pattern" :options ["four on the floor" "backbeat" "broken"])` gives the slot as the index of the chosen entry. [Pipes and primitives](../language.md#pipes-and-primitives) describes the `clip-player`'s other ports, `:playing` and `:queued`.

## The loop matrix

Replace `main.lisp` with:

```lisp
;; A drum machine that loops the clips of clips/drums/, chosen on the page.

(import "std/instruments")

(defpiece main ()
  (tempo 112bpm)
  (let ((notes (clips "Drums" :initial 0))
        (out   (pipe :audio)))
    (play (make drum-kit) notes out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (page (panel "Loops" (section notes)))))
```

`(clips "Drums")` makes two things: the `clip-player` of bank `drums`, and a row on the page of its eight slots, a cell for each, with a stop button. It returns the player's notes for an instrument to play. The bank is the label as an address takes it, `"Drums"` as `drums`, unless `:bank` names another. `:slots` (8), `:grid` (`16beat`), and `:initial` are the player's; `:initial 0` starts the row on slot 1, and without it the row starts with nothing playing. Until the page sends a slot, the player plays `:initial`, so the piece plays as written without the page.

A row of `clips` is a control on the page, so this piece needs the `:page-port 8000` of `project.lisp`; without it, the compile fails at `clips`. Play the piece and open `http://localhost:8000/`, or the GUI editor's "Page · band" tab. Each cell shows its slot number and its clip's title, or Empty.

- A press on a cell that holds a clip starts it on the next grid line. The cell flashes and reads Next until the line, and the playing cell pulses.
- A press on the playing cell, or on the stop button, stops the row on the next grid line; the cell reads Stops until then. A press on a cell waiting to start takes the change back.
- A press on an empty cell, a cell's Load button, or L on a cell opens a MIDI file. The page uploads it, and the server saves it as the slot's clip, `clips/drums/<slot> <title>.mid` titled after the file's name, in place of the slot's file before. The clip is in the bank at once and on every later run; a press on the cell starts it.

The arrow keys move between cells. A section of several rows of `clips` stacks them, one a line: a loop matrix. [Controls in the language](../language.md#controls-in-the-language) describes the row's addresses, and [Pages](../guide/io.md#pages) the page port.

## A band

Replace `main.lisp` with:

```lisp
;; A band of three instruments that loop the clips of their banks: a
;; plucked bass, a synth pad, and a drum machine, each through an effect
;; chain into a master chain. A tab for each instrument, and one for the
;; loops and the master chain.

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (tempo 112bpm)
  (let ((bass  (make pluck :feedback 0.995 :damping 0.55 :level -6db))
        (pad   (make subtractive :attack 300ms :release 1.2s :cutoff 700hz :level -20db))
        (drums (make drum-kit))
        (bass-notes (clips "Bass" :initial 0))
        (pad-notes  (clips "Pad" :initial 0))
        (drum-notes (clips "Drums" :initial 0))
        (bass-out (pipe :audio))
        (pad-out  (pipe :audio))
        (drum-out (pipe :audio))
        (bus (pipe :audio))
        (out (pipe :audio)))
    (define bass-panel (synth-panel "plucked bass" bass))
    (define pad-panel (synth-panel "synth pad" pad))
    (define drum-panel (synth-panel "drum machine" drums))
    (play bass bass-notes bass-out)
    (play pad pad-notes pad-out)
    (play drums drum-notes drum-out)
    (define bass-chain (chain bass-out bus (make overdrive :drive 2 :tone 2khz :level -4db :mix 0.7)))
    (define pad-chain (chain pad-out bus (make chorus) (make reverb :mix 0.4)))
    (define drum-chain (chain drum-out bus (make reverb :size 0.6 :mix 0.12)))
    (define master (chain bus out (make overdrive :drive 1.5 :tone 6khz :level -3db :mix 0.3)))
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (page (tab "Bass" bass-panel bass-chain)
          (tab "Pad" pad-panel pad-chain)
          (tab "Drums" drum-panel drum-chain)
          (tab "Master" (panel "Loops" (section bass-notes pad-notes drum-notes)) master))))
```

- Each instrument has a bank and a row: `(clips "Bass")` plays `clips/bass/`, and `(clips "Pad")` plays `clips/pad/`. One `clip-player` plays a bank. All three players share the beat clock and the 16-beat grid, so a clip chosen in one row starts on the line where the others change.
- `pluck` is a plucked string and `subtractive` a filtered sawtooth. Each `synth-panel` draws its instrument's controls as knobs, and comes before `play`, which builds the voices that read them.
- Each instrument plays into a pipe of its own, and `chain` runs it through its effects into `bus`. Writers to an audio pipe sum, so `bus` holds the three chains' mix, and the master chain runs from `bus` to `out`. Each `chain` returns a pedalboard of its effects' pedals. [Pedals and chains](../library.md#pedals-and-chains) describes `chain`.
- Each `tab` puts an instrument's panel and pedalboard on a page of its own, and the Master tab holds the loop matrix, a section of the three rows, and the master chain. The page shows one tab at a time, chosen from the strip along its top, and each tab shows the tabs its audio comes from and goes to: Bass, Pad, and Drums go to Master, and Master goes to the output. A tab is a view: a row of cells plays the same with any tab shown.

Play the piece. The bass, the pad, and the drums start on their first clips; a press on a cell of the matrix changes a row on the next grid line. `examples/autoband/` plays this band with a fourth instrument, an FM lead whose row starts with nothing playing.

## Next

- [Controls in the language](../language.md#controls-in-the-language) describes the other controls, groups, and tabs, and [Pages](../language.md#pages) the page port's requests, among them the bank listing and clip upload.
- The [standard library](../library.md) lists the instruments and effects, their controls, and how to write a class of either.
- `examples/autoband/` plays this band with four instruments; `examples/README.md` describes it and how to run it.
