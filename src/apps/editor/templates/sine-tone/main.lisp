;; A sine tone of 440 Hz at -18 dB on both channels. Change the frequency
;; or the level and press Play again to update the playing piece.

(module main)

(defpiece main ()
  (let ((osc (pipe :audio))
        (out (pipe :audio)))
    (sine-osc :freq 440hz :out osc)
    (gain :in osc :amount -18db :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
