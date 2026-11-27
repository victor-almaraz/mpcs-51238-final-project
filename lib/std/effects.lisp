;; Effects built from primitives (interpreter.md §7). Each is a class
;; whose process method writes its wet signal; an :around method on
;; effect mixes it with the dry signal. An effect runs as many channels as
;; the pipe it writes: its inner pipes take that pipe's channel count.

(module effects)

(import "std/modulators")

;; Every effect: how much of its output is wet, from 0 (dry) to 1. Its
;; accessor is wet-mix, as mix is a primitive; other effects' accessors
;; are named after their slots. A slot's :control is the knob a pedal
;; draws it as (pedal, chain); slots a pipe cannot set have none.
(defclass effect ()
  ((mix         :initarg :mix :initform 0.5 :accessor wet-mix :control (knob :unit :percent))
   (modulations :initform [])))

;; (process effect in out) writes the audio pipe in, processed, to the
;; audio pipe out.
(defgeneric process (effect in out))

;; (output-channels effect in) is the channel count of what effect writes
;; given the audio pipe in, for chain: by default in's. An effect that
;; changes the count, such as a splitter, defines a method.
(defgeneric output-channels (effect in))

(defmethod output-channels ((e effect) in)
  (channel-count in))

;; The modulations modulate recorded move their slots' knobs, which the
;; page has drawn by now, before the effect is built.
(defmethod process :before ((e effect) in out)
  (apply-modulations e))

;; The primary methods write the wet signal; this mixes it with the dry
;; input in proportion to the effect's mix.
(defmethod process :around ((e effect) in out)
  (let ((wet (pipe :audio :channels (channel-count out))))
    (call-next-method e in wet)
    (crossfade :a in :b wet :position (wet-mix e) :law :linear :out out)))

;; An echo: repeats every time, each feedback times the last and darker
;; by damping. A time in beats follows the tempo.
(defclass echo (effect)
  ((time     :initarg :time     :initform 0.5beat :accessor time)
   (feedback :initarg :feedback :initform 0.4     :accessor feedback :control (knob :max 0.95 :unit :percent))
   (damping  :initarg :damping  :initform 0.3     :accessor damping :control (knob :unit :percent))))

(defmethod process ((e echo) in out)
  (comb :in in :time (time e) :max-time 4s
        :feedback (feedback e) :damping (damping e) :out out))

;; A chorus: the input delayed by about delay, the delay swept by depth
;; at rate, which detunes the copy against the dry signal.
(defclass chorus (effect)
  ((rate  :initarg :rate  :initform 0.8hz :accessor rate :control (knob :min 0.05hz :max 5hz))
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
   (damping :initarg :damping :initform 0.2  :accessor damping :control (knob :unit :percent))))

(defmethod process ((r reverb) in out)
  (let ((scaled   (pipe :audio :channels (channel-count out)))
        (combs    (pipe :audio :channels (channel-count out)))
        (diffused (pipe :audio :channels (channel-count out))))
    ;; Each comb's gain grows as 1 / (1 - size), and four sum: scale the input to match.
    (gain :in in :amount (- 1 (size r)) :out scaled)
    (map (fn (t) (comb :in scaled :time t :max-time 40ms :feedback (size r) :damping (damping r) :out combs))
         [25.31ms 28.96ms 32.24ms 35.31ms])
    (allpass :in combs :time 12.61ms :feedback 0.5 :out diffused)
    (allpass :in diffused :time 7.73ms :feedback 0.5 :out out)))

;; Overdrive: the input driven into tanh saturation, then a low-pass tone
;; filter that tames the added harmonics.
(defclass overdrive (effect)
  ((drive :initarg :drive :initform 4    :accessor drive :control (knob :min 1 :max 20 :scale :exp))
   (tone  :initarg :tone  :initform 3khz :accessor tone :control (knob :min 500hz :max 8khz))
   (level :initarg :level :initform -6db :accessor level :control (knob :min -30db :max 0db))))

(defmethod process ((o overdrive) in out)
  (let ((driven (pipe :audio :channels (channel-count out))))
    (gain :in (mapper in (fn (x) (tanh (* x (drive o))))) :amount (level o) :out driven)
    (biquad :in driven :mode :low-pass :freq (tone o) :out out)))

