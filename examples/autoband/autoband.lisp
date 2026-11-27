;; autoband: a band of four instruments that plays loops of MIDI files.
;; A plucked bass, a synth pad, a synth lead, and a drum machine each play
;; the clip chosen from a bank of eight, through an effect chain of their
;; own into a master chain, at 112 BPM.
;;
;; Its page, drawn from the layout below, has five tabs. Bass, Pad, Lead,
;; and Drums each hold their instrument's panel and the pedals of its
;; chain, and go to Master. Master holds the master chain, which goes to
;; the output, and the loops: a row for each instrument, a cell for each
;; of its eight slots. A press on a cell starts its clip on the next line
;; of a four-bar grid, the clip before playing until then; a press on the
;; playing cell, or a row's stop button, stops it there. A cell's Load
;; button saves a MIDI file into its slot, under clips/, so the next run
;; finds it. The bass, the pad, and the drums start on their first clip;
;; the lead waits to be started. make-clips.py, in examples/, writes the
;; starter clips. Without the page, the piece plays the first clips with
;; the values written here.

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (tempo 112bpm)
  (let ((bass  (make pluck :feedback 0.995 :damping 0.55 :level -6db))
        (pad   (make subtractive :attack 300ms :release 1.2s :cutoff 700hz :env-amount 1khz :level -20db))
        (lead  (make fm :ratio 3 :index 2 :attack 5ms :release 200ms :level -18db))
        (drums (make drum-kit))
        ;; The loops: a row of eight slots for each instrument's bank, in
        ;; clips/bass/, clips/pad/, clips/lead/, and clips/drums/.
        (bass-notes (clips "Bass" :initial 0))
        (pad-notes  (clips "Pad" :initial 0))
        (lead-notes (clips "Lead"))
        (drum-notes (clips "Drums" :initial 0))
        (bass-out (pipe :audio))
        (pad-out  (pipe :audio))
        (lead-out (pipe :audio))
        (drum-out (pipe :audio))
        (bus (pipe :audio))
        (out (pipe :audio)))
    ;; The panels draw the instruments' knobs before they play.
    (define bass-panel (synth-panel "plucked bass" bass))
    (define pad-panel (synth-panel "synth pad" pad))
    (define lead-panel (synth-panel "synth lead" lead))
    (define drum-panel (synth-panel "drum machine" drums))
    (play bass bass-notes bass-out)
    (play pad pad-notes pad-out)
    (play lead lead-notes lead-out)
    (play drums drum-notes drum-out)
    ;; Each instrument's chain into the bus; the master chain from it.
    (define bass-chain (chain bass-out bus (make overdrive :drive 2 :tone 2khz :level -4db :mix 0.7)))
    (define pad-chain (chain pad-out bus (make chorus) (make reverb :mix 0.4)))
    (define lead-chain (chain lead-out bus (make echo :time 0.75beat :feedback 0.35 :mix 0.3)))
    (define drum-chain (chain drum-out bus (make reverb :size 0.6 :mix 0.12)))
    (define master (chain bus out (make overdrive :drive 1.5 :tone 6khz :level -3db :mix 0.3)))
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)
    (page (tab "Bass" bass-panel bass-chain)
          (tab "Pad" pad-panel pad-chain)
          (tab "Lead" lead-panel lead-chain)
          (tab "Drums" drum-panel drum-chain)
          (tab "Master" (panel "Loops" (section bass-notes pad-notes lead-notes drum-notes)) master))))
