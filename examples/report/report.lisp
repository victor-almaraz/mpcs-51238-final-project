;; A piece that reports its own loudness to a web endpoint.
;;
;; Three detuned sines swell and fade under an LFO. The level of their sum,
;; its magnitude averaged over 300 ms, goes to web-out every 4 beats, which
;; posts {"loudness": x} to a listener on this machine
;; (examples/README.md has one). The engine does not limit how often a
;; piece sends; the metronome does.

(module report)

(defpiece main ()
  (let ((swell    (pipe :control))
        (tones    (pipe :audio))
        (out      (pipe :audio))
        (loudness (pipe :control))
        (bar      (pipe :event))
        (dropped  (pipe :control)))
    (lfo :rate 0.1hz :depth 0.5 :offset 0.5 :out swell)
    (sine-osc :freq 220hz :out tones)
    (sine-osc :freq 277.5hz :out tones)
    (sine-osc :freq 330.4hz :out tones)
    (gain :in tones :amount (mapper swell (fn (s) (* s 0.2))) :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)

    (smoother :in (mapper out (fn (x) (abs x))) :time 300ms :out loudness)
    (metro :every 4beat :out bar)
    (web-out :url "http://localhost:8080/loudness"
             :fields ["loudness"]
             :in bar
             :value loudness
             :dropped dropped)))
