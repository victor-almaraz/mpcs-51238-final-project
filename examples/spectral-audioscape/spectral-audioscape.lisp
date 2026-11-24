;; spectral-audioscape: a generative audioscape. One head loops a short
;; FM piano arpeggio from a buffer into a spectral delay of eight bands, each
;; band with a slightly different delay and feedback. The delay's output
;; passes a spectral freeze, which a coin toss engages from time to time,
;; and a reverb.

(import "std/effects")

;; The arpeggio's length, which the head sweeps once a loop.
(define loop-length 6)

;; The eight bands, two thirds of an octave apart: each band's centre,
;; delay, and feedback.
(define bands
  [[250hz 400ms 0.80] [400hz 450ms 0.83] [630hz 500ms 0.86] [1000hz 550ms 0.89]
   [1600hz 600ms 0.82] [2500hz 650ms 0.85] [4000hz 700ms 0.88] [6300hz 750ms 0.81]])

(defpiece main ()
  (let ((arpeggio (buffer :file "samples/arpeggio.wav"))
        (at       (pipe :control))
        (head     (pipe :audio))
        (delayed  (pipe :audio))
        (held     (pipe :audio))
        (room     (pipe :audio))
        (loud     (pipe :audio))
        (out      (pipe :audio :channels 2))
        (slow     (pipe :event))
        (flip     (pipe :event))
        (hold     (pipe :control)))
    ;; The head: a saw from 0 to the arpeggio's end, once a loop.
    (lfo :shape :saw :rate (/ 1 loop-length) :depth (/ loop-length 2) :offset (/ loop-length 2) :out at)
    (buffer-read :buffer arpeggio :position at :out head)
    ;; The spectral delay: each band of the head through a feedback delay
    ;; of its own, summed.
    (map (fn (band)
           (let ((part (pipe :audio)))
             (svf :in head :mode :band :cutoff (nth band 0) :q 2 :out part)
             (comb :in part :time (nth band 1) :max-time 1s :feedback (nth band 2) :damping 0 :out delayed)))
         bands)
    ;; Every 16 beats, a coin toss turns the freeze on or off, starting off.
    (metro :every 16beat :out slow)
    (chance :in slow :probability 0.5 :out flip)
    (sequencer :in flip :values [0 1] :out hold)
    (process (make freeze :hold hold :fade 2s :size 4096) delayed held)
    (process (make reverb :size 0.9 :mix 0.4) held room)
    ;; The bands' echoes pile up to peaks near full scale: leave headroom.
    (gain :in room :amount 0.6 :out loud)
    (pan :in loud :position 0 :out out)
    (audio-out :in out :channel 0)))
