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
;; | Wheel A (CC 1) | Registration: from a soft flute (down) to a full chorus (up) |
;; | Wheel B (CC 2) | The tape loop's feedback: from one repeat (down) to repeats that fade over minutes (up) |

(module main)

(import "std/effects")
(import "organ")
(import "effects")

;; The two wheels' controller numbers.
(define wheel-a 1)
(define wheel-b 2)

;; How far each drawbar's LFO moves its level, 0 to 1.
(define drawbar-drift 0.35)

;; A wheel's value, 0 to 1, smoothed over 30 ms.
(define (wheel notes cc)
  (let ((raw (pipe :control))
        (smooth (pipe :control)))
    (control-change :in notes :cc cc :out raw)
    (smoother :in raw :time 30ms :out smooth)
    smooth))

;; One side's chain: the rotary, the tape loop, and the reverb, from in to out.
(define (side in out horn tape hold)
  (let ((turned (pipe :audio))
        (looped (pipe :audio)))
    (process (make rotary :phase horn) in turned)
    (process (make tape-loop :time tape :feedback hold) turned looped)
    (process (make reverb :size 0.9 :damping 0.3 :mix 0.35) looped out)))

(defpiece main ()
  (let ((notes  (pipe :event))
        (organ  (pipe :audio))
        (left   (pipe :audio))
        (right  (pipe :audio))
        (master (pipe :audio :channels 2)))
    (midi-in :device "" :out notes)
    (let ((morph (wheel notes wheel-a))
          ;; Feedback from 0 to 0.96.
          (hold  (mapper (wheel notes wheel-b) (fn (w) (* w 0.96)))))
      (play (make drawbar-organ :drawbars (drifting-drawbars morph drawbar-drift)) notes organ)
      (side organ left 0 7.5s hold)
      (side organ right 0.5 8.1s hold))
    (pan :in left :position -1 :out master)
    (pan :in right :position 1 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)))
