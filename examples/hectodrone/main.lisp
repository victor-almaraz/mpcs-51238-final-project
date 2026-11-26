;; hectodrone for any MIDI keyboard: four voices of one hundred
;; oscillators, their detuning on the pitch-bend wheel and their filter on
;; the modulation wheel, in a reverb.
;;
;; Its page (ui/index.html) is the synthesizer's panel above a reverb
;; pedal. The panel holds the detuning, which adds to the pitch bend; the
;; filter's cutoff with the modulation wheel down and up, and its
;; resonance; the envelope; the volume; the output's level; and a keyboard
;; that plays the voices as the MIDI keyboard does. The pedal holds the
;; reverb's mix and damping and a footswitch that bypasses it. Without the
;; page, the knobs keep the values written here.

(module main)

(import "std/effects")
(import "hectodrone")

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

(defpiece main ()
  (let ((notes  (pipe :event))
        (keys   (pipe :event))
        (left   (pipe :audio))
        (right  (pipe :audio))
        (wet-l  (pipe :audio))
        (wet-r  (pipe :audio))
        (master (pipe :audio :channels 2))
        (level  (pipe :control))
        ;; The pedal's controls, shared by both sides' reverbs; the
        ;; footswitch at 0 bypasses them.
        (engaged (control "/reverb/on" 1))
        (wet     (mapper (control "/reverb/mix" 0.3) (fn (m) (* m engaged))))
        (damping (control "/reverb/damping" 0.2)))
    (midi-in :device "" :out notes)
    ;; The page's keyboard plays the same notes.
    (osc-in :port :page :out keys)
    (osc-notes :in keys :out notes)
    (play (make hectodrone
                :detune    (control "/detune" 0)
                :darkest   (control "/filter/cutoff" 300hz)
                :brightest (control "/filter/wheel" 12khz)
                :resonance (control "/filter/resonance" 0.2)
                :attack    (control "/attack" 10ms)
                :decay     (control "/decay" 1s)
                :sustain   (control "/sustain" 0.8)
                :release   (control "/release" 400ms)
                :level     (control "/volume" 0db))
          notes [left right])
    (process (make reverb :size 0.9 :damping damping :mix wet) left wet-l)
    (process (make reverb :size 0.9 :damping damping :mix wet) right wet-r)
    (pan :in wet-l :position -1 :out master)
    (pan :in wet-r :position 1 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)
    ;; The panel's meter: the left channel's level.
    (smoother :in (mapper wet-l (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
