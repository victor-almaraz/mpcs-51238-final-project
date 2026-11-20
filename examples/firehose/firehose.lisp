;; Bluesky, post by post.
;;
;; web-socket reads Jetstream, a WebSocket that carries every public post
;; on Bluesky as JSON, tens a second. Each post plays a short FM note. The
;; posts carry no number for their length, and a field path reads numbers,
;; not the length of a text, so the note's pitch and duration are drawn at
;; random from a scale and a set of lengths, seeded by the project.

(module firehose)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((posts  (pipe :event))
        (pitch  (pipe :control))
        (length (pipe :control))
        (notes  (pipe :event))
        (bells  (pipe :audio))
        (out    (pipe :audio)))
    ;; time_us is in every message, so each post sends an event.
    (web-socket :url "wss://jetstream2.us-east.bsky.network/subscribe?wantedCollections=app.bsky.feed.post"
                :fields ["/time_us"]
                :out posts)
    (sequencer :in posts :values [62 65 67 69 72 74 77 79 81 84] :order :random :out pitch)
    (sequencer :in posts :values [0.06 0.12 0.25 0.5] :order :random :out length)
    (trigger-notes :in posts :pitch pitch :velocity 0.5 :length length :out notes)
    (play (make fm :voices 10 :ratio 3.5 :index 2 :attack 2ms :decay 400ms :sustain 0.2 :release 300ms
                   :level -18db)
          notes bells)
    (process (make reverb :size 0.9 :mix 0.35) bells out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
