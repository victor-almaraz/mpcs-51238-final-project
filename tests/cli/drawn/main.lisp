;; A piece whose page is drawn from its layout (dsl-ui.md §8), for --page.
(defpiece main ()
  (let ((pitch (knob "Pitch" :min 55hz :max 440hz :initial 110hz)))
    (sine-osc :freq pitch :out (pipe :audio))
    (page (panel "Drawn" (section "Oscillators" pitch)))))
