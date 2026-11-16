;; Polyphony: notes split among voices (primitives.md §7.1.5, §7.8).

(module voices (export voices))

;; Splits the notes on the event pipe `notes` among `count` voices with
;; voice-split, and calls (build voice-notes) once for each voice, with
;; the voice's own event pipe. When every voice is sounding, a new note
;; takes the voice of the oldest. The calls are numbered in voice order,
;; so a voice keeps its node IDs, and its state, when the piece changes.
;; Returns the list of build's results.
(define (voices count notes build)
  (let ((outs (map (fn (i) (pipe :event)) (range count))))
    (voice-split :in notes :out outs)
    (map build outs)))
