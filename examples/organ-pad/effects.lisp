;; The organ's effects: a rotary speaker and a tape loop.
;;
;; Both are subclasses of the standard library's effect, so each has a
;; mix, and the standard library's reverb follows them in the chain.

(module effects)

(import "std/effects")

;; A rotary speaker: a short delay swept by the turning horn bends the
;; pitch back and forth, and the horn's direction raises and lowers the
;; level. The speed itself drifts on a slow LFO, between speed - drift
;; and speed + drift. The phase sets where the horn starts, so two
;; rotaries, one for each side, turn apart.
(defclass rotary (effect)
  ((mix   :initform 0.7)
   (speed :initarg :speed :initform 0.9hz :accessor speed)
   (drift :initarg :drift :initform 0.3hz :accessor drift)
   (phase :initarg :phase :initform 0     :accessor phase)))

(defmethod process ((r rotary) in out)
  (let ((rate     (pipe :control))
        (sweep    (pipe :control))
        (swell    (pipe :control))
        (bent     (pipe :audio)))
    (lfo :shape :sine :rate 0.02hz :depth (unit-value (drift r) 'hz) :offset (unit-value (speed r) 'hz)
         :out rate)
    ;; The horn's distance: 3 ms, 1.2 ms nearer or farther.
    (lfo :shape :sine :rate rate :phase (phase r) :depth 0.0012 :offset 0.003 :out sweep)
    (delay-line :in in :time sweep :max-time 10ms :out bent)
    ;; Its direction: the level rises by up to 25% as the horn faces out.
    (lfo :shape :sine :rate rate :phase (+ (phase r) 0.25) :depth 0.25 :offset 1 :out swell)
    (multiply :in [bent swell] :out out)))

;; A tape loop for playing with oneself: what is played comes back after
;; time, and again at feedback times the level of the pass before, each
;; pass darker by damping. The tape's speed wavers by wow, so the repeats
;; drift in pitch, very slightly. Feedback may be a control pipe, such as
;; a wheel's. The output is the input with its repeats: the effect's mix
;; is 1, and the input is not attenuated.
(defclass tape-loop (effect)
  ((mix      :initform 1)
   (time     :initarg :time     :initform 7.5s  :accessor time)
   (feedback :initarg :feedback :initform 0.5   :accessor feedback)
   (damping  :initarg :damping  :initform 0.3   :accessor damping)
   (wow      :initarg :wow      :initform 1.5ms :accessor wow)))

(defmethod process ((t tape-loop) in out)
  (let ((length (pipe :control)))
    (lfo :shape :sine :rate 0.25hz :depth (unit-value (wow t) 's) :offset (unit-value (time t) 's)
         :out length)
    (comb :in in :time length :max-time 20s :feedback (feedback t) :damping (damping t) :out out)
    (gain :in in :amount 1 :out out)))
