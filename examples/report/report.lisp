;; A piece that reports its own loudness to a web endpoint.
;;
;; Three detuned sines swell and fade under an LFO. The level of their sum,
;; its magnitude averaged over 300 ms, goes to web-out every 4 beats, which
;; posts {"loudness": x} to a listener on this machine
;; (examples/README.md has one). The engine does not limit how often a
;; piece sends; the metronome does.
;;
;; Its page, drawn from the layout below, is the synthesizer's panel: the
;; three sines' pitches and the swell's rate, centre, and depth, and the
;; loudness it reports; its screens (ui/script.js and ui/style.css) show
;; the swell's last 20 s, and the last report as it was posted, a light at
;; each report, and the requests dropped. Without the page, the knobs keep
;; the values written here.

(module report)

(defpiece main ()
  (let ((swell    (pipe :control))
        (tones    (pipe :audio))
        (out      (pipe :audio))
        (loudness (pipe :control))
        (bar      (pipe :event))
        (dropped  (pipe :control))
        (tally    (pipe :control))
        (rate     (knob "Rate" :min 0.01hz :max 2hz :initial 0.1hz))
        (centre   (knob "Centre" :initial 0.5 :unit :percent))
        (depth    (knob "Depth" :max 0.5 :initial 0.5 :unit :percent))
        (sine-1   (knob "Sine 1" :min 55hz :max 880hz :initial 220hz))
        (sine-2   (knob "Sine 2" :min 55hz :max 880hz :initial 277.5hz))
        (sine-3   (knob "Sine 3" :min 55hz :max 880hz :initial 330.4hz)))
    (lfo :rate rate :depth depth :offset centre :out swell)
    (sine-osc :freq sine-1 :out tones)
    (sine-osc :freq sine-2 :out tones)
    (sine-osc :freq sine-3 :out tones)
    (gain :in tones :amount (mapper swell (fn (s) (* s 0.2))) :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)

    (smoother :in (mapper out (fn (x) (abs x))) :time 300ms :out loudness)
    (metro :every 4beat :out bar)
    (web-out :url "http://localhost:8080/loudness"
             :fields ["loudness"]
             :in bar
             :value loudness
             :dropped dropped)

    ;; The screens' figures: the swell, a count of reports from 1 to 8 that
    ;; lights the screen's lamp, and the requests dropped.
    (osc-out :port :page :address "/swell" :value (mapper swell (fn (s) (quantize s 0.01))))
    (sequencer :in bar :values [1 2 3 4 5 6 7 8] :out tally)
    (osc-out :port :page :address "/reports" :value tally)
    (osc-out :port :page :address "/dropped" :value dropped)
    (page
      (synth-panel "SELF REPORT"
        (section "Oscillators" sine-1 sine-2 sine-3)
        (section "Swell" rate centre depth :columns 9)
        (section "Report" (meter "Loudness" (mapper loudness (fn (l) (quantize l 0.001))) :max 0.25)
                 :columns 11)
        :maker "Three-sine swell · loudness over HTTP")
      :accent "#8fd16a")))
