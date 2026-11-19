;; wavetable: a drone through eight tables, from a sine to a bright saw to
;; a hollow square. A slow LFO sweeps the position across them, from the
;; sine up to the square and back every ten seconds; three oscillators a
;; fifth and an octave apart share it. The tables load as they are, not
;; resampled, so each stays 2048 samples long at any server rate.

(defpiece main ()
  (let ((tables (buffer :file "samples/tables.wav" :resample #f))
        (morph  (pipe :control))
        (tone   (pipe :audio))
        (out    (pipe :audio)))
    (lfo :shape :triangle :rate 0.1hz :depth 0.5 :offset 0.5 :out morph)
    (map (fn (f) (wavetable-osc :buffer tables :freq f :position morph :out tone))
         [110hz 165hz 220hz])
    (gain :in tone :amount -15db :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
