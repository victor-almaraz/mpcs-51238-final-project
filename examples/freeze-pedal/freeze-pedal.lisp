;; freeze-pedal: a granular delay on audio input 1, frozen by the sustain
;; pedal. While the pedal (MIDI controller 64) is down, the delay stops
;; recording, and its grains keep replaying what it heard 300 to 140 ms before.
;;
;; Its page (ui/index.html) is the pedal: Mix, Density, Grain, and Spread
;; knobs, and a footswitch that freezes it as the sustain pedal does; the
;; LED is lit while either holds it. Without the page, the knobs keep the
;; values written here.

(import "std/effects")

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

(defpiece main ()
  (let ((in     (pipe :audio))
        (pedals (pipe :event))
        (pedal  (pipe :control))
        (stomp  (control "/freeze" 0))
        (frozen (pipe :control))
        (out    (pipe :audio)))
    (audio-in :channel 0 :out in)
    (midi-in :device "" :out pedals)
    (control-change :in pedals :cc 64 :out pedal)
    ;; Frozen while the pedal or the footswitch is down.
    (max :in pedal :b stomp :out frozen)
    (process (make granular-delay :time 300ms
                   :density (control "/density" 25hz)
                   :duration (control "/grain" 120ms)
                   :jitter (control "/spread" 40ms)
                   :mix (control "/mix" 0.5)
                   :record (mapper frozen (fn (x) (- 1 (round x)))))
             in out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (osc-out :port :page :address "/frozen" :value (mapper frozen (fn (x) (round x))))))
