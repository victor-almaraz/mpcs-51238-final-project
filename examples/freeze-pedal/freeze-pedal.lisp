;; freeze-pedal: a granular delay on audio input 1, frozen by the sustain
;; pedal. While the pedal (MIDI controller 64) is down, the delay stops
;; recording, and its grains keep playing the last two seconds it heard.

(import "std/effects")

(defpiece main ()
  (let ((in     (pipe :audio))
        (pedals (pipe :event))
        (pedal  (pipe :control))
        (out    (pipe :audio)))
    (audio-in :channel 0 :out in)
    (midi-in :device "" :out pedals)
    (control-change :in pedals :cc 64 :out pedal)
    (process (make granular-delay :time 300ms :density 25hz :duration 120ms :jitter 40ms :mix 0.5
                   :record (mapper pedal (fn (x) (- 1 x))))
             in out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
