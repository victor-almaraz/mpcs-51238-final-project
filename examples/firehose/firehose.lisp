;; Bluesky, post by post.
;;
;; web-socket reads Jetstream, a WebSocket that carries every public post
;; on Bluesky as JSON, tens a second. Each post plays a short FM note. The
;; posts carry no number for their length, and a field path reads numbers,
;; not the length of a text, so the note's pitch and duration are drawn at
;; random from a scale and a set of lengths, seeded by the project.
;;
;; Its page, drawn from the layout below, is the FM synthesizer's panel
;; and its reverb pedal: the modulation index, the notes' velocity, the
;; envelope's attack, sustain, and release, the volume, and the reverb's
;; controls and a footswitch that bypasses it; the panel shows the output's
;; level, and the stream's screen (ui/script.js and ui/style.css) the posts
;; a second and the last post's note. Without the page, the knobs keep the
;; values written here.

(module firehose)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((posts  (pipe :event))
        (pitch  (pipe :control))
        (length (pipe :control))
        (notes  (pipe :event))
        (bells  (pipe :audio))
        (out    (pipe :audio))
        (tally  (pipe :control))
        (level  (pipe :control))
        (index    (knob "Index" :max 10 :initial 2 :size :big))
        (velocity (knob "Velocity" :initial 0.5 :unit :percent))
        (attack   (knob "Attack" :min 1ms :max 500ms :initial 2ms :scale :exp))
        (sustain  (knob "Sustain" :initial 0.2 :unit :percent))
        (release  (knob "Release" :min 20ms :max 3s :initial 300ms :scale :exp))
        (volume   (knob "Volume" :max 0db :initial -18db)))
    ;; time_us is in every message, so each post sends an event.
    (web-socket :url "wss://jetstream2.us-east.bsky.network/subscribe?wantedCollections=app.bsky.feed.post"
                :fields ["/time_us"]
                :out posts)
    (sequencer :in posts :values [62 65 67 69 72 74 77 79 81 84] :order :random :out pitch)
    (sequencer :in posts :values [0.06 0.12 0.25 0.5] :order :random :out length)
    (trigger-notes :in posts :pitch pitch :velocity velocity :length length :out notes)
    (play (make fm :voices 10 :ratio 3.5 :index index :attack attack :decay 400ms
                   :sustain sustain :release release :level volume)
          notes bells)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    ;; The screen's figures: the last post's note, and a count of posts
    ;; from 1 to 8 that it turns into posts a second.
    (osc-out :port :page :address "/pitch" :value pitch)
    (sequencer :in posts :values [1 2 3 4 5 6 7 8] :out tally)
    (osc-out :port :page :address "/posts" :value tally)
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "FIREHOSE"
        (section "Stream" :columns 11)
        (section "Operator" index velocity)
        (section "Envelope" attack sustain release)
        (section "Output" volume
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical)))
      (stomp (make reverb :size 0.9 :mix 0.35) bells out)
      :accent "#5aa9ff")))
