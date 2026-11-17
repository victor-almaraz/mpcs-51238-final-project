;; A level you can see: a tone that swells and fades, and a bar that rises
;; and falls with it.
;;
;; A 0.5 Hz sine on an audio pipe is the swell. Through gain it sets the
;; tone's loudness; through a mapper it sets the top of the bar. An image
;; parameter that follows an audio pipe reads the pipe's latest sample at
;; each frame.

(module meter)

(defpiece main ()
  (let ((swell  (pipe :audio))
        (tone   (pipe :audio))
        (shaped (pipe :audio))
        (panel  (pipe :video))
        (bar    (pipe :video)))
    ;; Sound: a 330 Hz sine whose gain follows the swell, from 0 to 0.3.
    (sine-osc :freq 0.5hz :out swell)
    (sine-osc :freq 330hz :out tone)
    (gain :in tone :amount (mapper swell (fn (v) (scale v -1 1 0 0.3))) :out shaped)
    (audio-out :in shaped :channel 0)
    ;; Picture: a bar from the bottom at y 660 up to a top that follows the
    ;; swell, from 660 (silent) to 60 (loudest).
    (background :color (rgb 0.08 0.08 0.08) :out panel)
    (rect :in panel :mode :corners :x 560 :y 660 :width 720
          :height (mapper swell (fn (v) (scale v -1 1 660 60)))
          :fill (hsb 0.33 0.7 0.9) :stroke :none :out bar)
    (video-out :in bar)))
