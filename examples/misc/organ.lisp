;; A new instrument: a drawbar organ, written as a subclass of the
;; standard library's instrument. Its voice method adds sines at the
;; first four harmonics, at the levels of its drawbars, under the
;; library's amplitude envelope; play, which it inherits, gives it
;; voices. A sequencer plays a chord progression on it.

(module organ)

(import "std/instruments")

(defclass organ (instrument enveloped)
  ((drawbars :initarg :drawbars :initform [1 0.5 0.3 0.2] :accessor drawbars)))

(defmethod voice ((o organ) notes out)
  (let ((freq  (pipe :control))
        (tones (pipe :audio)))
    (note-to-freq :in notes :out freq)
    ;; Harmonic h + 1 of the note, at drawbar h's level.
    (map (fn (h)
           (let ((partial (pipe :audio)))
             (sine-osc :freq (mapper freq (fn (f) (* f (+ h 1)))) :out partial)
             (gain :in partial :amount (nth (drawbars o) h) :out tones)))
         (range 4))
    (amplitude o notes tones out)))

(defpiece main ()
  (let ((clock  (pipe :event))
        (roots  (pipe :event))
        (thirds (pipe :event))
        (fifths (pipe :event))
        (notes  (pipe :event))
        (out    (pipe :audio)))
    ;; A chord every two beats: C, A minor, F, G.
    (metro :every 2beat :out clock)
    (sequencer :in clock :values [60 57 53 55] :events roots)
    (sequencer :in clock :values [64 60 57 59] :events thirds)
    (sequencer :in clock :values [67 64 60 62] :events fifths)
    (trigger-notes :in roots :length 1.8beat :out notes)
    (trigger-notes :in thirds :length 1.8beat :out notes)
    (trigger-notes :in fifths :length 1.8beat :out notes)
    (play (make organ :voices 4 :attack 20ms :decay 100ms :sustain 0.9 :release 150ms :level -14db)
          notes out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
