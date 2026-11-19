;; Effects built from primitives (interpreter.md §7). Each is a class
;; whose process method writes its wet signal; an :around method on
;; effect mixes it with the dry signal.

(module effects)

;; Every effect: how much of its output is wet, from 0 (dry) to 1. Its
;; accessor is wet-mix, as mix is a primitive; other effects' accessors
;; are named after their slots.
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
  ((time     :initarg :time     :initform 0.5beat :accessor time)
   (feedback :initarg :feedback :initform 0.4     :accessor feedback)
   (damping  :initarg :damping  :initform 0.3     :accessor damping)))

(defmethod process ((e echo) in out)
  (comb :in in :time (time e) :max-time 4s
        :feedback (feedback e) :damping (damping e) :out out))

;; A chorus: the input delayed by about delay, the delay swept by depth
;; at rate, which detunes the copy against the dry signal.
(defclass chorus (effect)
  ((rate  :initarg :rate  :initform 0.8hz :accessor rate)
   (delay :initarg :delay :initform 15ms  :accessor delay)
   (depth :initarg :depth :initform 4ms   :accessor depth)))

(defmethod process ((c chorus) in out)
  (let ((time (pipe :control)))
    (lfo :shape :sine :rate (rate c)
         :depth (unit-value (depth c) 's) :offset (unit-value (delay c) 's) :out time)
    (delay-line :in in :time time :max-time 50ms :out out)))

;; A reverb: Schroeder's reverberator with Freeverb's tunings, four damped
;; feedback combs in parallel and two allpass diffusers in series. Size
;; sets the combs' feedback, so the decay time.
(defclass reverb (effect)
  ((size    :initarg :size    :initform 0.84 :accessor size)
   (damping :initarg :damping :initform 0.2  :accessor damping)))

(defmethod process ((r reverb) in out)
  (let ((scaled   (pipe :audio))
        (combs    (pipe :audio))
        (diffused (pipe :audio)))
    ;; Each comb's gain grows as 1 / (1 - size), and four sum: scale the input to match.
    (gain :in in :amount (- 1 (size r)) :out scaled)
    (map (fn (t) (comb :in scaled :time t :max-time 40ms :feedback (size r) :damping (damping r) :out combs))
         [25.31ms 28.96ms 32.24ms 35.31ms])
    (allpass :in combs :time 12.61ms :feedback 0.5 :out diffused)
    (allpass :in diffused :time 7.73ms :feedback 0.5 :out out)))

;; Overdrive: the input driven into tanh saturation, then a low-pass tone
;; filter that tames the added harmonics.
(defclass overdrive (effect)
  ((drive :initarg :drive :initform 4    :accessor drive)
   (tone  :initarg :tone  :initform 3khz :accessor tone)
   (level :initarg :level :initform -6db :accessor level)))

(defmethod process ((o overdrive) in out)
  (let ((driven (pipe :audio)))
    (gain :in (mapper in (fn (x) (tanh (* x (drive o))))) :amount (level o) :out driven)
    (biquad :in driven :mode :low-pass :freq (tone o) :out out)))

;; A granular delay (samples.md §8.4): the input recorded into a ring of
;; length, and grains read from it time behind the writer's head, each up
;; to jitter later. A record of 0, a constant or a control pipe, freezes
;; the ring, and the grains keep playing what it holds.
(defclass granular-delay (effect)
  ((time         :initarg :time         :initform 250ms :accessor time)
   (length       :initarg :length       :initform 2s    :accessor ring-length)
   (density      :initarg :density      :initform 20hz  :accessor density)
   (duration     :initarg :duration     :initform 80ms  :accessor duration)
   (jitter       :initarg :jitter       :initform 20ms  :accessor jitter)
   (pitch        :initarg :pitch        :initform 0st   :accessor pitch)
   (pitch-jitter :initarg :pitch-jitter :initform 0st   :accessor pitch-jitter)
   (record       :initarg :record       :initform 1     :accessor record)))

(defmethod process ((g granular-delay) in out)
  (let ((ring   (buffer :seconds (ring-length g)))
        (head   (pipe :control))
        (behind (pipe :control)))
    (buffer-write :buffer ring :in in :record (record g) :position head)
    (subtract :in head :b (unit-value (time g) 's) :out behind)
    (grains :buffer ring :position behind :density (density g) :duration (duration g)
            :jitter (jitter g) :pitch (pitch g) :pitch-jitter (pitch-jitter g)
            :out out)))
