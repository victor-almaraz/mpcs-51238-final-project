;; sampler-keys: a sampled piano for any MIDI keyboard. One recording of
;; middle C plays across the keyboard: an octave up plays it at twice the
;; speed. Eight voices; each note's envelope releases over 400 ms.
;;
;; Its page, drawn from the layout below, is the sampler's panel: where in
;; the recording each note starts, the attack and the release, the volume,
;; the output's level, and a keyboard that plays the voices as the MIDI
;; keyboard does; its screen (ui/script.js and ui/style.css) draws the
;; recording and where notes start in it. Without the page, the knobs keep
;; the values written here.

(import "std/instruments")

(defpiece main ()
  (let ((piano (buffer :file "samples/c4.wav"))
        (out     (pipe :audio))
        (level   (pipe :control))
        (start   (knob "Start" :max 1.5s :initial 0s :size :big))
        (attack  (knob "Attack" :min 1ms :max 2s :initial 5ms :scale :exp))
        (release (knob "Release" :min 10ms :max 4s :initial 400ms :scale :exp))
        (volume  (knob "Volume" :max 0db :initial -6db))
        (notes   (keys :low 36 :high 96 :address "/note")))
    ;; The MIDI keyboard's notes join the page keyboard's, as writers to an
    ;; event pipe merge.
    (midi-in :device "" :out notes)
    (play (make sampler :buffer piano :root 60 :voices 8
                :start start :attack attack :release release :level volume)
          notes out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "sampler-keys"
        (section "Sample" (screen "Recording" :span [13 5]) start :columns 17)
        (section "Envelope" attack release)
        (section "Output" volume
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical))
        notes)
      :accent "#6cb4ff")))
