;; A project for the command-line tests: an import, and a piece that
;; compiles with a warning.
(module main)
(import "voices")

(defpiece main ()
  (let ((osc (pipe :audio)))
    (tone 220hz osc)
    (audio-out :in osc :channel 0)))

(defpiece loud ()
  (let ((osc (pipe :audio)) (out (pipe :audio)))
    (tone 220hz osc)
    (gain :in osc :amount 40db :out out)
    (audio-out :in out :channel 0)))
