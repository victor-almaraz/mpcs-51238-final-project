;; Bluesky, post by post.
;;
;; web-socket reads Jetstream, a WebSocket that carries every public post
;; on Bluesky as JSON, tens a second. Each post plays a short FM note. The
;; posts carry no number for their length, and a field path reads numbers,
;; not the length of a text, so the note's pitch and duration are drawn at
;; random from a scale and a set of lengths, seeded by the project.
;;
;; Its page (ui/index.html) is the FM synthesizer's panel and its reverb
;; pedal: the notes' velocity, the modulation index, the envelope's attack,
;; sustain, and release, the volume, and the reverb's mix and a footswitch
;; that bypasses it; the panel shows the posts a second, the last post's
;; note, and the output's level. Without the page, the knobs keep the
;; values written here.

(module firehose)

(import "std/instruments")
(import "std/effects")

;; The page's control at address, at initial until the page sends one.
(define (control address initial)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :initial initial :value value)
    value))

;; An effect's mix, 0 while the page's footswitch at address is off.
(define (engaged mix address)
  (let ((wet (pipe :control)))
    (multiply :in [mix (control address 1)] :out wet)
    wet))

(defpiece main ()
  (let ((posts  (pipe :event))
        (pitch  (pipe :control))
        (length (pipe :control))
        (notes  (pipe :event))
        (bells  (pipe :audio))
        (out    (pipe :audio))
        (tally  (pipe :control))
        (level  (pipe :control)))
    ;; time_us is in every message, so each post sends an event.
    (web-socket :url "wss://jetstream2.us-east.bsky.network/subscribe?wantedCollections=app.bsky.feed.post"
                :fields ["/time_us"]
                :out posts)
    (sequencer :in posts :values [62 65 67 69 72 74 77 79 81 84] :order :random :out pitch)
    (sequencer :in posts :values [0.06 0.12 0.25 0.5] :order :random :out length)
    (trigger-notes :in posts :pitch pitch :velocity (control "/velocity" 0.5) :length length :out notes)
    (play (make fm :voices 10 :ratio 3.5 :index (control "/index" 2)
                   :attack (control "/attack" 2ms) :decay 400ms
                   :sustain (control "/sustain" 0.2) :release (control "/release" 300ms)
                   :level (control "/volume" -18db))
          notes bells)
    (process (make reverb :size 0.9 :mix (engaged (control "/vault/mix" 0.35) "/vault/on")) bells out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    ;; The panel's displays: the last post's note, a count of posts from 1
    ;; to 8 that the page turns into posts a second, and the output's level.
    (osc-out :port :page :address "/pitch" :value pitch)
    (sequencer :in posts :values [1 2 3 4 5 6 7 8] :out tally)
    (osc-out :port :page :address "/posts" :value tally)
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
