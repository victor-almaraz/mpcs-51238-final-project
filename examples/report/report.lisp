;; A piece that reports its own loudness to a web endpoint.
;;
;; Three detuned sines swell and fade under an LFO. The level of their sum,
;; its magnitude averaged over 300 ms, goes to web-out every 4 beats, which
;; posts {"loudness": x} to a listener on this machine
;; (examples/README.md has one). The engine does not limit how often a
;; piece sends; the metronome does.
;;
;; Its page (ui/index.html) is the synthesizer's panel: the three sines'
;; pitches and the swell's rate, centre, and depth; the panel shows the
;; swell, the loudness it reports, a light at each report, and the
;; requests dropped. Without the page, the knobs keep the values written
;; here.

(module report)

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

(defpiece main ()
  (let ((swell    (pipe :control))
        (tones    (pipe :audio))
        (out      (pipe :audio))
        (loudness (pipe :control))
        (bar      (pipe :event))
        (dropped  (pipe :control))
        (tally    (pipe :control)))
    (lfo :rate (control "/swell/rate" 0.1hz) :depth (control "/swell/depth" 0.5)
         :offset (control "/swell/centre" 0.5) :out swell)
    (sine-osc :freq (control "/sine/1" 220hz) :out tones)
    (sine-osc :freq (control "/sine/2" 277.5hz) :out tones)
    (sine-osc :freq (control "/sine/3" 330.4hz) :out tones)
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

    ;; The panel's displays: the swell, the loudness, a count of reports
    ;; from 1 to 8 that lights the page's lamp, and the requests dropped.
    (osc-out :port :page :address "/swell" :value (mapper swell (fn (s) (quantize s 0.01))))
    (osc-out :port :page :address "/loudness" :value (mapper loudness (fn (l) (quantize l 0.001))))
    (sequencer :in bar :values [1 2 3 4 5 6 7 8] :out tally)
    (osc-out :port :page :address "/reports" :value tally)
    (osc-out :port :page :address "/dropped" :value dropped)))
