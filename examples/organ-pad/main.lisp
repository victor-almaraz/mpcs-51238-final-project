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
;; Its page (ui/index.html) is the organ's panel above three pedals. The
;; panel holds the nine drawbars, 0 to 8 each, starting at the soft flute;
;; the registration, which morphs from them to the full chorus and which
;; the page and wheel A set together, the higher of the two counting; the
;; drawbars' drift; the attack, the release, and the volume; the output's
;; level; and a keyboard that plays the voices as the MIDI
;; keyboard does. The rotary pedal holds its mix, the tape loop its repeats,
;; set with wheel B as the registration is with wheel A, and its damping,
;; and the reverb its mix and damping; each pedal's footswitch bypasses it.
;; Without the page, the knobs keep the values written here.

(module main)

(import "std/effects")
(import "organ")
(import "effects")

;; The two wheels' controller numbers.
(define wheel-a 1)
(define wheel-b 2)

;; How far each drawbar's LFO moves its level, 0 to 1.
(define drawbar-drift 0.35)

;; The page's addresses of the drawbars, from 16' to 1'.
(define drawbar-addresses ["/drawbar/1" "/drawbar/2" "/drawbar/3" "/drawbar/4" "/drawbar/5"
                           "/drawbar/6" "/drawbar/7" "/drawbar/8" "/drawbar/9"])

;; A wheel's value, 0 to 1, smoothed over 30 ms.
(define (wheel notes cc)
  (let ((raw (pipe :control))
        (smooth (pipe :control)))
    (control-change :in notes :cc cc :out raw)
    (smoother :in raw :time 30ms :out smooth)
    smooth))

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

;; An effect's mix, the page's control at address, times its footswitch
;; at address/on, which bypasses the effect at 0.
(define (pedal-mix address initial)
  (let ((engaged (control (string-append address "/on") 1)))
    (mapper (control (string-append address "/mix") initial) (fn (m) (* m engaged)))))

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
  (let ((notes  (pipe :event))
        (keys   (pipe :event))
        (organ  (pipe :audio))
        (left   (pipe :audio))
        (right  (pipe :audio))
        (master (pipe :audio :channels 2))
        (level  (pipe :control)))
    (midi-in :device "" :out notes)
    ;; The page's keyboard plays the same notes.
    (osc-in :port :page :out keys)
    (osc-notes :in keys :out notes)
    (let ((registration (control "/registration" 0))
          (repeats      (control "/tape/repeats" 0))
          (morph        (mapper (wheel notes wheel-a) (fn (w) (max w registration))))
          ;; Feedback from 0 to 0.96.
          (hold         (mapper (wheel notes wheel-b) (fn (w) (* (max w repeats) 0.96))))
          ;; The page's drawbars, 0 to 8 each, starting at the flute registration.
          (set          (map (fn (k) (control (nth drawbar-addresses k) (nth flute k))) (range (length flute))))
          (drawbars     (drifting-drawbars set morph (control "/drift" drawbar-drift)))
          (pedals       [(pedal-mix "/rotary" 0.7)
                         (control "/tape/damping" 0.3)
                         (pedal-mix "/tape" 1)
                         (control "/reverb/damping" 0.3)
                         (pedal-mix "/reverb" 0.35)]))
      (play (make drawbar-organ :drawbars drawbars
                  :attack  (control "/attack" 400ms)
                  :release (control "/release" 1.8s)
                  :level   (control "/volume" -24db))
            notes organ)
      (side organ left 0 7.5s hold pedals)
      (side organ right 0.5 8.1s hold pedals))
    (pan :in left :position -1 :out master)
    (pan :in right :position 1 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)
    ;; The panel's meter: the left channel's level.
    (smoother :in (mapper left (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
