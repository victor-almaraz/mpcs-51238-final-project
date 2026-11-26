;; Wikipedia, edit by edit.
;;
;; web-stream holds open Wikimedia's stream of recent changes: a
;; server-sent event for each edit to any Wikimedia wiki, tens a second.
;; Each edit plucks a note of a pentatonic scale, drawn at random. Its
;; size, the page's new length in bytes, sets the velocity on a log scale:
;; soft for a stub, full for a long article. Events without a length, such
;; as log entries, play nothing.
;;
;; Its page (ui/index.html) is the string synthesizer's panel and its
;; reverb pedal: the strings' volume, and the reverb's mix and a footswitch
;; that bypasses it; the panel shows the edits a second, the last edit's
;; size and the velocity it played, and the output's level. Without the
;; page, the knobs keep the values written here.

(module edits)

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
  (let ((edits   (pipe :event))
        (size    (pipe :control))
        (pitch   (pipe :control))
        (notes   (pipe :event))
        (plucked (pipe :audio))
        (out     (pipe :audio))
        (tally   (pipe :control))
        (level   (pipe :control)))
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
    (play (make pluck :voices 12 :level (control "/volume" -14db)) notes plucked)
    (process (make reverb :size 0.85 :mix (engaged (control "/hall/mix" 0.3) "/hall/on")) plucked out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    ;; The panel's displays: the last edit's size, a count of edits from 1
    ;; to 8 that the page turns into edits a second, and the level.
    (osc-out :port :page :address "/size" :value size)
    (sequencer :in edits :values [1 2 3 4 5 6 7 8] :out tally)
    (osc-out :port :page :address "/edits" :value tally)
    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
