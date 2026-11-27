;; Phasing: generative loops in the manner of Steve Reich, played from a
;; MIDI keyboard with two modulation wheels.
;;
;; The held notes, up to five, are the loops' pitches, numbered by the
;; lowest number free as each is pressed. Two marimbas, one to each side, play the same pattern of
;; twelve sixteenths on clocks of their own. Under them, a pad holds the
;; held notes and pulses in eighths, swelling and fading like breath.
;;
;; | Control         | Effect                                                     |
;; |-----------------|------------------------------------------------------------|
;; | Keys            | The loops' pitches; the pad plays them as a chord          |
;; | Wheel A (CC 1)  | Phasing: the right marimba runs up to 3% faster than the left. At 0, both hold the offset they have reached |
;; | Wheel B (CC 2)  | Fill: the pattern's steps join one at a time as it rises   |
;;
;; Its page, drawn from the layout below, is the panel above a reverb
;; pedal. The panel holds the tempo, the phasing, and the fill, which the
;; page and the wheels set together, the higher of the two counting; the
;; pad's cutoff, resonance, attack, and release; the breath's rate and
;; depth; the marimbas' and the pad's levels; the output's level; and a
;; keyboard that holds notes as the MIDI keyboard does. The pedal holds the
;; reverb's mix and damping and a footswitch that silences its return. Its
;; screen (ui/script.js and ui/style.css) shows the pattern's twelve steps
;; with each marimba's place in it and the steps the fill has joined.
;; Without the page, the knobs keep the values written here.

(module main)

(import "std/instruments")
(import "std/effects")
(import "marimba")
(import "loops")

;; The two wheels' controller numbers.
(define wheel-a 1)
(define wheel-b 2)

;; Sixteenths at 99 BPM.
(define pulse-rate 6.6)

;; The held notes the loops can name.
(define held-notes 5)

;; A wheel's value, 0 to 1, smoothed over 30 ms.
(define (wheel notes cc)
  (let ((raw (pipe :control))
        (smooth (pipe :control)))
    (control-change :in notes :cc cc :out raw)
    (smoother :in raw :time 30ms :out smooth)
    smooth))

;; Every other event on ticks.
(define (halve ticks)
  (let ((every-other (pipe :control))
        (out (pipe :event)))
    (sequencer :in ticks :values [1 0] :out every-other)
    (chance :in ticks :probability every-other :out out)
    out))

(defpiece main ()
  (let ((notes   (keys "Keys" :low 48 :high 84 :address "/note"))
        (slots   (map (fn (ignored) (pipe :event)) (range held-notes)))
        (left    (pipe :event))
        (right   (pipe :event))
        (bars-l  (pipe :audio))
        (bars-r  (pipe :audio))
        (pad     (pipe :audio))
        (breath  (pipe :control))
        (pulse   (pipe :control))
        (pulsed  (pipe :audio))
        (send    (pipe :audio))
        (wet     (pipe :audio))
        (master  (pipe :audio :channels 2))
        (level   (pipe :control))
        (tempo   (knob "Tempo" :min 3.2 :max 13.2 :initial pulse-rate :size :big))
        (phasing (knob "Phasing" :initial 0))
        (filling (knob "Fill" :initial 0 :unit :percent))
        (cutoff  (knob "Cutoff" :min 100hz :max 4khz :initial 700hz))
        (resonance (knob "Resonance" :initial 0.2 :unit :percent))
        (attack  (knob "Attack" :min 5ms :max 4s :initial 150ms :scale :exp))
        (release (knob "Release" :min 50ms :max 8s :initial 1.2s :scale :exp))
        (rate    (knob "Rate" :min 0.01hz :max 1hz :initial 0.07hz))
        (depth   (knob "Depth" :max 0.65 :initial 0.35 :unit :percent))
        (marimba-level (knob "Marimbas" :max 0db :initial -10db))
        (pad-level (knob "Pad" :max 0db :initial -20db))
        (reverb-mix (knob "Mix" :initial 1 :unit :percent))
        (reverb-damping (knob "Damping" :initial 0.2 :unit :percent))
        ;; The reverb's footswitch: at 0, its return is silent.
        (return  (footswitch "On")))
    ;; The MIDI keyboard's notes join the page keyboard's.
    (midi-in :device "" :out notes)
    (voice-split :in notes :out slots)
    (let ((freqs (map (fn (s) (let ((f (pipe :control))) (note-to-freq :in s :out f) f)) slots))
          (held  (map (fn (s) (let ((g (pipe :control))) (gate :in s :out g) g)) slots))
          ;; Each wheel, or its knob on the page where that is higher.
          (drift (mapper (wheel notes wheel-a) (fn (w) (max w phasing))))
          (fill  (mapper (wheel notes wheel-b) (fn (w) (max w filling))))
          (ticks-l (clock tempo))
          (ticks-r (clock (mapper drift (fn (w) (* tempo (+ 1 (* 0.03 w))))))))
      (pattern-loop ticks-l freqs held fill left)
      (pattern-loop ticks-r freqs held fill right)
      (play (make marimba :level marimba-level) left bars-l)
      (play (make marimba :level marimba-level) right bars-r)
      ;; The panel's screen: each marimba's step, and the fill.
      (osc-out :port :page :address "/step/left" :value (steps ticks-l (range 12)))
      (osc-out :port :page :address "/step/right" :value (steps ticks-r (range 12)))
      (osc-out :port :page :address "/fill/now" :value (mapper fill (fn (f) (quantize f 0.01))))

      ;; The pad: the held notes as a chord, pulsing in eighths of the
      ;; left clock, swelling and fading over fourteen seconds.
      (play (make subtractive :voices held-notes :cutoff cutoff :env-amount 400hz :resonance resonance
                              :attack attack :decay 1s :sustain 1 :release release :level pad-level)
            notes pad)
      (adsr :gate (let ((p (pipe :event))) (trigger-notes :in (halve ticks-l) :length 40ms :out p) p)
            :attack 6ms :decay 180ms :sustain 0.3 :release 120ms :out pulse)
      (lfo :shape :sine :rate rate :depth depth :offset 0.65 :out breath)
      (multiply :in [pad pulse breath] :out pulsed))

    (pan :in bars-l :position -0.6 :out master)
    (pan :in bars-r :position 0.6 :out master)
    (pan :in pulsed :position 0 :out master)
    (mix :in [bars-l bars-r pulsed] :levels [0.3 0.3 0.4] :out send)
    (process (make reverb :size 0.86 :damping reverb-damping :mix reverb-mix) send wet)
    (pan :in (mapper wet (fn (x) (* x return))) :position 0 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)
    ;; The panel's meter: the sum of the marimbas and the pad, before the reverb.
    (smoother :in (mapper send (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "phasing"
        (section "Pattern" tempo phasing filling :columns 11)
        (section "Pad" cutoff resonance attack release :columns 6)
        (section "Breath" rate depth :columns 3)
        (section "Mix" marimba-level pad-level
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical))
        notes)
      (pedal "Reverb" reverb-mix reverb-damping return)
      :accent "#a78bfa")))
