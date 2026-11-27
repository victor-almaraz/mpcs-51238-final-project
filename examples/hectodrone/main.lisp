;; hectodrone for any MIDI keyboard: four voices of one hundred
;; oscillators, their detuning on the pitch-bend wheel and their filter on
;; the modulation wheel, in a reverb.
;;
;; Its page, drawn from the layout below, is the synthesizer's panel above
;; a reverb pedal. The panel holds the detuning, which adds to the pitch
;; bend; the filter's cutoff with the modulation wheel down and up, and its
;; resonance; the envelope; the volume; the output's level; and a keyboard
;; that plays the voices as the MIDI keyboard does. The pedal holds the
;; reverb's mix and damping and a footswitch that bypasses it. Its screen
;; (ui/script.js and ui/style.css) draws the hundred oscillators' detuning.
;; Without the page, the knobs keep the values written here.

(module main)

(import "std/effects")
(import "hectodrone")

(defpiece main ()
  (let ((notes  (keys "Keys" :low 36 :high 84 :address "/note"))
        (left   (pipe :audio))
        (right  (pipe :audio))
        (wet-l  (pipe :audio))
        (wet-r  (pipe :audio))
        (master (pipe :audio :channels 2))
        (level  (pipe :control))
        (detune    (knob "Detune" :min -1 :max 1 :initial 0 :size :big))
        (darkest   (knob "Cutoff" :min 40hz :max 4khz :initial 300hz))
        (brightest (knob "Wheel up" :min 1khz :max 18khz :initial 12khz))
        (resonance (knob "Resonance" :initial 0.2 :unit :percent))
        (attack    (knob "Attack" :min 1ms :max 4s :initial 10ms :scale :exp))
        (decay     (knob "Decay" :min 10ms :max 8s :initial 1s :scale :exp))
        (sustain   (knob "Sustain" :initial 0.8 :unit :percent))
        (release   (knob "Release" :min 10ms :max 8s :initial 400ms :scale :exp))
        (volume    (knob "Volume" :max 0db :initial 0db))
        ;; The pedal's controls, shared by both sides' reverbs; the
        ;; footswitch at 0 bypasses them.
        (engaged (footswitch "On"))
        (mix     (knob "Mix" :initial 0.3 :unit :percent))
        (damping (knob "Damping" :initial 0.2 :unit :percent)))
    ;; The MIDI keyboard's notes join the page keyboard's.
    (midi-in :device "" :out notes)
    (play (make hectodrone
                :detune detune :darkest darkest :brightest brightest :resonance resonance
                :attack attack :decay decay :sustain sustain :release release :level volume)
          notes [left right])
    (define wet (mapper mix (fn (m) (* m engaged))))
    (process (make reverb :size 0.9 :damping damping :mix wet) left wet-l)
    (process (make reverb :size 0.9 :damping damping :mix wet) right wet-r)
    (pan :in wet-l :position -1 :out master)
    (pan :in wet-r :position 1 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)
    ;; The panel's meter: the left channel's level.
    (smoother :in (mapper wet-l (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "HECTODRONE"
        (section "Oscillators" detune :columns 12)
        (section "Filter" darkest brightest resonance)
        (section "Envelope" attack decay sustain release)
        (section "Output" volume
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical))
        notes)
      (pedal "Reverb" mix damping engaged)
      :accent "#ff8a3d")))
