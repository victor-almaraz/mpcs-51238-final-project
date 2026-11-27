;; The weather in Chicago, as a tone and a wind.
;;
;; Two web-in nodes poll Open-Meteo once a minute, one for the current
;; temperature and one for the wind speed; with one URL and interval they
;; share each request. The temperature, -20 to 40 degrees Celsius, sets the
;; pitch of a sine and a triangle an octave up, from 110 Hz to 880 Hz; the
;; wind, 0 to 60 km/h, raises a pink noise from silence. Both glide over
;; 2 s, so a new reading moves the sound rather than jumping it.
;;
;; Its page, drawn from the layout below, is the synthesizer's panel: the
;; tone's volume and pitch, the wind's level, and the output's level; its
;; screen (ui/script.js and ui/style.css) shows the temperature and the
;; wind as they glide. Without the page, the knob keeps the value written
;; here.

(module weather)

(define forecast
  "https://api.open-meteo.com/v1/forecast?latitude=41.88&longitude=-87.63&current=temperature_2m,wind_speed_10m")

(defpiece main ()
  (let ((temp   (pipe :control))
        (wind   (pipe :control))
        (warmth (pipe :control))
        (gusts  (pipe :control))
        (tone   (pipe :audio))
        (air    (pipe :audio))
        (mix    (pipe :audio))
        (level  (pipe :control))
        (volume (knob "Volume" :max 0db :initial -18db :size :big)))
    (web-in :url forecast :every 60s :fields ["/current/temperature_2m"] :value temp)
    (web-in :url forecast :every 60s :fields ["/current/wind_speed_10m"] :value wind)
    (smoother :in temp :time 2s :out warmth)
    (smoother :in wind :time 2s :out gusts)

    ;; The tone: 110 Hz at -20 degrees, doubling every 20 degrees.
    (define pitch (mapper warmth (fn (t) (scale-exp t -20 40 110hz 880hz))))
    (sine-osc :freq pitch :out tone)
    (triangle-osc :freq (mapper pitch (fn (p) (* p 2))) :out tone)
    (gain :in tone :amount volume :out mix)

    ;; The wind: pink noise, silent when calm.
    (define blowing (mapper gusts (fn (w) (clamp (scale w 0 60 0 0.8) 0 0.8))))
    (noise :color :pink :out air)
    (gain :in air :amount blowing :out mix)

    (audio-out :in mix :channel 0)
    (audio-out :in mix :channel 1)

    ;; The screen's figures: the temperature and the wind as they glide.
    (osc-out :port :page :address "/temperature" :value (mapper warmth (fn (t) (quantize t 0.1))))
    (osc-out :port :page :address "/wind" :value (mapper gusts (fn (w) (quantize w 0.1))))
    (smoother :in (mapper mix (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "LAKE EFFECT"
        (section "Chicago now" :columns 13)
        (section "Tone" volume (readout "Pitch" (mapper pitch (fn (p) (quantize p 1hz))) :unit :hz))
        (section "Output"
                 (meter "Wind" (mapper blowing (fn (b) (quantize b 0.01))) :max 0.8 :hold 0)
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.5)
                 :columns 6)
        :maker "Chicago weather drone · Open-Meteo")
      :accent "#f2a65a")))
