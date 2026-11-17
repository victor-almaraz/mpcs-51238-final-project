;; A monophonic synthesizer played from any MIDI keyboard.
;;
;; Two oscillators glide to each note and follow pitch bend (2 semitones).
;; A resonant ladder filter takes its cutoff from controller 74 plus a
;; filter envelope, and its resonance from controller 71. An amplitude
;; envelope is gated by the keys.

(module monosynth)

(defpiece main ()
  (let ((notes      (pipe :event))
        (pitch      (pipe :control))
        (bend       (pipe :control))
        (freq       (pipe :control))
        (glide      (pipe :audio))
        (detuned    (pipe :audio))
        (osc        (pipe :audio))
        (brightness (pipe :control))
        (resonance  (pipe :control))
        (filter-env (pipe :control))
        (cutoff     (pipe :control))
        (filtered   (pipe :audio))
        (voice      (pipe :audio))
        (stereo     (pipe :audio :channels 2)))
    (midi-in :device "" :out notes)

    ;; Pitch: the latest note, bent by 2^(semitones / 12), with glide.
    (note-to-freq :in notes :out pitch)
    (pitch-bend :in notes :range 2st :out bend)
    (multiply :in [pitch (mapper bend (fn (st) (exp (* st 0.0577623))))] :out freq)
    (smoother :in freq :time 40ms :out glide)

    ;; Oscillators: a sawtooth and a slightly sharp pulse.
    (saw-osc :freq glide :out osc)
    (gain :in glide :amount 1.006 :out detuned)
    (pulse-osc :freq detuned :width 0.35 :out osc)

    ;; Filter: controller 74 sets the base cutoff; the envelope opens it.
    (control-change :in notes :cc 74 :out brightness)
    (control-change :in notes :cc 71 :out resonance)
    (adsr :gate notes :attack 2ms :decay 250ms :sustain 0.2 :release 200ms :out filter-env)
    (add :in [(mapper brightness (fn (x) (scale-exp x 0 1 300hz 8khz)))
              (mapper filter-env (fn (e) (scale e 0 1 0hz 3khz)))]
         :out cutoff)
    (ladder :in osc
            :cutoff cutoff
            :resonance (mapper resonance (fn (r) (scale r 0 1 0 0.9)))
            :drive 0.7
            :out filtered)

    ;; Amplitude.
    (adsr :gate notes :in filtered :attack 4ms :decay 300ms :sustain 0.6 :release 250ms
          :gain -10db :out voice)
    (pan :in voice :position 0 :out stereo)
    (audio-out :in stereo :channel 0)))
