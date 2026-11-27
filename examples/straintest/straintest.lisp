;; straintest: the piece of the v2 load test. Eight instruments of eight
;; voices, each through two effects. Each instrument plays a note every
;; quarter beat that lasts two beats, so all eight of its voices sound
;; once the first two beats have passed: 64 voices.
;;
;; Each voice is a stack of detuned layers, as a unison voice of a
;; polyphonic synthesizer is, so that the piece needs more than one
;; processor core. Layers sets the weight of the whole piece.
;;
;; Its page, drawn from the layout below, is the test rig's panel: a strip
;; for each instrument with its velocity and the mix of its two effects,
;; the note its sequencer plays, and the level of the instrument before its
;; effects; and the level of the whole piece. ui/script.js names the notes
;; and flashes a light at each. Two inputs and three outputs an instrument,
;; and nothing per voice, keep the page's cost out of the test. Without the
;; page, the knobs keep the values written here.

(module straintest)

(import "std/instruments")
(import "std/effects")

;; The level of the audio pipe in, smoothed over 100 ms, for a meter.
(define (level-of in)
  (let ((level (pipe :control)))
    (smoother :in (mapper in (fn (x) (abs x))) :time 100ms :out level)
    (mapper level (fn (l) (quantize l 0.01)))))

;; The layers of every voice.
(define layers 8)

;; The frequency ratio of layer i: the layers spread evenly about the
;; note, 0.3% apart.
(define (detune i)
  (+ 1 (* 0.003 (- i (/ (- layers 1) 2)))))

;; The amplitude envelope of a voice of i, as amplitude's, with the
;; velocity shared among the layers.
(define (layered-amplitude i notes in out)
  (adsr :gate notes :in in
        :attack (attack i) :decay (decay i) :sustain (sustain i) :release (release i)
        :gain (mapper (note-velocity notes) (fn (v) (/ v layers)))
        :out out))

;; Each layer of a frequency: f applied to the layer's frequency pipe.
(define (each-layer freq f)
  (map (fn (i) (f (mapper freq (fn (x) (* x (detune i)))))) (range layers)))

;; Subtractive synthesis with a sawtooth or pulse oscillator and a ladder
;; filter for each layer, the filters opened by one envelope.
(defclass stack (subtractive) ())

