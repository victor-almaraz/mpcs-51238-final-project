;; A stereo echo on the first two audio inputs.
;;
;; A feedback comb repeats the input every 375 ms (a dotted eighth at
;; 120 BPM) at 45% feedback, and its damping darkens each repeat. A slow
;; LFO moves the delay time by 2 ms, like tape wow. A low-pass filter
;; softens the echoes before they are mixed under the dry signal.

(module delay)

(defpiece main ()
  (let ((dry    (pipe :audio :channels 2))
        (time   (pipe :control))
        (echoes (pipe :audio :channels 2))
        (soft   (pipe :audio :channels 2))
        (out    (pipe :audio :channels 2)))
    (audio-in :channel 0 :out dry)
    ;; depth and offset are plain numbers; time reads them as seconds.
    (lfo :shape :sine :rate 0.3hz :depth 0.002 :offset 0.375 :out time)
    (comb :in dry :time time :max-time 1s :feedback 0.45 :damping 0.3 :out echoes)
    (biquad :in echoes :mode :low-pass :freq 3khz :out soft)
    (mix :in [dry soft] :levels [1 0.63] :out out)
    (audio-out :in out :channel 0)))
