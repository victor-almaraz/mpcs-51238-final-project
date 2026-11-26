;; sampler-keys: a sampled piano for any MIDI keyboard. One recording of
;; middle C plays across the keyboard: an octave up plays it at twice the
;; speed. Eight voices; each note's envelope releases over 400 ms.
;;
;; Its page (ui/index.html) is the sampler's panel: where in the recording
;; each note starts, drawn on a screen of the recording; the attack and the
;; release; the volume; the output's level; and a keyboard that plays the
;; voices as the MIDI keyboard does. Without the page, the knobs keep the
;; values written here.

(import "std/instruments")

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

(defpiece main ()
  (let ((piano (buffer :file "samples/c4.wav"))
        (notes (pipe :event))
        (keys  (pipe :event))
        (out   (pipe :audio))
        (level (pipe :control)))
    (midi-in :device "" :out notes)
    ;; The page's keyboard plays the same notes.
    (osc-in :port :page :out keys)
    (osc-notes :in keys :out notes)
    (play (make sampler :buffer piano :root 60 :voices 8
                :start   (control "/start" 0s)
                :attack  (control "/attack" 5ms)
                :release (control "/release" 400ms)
                :level   (control "/volume" -6db))
          notes out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    ;; The panel's meter.
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
