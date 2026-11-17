;; Phasing: generative loops in the manner of Steve Reich, played from a
;; MIDI keyboard with two modulation wheels.
;;
;; The held notes, up to five, are the loops' pitches, in the order they
;; were pressed. Two marimbas, one to each side, play the same pattern of
;; twelve sixteenths on clocks of their own. Under them, a pad holds the
;; held notes and pulses in eighths, swelling and fading like breath.
;;
;; | Control         | Effect                                                     |
;; |-----------------|------------------------------------------------------------|
;; | Keys            | The loops' pitches; the pad plays them as a chord          |
;; | Wheel A (CC 1)  | Phasing: the right marimba runs up to 3% faster than the left. At 0, both hold the offset they have reached |
;; | Wheel B (CC 2)  | Fill: the pattern's steps join one at a time as it rises   |

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
  (let ((notes   (pipe :event))
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
        (master  (pipe :audio :channels 2)))
    (midi-in :device "" :out notes)
    (voice-split :in notes :out slots)
    (let ((freqs (map (fn (s) (let ((f (pipe :control))) (note-to-freq :in s :out f) f)) slots))
          (held  (map (fn (s) (let ((g (pipe :control))) (gate :in s :out g) g)) slots))
          (drift (wheel notes wheel-a))
          (fill  (wheel notes wheel-b))
          (ticks-l (clock pulse-rate))
          (ticks-r (clock (mapper drift (fn (w) (* pulse-rate (+ 1 (* 0.03 w))))))))
      (pattern-loop ticks-l freqs held fill left)
      (pattern-loop ticks-r freqs held fill right)
      (play (make marimba) left bars-l)
      (play (make marimba) right bars-r)

      ;; The pad: the held notes as a chord, pulsing in eighths of the
      ;; left clock, swelling and fading over fourteen seconds.
      (play (make subtractive :voices held-notes :cutoff 700hz :env-amount 400hz :resonance 0.2
                              :attack 150ms :decay 1s :sustain 1 :release 1.2s :level -20db)
            notes pad)
      (adsr :gate (let ((p (pipe :event))) (trigger-notes :in (halve ticks-l) :length 40ms :out p) p)
            :attack 6ms :decay 180ms :sustain 0.3 :release 120ms :out pulse)
      (lfo :shape :sine :rate 0.07hz :depth 0.35 :offset 0.65 :out breath)
      (multiply :in [pad pulse breath] :out pulsed))

    (pan :in bars-l :position -0.6 :out master)
    (pan :in bars-r :position 0.6 :out master)
    (pan :in pulsed :position 0 :out master)
    (mix :in [bars-l bars-r pulsed] :levels [0.3 0.3 0.4] :out send)
    (process (make reverb :size 0.86 :mix 1) send wet)
    (pan :in wet :position 0 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)))
