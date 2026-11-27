;; An organ pad for a MIDI keyboard with two modulation wheels, and a tape
;; loop to play along with.
;;
;; The drawbar organ's nine drawbars drift on slow LFOs of their own. Its
;; sound passes, on each side, a rotary speaker, a tape loop, and a reverb;
;; the two sides' horns turn apart and their tapes differ in length, so the
;; repeats move between the speakers.
;;
;; | Control        | Effect                                                         |
;; |----------------|----------------------------------------------------------------|
;; | Keys           | Six voices; velocity sets each note's level                   |
;; | Wheel A (CC 1) | Registration: from the drawbars as set (a soft flute without the page; down) to a full chorus (up) |
;; | Wheel B (CC 2) | The tape loop's feedback: from one repeat (down) to repeats that fade over minutes (up) |
;;
;; Its page, drawn from the layout below, is the organ's panel above three
;; pedals. The panel holds the nine drawbars, 0 to 8 each, starting at the
;; soft flute; the registration, which morphs from them to the full chorus
;; and which the page and wheel A set together, the higher of the two
;; counting; the drawbars' drift; the attack, the release, and the volume;
;; the output's level; and a keyboard that plays the voices as the MIDI
;; keyboard does. The rotary pedal holds its mix, the tape loop its
;; repeats, set with wheel B as the registration is with wheel A, and its
;; damping, and the reverb its mix and damping; each pedal's footswitch
;; bypasses it. ui/script.js names the registration's and the repeats'
;; units, and ui/style.css colors the drawbars. Without the page, the
;; controls keep the values written here.

(module main)

(import "std/effects")
(import "organ")
(import "effects")

;; The two wheels' controller numbers.
(define wheel-a 1)
(define wheel-b 2)

;; How far each drawbar's LFO moves its level, 0 to 1.
(define drawbar-drift 0.35)

;; The drawbars' labels, from 16' to 1', and their addresses, which the
;; labels' fractions would not spell.
(define drawbar-labels ["16′" "5⅓′" "8′" "4′" "2⅔′" "2′" "1⅗′" "1⅓′" "1′"])
(define drawbar-addresses ["/drawbars/1" "/drawbars/2" "/drawbars/3" "/drawbars/4" "/drawbars/5"
                           "/drawbars/6" "/drawbars/7" "/drawbars/8" "/drawbars/9"])

;; A wheel's value, 0 to 1, smoothed over 30 ms.
(define (wheel notes cc)
  (let ((raw (pipe :control))
        (smooth (pipe :control)))
    (control-change :in notes :cc cc :out raw)
    (smoother :in raw :time 30ms :out smooth)
    smooth))

;; An effect's mix, the knob mix, times the footswitch engaged, which
;; bypasses the effect at 0.
(define (switched mix engaged)
  (mapper mix (fn (m) (* m engaged))))

;; One side's chain: the rotary, the tape loop, and the reverb, from in to
;; out. Pedals holds the effects' controls, which both sides share: the
;; rotary's mix, the tape loop's damping and mix, and the reverb's damping
;; and mix.
(define (side in out horn tape hold pedals)
  (let ((turned (pipe :audio))
        (looped (pipe :audio)))
    (process (make rotary :phase horn :mix (nth pedals 0)) in turned)
    (process (make tape-loop :time tape :feedback hold :damping (nth pedals 1) :mix (nth pedals 2))
             turned looped)
    (process (make reverb :size 0.9 :damping (nth pedals 3) :mix (nth pedals 4)) looped out)))

(defpiece main ()
  (let ((notes  (keys "Keys" :low 36 :high 84 :address "/note"))
        (organ  (pipe :audio))
        (left   (pipe :audio))
        (right  (pipe :audio))
        (master (pipe :audio :channels 2))
        (level  (pipe :control))
        ;; The drawbars, 0 to 8 each, starting at the flute registration.
        (set          (map (fn (k) (drawbar (nth drawbar-labels k) :initial (nth flute k)
                                             :address (nth drawbar-addresses k)))
                           (range (length flute))))
        (registration (knob "Registration" :initial 0 :size :big))
        (drift        (knob "Drift" :initial drawbar-drift :unit :percent))
        (attack       (knob "Attack" :min 5ms :max 5s :initial 400ms :scale :exp))
        (release      (knob "Release" :min 50ms :max 10s :initial 1.8s :scale :exp))
        (volume       (knob "Volume" :max 0.25 :initial -24db))
        (rotary-mix   (knob "Mix" :initial 0.7 :unit :percent))
        (rotary-on    (footswitch "On"))
        (repeats      (knob "Repeats" :initial 0))
        (tape-damping (knob "Damping" :initial 0.3 :unit :percent))
        (tape-mix     (knob "Mix" :initial 1 :unit :percent))
        (tape-on      (footswitch "On"))
        (reverb-mix   (knob "Mix" :initial 0.35 :unit :percent))
        (reverb-damp  (knob "Damping" :initial 0.3 :unit :percent))
        (reverb-on    (footswitch "On")))
    ;; The MIDI keyboard's notes join the page keyboard's.
    (midi-in :device "" :out notes)
    (let ((morph    (mapper (wheel notes wheel-a) (fn (w) (max w registration))))
          ;; Feedback from 0 to 0.96.
          (hold     (mapper (wheel notes wheel-b) (fn (w) (* (max w repeats) 0.96))))
          (pedals   [(switched rotary-mix rotary-on) tape-damping (switched tape-mix tape-on)
                     reverb-damp (switched reverb-mix reverb-on)]))
      (play (make drawbar-organ :drawbars (drifting-drawbars set morph drift)
                  :attack attack :release release :level volume)
            notes organ)
      (side organ left 0 7.5s hold pedals)
      (side organ right 0.5 8.1s hold pedals))
    (pan :in left :position -1 :out master)
    (pan :in right :position 1 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)
    ;; The panel's meter: the left channel's level.
    (smoother :in (mapper left (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "ORGAN PAD"
        (section "Drawbars" set registration drift)
        (section "Envelope" attack release)
        (section "Output" volume
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical))
        notes
        :maker "Nine drifting drawbars")
      (pedal "Rotary" rotary-mix rotary-on)
      (pedal "Tape loop" repeats tape-damping tape-mix tape-on)
      (pedal "Reverb" reverb-mix reverb-damp reverb-on)
      :accent "#e9b44c")))
