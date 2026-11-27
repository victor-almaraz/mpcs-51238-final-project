;; spectral-audioscape: a generative audioscape. One head loops a short
;; FM piano arpeggio from a buffer into a spectral delay of eight bands, each
;; band with its own delay and feedback. The delay's output
;; passes a spectral freeze, which a coin toss engages from time to time,
;; and a reverb.
;;
;; Its page, drawn from the layout below, is a pedalboard of the chain:
;; the loop, with the head's speed; the spectral delay, with the bands' Q
;; and damping; the freeze, with the coin's chance and a footswitch that
;; holds it as the coin does, its LED lit while either holds it; and the
;; reverb, with its mix and tone. A panel under the board holds a feedback
;; slider for each band, the volume, and the level. Without the page, the
;; controls keep the values written here.

(import "std/effects")

;; The arpeggio's length, which the head sweeps once a loop.
(define loop-length 6)

;; The eight bands, two thirds of an octave apart: each band's centre,
;; delay, and feedback, and the label of its feedback slider. The
;; delays, from 0.5 to 5 s, and the feedback, from 0.3 to 0.8, were drawn
;; at random once and are fixed here.
(define bands
  [[250hz 4751ms 0.52 "250"] [400hz 2826ms 0.63 "400"] [630hz 3082ms 0.30 "630"]
   [1000hz 2761ms 0.33 "1k"] [1600hz 2112ms 0.40 "1.6k"] [2500hz 4591ms 0.51 "2.5k"]
   [4000hz 1870ms 0.49 "4k"] [6300hz 3920ms 0.49 "6.3k"]])

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
        (stomp    (footswitch "Hold" :initial 0))
        (frozen   (pipe :control))
        (speed    (knob "Speed" :min (/ 0.25 loop-length) :max (/ 2 loop-length) :initial (/ 1 loop-length)
                        :scale :exp))
        (q        (knob "Q" :min 0.5 :max 10 :initial 2 :scale :exp))
        (darkness (knob "Damping" :initial 0 :unit :percent))
        (odds     (knob "Chance" :initial 0.5 :unit :percent))
        (mix      (knob "Mix" :initial 0.4 :unit :percent))
        (tone     (knob "Tone" :initial 0.2 :unit :percent))
        (volume   (knob "Volume" :max 0db :initial 0.6))
        (feedback (map (fn (band) (slider (nth band 3) :max 0.95 :initial (nth band 2) :unit :percent
                                          :orientation :vertical))
                       bands))
        (level    (pipe :control)))
    ;; The head: a saw from 0 to the arpeggio's end, once a loop.
    (lfo :shape :saw :rate speed :depth (/ loop-length 2) :offset (/ loop-length 2) :out at)
    (buffer-read :buffer arpeggio :position at :out head)
    ;; The spectral delay: each band of the head through a feedback delay
    ;; of its own, summed.
    (map (fn (i)
           (let ((band (nth bands i)) (part (pipe :audio)))
             (svf :in head :mode :band :cutoff (nth band 0) :q q :out part)
             (comb :in part :time (nth band 1) :max-time 5s :feedback (nth feedback i)
                   :damping darkness :out delayed)))
         (range (length bands)))
    ;; Every 16 beats, a coin toss turns the freeze on or off, starting off.
    (metro :every 16beat :out slow)
    (chance :in slow :probability odds :out flip)
    (sequencer :in flip :values [0 1] :out hold)
    ;; Held while the coin or the footswitch holds it.
    (max :in hold :b stomp :out frozen)
    (process (make freeze :hold frozen :fade 2s :size 4096) delayed held)
    (process (make reverb :size 0.9 :mix mix :damping tone) held room)
    ;; The bands' echoes pile up to peaks near full scale: leave headroom.
    (gain :in room :amount volume :out loud)
    (pan :in loud :position 0 :out out)
    (audio-out :in out :channel 0)
    (smoother :in (mapper loud (fn (x) (abs x))) :time 100ms :out level)
    ;; The spectral delay's and the reverb's LEDs follow the level; the
    ;; freeze's is lit while it holds.
    (define shown (mapper level (fn (l) (quantize l 0.01))))
    (page
      (pedalboard
        (pedal "Loop" speed)
        (pedal "Spectral" q darkness (lamp "Level" shown :max 0.3))
        (pedal "Freeze" odds (lamp "Frozen" (mapper frozen (fn (x) (round x)))) stomp)
        (pedal "Reverb" mix tone (lamp "Level" shown :max 0.3)))
      (panel "Spectral bands"
        (section "Feedback" feedback)
        (section "Out" volume (meter "Level" shown :max 0.5 :orientation :vertical))))))
