;; grain-cloud: a cloud of grains over struck glass. One slow LFO moves
;; where the grains read, another how many start each second; each grain
;; is jittered in time and pitch, sometimes reversed, and spread across
;; the stereo field.
;;
;; Its page (ui/index.html) is the granulator's panel: the scan's rate,
;; centre, and depth across the recording, whose screen marks where the
;; grains read; the density's rate, centre, and depth, with the density
;; it reaches; each grain's length, jitter, pitch jitter, reverse chance,
;; and spread; and the volume and the level. Without the page, the knobs
;; keep the values written here.

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

(defpiece main ()
  (let ((glass   (buffer :file "samples/glass.wav"))
        (where   (pipe :control))
        (density (pipe :control))
        (cloud   (pipe :audio :channels 2))
        (out     (pipe :audio :channels 2))
        (level   (pipe :control)))
    (lfo :shape :triangle :rate (control "/scan/rate" 0.05hz) :depth (control "/scan/depth" 1.2)
         :offset (control "/scan/centre" 1.5) :out where)
    (lfo :shape :sine :rate (control "/density/rate" 0.13hz) :depth (control "/density/depth" 12)
         :offset (control "/density/centre" 18) :out density)
    (grains :buffer glass :position where :density density
            :duration (control "/grain" 150ms) :jitter (control "/jitter" 80ms)
            :pitch-jitter (control "/pitch-jitter" 0.3) :reverse (control "/reverse" 0.2)
            :spread (control "/spread" 0.8) :out cloud)
    (gain :in cloud :amount (control "/volume" 0.5) :out out)
    (audio-out :in out :channel 0)
    ;; The panel's displays: where the grains read, how many start each
    ;; second, and the level.
    (osc-out :port :page :address "/position" :value (mapper where (fn (w) (quantize w 0.01))))
    (osc-out :port :page :address "/rate" :value (mapper density (fn (d) (quantize d 1))))
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
