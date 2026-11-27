;; freeze-pedal: a granular delay on audio input 1, frozen by the sustain
;; pedal. While the pedal (MIDI controller 64) is down, the delay stops
;; recording, and its grains keep replaying what it heard 300 to 140 ms before.
;;
;; Its page, drawn from the layout below, is the pedal: Mix, Density,
;; Grain, and Spread knobs, a footswitch that freezes it as the sustain
;; pedal does, and a lamp lit while either holds it. Without the page, the
;; knobs keep the values written here.

(import "std/effects")

(defpiece main ()
  (let ((in     (pipe :audio))
        (pedals (pipe :event))
        (sustain (pipe :control))
        (stomp  (footswitch "Hold" :initial 0))
        (mix     (knob "Mix" :initial 0.5 :unit :percent))
        (density (knob "Density" :min 2hz :max 80hz :initial 25hz))
        (grain   (knob "Grain" :min 20ms :max 400ms :initial 120ms :scale :exp))
        (spread  (knob "Spread" :max 150ms :initial 40ms))
        (frozen (pipe :control))
        (out    (pipe :audio)))
    (audio-in :channel 0 :out in)
    (midi-in :device "" :out pedals)
    (control-change :in pedals :cc 64 :out sustain)
    ;; Frozen while the pedal or the footswitch is down.
    (max :in sustain :b stomp :out frozen)
    (process (make granular-delay :time 300ms
                   :density density
                   :duration grain
                   :jitter spread
                   :mix mix
                   :record (mapper frozen (fn (x) (- 1 (round x)))))
             in out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (page (pedal "Freeze" mix density grain spread
                 (lamp "Frozen" (mapper frozen (fn (x) (round x))))
                 stomp))))
