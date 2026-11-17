;; A marimba: a struck bar, modeled by its first three modes.
;;
;; A bar's modes are not harmonic. The fundamental rings longest; the
;; second mode, near 3.93 times the fundamental, and the third, near 9.2
;; times, are brighter and die sooner. A short burst of band-passed noise
;; is the mallet. Brightness scales the upper modes and the mallet.

(module marimba)

(import "std/instruments")

(defclass marimba (instrument)
  ((voices     :initform 6)
   (level      :initform -10db)
   ;; How long the fundamental rings.
   (decay      :initarg :decay      :initform 900ms :accessor decay)
   ;; Level of the upper modes and the mallet, 0 to 1.
   (brightness :initarg :brightness :initform 0.6   :accessor brightness)))

;; The modes: [ratio level decay-share], each mode's decay a share of the
;; fundamental's.
(define modes [[1 1 1] [3.93 0.35 0.2] [9.2 0.12 0.06]])

(defmethod voice ((m marimba) notes out)
  (let ((freq  (pipe :control))
        (vel   (note-velocity notes))
        (noisy (pipe :audio))
        (thump (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (map (fn (mode)
           (let ((ratio (nth mode 0))
                 (share (nth mode 2))
                 (tone  (pipe :audio))
                 (level (if (= (nth mode 0) 1) (nth mode 1) (* (nth mode 1) (brightness m)))))
             (sine-osc :freq (mapper freq (fn (f) (* f ratio))) :out tone)
             (adsr :gate notes :in tone :attack 1ms
                   :decay (* share (decay m)) :sustain 0 :release (* share (decay m))
                   :gain (mapper vel (fn (v) (* v level)))
                   :out out)))
         modes)
    ;; The mallet: 4 ms of noise, band-passed two octaves above the note.
    (noise :color :white :out noisy)
    (biquad :in noisy :mode :band-pass :freq (mapper freq (fn (f) (* f 4))) :q 1.5 :out thump)
    (adsr :gate notes :in thump :attack 0.3ms :decay 4ms :sustain 0 :release 4ms
          :gain (mapper vel (fn (v) (* v 0.5 (brightness m))))
          :out out)))
