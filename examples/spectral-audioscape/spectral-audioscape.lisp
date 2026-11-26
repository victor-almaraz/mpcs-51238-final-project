;; spectral-audioscape: a generative audioscape. One head loops a short
;; FM piano arpeggio from a buffer into a spectral delay of eight bands, each
;; band with its own delay and feedback. The delay's output
;; passes a spectral freeze, which a coin toss engages from time to time,
;; and a reverb.
;;
;; Its page (ui/index.html) is a pedalboard of the chain: the loop, with
;; the head's speed; the spectral delay, with a feedback slider for each
;; band, the bands' Q, and their damping; the freeze, with the coin's
;; chance and a footswitch that holds it as the coin does, its LED lit
;; while either holds it; the reverb, with its mix and tone; and the
;; volume and the level. Without the page, the knobs keep the values
;; written here.

(import "std/effects")

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

;; The arpeggio's length, which the head sweeps once a loop.
(define loop-length 6)

;; The eight bands, two thirds of an octave apart: each band's centre,
;; delay, and feedback, and the page's address of its feedback. The
;; delays, from 0.5 to 5 s, and the feedback, from 0.3 to 0.8, were drawn
;; at random once and are fixed here.
(define bands
  [[250hz 4751ms 0.52 "/band/250"] [400hz 2826ms 0.63 "/band/400"] [630hz 3082ms 0.30 "/band/630"]
   [1000hz 2761ms 0.33 "/band/1000"] [1600hz 2112ms 0.40 "/band/1600"] [2500hz 4591ms 0.51 "/band/2500"]
   [4000hz 1870ms 0.49 "/band/4000"] [6300hz 3920ms 0.49 "/band/6300"]])

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
        (hold     (pipe :control))
        (stomp    (control "/freeze" 0))
        (frozen   (pipe :control))
        (q        (control "/q" 2))
        (darkness (control "/damping" 0))
        (level    (pipe :control)))
    ;; The head: a saw from 0 to the arpeggio's end, once a loop.
    (lfo :shape :saw :rate (control "/speed" (/ 1 loop-length)) :depth (/ loop-length 2) :offset (/ loop-length 2)
         :out at)
    (buffer-read :buffer arpeggio :position at :out head)
    ;; The spectral delay: each band of the head through a feedback delay
    ;; of its own, summed.
    (map (fn (band)
           (let ((part (pipe :audio)))
             (svf :in head :mode :band :cutoff (nth band 0) :q q :out part)
             (comb :in part :time (nth band 1) :max-time 5s :feedback (control (nth band 3) (nth band 2))
                   :damping darkness :out delayed)))
         bands)
    ;; Every 16 beats, a coin toss turns the freeze on or off, starting off.
    (metro :every 16beat :out slow)
    (chance :in slow :probability (control "/chance" 0.5) :out flip)
    (sequencer :in flip :values [0 1] :out hold)
    ;; Held while the coin or the footswitch holds it.
    (max :in hold :b stomp :out frozen)
    (process (make freeze :hold frozen :fade 2s :size 4096) delayed held)
    (process (make reverb :size 0.9 :mix (control "/reverb/mix" 0.4) :damping (control "/reverb/tone" 0.2))
             held room)
    ;; The bands' echoes pile up to peaks near full scale: leave headroom.
    (gain :in room :amount (control "/volume" 0.6) :out loud)
    (pan :in loud :position 0 :out out)
    (audio-out :in out :channel 0)
    ;; The board's displays: the freeze's LED, and the level.
    (osc-out :port :page :address "/frozen" :value (mapper frozen (fn (x) (round x))))
    (smoother :in (mapper loud (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
