;; sampler-keys: a sampled piano for any MIDI keyboard. One recording of
;; middle C plays across the keyboard: an octave up plays it at twice the
;; speed. Eight voices; each note's envelope releases over 400 ms.

(import "std/instruments")

(defpiece main ()
  (let ((piano (buffer :file "samples/c4.wav"))
        (notes (pipe :event))
        (out   (pipe :audio)))
    (midi-in :device "" :out notes)
    (play (make sampler :buffer piano :root 60 :voices 8 :release 400ms :level -6db) notes out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
