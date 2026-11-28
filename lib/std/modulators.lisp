;; Modulators (modulators.md): sources of a control signal that move the
;; knobs of instruments and effects. A modulator is a class whose settings
;; are :control slots, so a page draws it as it draws an effect: (section
;; "LFO" wobble). Its signal runs from 0 to 1.
;;
;; (modulate instance 'slot source amount) moves the slot's knob by the
;; source; the page still draws the knob. An amount is a fraction of the
;; knob's travel, the span the modulation sweeps, signed: a bipolar
;; modulation sweeps half of it either side of the knob's value, a
;; unipolar one all of it upward. The value is kept within the travel and
;; follows the knob's scale, so an amount reads the same on a cutoff in
;; hertz and a mix in percent.

(module modulators)

;; --- Vectors ------------------------------------------------------------

;; Vector v with x after its last item.
(define (pushed v x)
  (let ((n (length v)))
    (map (fn (i) (if (< i n) (nth v i) x)) (range (+ n 1)))))

;; The items of v for which keep? is true, in order.
(define (kept keep? v)
  (let ((out []))
    (map (fn (x) (if (keep? x) (set! out (pushed out x)) #f)) v)
    out))

;; --- Modulators ---------------------------------------------------------

;; Every modulator: its signal, built once (signal), the polarity its
;; modulations take unless they say otherwise, and the modulations of its
;; own knobs, so one modulator can move another's rate.
(defclass modulator ()
  ((built       :initform #f)
   (modulations :initform [])))

;; (make-signal modulator) builds the modulator's nodes and returns its
;; control pipe, from 0 to 1.
(defgeneric make-signal (modulator))

;; (default-polarity modulator) is ':bipolar or ':unipolar.
(defgeneric default-polarity (modulator))

(defmethod default-polarity ((m modulator)) ':bipolar)

;; The modulator's control pipe, from 0 to 1: built the first time it is
;; asked, so one modulator can move several knobs. Its own knobs must be on
;; the page by then, as its nodes read them.
(define (signal m)
  (if (slot-value m 'built)
      (slot-value m 'built)
      (do (apply-modulations m)
          (let ((out (make-signal m)))
            (set! (slot-value m 'built) out)
            out))))

;; An LFO: a shape (':sine, ':triangle, ':saw, ':square, or
;; ':sample-and-hold) at rate, from phase, a turn being 1.
(defclass lfo-mod (modulator)
  ((rate  :initarg :rate  :initform 1hz   :accessor rate :control (knob :min 0.05hz :max 20hz))
   (shape :initarg :shape :initform ':sine :accessor lfo-shape)
   (phase :initarg :phase :initform 0     :accessor lfo-phase)))

(defmethod make-signal ((l lfo-mod))
  (let ((out (pipe :control)))
    (lfo :shape (lfo-shape l) :rate (rate l) :phase (lfo-phase l) :depth 0.5 :offset 0.5 :out out)
    out))

;; A random source: a new value at rate, drawn from a generator seeded from
;; the project seed and the node, gliding to it over smooth, a time fixed
;; when the signal is built.
(defclass random-mod (modulator)
  ((rate   :initarg :rate   :initform 2hz  :accessor rate :control (knob :min 0.05hz :max 20hz))
   (smooth :initarg :smooth :initform 50ms :accessor smooth)))

(defmethod make-signal ((r random-mod))
  (let ((held (pipe :control)) (out (pipe :control)))
    (lfo :shape :sample-and-hold :rate (rate r) :depth 0.5 :offset 0.5 :out held)
    (smoother :in held :time (smooth r) :out out)
    out))

;; An envelope: rises over attack at each note on notes, an event pipe
;; given at make, falls over decay to sustain while the note is held, and
;; over release after it. Unipolar.
(defclass envelope-mod (modulator)
  ((notes   :initarg :notes                    :accessor envelope-notes)
   (attack  :initarg :attack  :initform 10ms   :accessor attack :control (knob :min 1ms :max 4s :scale :exp))
   (decay   :initarg :decay   :initform 300ms  :accessor decay :control (knob :min 5ms :max 8s :scale :exp))
   (sustain :initarg :sustain :initform 0.5    :accessor sustain :control (knob :unit :percent))
   (release :initarg :release :initform 500ms  :accessor release :control (knob :min 5ms :max 8s :scale :exp))))

(defmethod default-polarity ((e envelope-mod)) ':unipolar)

(defmethod make-signal ((e envelope-mod))
  (let ((out (pipe :control)))
    (adsr :gate (envelope-notes e) :attack (attack e) :decay (decay e) :sustain (sustain e)
          :release (release e) :out out)
    out))

;; An envelope follower: the level of the audio pipe given at make as
;; :in, its magnitude smoothed over time (fixed when the signal is built)
;; and kept within 0 to 1. Unipolar.
(defclass follower (modulator)
  ((in   :initarg :in                  :accessor followed)
   (time :initarg :time :initform 50ms :accessor follow-time)))

(defmethod default-polarity ((f follower)) ':unipolar)

(defmethod make-signal ((f follower))
  (let ((level (pipe :control)))
    (smoother :in (mapper (followed f) (fn (x) (abs x))) :time (follow-time f) :out level)
    (mapper level (fn (l) (clamp l 0 1)))))

;; A step source: the values of steps, each from 0 to 1, one after
;; another, a step at rate.
(defclass step-mod (modulator)
  ((rate  :initarg :rate  :initform 4hz         :accessor rate :control (knob :min 0.1hz :max 30hz))
   (steps :initarg :steps :initform [0 0.5 1 0.5] :accessor steps)))

(defmethod make-signal ((s step-mod))
  (let ((n (length (steps s))) (values (steps s)) (ramp (pipe :control)) (r (rate s)))
    (if (= n 0) (error "step-mod needs at least one step in :steps") #f)
    ;; A ramp from 0 to 1 over all the steps; the step is its whole part
    ;; of n, and each step's value is picked by arithmetic, so the first
    ;; plays from the start.
    (lfo :shape :saw :rate (if (pipe? r) (mapper r (fn (x) (/ x n))) (/ r n)) :depth 0.5 :offset 0.5 :out ramp)
    (mapper ramp
            (fn (u)
              (let ((i (min (floor (* u n)) (- n 1))) (total 0))
                (map (fn (k) (set! total (+ total (* (nth values k) (clamp (- 1 (abs (- i k))) 0 1)))))
                     (range n))
                total)))))

;; A rate of one cycle every period, a beat value, following the tempo:
;; (make lfo-mod :rate (beat-rate 1beat)). A slot given a pipe has no knob.
(define (beat-rate period)
  (let ((seconds (pipe :control)))
    (beats-to-seconds :beats (unit-value period 'beat) :out seconds)
    (mapper seconds (fn (s) (/ 1 s)))))

;; --- Moving a knob ------------------------------------------------------

;; The position, 0 to 1, of the control pipe v in range [min max scale],
;; as the page maps it: linear; exponential (equal ratios); or in
;; decibels, from -60 dB when min is 0.
(define (position-in range v)
  (let ((lo (nth range 0)) (hi (nth range 1)) (scale (nth range 2)))
    (cond ((by-ratio scale lo hi)
           (let ((bottom (log (exp-bottom scale lo))) (span (- (log hi) (log (exp-bottom scale lo)))))
             (mapper v (fn (x) (clamp (/ (- (log (max x 0.000000001)) bottom) span) 0 1)))))
          ((= hi lo) (mapper v (fn (x) (* x 0))))
          (else (mapper v (fn (x) (clamp (/ (- x lo) (- hi lo)) 0 1)))))))

;; The value at the position p, a control pipe from 0 to 1, of range,
;; on its steps when it has them.
(define (value-in range p)
  (let ((lo (nth range 0)) (step (nth range 3)) (v (unstepped-value range p)))
    (if (> step 0)
        (mapper v (fn (x) (+ lo (* step (round (/ (- x lo) step))))))
        v)))

(define (unstepped-value range p)
  (let ((lo (nth range 0)) (hi (nth range 1)) (scale (nth range 2)))
    (if (by-ratio scale lo hi)
        (let ((bottom (exp-bottom scale lo)) (ratio (log (/ hi (exp-bottom scale lo)))))
          ;; A decibel range from 0 reaches 0 only at position 0.
          (if (= lo 0)
              (mapper p (fn (x) (* bottom (exp (* x ratio)) (min 1 (* x 1000000)))))
              (mapper p (fn (x) (* bottom (exp (* x ratio)))))))
        (mapper p (fn (x) (+ lo (* x (- hi lo))))))))

;; Whether range [lo hi] of scale maps by ratios: an exponential range
;; above 0, or a decibel range whose top is above 0.
(define (by-ratio scale lo hi)
  (cond ((= scale ':exp) (if (> lo 0) (> hi 0) #f))
        ((= scale ':db) (> hi 0))
        (else #f)))

;; The bottom of a ratio range: lo, or -60 dB for a decibel range from 0.
(define (exp-bottom scale lo)
  (if (> lo 0) lo 0.001))

;; The offset, in the travel, of a modulation [source amount polarity]:
;; amount times the source's signal, centred on 0 when bipolar.
(define (offset-of modulation)
  (let ((u (signal (nth modulation 0)))
        (amount (nth modulation 1))
        (bipolar (= (nth modulation 2) ':bipolar)))
    (if bipolar
        (mapper u (fn (x) (* amount (- x 0.5))))
        (mapper u (fn (x) (* amount x))))))

;; The span, [low high] in the travel, that a modulation [source amount
;; polarity] reaches from the knob's position.
(define (reach modulation)
  (let ((amount (nth modulation 1)))
    (cond ((= (nth modulation 2) ':bipolar) [(* -0.5 (abs amount)) (* 0.5 (abs amount))])
          ((< amount 0) [amount 0])
          (else [0 amount]))))

;; The knob's pipe moved by modulations, a vector of [source amount
;; polarity], their offsets summed and the position kept in the travel.
;; The page draws the span they reach around the knob.
(define (moved knob modulations &optional what)
  (let ((range (if what (control-range knob what) (control-range knob)))
        (total #f)
        (low 0)
        (high 0))
    (map (fn (m)
           (let ((r (reach m)))
             (set! low (+ low (nth r 0)))
             (set! high (+ high (nth r 1)))))
         modulations)
    (show-modulation knob low high)
    (map (fn (m)
           (let ((o (offset-of m)))
             (set! total (if total (mapper total (fn (t) (+ t o))) o))))
         modulations)
    (let ((p (position-in range knob)))
      (value-in range (mapper p (fn (x) (clamp (+ x total) 0 1)))))))

;; A modulation of a source by amount, with polarity or the source's own.
(define (modulation source amount polarity)
  [source amount (if polarity polarity (default-polarity source))])

;; (modulated knob source amount :polarity p): the pipe of a knob a piece
;; placed itself, moved by source.
(define (modulated knob source amount &key polarity)
  (moved knob [(modulation source amount polarity)]))

;; (modulate instance 'slot source amount :polarity p): records that the
;; slot's knob is moved by source, and returns the instance, so it reads
;; inside make's place: (chain in out (modulate (make reverb) 'mix lfo 0.2)).
;; The page draws the knob as it would; play and process apply the
;; modulations to it (apply-modulations). Several modulations of one slot
;; sum.
(define (modulate instance slot source amount &key polarity)
  (if (sealed? instance)
      (error "modulate moves " slot " of an instance whose nodes are built already: modulate it before "
             "(play ...), (process ...), (chain ...), or its signal is used")
      #f)
  (set! (slot-value instance 'modulations)
        (pushed (slot-value instance 'modulations) [slot (modulation source amount polarity)]))
  instance)

;; Moves each modulated slot of instance, which now holds its knob's pipe,
;; forgets the modulations, so applying them again does nothing, and seals
;; the instance: its nodes are about to read its slots.
(define (apply-modulations instance)
  (let ((pending (slot-value instance 'modulations)))
    (set! (slot-value instance 'modulations) [])
    (seal instance)
    (map (fn (i)
           (let ((slot (nth (nth pending i) 0)))
             ;; Each slot once, at its first modulation, with all of its.
             (if (= (length (kept (fn (j) (= (nth (nth pending j) 0) slot)) (range i))) 0)
                 (set! (slot-value instance slot)
                       (moved (slot-value instance slot)
                              (map (fn (e) (nth e 1)) (kept (fn (e) (= (nth e 0) slot)) pending))
                              slot))
                 #f)))
         (range (length pending)))
    instance))
