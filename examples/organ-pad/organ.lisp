;; An additive drawbar organ, with drawbars that drift.
;;
;; Each voice adds nine sines at the footages of a drawbar organ, from the
;; sub-octave to three octaves up. The drawbars' levels are control pipes
;; shared by every voice: a morph from one registration to another, and on
;; each drawbar a slow LFO of its own, so the tone changes without
;; repeating.

(module organ)

(import "std/instruments")

;; The drawbars' frequencies, as multiples of the note's: 16', 5 1/3', 8',
;; 4', 2 2/3', 2', 1 3/5', 1 1/3', and 1'.
(define footages [0.5 1.5 1 2 3 4 5 6 8])

;; Two registrations, each drawbar 0 to 8 as on an organ: a soft flute
;; and a full chorus.
(define flute [6 0 8 5 0 3 0 0 0])
(define full  [8 5 8 8 6 7 4 5 6])

;; Each drawbar's LFO: its rate, in hertz, and its starting phase. The
;; rates are unrelated, so the drawbars never move together.
(define drifts [[0.031 0.0] [0.047 0.3] [0.023 0.6] [0.061 0.1] [0.089 0.8]
                [0.073 0.45] [0.113 0.2] [0.097 0.7] [0.131 0.55]])

(defclass drawbar-organ (instrument enveloped)
  ((voices   :initform 6)
   (level    :initform -24db)
   (attack   :initform 400ms)
   (decay    :initform 1s)
   (sustain  :initform 1)
   (release  :initform 1.8s)
   ;; The drawbars' levels: a vector of nine control pipes, 0 to 1.
   (drawbars :initarg :drawbars :initform nil :accessor drawbars)))

;; The drawbars' levels, as control pipes from 0 to 1: the flute
;; registration with morph at 0, the full one at 1, and each level moved
;; by depth (0 to 1) of its LFO.
(define (drifting-drawbars morph depth)
  (map (fn (k)
         (let ((from  (/ (nth flute k) 8))
               (to    (/ (nth full k) 8))
               (drift (pipe :control))
               (level (pipe :control)))
           (lfo :shape :sine :rate (nth (nth drifts k) 0) :phase (nth (nth drifts k) 1)
                :depth depth :offset 1 :out drift)
           (multiply :in [(mapper morph (fn (m) (+ from (* m (- to from))))) drift] :out level)
           level))
       (range (length footages))))

(defmethod voice ((o drawbar-organ) notes out)
  (let ((freq  (pipe :control))
        (tones (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (map (fn (k)
           (let ((partial (pipe :audio))
                 (ratio   (nth footages k)))
             (sine-osc :freq (mapper freq (fn (f) (* f ratio))) :out partial)
             (gain :in partial :amount (nth (drawbars o) k) :out tones)))
         (range (length footages)))
    (amplitude o notes tones out)))
