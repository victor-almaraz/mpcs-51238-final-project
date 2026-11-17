;; A polyphonic synthesizer for any MIDI keyboard, from the standard
;; library: eight subtractive voices through a chorus and a reverb.

(module keys)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((notes  (pipe :event))
        (synth  (pipe :audio))
        (wide   (pipe :audio))
        (out    (pipe :audio)))
    (midi-in :device "" :out notes)
    (play (make subtractive :voices 8 :cutoff 600hz :env-amount 2khz :resonance 0.35
                            :attack 10ms :release 600ms :level -6db)
          notes synth)
    (process (make chorus :mix 0.35) synth wide)
    (process (make reverb :size 0.88 :mix 0.25) wide out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
