;; A drone whose brightness follows a potentiometer on a board that
;; prints its readings over USB serial, one a line (examples/README.md
;; has the sketch).
;;
;; serial-in reads the first USB serial device at 115200 baud. The
;; reading, 0 to 1023, is smoothed over 30 ms and sets a ladder filter's
;; cutoff from 200 Hz to 4 kHz on an exponential scale.
;;
;; Its page, drawn from the layout below, is the drone's panel: the pitch
;; of the two sawtooths, the filter's resonance, and the volume; the panel
;; shows the board's reading, the cutoff it sets on its travel, and the
;; output's level. The board alone turns the cutoff. Without the page, the
;; knobs keep the values written here.

(module serial-knob)

(defpiece main ()
  (let ((reading  (pipe :control))
        (smooth   (pipe :control))
        (root     (knob "Pitch" :min 27.5hz :max 220hz :initial 55hz :size :big))
        (volume   (knob "Volume" :max 0db :initial -12db))
        (osc      (pipe :audio))
        (filtered (pipe :audio))
        (out      (pipe :audio))
        (level    (pipe :control)))
    (serial-in :device "" :baud 115200 :value reading)
    (smoother :in reading :time 30ms :out smooth)

    ;; Two sawtooths 0.4 Hz apart, so the drone beats slowly.
    (saw-osc :freq root :out osc)
    (saw-osc :freq (mapper root (fn (r) (+ r 0.4hz))) :out osc)
    (let ((cutoff    (mapper smooth (fn (k) (scale-exp k 0 1023 200hz 4khz))))
          (resonance (knob "Resonance" :initial 0.4 :unit :percent)))
      (ladder :in osc
              :cutoff cutoff
              :resonance resonance
              :out filtered)
      (gain :in filtered :amount volume :out out)
      (audio-out :in out :channel 0)
      (audio-out :in out :channel 1)
      (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
      (page
        (synth-panel "serial-knob"
          (section "Oscillators" root)
          (section "Board"
                   (readout "Reading" (mapper smooth (fn (k) (quantize k 1))))
                   (readout "Cutoff" (mapper cutoff (fn (c) (quantize c 1hz))) :unit :hz)
                   (meter "Travel" cutoff :min 200hz :max 4khz :scale :exp :hold 0)
                   :columns 6)
          (section "Filter" resonance)
          (section "Output" volume
                   (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical)))
        :accent "#e8a33d"))))
