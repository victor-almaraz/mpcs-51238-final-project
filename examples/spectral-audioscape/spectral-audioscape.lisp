;; spectral-audioscape: a generative audioscape. One head loops a short
;; FM piano arpeggio from a buffer into a spectral delay of eight bands, each
;; band with its own delay and feedback. The delay's output
;; passes a spectral freeze, which a coin toss engages from time to time,
;; and a reverb.

(import "std/effects")

;; The arpeggio's length, which the head sweeps once a loop.
(define loop-length 6)

;; The eight bands, two thirds of an octave apart: each band's centre,
;; delay, and feedback. The delays, from 0.5 to 5 s, and the feedback,
;; from 0.3 to 0.8, were drawn at random once and are fixed here.
(define bands
  [[250hz 4751ms 0.52] [400hz 2826ms 0.63] [630hz 3082ms 0.30] [1000hz 2761ms 0.33]
   [1600hz 2112ms 0.40] [2500hz 4591ms 0.51] [4000hz 1870ms 0.49] [6300hz 3920ms 0.49]])

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
             (comb :in part :time (nth band 1) :max-time 5s :feedback (nth band 2) :damping 0 :out delayed)))
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
