;; A synthesizer and its pedals, played from a web page.
;;
;; With project.lisp's :page-port 8000, the server serves the page at
;; http://localhost:8000/ while the piece plays. The page is drawn from the
;; instrument and the effects themselves: synth-panel draws a knob for each
;; of the subtractive synthesizer's :control slots, starting at the value
;; make gives it, and chain draws each effect as a pedal with a footswitch
;; that bypasses it. The page's keyboard plays the eight voices; the
;; output's level goes back to the panel's meter.

(module page-keys)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((synth  (make subtractive :voices 8 :cutoff 1200hz :env-amount 2khz :resonance 0.25
                                  :attack 5ms :release 400ms :level -9db))
        (notes  (keys "Keys" :low 48 :high 71 :address "/note"))
        (voiced (pipe :audio))
        (out    (pipe :audio))
        (level  (pipe :control)))
    ;; The panel first: it sets the synthesizer's slots to its knobs' pipes,
    ;; which play then builds the voices from.
    (define panel
      (synth-panel "PAGE KEYS" synth
                   (section "Keys" notes)
                   (section "Output" (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25))
                   :maker "Eight-voice subtractive"))
    (play synth notes voiced)
    (define board (chain voiced out (make chorus :mix 0.3) (make echo :time 300ms :mix 0.25)
                         (make reverb :size 0.8 :mix 0.2)))
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (page panel board :accent "#e8a33d")))
