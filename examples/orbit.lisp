;; A tone you can see: a sine, and a circle that orbits with it.
;;
;; A slow LFO drives both the sound and the picture. It bends the sine's
;; pitch a little, moves the circle around the center of a 1280 x 720
;; frame, and blends the circle's color from orange to blue. A rect
;; frames the scene, and a line joins the center to the circle.

(module orbit)

(defpiece main ()
  (let ((sweep (pipe :control))
        (turn  (pipe :control))
        (tone  (pipe :audio))
        (shade (pipe :control :channels 4))
        (sky   (pipe :video))
        (frame (pipe :video))
        (ray   (pipe :video))
        (ball  (pipe :video)))
    ;; Sound: a sine whose pitch follows the LFO, quietly.
    (lfo :shape :sine :rate 0.25hz :depth 1 :out sweep)
    (sine-osc :freq (mapper sweep (fn (v) (scale v -1 1 220hz 330hz))) :out tone)
    (audio-out :in (mapper tone (fn (s) (* s 0.2))) :channel 0)
    ;; The circle's place on the orbit: the same LFO, a quarter turn later.
    (lfo :shape :sine :rate 0.25hz :phase 0.25 :depth 1 :out turn)
    (lerp-color :color-a (rgb 1 0.5 0) :color-b (rgb 0.2 0.4 1)
                :amount (mapper sweep (fn (v) (scale v -1 1 0 1))) :out shade)
    ;; Picture: background, frame, ray, ball.
    (background :color (rgb 0.05 0.05 0.08) :out sky)
    (rect :in sky :x 640 :y 360 :width 1200 :height 640 :mode :center
          :fill :none :stroke (rgb 0.3 0.3 0.35) :stroke-weight 2 :out frame)
    (line :in frame :x1 640 :y1 360
          :x2 (mapper turn (fn (v) (scale v -1 1 340 940)))
          :y2 (mapper sweep (fn (v) (scale v -1 1 160 560)))
          :stroke (rgb 1 1 1 0.5) :stroke-weight 3 :out ray)
    (circle :in ray
            :x (mapper turn (fn (v) (scale v -1 1 340 940)))
            :y (mapper sweep (fn (v) (scale v -1 1 160 560)))
            :diameter 90 :fill shade :stroke :none :out ball)
    (video-out :in ball)))
