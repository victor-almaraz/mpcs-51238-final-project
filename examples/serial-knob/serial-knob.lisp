;; A drone whose brightness follows a potentiometer on a board that
;; prints its readings over USB serial, one a line (examples/README.md
;; has the sketch).
;;
;; serial-in reads the first USB serial device at 115200 baud. The
;; reading, 0 to 1023, is smoothed over 30 ms and sets a ladder filter's
;; cutoff from 200 Hz to 4 kHz on an exponential scale.

(module serial-knob)

(defpiece main ()
  (let ((knob     (pipe :control))
        (smooth   (pipe :control))
        (osc      (pipe :audio))
        (filtered (pipe :audio))
        (out      (pipe :audio)))
    (serial-in :device "" :baud 115200 :value knob)
    (smoother :in knob :time 30ms :out smooth)

    ;; Two sawtooths a little apart, so the drone beats slowly.
    (saw-osc :freq 55hz :out osc)
    (saw-osc :freq 55.4hz :out osc)
    (ladder :in osc
            :cutoff (mapper smooth (fn (k) (scale-exp k 0 1023 200hz 4khz)))
            :resonance 0.4
            :out filtered)
    (gain :in filtered :amount -12db :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
