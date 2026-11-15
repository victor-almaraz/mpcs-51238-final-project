# Standard library

The standard library is a set of files written in the language and compiled into the client. `(import "std/name")` loads one. Its node IDs and errors name the file as `std/name.lisp`.

| File | Contents |
|---|---|
| `std/voices` | `voices`: polyphony over `voice-split` |

## std/voices

```lisp
(import "std/voices")

(defpiece main ()
  (let ((notes (pipe :event))
        (out   (pipe :audio)))
    (midi-in :device "" :out notes)
    (voices 8 notes
            (fn (n)
              (let ((freq (pipe :control))
                    (osc  (pipe :audio)))
                (note-to-freq :in n :out freq)
                (saw-osc :freq freq :out osc)
                (adsr :gate n :in osc :gain -18db :out out))))
    (audio-out :in out :channel 0)))
```

`(voices count notes build)` splits the notes on the event pipe `notes` among `count` voices with `voice-split` and calls `(build voice-notes)` once for each voice, with an event pipe that carries that voice's notes. When every voice is sounding, a new note takes the voice of the oldest note, which receives a note-off first. Each call to `build` is keyed by its voice number, so each voice keeps its node IDs, and its state, when the piece is replaced. `voices` returns the list of `build`'s results. Voices that write the same audio pipe sum there.
