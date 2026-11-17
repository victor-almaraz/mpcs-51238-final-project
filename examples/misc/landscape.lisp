;; A still picture: sky, sun, two mountains, and the ground.
;;
;; Each call draws over the frame before it, so the order of the calls is
;; the order of the layers: the sky first, the ground last. The colors are
;; written with hsb, hue first, so the two mountains share a blue hue and
;; differ only in saturation and brightness.

(module landscape)

(defpiece main ()
  (let ((sky    (pipe :video))
        (sun    (pipe :video))
        (far    (pipe :video))
        (near   (pipe :video))
        (ground (pipe :video)))
    (background :color (hsb 0.58 0.45 0.95) :out sky)
    (circle :in sky :x 960 :y 220 :diameter 160
            :fill (hsb 0.12 0.8 1) :stroke :none :out sun)
    (triangle :in sun :x1 100 :y1 560 :x2 420 :y2 200 :x3 740 :y3 560
              :fill (hsb 0.6 0.25 0.55) :stroke :none :out far)
    (triangle :in far :x1 480 :y1 560 :x2 860 :y2 300 :x3 1240 :y3 560
              :fill (hsb 0.6 0.35 0.4) :stroke :none :out near)
    (rect :in near :x 0 :y 540 :width 1280 :height 180
          :fill (hsb 0.3 0.5 0.45) :stroke :none :out ground)
    (video-out :in ground)))
