;; Loops in the manner of Steve Reich's phase and additive processes.
;;
;; A loop steps through a pattern of twelve steps on a clock of its own.
;; Each step names one of the held notes by its number (the lowest free
;; as it was pressed), an octave, and the fill level at which the step joins the
;; pattern. A step whose note is not held rests, so the pattern grows as
;; more notes are held. Two loops on the same pattern, one clock slightly
;; faster than the other, drift apart step by step and realign: the phase
;; process. Raising the fill adds the steps one at a time, in a fixed
;; order: the additive process.

(module loops)

;; The pattern, a step to a line: [held note, octave, fill at which it
;; joins]. The first step sounds at any fill.
(define pattern
  [[0 0 0]
   [1 0 0.5]
   [2 0 0.2]
   [3 0 0.75]
   [1 0 0.1]
   [4 0 0.6]
   [0 1 0.3]
   [3 0 0.85]
   [2 0 0.15]
   [1 0 0.65]
   [4 0 0.4]
   [3 -1 0.95]])

(define (column k) (map (fn (step) (nth step k)) pattern))

;; Events at rate, a frequency or a control pipe of one: a sawtooth LFO
;; on an audio pipe, so each crossing of 0 falls on its sample.
(define (clock rate)
  (let ((ramp  (pipe :audio))
        (ticks (pipe :event)))
    (lfo :shape :saw :rate rate :out ramp)
    (threshold :in ramp :level 0 :direction :rising :out ticks)
    ticks))

;; A control pipe that steps through values at each event on ticks.
(define (steps ticks values)
  (let ((p (pipe :control)))
    (sequencer :in ticks :values values :out p)
    p))

;; For count, a control pipe for each k below it: 1 while index is k, else 0.
(define (indicators index count)
  (map (fn (k)
         (let ((p (pipe :control)))
           (clip :in (mapper index (fn (x) (- 1 (abs (- x k))))) :low 0 :high 1 :out p)
           p))
       (range count)))

;; The sum of the products of the pipes of xs and ys, in pairs.
(define (dot xs ys)
  (let ((sum (pipe :control)))
    (add :in (map (fn (k)
                    (let ((product (pipe :control)))
                      (multiply :in [(nth xs k) (nth ys k)] :out product)
                      product))
                  (range (length xs)))
         :out sum)
    sum))

;; Semitones per natural-log unit, for the MIDI note of a frequency below.
(define semitones-per-log (/ 12 (log 2)))

;; Notes into notes: the pattern at each event on ticks, over the held
;; notes' frequencies freqs and gates held, with the steps whose fill is
;; at most fill.
(define (pattern-loop ticks freqs held fill notes)
  (let ((which   (indicators (steps ticks (column 0)) (length freqs)))
        (octave  (steps ticks (column 1)))
        (entry   (steps ticks (column 2)))
        (freq    (dot which freqs))
        (playing (dot which held))
        (joined  (pipe :control))
        (chance-of-step (pipe :control))
        (hits    (pipe :event)))
    ;; 1 once the fill reaches the step's entry, rising from 0 over the 0.02 below it.
    (clip :in (mapper fill (fn (f) (+ 1 (* 50 (+ f (* -1 entry)))))) :low 0 :high 1 :out joined)
    (multiply :in [playing joined] :out chance-of-step)
    (chance :in ticks :probability chance-of-step :out hits)
    (trigger-notes :in hits
                   :pitch (mapper freq (fn (f) (+ 69 (* semitones-per-log (log (/ (max f 20) 440)))
                                                  (* 12 octave))))
                   :velocity 0.8 :length 80ms
                   :out notes)))
