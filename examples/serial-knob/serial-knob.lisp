;; A drone whose brightness follows a potentiometer on a board that
;; prints its readings over USB serial, one a line (examples/README.md
;; has the sketch).
;;
;; serial-in reads the first USB serial device at 115200 baud. The
;; reading, 0 to 1023, is smoothed over 30 ms and sets a ladder filter's
;; cutoff from 200 Hz to 4 kHz on an exponential scale.
;;
;; Its page (ui/index.html) is the drone's panel: the pitch of the two
;; sawtooths, the filter's resonance, and the volume; the panel shows the
;; board's reading, the cutoff it sets, and the output's level. The board
;; alone turns the cutoff. Without the page, the knobs keep the values
;; written here.

(module serial-knob)

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

(defpiece main ()
  (let ((knob     (pipe :control))
        (smooth   (pipe :control))
        (root     (control "/pitch" 55hz))
        (osc      (pipe :audio))
        (filtered (pipe :audio))
        (out      (pipe :audio))
        (level    (pipe :control)))
    (serial-in :device "" :baud 115200 :value knob)
    (smoother :in knob :time 30ms :out smooth)

    ;; Two sawtooths 0.4 Hz apart, so the drone beats slowly.
    (saw-osc :freq root :out osc)
    (saw-osc :freq (mapper root (fn (r) (+ r 0.4hz))) :out osc)
    (let ((cutoff (mapper smooth (fn (k) (scale-exp k 0 1023 200hz 4khz)))))
      (ladder :in osc
              :cutoff cutoff
              :resonance (control "/resonance" 0.4)
              :out filtered)
      ;; The panel's displays: the board's reading and the cutoff it sets.
      (osc-out :port :page :address "/reading" :value (mapper smooth (fn (k) (quantize k 1))))
      (osc-out :port :page :address "/cutoff" :value (mapper cutoff (fn (c) (quantize c 1hz)))))
    (gain :in filtered :amount (control "/volume" -12db) :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
