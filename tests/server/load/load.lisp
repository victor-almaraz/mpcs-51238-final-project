;; The load piece of the v2 load test (AGENTS.md, threads.md §1.2): eight
;; instruments of eight voices, each through two effects. Each instrument
;; plays a note every quarter beat that lasts two beats, so all eight of
;; its voices sound once the first two beats have passed: 64 voices.

(module load)

(import "std/instruments")
(import "std/effects")

;; An instrument playing pitches, a note every quarter beat for two beats,
;; through two effects into out.
(define (player instrument first second pitches out)
  (let ((steps (pipe :event))
        (pitch (pipe :control))
        (notes (pipe :event))
        (dry   (pipe :audio))
        (wet   (pipe :audio)))
    (metro :every 0.25beat :out steps)
    (sequencer :in steps :values pitches :out pitch)
    (trigger-notes :in steps :pitch pitch :velocity 0.7 :length 2beat :out notes)
    (play instrument notes dry)
    (process first dry wet)
    (process second wet out)))

(defpiece main ()
  (let ((mixed  (pipe :audio))
        (stereo (pipe :audio :channels 2)))
    (player (make subtractive :wave :saw :cutoff 300hz) (make chorus) (make reverb)
            [36 43 48 55 60 67 72 79] mixed)
    (player (make subtractive :wave :pulse :cutoff 600hz) (make echo) (make reverb :size 0.7)
            [40 47 52 59 64 71 76 83] mixed)
    (player (make fm :ratio 2 :index 3) (make chorus :rate 0.5hz) (make echo :time 0.75beat)
            [43 50 55 62 67 74 79 86] mixed)
    (player (make fm :ratio 3.5 :index 1.5) (make overdrive) (make reverb :size 0.9)
            [45 52 57 64 69 76 81 88] mixed)
    (player (make pluck) (make echo) (make chorus)
            [48 55 60 67 72 79 84 91] mixed)
    (player (make pluck :feedback 0.995 :damping 0.5) (make reverb) (make overdrive)
            [50 57 62 69 74 81 86 93] mixed)
    (player (make subtractive :wave :saw :cutoff 1200hz :resonance 0.6) (make overdrive) (make echo :time 0.5beat)
            [52 59 64 71 76 83 88 95] mixed)
    (player (make fm :ratio 1.5 :index 5) (make reverb :size 0.8) (make chorus :rate 1.2hz)
            [55 62 67 74 79 86 91 98] mixed)
    (pan :in (mapper mixed (fn (x) (* x 0.25))) :position 0 :out stereo)
    (audio-out :in stereo :channel 0)))
