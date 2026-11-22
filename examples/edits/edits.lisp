;; Wikipedia, edit by edit.
;;
;; web-stream holds open Wikimedia's stream of recent changes: a
;; server-sent event for each edit to any Wikimedia wiki, tens a second.
;; Each edit plucks a note of a pentatonic scale, drawn at random. Its
;; size, the page's new length in bytes, sets the velocity on a log scale:
;; soft for a stub, full for a long article. Events without a length, such
;; as log entries, play nothing.

(module edits)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((edits   (pipe :event))
        (size    (pipe :control))
        (pitch   (pipe :control))
        (notes   (pipe :event))
        (plucked (pipe :audio))
        (out     (pipe :audio)))
    ;; Wikimedia asks clients to name themselves; add a contact to this
    ;; header before sharing a piece that uses the stream.
    (web-stream :url "https://stream.wikimedia.org/v2/stream/recentchange"
                :headers ["User-Agent: espresso-edits-example"]
                :fields ["/length/new"]
                :value size
                :out edits)
    (sequencer :in edits :values [57 60 62 64 67 69 72 74 76 79] :order :random :out pitch)
    (trigger-notes :in edits
                   :pitch pitch
                   :velocity (mapper size (fn (n) (clamp (scale (log (+ n 1)) 4 12 0.15 1) 0.15 1)))
                   :length 120ms
                   :out notes)
    (play (make pluck :voices 12 :level -14db) notes plucked)
    (process (make reverb :size 0.85 :mix 0.3) plucked out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