(defmethod voice ((s stack) notes out)
  (let ((freq     (pipe :control))
        (opening  (pipe :control))
        (filtered (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (adsr :gate notes :attack 2ms :decay (decay s) :sustain 0.1 :release (release s) :out opening)
    (let ((cutoff-freq (mapper opening (fn (e) (scale e 0 1 (cutoff s) (+ (cutoff s) (env-amount s)))))))
      (each-layer freq
        (fn (f)
          (let ((osc (pipe :audio)))
            (if (= (wave s) ':pulse)
                (pulse-osc :freq f :width 0.4 :out osc)
                (saw-osc :freq f :out osc))
            (ladder :in osc :cutoff cutoff-freq :resonance (resonance s) :out filtered)))))
    (layered-amplitude s notes filtered out)))

;; Two-operator frequency modulation with a modulator and a carrier for
;; each layer, their depth set by one envelope.
(defclass fm-stack (fm) ())

(defmethod voice ((f fm-stack) notes out)
  (let ((freq  (pipe :control))
        (depth (pipe :control))
        (tone  (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (adsr :gate notes :attack 1ms :decay (* 2 (decay f)) :sustain 0.3 :release (release f)
          :gain (index f) :out depth)
    (each-layer freq
      (fn (layer)
        (let ((modulator (pipe :audio))
              (deviation (pipe :audio))
              (carrier   (pipe :audio)))
          (sine-osc :freq (mapper layer (fn (x) (* x (ratio f)))) :out modulator)
          (multiply :in [modulator depth layer] :out deviation)
          (add :in [layer deviation] :out carrier)
          (sine-osc :freq carrier :out tone))))
    (layered-amplitude f notes tone out)))

;; Plucked strings: one burst of noise excites a comb for each layer, as
;; the courses of a twelve-string guitar are struck together.
(defclass strings (pluck) ())

(defmethod voice ((p strings) notes out)
  (let ((freq  (pipe :control))
        (noisy (pipe :audio))
        (burst (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (noise :color :white :out noisy)
    (adsr :gate notes :in noisy :attack 0.5ms :decay 10ms :sustain 0 :release 5ms
          :gain (mapper (note-velocity notes) (fn (v) (/ v layers)))
          :out burst)
    (each-layer freq
      (fn (f)
        (comb :in burst
              :time (mapper f (fn (x) (pow (max x 20) -1)))
              :max-time 50ms
              :feedback (feedback p)
              :damping (damping p)
              :out out)))))

;; An instrument playing pitches, a note every quarter beat for two beats,
;; through two effects into out: effects makes them from the strip's Wet
;; knob, which both read. Returns the instrument's strip, a section
;; labelled label.
(define (player label instrument effects pitches out)
  (let ((steps    (pipe :event))
        (pitch    (pipe :control))
        (notes    (pipe :event))
        (dry      (pipe :audio))
        (wet      (pipe :audio))
        (velocity (knob "Velocity" :initial 0.7 :unit :percent))
        (mix      (knob "Wet" :initial 0.5 :unit :percent)))
    (metro :every 0.25beat :out steps)
    (sequencer :in steps :values pitches :out pitch)
    (trigger-notes :in steps :pitch pitch :velocity velocity :length 2beat :out notes)
    (play instrument notes dry)
    (let ((made (effects mix)))
      (process (nth made 0) dry wet)
      (process (nth made 1) wet out))
    (section label
             (readout "Note" pitch :span [5 2])
             (meter "Level" (level-of dry) :max 0.25 :orientation :vertical)
             velocity mix
             :columns 5)))

(defpiece main ()
  (let ((mixed  (pipe :audio))
        (stereo (pipe :audio :channels 2)))
    (define strips
      [(player "1 · Saw stack" (make stack :wave :saw :cutoff 300hz)
               (fn (wet) [(make chorus :mix wet) (make reverb :mix wet)])
               [36 43 48 55 60 67 72 79] mixed)
       (player "2 · Pulse stack" (make stack :wave :pulse :cutoff 600hz)
               (fn (wet) [(make echo :mix wet) (make reverb :size 0.7 :mix wet)])
               [40 47 52 59 64 71 76 83] mixed)
       (player "3 · FM stack" (make fm-stack :ratio 2 :index 3)
               (fn (wet) [(make chorus :rate 0.5hz :mix wet) (make echo :time 0.75beat :mix wet)])
               [43 50 55 62 67 74 79 86] mixed)
       (player "4 · FM stack" (make fm-stack :ratio 3.5 :index 1.5)
               (fn (wet) [(make overdrive :mix wet) (make reverb :size 0.9 :mix wet)])
               [45 52 57 64 69 76 81 88] mixed)
       (player "5 · Strings" (make strings)
               (fn (wet) [(make echo :mix wet) (make chorus :mix wet)])
               [48 55 60 67 72 79 84 91] mixed)
       (player "6 · Strings" (make strings :feedback 0.995 :damping 0.5)
               (fn (wet) [(make reverb :mix wet) (make overdrive :mix wet)])
               [50 57 62 69 74 81 86 93] mixed)
       (player "7 · Saw stack" (make stack :wave :saw :cutoff 1200hz :resonance 0.6)
               (fn (wet) [(make overdrive :mix wet) (make echo :time 0.5beat :mix wet)])
               [52 59 64 71 76 83 88 95] mixed)
       (player "8 · FM stack" (make fm-stack :ratio 1.5 :index 5)
               (fn (wet) [(make reverb :size 0.8 :mix wet) (make chorus :rate 1.2hz :mix wet)])
               [55 62 67 74 79 86 91 98] mixed)])
    (pan :in (mapper mixed (fn (x) (* x 0.25))) :position 0 :out stereo)
    (audio-out :in stereo :channel 0)
    (page
      (synth-panel "straintest"
        strips
        (section "Master" (meter "Level" (level-of stereo) :max 0.5 :orientation :vertical)))
      :accent "#9ad94f")))
