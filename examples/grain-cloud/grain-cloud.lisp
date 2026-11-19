;; grain-cloud: a cloud of grains over struck glass. One slow LFO moves
;; where the grains read, another how many start each second; each grain
;; is jittered in time and pitch, sometimes reversed, and spread across
;; the stereo field.

(defpiece main ()
  (let ((glass   (buffer :file "samples/glass.wav"))
        (where   (pipe :control))
        (density (pipe :control))
        (cloud   (pipe :audio :channels 2))
        (out     (pipe :audio :channels 2)))
    (lfo :shape :triangle :rate 0.05hz :depth 1.2 :offset 1.5 :out where)
    (lfo :shape :sine :rate 0.13hz :depth 12 :offset 18 :out density)
    (grains :buffer glass :position where :density density :duration 150ms :jitter 80ms
            :pitch-jitter 0.3 :reverse 0.2 :spread 0.8 :out cloud)
    (gain :in cloud :amount 0.5 :out out)
    (audio-out :in out :channel 0)))
