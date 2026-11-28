;; The organ's effects: a rotary speaker and a tape loop, both of two
;; sides.
;;
;; Both are subclasses of the standard library's effect, so each has a
;; mix, and the standard library's reverb follows them in the chain. The
;; rotary widens the mono organ to two channels (output-channels), and the
;; tape loop and the reverb then run on both.

(module effects)

(import "std/effects")

;; An audio pipe of two channels, the left from the audio pipe left and
;; the right from right: a value for each side, for a parameter that
;; reads one per channel.
(define (sides left right)
  (let ((both (pipe :audio :channels 2)))
    (pan :in left :position -1 :out both)
    (pan :in right :position 1 :out both)
    both))

;; A sine LFO on an audio pipe of one channel.
(define (swing rate phase depth offset)
  (let ((out (pipe :audio)))
    (lfo :shape :sine :rate rate :phase phase :depth depth :offset offset :out out)
    out))

;; A rotary speaker of two horns, one for each side: a short delay swept
;; by each turning horn bends the pitch back and forth, and the horn's
;; direction raises and lowers the level. The speed drifts on a slow LFO,
;; between speed - drift and speed + drift. Phases sets where each horn
;; starts, so the two turn apart.
(defclass rotary (effect)
  ((mix    :initform 0.7)
   (speed  :initarg :speed  :initform 0.9hz   :accessor speed)
   (drift  :initarg :drift  :initform 0.3hz   :accessor drift)
   (phases :initarg :phases :initform [0 0.5] :accessor phases)))

(defmethod output-channels ((r rotary) in) 2)

(defmethod process ((r rotary) in out)
  (let ((rate (pipe :control))
        (bent (pipe :audio :channels 2))
        (left (nth (phases r) 0))
        (right (nth (phases r) 1)))
    (lfo :shape :sine :rate 0.02hz :depth (unit-value (drift r) 'hz) :offset (unit-value (speed r) 'hz)
         :out rate)
    ;; Each horn's distance: 3 ms, 1.2 ms nearer or farther.
    (delay-line :in in :time (sides (swing rate left 0.0012 0.003) (swing rate right 0.0012 0.003))
                :max-time 10ms :out bent)
    ;; Its direction: the level rises by up to 25% as the horn faces out.
    (multiply :in [bent (sides (swing rate (+ left 0.25) 0.25 1) (swing rate (+ right 0.25) 0.25 1))]
              :out out)))

;; A tape loop for playing with oneself: what is played comes back after
;; each side's time, and again at the feedback times the level of the pass
;; before, each pass darker by damping. The feedback is repeats, or wheel,
;; a control pipe such as a modulation wheel's, where that is higher, from
;; 0 to 0.96. The tape's speed wavers by wow, so the repeats drift in
;; pitch, very slightly. The output is the input with its repeats: the
;; effect's mix is 1, and the input is not attenuated.
(defclass tape-loop (effect)
  ((mix     :initform 1)
   (times   :initarg :times   :initform [7.5s 8.1s] :accessor times)
   (repeats :initarg :repeats :initform 0           :accessor repeats :control (knob))
   (wheel   :initarg :wheel   :initform 0           :accessor wheel)
   (damping :initarg :damping :initform 0.3         :accessor damping :control (knob :unit :percent))
   (wow     :initarg :wow     :initform 1.5ms       :accessor wow)))

(defmethod process ((t tape-loop) in out)
  (let ((reel (fn (time) (swing 0.25hz 0 (unit-value (wow t) 's) (unit-value time 's))))
        (wheel (wheel t)))
    (comb :in in :time (sides (reel (nth (times t) 0)) (reel (nth (times t) 1))) :max-time 20s
          :feedback (mapper (repeats t) (fn (r) (* (max r wheel) 0.96)))
          :damping (damping t) :out out)
    (gain :in in :amount 1 :out out)))
