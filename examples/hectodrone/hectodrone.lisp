;; hectodrone: a polyphonic synthesizer of four voices, each of one
;; hundred sawtooth oscillators.
;;
;; Each note takes a voice, and every oscillator of the voice plays it.
;; With every voice sounding, a new note takes the voice of the oldest,
;; at once, and restarts its envelope; releasing a note whose voice a
;; newer one has taken does nothing.
;;
;; The oscillators of a voice are detuned around its note's pitch in a normal
;; distribution: each oscillator has a fixed place, a quantile of the
;; standard normal distribution, and its detuning is its place times the
;; standard deviation. MIDI pitch bend sets the standard deviation: at
;; rest, the fine deviation; bent fully up, the widest; bent fully down,
;; none, so the oscillators play in unison. Pitch bend does not change the
;; notes' pitch. The modulation wheel (controller 1) sets the cutoff of a
;; low-pass filter on each channel of each voice.
;;
;; The oscillators alternate between the left channel and the right, so
;; each note is as wide as the stereo image.

(module hectodrone)

(import "std/instruments")
(import "std/voices")

(define oscillator-count 100)

;; The scale of each channel's fifty oscillators before the filter, which
;; saturates: their sum, about 4 RMS, comes to about 0.2, within the
;; filter's linear range.
(define headroom -26db)

(defclass hectodrone (instrument enveloped)
  ((voices    :initform 4)
   (level     :initform 0db)
   (attack    :initform 10ms)
   (decay     :initform 1s)
   (sustain   :initform 0.8)
   (release   :initform 400ms)
   ;; Time the pitch takes to glide to a new note; 0 for none.
   (glide     :initarg :glide     :initform 0ms    :accessor glide)
   ;; The standard deviation of the detuning, at rest and bent fully up.
   (fine      :initarg :fine      :initform 1ct    :accessor fine)
   (widest    :initarg :widest    :initform 10ct   :accessor widest)
   ;; The filter: its cutoff with the modulation wheel down and fully up,
   ;; the time the cutoff takes to follow the wheel, and the resonance.
   (darkest   :initarg :darkest   :initform 300hz  :accessor darkest)
   (brightest :initarg :brightest :initform 12khz  :accessor brightest)
   (wheel-lag :initarg :wheel-lag :initform 30ms   :accessor wheel-lag)
   (resonance :initarg :resonance :initform 0.2    :accessor resonance)))

;; --- The normal distribution ---------------------------------------------------

;; The polynomial with coefficients cs, highest power first, at x.
(define (horner cs x)
  (let ((sum 0))
    (map (fn (c) (set! sum (+ (* sum x) c))) cs)
    sum))

;; Acklam's rational approximations of the inverse normal distribution
;; function, accurate to a relative error of 1.15e-9.
(define central-numerator [-39.69683028665376 220.9460984245205 -275.9285104469687
                           138.3577518672690 -30.66479806614716 2.506628277459239])
(define central-denominator [-54.47609879822406 161.5858368580409 -155.6989798598866
                             66.80131188771972 -13.28068155288572 1])
(define tail-numerator [-0.007784894002430293 -0.3223964580411365 -2.400758277161838
                        -2.549732539343734 4.374664141464968 2.938163982698783])
(define tail-denominator [0.007784695709041462 0.3224671290700398 2.445134137142996
                          3.754408661907416 1])

;; The lower tail, for p below 0.02425.
(define (lower-tail p)
  (let ((q (pow (* -2 (log p)) 0.5)))
    (/ (horner tail-numerator q) (horner tail-denominator q))))

;; The standard normal quantile of p, from 0 to 1: the z below which a
;; proportion p of the distribution lies.
(define (normal-quantile p)
  (cond ((< p 0.02425) (lower-tail p))
        ((> p 0.97575) (- (lower-tail (- 1 p))))
        (else (let ((q (- p 0.5)))
                (/ (* q (horner central-numerator (* q q)))
                   (horner central-denominator (* q q)))))))

