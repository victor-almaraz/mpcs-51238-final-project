;; wavetable: a drone through eight tables, from a sine to a bright saw to
;; a hollow square. A slow LFO sweeps the position across them, from the
;; sine up to the square and back every ten seconds; three oscillators a
;; fifth and an octave apart share it. The tables load as they are, not
;; resampled, so each stays 2048 samples long at any server rate.
;;
;; Its page is the synthesizer's panel, drawn from the layout below: the
;; pitch of the lowest oscillator, the sweep's rate, centre, and depth, and
;; the volume; the panel shows the output's level, and the sweep's screen
;; (ui/script.js and ui/style.css) the eight tables and where the sweep is.
;; Without the page, the knobs keep the values written here.

(defpiece main ()
  (let ((tables (buffer :file "samples/tables.wav" :resample #f))
        (root   (knob "Pitch" :min 55hz :max 440hz :initial 110hz :size :big))
        (rate   (knob "Rate" :min 0.01hz :max 2hz :initial 0.1hz))
        (centre (knob "Centre" :initial 0.5 :unit :percent))
        (depth  (knob "Depth" :max 0.5 :initial 0.5 :unit :percent))
        (volume (knob "Volume" :max 0db :initial -15db))
        (morph  (pipe :control))
        (tone   (pipe :audio))
        (out    (pipe :audio))
        (level  (pipe :control)))
    (lfo :shape :triangle :rate rate :depth depth :offset centre :out morph)
    ;; The root, a fifth above it, and an octave above it.
    (map (fn (ratio) (wavetable-osc :buffer tables :freq (mapper root (fn (r) (* r ratio))) :position morph :out tone))
         [1 1.5 2])
    (gain :in tone :amount volume :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    ;; Where the sweep is, for the screen, and the level.
    (osc-out :port :page :address "/position" :value (mapper morph (fn (m) (quantize m 0.01))))
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "WAVETABLE"
        (section "Oscillators" root)
        (section "Sweep" rate centre depth :columns 9)
        (section "Output" volume
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical))
        :maker "Eight-table drone")
      :accent "#4fd1c5")))
