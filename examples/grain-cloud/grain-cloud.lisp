;; grain-cloud: a cloud of grains over struck glass. One slow LFO moves
;; where the grains read, another how many start each second; each grain
;; is jittered in time and pitch, sometimes reversed, and spread across
;; the stereo field.
;;
;; Its page, drawn from the layout below, is the granulator's panel: the
;; scan's rate, centre, and depth across the recording; the density's
;; rate, centre, and depth, with the density it reaches; each grain's
;; length, jitter, pitch jitter, and reverse chance; and the spread, the
;; volume, and the level. Its screen (ui/script.js and ui/style.css) marks
;; where the grains read in the recording. Without the page, the knobs keep
;; the values written here.

(defpiece main ()
  (let ((glass   (buffer :file "samples/glass.wav"))
        (where   (pipe :control))
        (density (pipe :control))
        (cloud   (pipe :audio :channels 2))
        (out     (pipe :audio :channels 2))
        (level   (pipe :control))
        (scan-rate    (knob "Rate" :min 0.01hz :max 1hz :initial 0.05hz))
        (scan-centre  (knob "Centre" :max 3s :initial 1.5s))
        (scan-depth   (knob "Depth" :max 1.5s :initial 1.2s))
        (dense-rate   (knob "Rate" :min 0.01hz :max 2hz :initial 0.13hz))
        (dense-centre (knob "Centre" :min 1hz :max 100hz :initial 18hz))
        (dense-depth  (knob "Depth" :max 40hz :initial 12hz :scale :linear))
        (length       (knob "Length" :min 10ms :max 500ms :initial 150ms :scale :exp :size :big))
        (jitter       (knob "Jitter" :max 300ms :initial 80ms))
        (pitch        (knob "Pitch" :max 4 :initial 0.3))
        (reverse      (knob "Reverse" :initial 0.2 :unit :percent))
        (spread       (knob "Spread" :initial 0.8 :unit :percent))
        (volume       (knob "Volume" :max 0db :initial 0.5)))
    (lfo :shape :triangle :rate scan-rate :depth scan-depth :offset scan-centre :out where)
    (lfo :shape :sine :rate dense-rate :depth dense-depth :offset dense-centre :out density)
    (grains :buffer glass :position where :density density
            :duration length :jitter jitter :pitch-jitter pitch :reverse reverse
            :spread spread :out cloud)
    (gain :in cloud :amount volume :out out)
    (audio-out :in out :channel 0)
    ;; Where the grains read, for the screen.
    (osc-out :port :page :address "/position" :value (mapper where (fn (w) (quantize w 0.01))))
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "GRAIN CLOUD"
        (section "Scan" scan-rate scan-centre scan-depth :columns 9)
        (section "Density"
                 (readout "Grains a second" (mapper density (fn (d) (quantize d 1))) :span [9 3])
                 dense-rate dense-centre dense-depth
                 :columns 9)
        (section "Grains" length jitter pitch reverse :columns 10)
        (section "Output" spread volume
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical))
        :maker "Granular glass")
      :accent "#b39dff")))