;; Oscillator i's place, for i from 0 to 99: the quantile at the middle of
;; the i-th hundredth of the distribution, from about -2.58 to 2.58.
(define (place i) (normal-quantile (/ (+ i 0.5) oscillator-count)))

(define (fraction x) (- x (floor x)))

;; Oscillator i's starting phase, from 0 to 1: a hash of i, so the
;; phases are unrelated, as those of free-running oscillators are. Evenly
;; spread phases would cancel while the oscillators are nearly in tune.
(define (start-phase i) (fraction (* 43758.5453 (sin (+ i 1)))))

;; Oscillator i's channel: 0 (left) for even i, 1 (right) for odd. The
;; places alternate in order, so both channels are spread like the whole.
(define (channel-of i) (if (= (fraction (/ i 2)) 0) 0 1))

;; --- The instrument ---------------------------------------------------------------

;; The standard deviation of the detuning, in cents, from the pitch bend
;; in semitones, -1 to 1: none at full bend down, fine at rest, widest at
;; full bend up.
(define (deviation h bend)
  (let ((fine-cents   (unit-value (fine h) 'ct))
        (widest-cents (unit-value (widest h) 'ct)))
    (mapper bend (fn (u) (+ fine-cents
                            (* (max u 0) (- widest-cents fine-cents))
                            (* (min u 0) fine-cents))))))

;; Cents to a natural-log frequency ratio.
(define per-cent (/ (log 2) 1200))

;; Each oscillator's frequency ratio, shared by the voices: e to the power
;; of its place times the deviation, in natural-log units, which is 2 to
;; the power of its place times the deviation in octaves.
(define (detune-ratios h bend)
  (let ((spread (deviation h bend)))
    (map (fn (i) (mapper spread (fn (cents) (exp (* cents (* (place i) per-cent))))))
         (range oscillator-count))))

;; One voice: its notes, played by every oscillator at its ratio, through
;; the filter and the envelope into out, the left and right audio pipes.
(define (hecto-voice h note ratios cutoff out)
  (let ((freq   (pipe :control))
        (glided (pipe :control))
        (vel    (note-velocity note))
        (sums   [(pipe :audio) (pipe :audio)])
        (scaled [(pipe :audio) (pipe :audio)])
        (toned  [(pipe :audio) (pipe :audio)]))
    (note-to-freq :in note :out freq)
    (smoother :in freq :time (glide h) :curve :linear :out glided)
    (map (fn (i)
           (let ((detuned (pipe :control)))
             (multiply :in [glided (nth ratios i)] :out detuned)
             (saw-osc :freq detuned :phase (start-phase i) :out (nth sums (channel-of i)))))
         (range oscillator-count))
    (map (fn (c)
           (gain :in (nth sums c) :amount headroom :out (nth scaled c))
           (ladder :in (nth scaled c) :cutoff cutoff :resonance (resonance h) :out (nth toned c))
           (adsr :gate note :in (nth toned c)
                 :attack (attack h) :decay (decay h) :sustain (sustain h) :release (release h)
                 :gain (mapper vel (fn (v) (* v (level h))))
                 :out (nth out c)))
         [0 1])))

;; Plays the notes on notes into out, a vector of two audio pipes: the
;; left channel and the right. The voices sum there.
(defmethod play ((h hectodrone) notes out)
  (let ((bend   (pipe :control))
        (wheel  (pipe :control))
        (cutoff (pipe :control)))
    (pitch-bend :in notes :range 1st :out bend)
    (control-change :in notes :cc 1 :out wheel)
    (smoother :in (mapper wheel (fn (w) (scale-exp w 0 1 (darkest h) (brightest h))))
              :time (wheel-lag h) :out cutoff)
    (let ((ratios (detune-ratios h bend)))
      (voices (voice-count h) notes (fn (note) (hecto-voice h note ratios cutoff out))))))
