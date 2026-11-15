;; Effects built from primitives (interpreter.md §7). Each is a class
;; whose process method writes its wet signal; an :around method on
;; effect mixes it with the dry signal.

(module effects)

;; Every effect: how much of its output is wet, from 0 (dry) to 1.
(defclass effect ()
  ((mix :initarg :mix :initform 0.5 :accessor wet-mix)))

;; (process effect in out) writes the audio pipe in, processed, to the
;; audio pipe out.
(defgeneric process (effect in out))

;; The primary methods write the wet signal; this mixes it with the dry
;; input in proportion to the effect's mix.
(defmethod process :around ((e effect) in out)
  (let ((wet (pipe :audio)))
    (call-next-method e in wet)
    (crossfade :a in :b wet :position (wet-mix e) :law :linear :out out)))

;; An echo: repeats every time, each feedback times the last and darker
;; by damping. A time in beats follows the tempo.
(defclass echo (effect)
  ((time     :initarg :time     :initform 0.5beat :accessor echo-time)
   (feedback :initarg :feedback :initform 0.4     :accessor echo-feedback)
   (damping  :initarg :damping  :initform 0.3     :accessor echo-damping)))

(defmethod process ((e echo) in out)
  (comb :in in :time (echo-time e) :max-time 4s
        :feedback (echo-feedback e) :damping (echo-damping e) :out out))

;; A chorus: the input delayed by about delay, the delay swept by depth
;; at rate, which detunes the copy against the dry signal.
(defclass chorus (effect)
  ((rate  :initarg :rate  :initform 0.8hz :accessor chorus-rate)
   (delay :initarg :delay :initform 15ms  :accessor chorus-delay)
   (depth :initarg :depth :initform 4ms   :accessor chorus-depth)))

(defmethod process ((c chorus) in out)
  (let ((time (pipe :control)))
    (lfo :shape :sine :rate (chorus-rate c)
         :depth (unit-value (chorus-depth c) 's) :offset (unit-value (chorus-delay c) 's) :out time)
    (delay-line :in in :time time :max-time 50ms :out out)))

;; A reverb: Schroeder's reverberator with Freeverb's tunings, four damped
;; feedback combs in parallel and two allpass diffusers in series. Size
;; sets the combs' feedback, so the decay time.
(defclass reverb (effect)
  ((size    :initarg :size    :initform 0.84 :accessor reverb-size)
   (damping :initarg :damping :initform 0.2  :accessor reverb-damping)))

(defmethod process ((r reverb) in out)
  (let ((combs    (pipe :audio))
        (diffused (pipe :audio)))
    (map (fn (delay) (comb :in in :time delay :feedback (reverb-size r) :damping (reverb-damping r) :out combs))
         [25.31ms 28.96ms 32.24ms 35.31ms])
    (allpass :in combs :time 12.61ms :feedback 0.5 :out diffused)
    (allpass :in diffused :time 7.73ms :feedback 0.5 :out out)))

;; Overdrive: the input driven into tanh saturation, then a low-pass tone
;; filter that tames the added harmonics.
(defclass overdrive (effect)
  ((drive :initarg :drive :initform 4    :accessor overdrive-drive)
   (tone  :initarg :tone  :initform 3khz :accessor overdrive-tone)
   (level :initarg :level :initform -6db :accessor overdrive-level)))

(defmethod process ((o overdrive) in out)
  (let ((driven (pipe :audio)))
    (gain :in (mapper in (fn (x) (tanh (* x (overdrive-drive o))))) :amount (overdrive-level o) :out driven)
    (biquad :in driven :mode :low-pass :freq (overdrive-tone o) :out out)))
