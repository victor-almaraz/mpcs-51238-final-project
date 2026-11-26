;; wavetable: a drone through eight tables, from a sine to a bright saw to
;; a hollow square. A slow LFO sweeps the position across them, from the
;; sine up to the square and back every ten seconds; three oscillators a
;; fifth and an octave apart share it. The tables load as they are, not
;; resampled, so each stays 2048 samples long at any server rate.
;;
;; Its page (ui/index.html) is the synthesizer's panel: the pitch of the
;; lowest oscillator, the sweep's rate, centre, and depth, and the volume;
;; the panel shows the sweep's position and the output's level. Without the
;; page, the knobs keep the values written here.

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

(defpiece main ()
  (let ((tables (buffer :file "samples/tables.wav" :resample #f))
        (root   (control "/pitch" 110hz))
        (morph  (pipe :control))
        (tone   (pipe :audio))
        (out    (pipe :audio))
        (level  (pipe :control)))
    (lfo :shape :triangle :rate (control "/sweep/rate" 0.1hz) :depth (control "/sweep/depth" 0.5)
         :offset (control "/sweep/centre" 0.5) :out morph)
    ;; The root, a fifth above it, and an octave above it.
    (map (fn (ratio) (wavetable-osc :buffer tables :freq (mapper root (fn (r) (* r ratio))) :position morph :out tone))
         [1 1.5 2])
    (gain :in tone :amount (control "/volume" -15db) :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    ;; The panel's displays: where the sweep is, and the level.
    (osc-out :port :page :address "/position" :value (mapper morph (fn (m) (quantize m 0.01))))
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
