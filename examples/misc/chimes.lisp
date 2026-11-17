;; Aleatory wind chimes: on each sixteenth note a chime sounds with a
;; chance of one in three, on a pitch drawn at random from a pentatonic
;; scale. The seed fixes the draws, so a render is the same every time.

(module chimes)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (tempo 72bpm)
  (let ((ticks   (pipe :event))
        (chosen  (pipe :event))
        (pitches (pipe :event))
        (notes   (pipe :event))
        (bells   (pipe :audio))
        (echoed  (pipe :audio))
        (out     (pipe :audio)))
    (metro :every 0.25beat :out ticks)
    (chance :in ticks :probability 0.33 :out chosen)
    (sequencer :in chosen :values [72 74 76 79 81 84 86 88] :order :random :events pitches)
    (trigger-notes :in pitches :length 1beat :out notes)
    (play (make fm :voices 6 :ratio 3.5 :index 1.5 :attack 1ms :decay 2s :sustain 0 :release 2s
                   :level -8db)
          notes bells)
    (process (make echo :time 0.75beat :feedback 0.45 :mix 0.3) bells echoed)
    (process (make reverb :size 0.92 :mix 0.4) echoed out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