;; A granular delay (samples.md §8.4): the input recorded into a ring of
;; length, and grains read from it time behind the writer's head, each up
;; to jitter later. A record of 0, a constant or a control pipe, freezes
;; the ring, and the grains keep playing what it holds.
(defclass granular-delay (effect)
  ((time         :initarg :time         :initform 250ms :accessor time)
   (length       :initarg :length       :initform 2s    :accessor ring-length)
   (density      :initarg :density      :initform 20hz  :accessor density :control (knob :min 2hz :max 80hz))
   (duration     :initarg :duration     :initform 80ms  :accessor duration :control (knob :min 20ms :max 400ms :scale :exp))
   (jitter       :initarg :jitter       :initform 20ms  :accessor jitter :control (knob :min 0ms :max 150ms))
   (pitch        :initarg :pitch        :initform 0st   :accessor pitch :control (knob :min -12st :max 12st))
   (pitch-jitter :initarg :pitch-jitter :initform 0st   :accessor pitch-jitter :control (knob :min 0st :max 12st))
   (record       :initarg :record       :initform 1     :accessor record)))

(defmethod process ((g granular-delay) in out)
  (let ((ring   (buffer :channels (channel-count out) :seconds (ring-length g)))
        (head   (pipe :control))
        (behind (pipe :control)))
    (buffer-write :buffer ring :in in :record (record g) :position head)
    (subtract :in head :b (unit-value (time g) 's) :out behind)
    (grains :buffer ring :position behind :density (density g) :duration (duration g)
            :jitter (jitter g) :pitch (pitch g) :pitch-jitter (pitch-jitter g)
            :out out)))

;; A spectral delay (samples.md §11.5): each bin of the input's spectrum
;; delayed by time, shaped across the spectrum by shape and spread, its
;; repeats fed back by feedback. The wet signal is size samples late.
(defclass spectral-delay (effect)
  ((time     :initarg :time     :initform 500ms    :accessor time :control (knob :min 10ms :max 4s :scale :exp))
   (max-time :initarg :max-time :initform 4s       :accessor max-time)
   (shape    :initarg :shape    :initform ':random :accessor shape)
   (spread   :initarg :spread   :initform 1        :accessor spread :control (knob :unit :percent))
   (feedback :initarg :feedback :initform 0.5      :accessor feedback :control (knob :max 0.95 :unit :percent))
   (size     :initarg :size     :initform 2048     :accessor size)))

(defmethod process ((d spectral-delay) in out)
  (let ((frames (pipe :event)) (delayed (pipe :event)))
    (fft :in in :size (size d) :out frames)
    (pv-delay :in frames :time (time d) :max-time (max-time d) :shape (shape d)
              :spread (spread d) :feedback (feedback d) :out delayed)
    (ifft :in delayed :out out)))

;; A spectral freeze (samples.md §11.5): while hold, a constant or a
;; control pipe, is above 0.5, the spectrum it caught sustains, fading in
;; and out over fade. Otherwise it passes its input, size samples late.
(defclass freeze (effect)
  ((hold :initarg :hold :initform 0    :accessor hold :control (toggle))
   (fade :initarg :fade :initform 50ms :accessor fade)
   (size :initarg :size :initform 2048 :accessor size)
   (mix  :initform 1)))

(defmethod process ((f freeze) in out)
  (let ((frames (pipe :event)) (held (pipe :event)))
    (fft :in in :size (size f) :out frames)
    (pv-freeze :in frames :hold (hold f) :fade (fade f) :out held)
    (ifft :in held :out out)))

;; One effect from in to out as a pedal on the page: its :control slots as
;; its knobs, and a footswitch that bypasses it, leaving the dry signal.
;; Returns the pedal.
(define (stomp effect in out)
  (let ((engaged (footswitch "On")) (eased (pipe :control)))
    ;; The bypass eases over 20 ms, so a stomp does not click.
    (smoother :in engaged :time 20ms :out eased)
    (let ((drawn (pedal effect engaged)))
      ;; The knobs move by their modulations before the footswitch scales
      ;; the mix's.
      (apply-modulations effect)
      (let ((mix (wet-mix effect)))
        (set! (wet-mix effect) (mapper eased (fn (on) (* on mix)))))
      (process effect in out)
      drawn)))

;; Effects in series from in to out, each a pedal (stomp), on a pedalboard
;; in the order of the chain. Each effect's output is as wide as
;; output-channels says, and the next effect reads it; the last writes
;; out. Returns the pedalboard, for page:
;; (page (chain guitar out (make overdrive) (make echo) (make reverb))).
(define (chain in out &rest effects)
  (if (= (length effects) 0) (error "chain needs at least one effect between in and out") #f)
  (let ((n (length effects))
        (from in))
    (pedalboard
      (map (fn (i)
             (let ((effect (nth effects i)))
               (let ((to (if (= i (- n 1)) out (pipe :audio :channels (output-channels effect from)))))
                 (let ((drawn (stomp effect from to)))
                   (set! from to)
                   drawn))))
           (range n)))))
