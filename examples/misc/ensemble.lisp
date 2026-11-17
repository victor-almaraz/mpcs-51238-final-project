;; A sequenced trio from the standard library: a bass line, a plucked
;; arpeggio, and FM bells, each through its own effects, at 100 BPM.
;;
;; Each part's metronome steps its sequencer, whose values pitch the
;; notes that trigger-notes makes, and an instrument plays them.

(module ensemble)

(import "std/instruments")
(import "std/effects")

;; Notes from a sequencer of pitches, stepped every `every` beats.
(define (melody every pitches length notes)
  (let ((clock (pipe :event))
        (steps (pipe :event)))
    (metro :every every :out clock)
    (sequencer :in clock :values pitches :events steps)
    (trigger-notes :in steps :length length :out notes)))

(defpiece main ()
  (tempo 100bpm)
  (let ((bass-notes  (pipe :event))
        (pluck-notes (pipe :event))
        (bell-notes  (pipe :event))
        (bass        (pipe :audio))
        (plucked     (pipe :audio))
        (chorused    (pipe :audio))
        (bells       (pipe :audio))
        (echoed      (pipe :audio))
        (band        (pipe :audio))
        (out         (pipe :audio)))
    (melody 1beat [45 45 48 43] 0.8beat bass-notes)
    (melody 0.25beat [57 60 64 67 69 67 64 60] 0.2beat pluck-notes)
    (melody 2beat [81 76 79 84] 1beat bell-notes)

    (play (make subtractive :voices 2 :cutoff 200hz :env-amount 1.5khz :resonance 0.4) bass-notes bass)
    (play (make pluck :voices 6 :level -16db) pluck-notes plucked)
    (play (make fm :voices 3 :ratio 3.5 :index 2 :decay 1s :sustain 0 :level -22db) bell-notes bells)

    (process (make chorus :mix 0.4) plucked chorused)
    (process (make echo :time 0.75beat :feedback 0.35 :mix 0.3) bells echoed)
    (mix :in [bass chorused echoed] :levels [2 2 2] :out band)
    (process (make reverb :mix 0.2) band out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
