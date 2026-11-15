;; A stereo reverb on the first audio input.
;;
;; Each side is a Schroeder reverberator with Freeverb's tunings: four
;; damped feedback combs in parallel, then two allpass diffusers in
;; series. The right side's delays are 0.52 ms longer, which decorrelates
;; the two sides and widens a mono input.

(module reverb)

(define (reverb-side in out spread)
  (let ((combs    (pipe :audio))
        (diffused (pipe :audio)))
    (comb :in in :time (+ 25.31ms spread) :feedback 0.84 :damping 0.2 :out combs)
    (comb :in in :time (+ 28.96ms spread) :feedback 0.84 :damping 0.2 :out combs)
    (comb :in in :time (+ 32.24ms spread) :feedback 0.84 :damping 0.2 :out combs)
    (comb :in in :time (+ 35.31ms spread) :feedback 0.84 :damping 0.2 :out combs)
    (allpass :in combs :time (+ 12.61ms spread) :feedback 0.5 :out diffused)
    (allpass :in diffused :time (+ 7.73ms spread) :feedback 0.5 :out out)))

(defpiece main ()
  (let ((input     (pipe :audio))
        (left-wet  (pipe :audio))
        (right-wet (pipe :audio))
        (left      (pipe :audio))
        (right     (pipe :audio)))
    (audio-in :channel 0 :out input)
    (reverb-side input left-wet 0ms)
    (reverb-side input right-wet 0.52ms)
    (mix :in [input left-wet] :levels [0.7 0.2] :out left)
    (mix :in [input right-wet] :levels [0.7 0.2] :out right)
    (audio-out :in left :channel 0)
    (audio-out :in right :channel 1)))
