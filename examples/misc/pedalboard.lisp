;; A guitar pedalboard on audio input 1, from the standard library:
;; overdrive, then chorus, then an echo a dotted eighth long at 120 BPM,
;; then a reverb.

(module pedalboard)

(import "std/effects")

(defpiece main ()
  (let ((guitar  (pipe :audio))
        (driven  (pipe :audio))
        (chorused (pipe :audio))
        (echoed  (pipe :audio))
        (out     (pipe :audio)))
    (audio-in :channel 0 :out guitar)
    (process (make overdrive :drive 6 :tone 4khz :level -9db :mix 1) guitar driven)
    (process (make chorus :rate 0.6hz :mix 0.4) driven chorused)
    (process (make echo :time 0.75beat :feedback 0.35 :mix 0.3) chorused echoed)
    (process (make reverb :size 0.8 :mix 0.2) echoed out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
