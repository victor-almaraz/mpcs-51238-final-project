;; hectodrone for any MIDI keyboard: four voices of one hundred
;; oscillators, their detuning on the pitch-bend wheel and their filter on
;; the modulation wheel, in a reverb.

(module main)

(import "std/effects")
(import "hectodrone")

(defpiece main ()
  (let ((notes  (pipe :event))
        (left   (pipe :audio))
        (right  (pipe :audio))
        (wet-l  (pipe :audio))
        (wet-r  (pipe :audio))
        (master (pipe :audio :channels 2)))
    (midi-in :device "" :out notes)
    (play (make hectodrone) notes [left right])
    (process (make reverb :size 0.9 :mix 0.3) left wet-l)
    (process (make reverb :size 0.9 :mix 0.3) right wet-r)
    (pan :in wet-l :position -1 :out master)
    (pan :in wet-r :position 1 :out master)
    (audio-out :in master :channel 0 :ceiling -1db)))
