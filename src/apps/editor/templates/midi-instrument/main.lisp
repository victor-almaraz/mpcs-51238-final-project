;; A subtractive synthesizer of eight voices for any MIDI keyboard, through
;; a reverb, on both channels. Help → Standard Library describes the
;; instruments and effects.

(module main)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((notes  (pipe :event))
        (played (pipe :audio))
        (out    (pipe :audio)))
    (midi-in :device "" :out notes)
    (play (make subtractive :voices 8 :cutoff 1200hz :resonance 0.3 :level -12db) notes played)
    (process (make reverb :size 0.7 :mix 0.25) played out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
