;; Wikipedia, edit by edit.
;;
;; web-stream holds open Wikimedia's stream of recent changes: a
;; server-sent event for each edit to any Wikimedia wiki, tens a second.
;; Each edit plucks a note of a pentatonic scale, drawn at random. Its
;; size, the page's new length in bytes, sets the velocity on a log scale:
;; soft for a stub, full for a long article. Events without a length, such
;; as log entries, play nothing.
;;
;; Its page, drawn from the layout below, is the string synthesizer's
;; panel and its reverb pedal: the strings' volume, and the reverb's mix and
;; a footswitch that bypasses it; the panel shows the velocity the last
;; edit played and the output's level, and the stream's screen
;; (ui/script.js and ui/style.css) the edits a second and the last edit's
;; size. Without the page, the knobs keep the values written here.

(module edits)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((edits   (pipe :event))
        (size    (pipe :control))
        (pitch   (pipe :control))
        (notes   (pipe :event))
        (plucked (pipe :audio))
        (out     (pipe :audio))
        (tally   (pipe :control))
        (level   (pipe :control))
        (volume  (knob "Volume" :max 0db :initial -14db :size :big)))
    ;; Wikimedia asks clients to name themselves; add a contact to this
    ;; header before sharing a piece that uses the stream.
    (web-stream :url "https://stream.wikimedia.org/v2/stream/recentchange"
                :headers ["User-Agent: espresso-edits-example"]
                :fields ["/length/new"]
                :value size
                :out edits)
    (sequencer :in edits :values [57 60 62 64 67 69 72 74 76 79] :order :random :out pitch)
    ;; The velocity: the size's log, from 4 to 12, over 0.15 to 1.
    (define velocity (mapper size (fn (n) (clamp (scale (log (+ n 1)) 4 12 0.15 1) 0.15 1))))
    (trigger-notes :in edits
                   :pitch pitch
                   :velocity velocity
                   :length 120ms
                   :out notes)
    (play (make pluck :voices 12 :level volume) notes plucked)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    ;; The screen's figures: the last edit's size, and a count of edits from
    ;; 1 to 8 that it turns into edits a second.
    (osc-out :port :page :address "/size" :value size)
    (sequencer :in edits :values [1 2 3 4 5 6 7 8] :out tally)
    (osc-out :port :page :address "/edits" :value tally)
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (page
      (synth-panel "RECENT CHANGES"
        (section "Stream" :columns 11)
        (section "Strings" volume (meter "Velocity" velocity :hold 0))
        (section "Output"
                 (meter "Level" (mapper level (fn (l) (quantize l 0.01))) :max 0.25 :orientation :vertical))
        :maker "Twelve-string pluck · Wikimedia stream")
      (stomp (make reverb :size 0.85 :mix 0.3) plucked out)
      :accent "#b48cf2")))
