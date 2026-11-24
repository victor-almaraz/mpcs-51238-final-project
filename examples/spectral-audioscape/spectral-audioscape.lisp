;; spectral-audioscape: a generative audioscape. A 20-second stereo ring,
;; preloaded with a short synth melody, is read by three heads that play it
;; at nearly its own speed and drift apart at slow, unrelated rates. Their mix passes a spectral delay,
;; whose random delays a coin toss redraws now and then; a spectral
;; freeze, which another engages from time to time; and a reverb. The
;; result is recorded back into the ring, so the material keeps changing:
;; quiet material grows with each pass, filling the ring past the end of
;; the melody, and the saturation on the way back holds loud material
;; to a level the ring keeps.

(import "std/effects")

(defpiece main ()
  (let ((ring    (buffer :channels 2 :seconds 20s :file "samples/melody.wav"))
        (heads   (pipe :audio))
        (mixed   (pipe :audio))
        (frames  (pipe :event))
        (delayed (pipe :event))
        (held    (pipe :event))
        (wet     (pipe :audio))
        (room    (pipe :audio))
        (clean   (pipe :audio))
        (back    (pipe :audio))
        (out     (pipe :audio :channels 2))
        (clock   (pipe :event))
        (toss    (pipe :event))
        (slow    (pipe :event))
        (flip    (pipe :event))
        (hold    (pipe :control))
        (loud    (pipe :audio)))
    ;; Three heads, a third of the ring apart, each swept through it by its
    ;; own saw: once in 20, 19.4, and 21.3 seconds, a few percent off the
    ;; ring's own speed.
    (map (fn (head)
           (let ((at (pipe :control)))
             (lfo :shape :saw :rate (nth head 0) :phase (nth head 1) :depth 10 :offset 10 :out at)
             (buffer-read :buffer ring :position at :out heads)))
         [[0.05hz 0] [0.0515hz 0.33] [0.047hz 0.67]])
    (gain :in heads :amount 1 :out mixed)
    ;; Every 8 beats, a coin toss redraws the delays; every 16, another
    ;; turns the freeze on or off, starting off.
    (metro :every 8beat :out clock)
    (chance :in clock :probability 0.5 :out toss)
    (metro :every 16beat :out slow)
    (chance :in slow :probability 0.5 :out flip)
    (sequencer :in flip :values [0 1] :out hold)
    (fft :in mixed :size 4096 :overlap 4 :out frames)
    (pv-delay :in frames :time 2s :max-time 6s :shape :random :feedback 0.5 :redraw toss :out delayed)
    (pv-freeze :in delayed :hold hold :fade 2s :out held)
    (ifft :in held :out wet)
    (process (make reverb :size 0.9 :mix 0.4) wet room)
    ;; Nothing in the loop removes DC, which feedback would build up.
    (biquad :in room :mode :high-pass :freq 40hz :out clean)
    ;; Back into the ring through a saturation: a gain of 3 for quiet
    ;; material, so it grows against the 0.3 the ring loses each pass, and
    ;; at most 0.3, so the ring holds at most 1.
    (gain :in (mapper clean (fn (x) (tanh (* x 10)))) :amount 0.3 :out back)
    (buffer-write :buffer ring :in back :keep 0.7)
    (gain :in clean :amount 0.35 :out loud)
    (pan :in loud :position 0 :out out)
    (audio-out :in out :channel 0)))
