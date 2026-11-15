;; A turning square, and a tone that rises as it turns.
;;
;; A saw LFO sweeps from -1 to 1 once every four seconds. transform turns
;; everything drawn after it by that sweep, a full turn about the center
;; of the frame, so the square is drawn about the origin. The same LFO
;; sets the tone's pitch, so each turn is heard as a rising sweep.

(module spin)

(defpiece main ()
  (let ((turn  (pipe :control))
        (tone  (pipe :audio))
        (sky   (pipe :video))
        (moved (pipe :video))
        (box   (pipe :video)))
    ;; Sound: a triangle wave from 220 to 440 Hz with each turn, quietly.
    (lfo :shape :saw :rate 0.25hz :depth 1 :out turn)
    (triangle-osc :freq (mapper turn (fn (v) (scale-exp v -1 1 220hz 440hz))) :out tone)
    (audio-out :in (mapper tone (fn (s) (* s 0.2))) :channel 0)
    ;; Picture: the origin moves to the center and turns with the LFO.
    (background :color (rgb 0.1 0.1 0.12) :out sky)
    (transform :in sky :translate-x 640 :translate-y 360
               :rotate (mapper turn (fn (v) (scale v -1 1 0 6.2831853)))
               :out moved)
    (rect :in moved :x 0 :y 0 :width 240 :height 240 :mode :center
          :fill (rgb 0.95 0.6 0.2) :stroke (rgb 1 1 1) :stroke-weight 4 :out box)
    (video-out :in box)))
