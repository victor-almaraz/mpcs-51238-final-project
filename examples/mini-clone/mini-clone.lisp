;; A monophonic synthesizer laid out after the Minimoog Model D, set from
;; its panel in a web page (ui/index.html) and played from a MIDI keyboard:
;; the M6 acceptance piece. The controls and their ranges follow the Model
;; D's operation manual.
;;
;; The panel sends each knob, selector, and switch as [address, value] to
;; the page port, and the piece's level returns to the panel as
;; ["/level", x]. The panel sends every control's value when it connects,
;; so the piece starts where the panel is. The keyboard's keys, pitch
;; wheel, and modulation wheel (controller 1) arrive over MIDI.
;;
;; Pitch and cutoff are summed in octaves. Each oscillator plays the key's
;; pitch, with glide, tune, and the pitch wheel (half an octave either
;; way), in its range (LO, 32', 16', 8', 4', 2') and, for
;; oscillators 2 and 3, its frequency (up to a major sixth either way), in
;; one of six waveforms: triangle, triangle and sawtooth (reverse sawtooth
;; on oscillator 3), sawtooth, square, and wide and narrow rectangles.
;; Oscillator 3 can leave the keyboard, to serve as a slow modulation
;; source; its frequency then sweeps three octaves either way. The modulation, oscillator 3 or noise as the
;; modulation mix sets it, scaled by the mod wheel, moves the first two
;; oscillators' pitch and the filter's cutoff when their switches are on.
;; The mixer sums the oscillators, an external input (device input 1), and
;; white or pink noise into a ladder filter, whose cutoff follows its knob,
;; the keyboard (by a third, two thirds, or all of the key's octaves), and
;; its contour. The filter and loudness contours release over their decay
;; when the decay switch is on, and at once when it is off. The output
;; volume sets the voice and the A-440 reference tone, while the main
;; output and power switches are on.

(module mini-clone)

(define ln2 0.6931472)
(define c4 8.0313842)  ; middle C, 261.63 Hz, in octaves above 1 Hz

;; The value of the panel control at address, on a control pipe.
(define (control address)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :value value)
    value))

;; A control pipe holding the sum of the signals in parts.
(define (sum parts)
  (let ((out (pipe :control)))
    (add :in parts :out out)
    out))

;; A control pipe holding the product of the signals in parts.
(define (product parts)
  (let ((out (pipe :control)))
    (multiply :in parts :out out)
    out))

;; The weight of position k, from 0 to 1, as a value moves past it: 1 at k,
;; falling to 0 one step either side.
(define (near value k)
  (mapper value (fn (v) (clamp (- 1 (abs (- v k))) 0 1))))

;; A range selector's position, 0 to 5, as octaves from 8': LO sits 7
;; octaves down, for slow modulation.
(define (range-octaves position)
  (mapper position (fn (p) (- (- p 3) (* 4 (clamp (- 1 p) 0 1))))))

;; Half of signal.
(define (half signal)
  (mapper signal (fn (w) (* w 0.5))))

;; One oscillator of the bank at the pitch in octaves, in the waveform the
;; selector at position chooses, into shape; third is #t for oscillator 3,
;; whose second waveform is the reverse sawtooth.
(define (oscillator octaves position third shape)
  (let ((freq  (mapper octaves (fn (o) (exp (* o ln2)))))
        (tri   (pipe :audio))
        (saw   (pipe :audio))
        (pulse (pipe :audio)))
    (triangle-osc :freq freq :out tri)
    (saw-osc :freq freq :out saw)
    (pulse-osc :freq freq :width (mapper position (fn (p) (clamp (- 0.5 (* 0.2 (- p 3))) 0.1 0.5))) :out pulse)
    ;; Triangle (0), triangle and sawtooth or reverse sawtooth (1), sawtooth
    ;; (2), square (3), wide rectangle (4), narrow rectangle (5).
    (gain :in tri :amount (if third (near position 0) (sum [(near position 0) (half (near position 1))])) :out shape)
    (gain :in saw
          :amount (sum [(if third (mapper (near position 1) (fn (w) (- w))) (half (near position 1))) (near position 2)])
          :out shape)
    (gain :in pulse :amount (mapper position (fn (p) (clamp (- p 2) 0 1))) :out shape)))

;; A mixer input: source into mixer at the level of the knob at address,
;; while the switch at on is on.
(define (mixer-input source address on mixer)
  (gain :in source :amount (product [(control address) (control on)]) :out mixer))

;; A contour's release: its decay while the decay switch is on, 10 ms
;; while it is off.
(define (release decay switch)
  (sum [(product [decay switch]) (mapper switch (fn (s) (* (- 1 s) 0.01)))]))

(defpiece main ()
  (let ((notes    (pipe :event))
        (note     (pipe :control))
        (bend     (pipe :control))
        (wheel    (pipe :control))
        (slow     (pipe :control))
        (slower   (pipe :control))
        (slowest  (pipe :control))
        (osc1     (pipe :audio))
        (osc2     (pipe :audio))
        (osc3     (pipe :audio))
        (white    (pipe :audio))
        (pink     (pipe :audio))
        (hiss     (pipe :audio))
        (source   (pipe :audio))
        (modulation (pipe :audio))
        (external (pipe :audio))
        (mixer    (pipe :audio))
        (contour  (pipe :control))
        (filtered (pipe :audio))
        (voice    (pipe :audio))
        (tone     (pipe :audio))
        (output   (pipe :audio))
        (out      (pipe :audio :channels 2))
        (level    (pipe :control)))
    ;; The keyboard: its latest note (before the first key, the lowest MIDI
    ;; note, which no envelope has opened), its pitch wheel in semitones,
    ;; and its modulation wheel.
    (midi-in :device "" :out notes)
    (note-to-freq :in notes :out note)
    (pitch-bend :in notes :range 6st :out bend)
    (control-change :in notes :cc 1 :out wheel)

    ;; Glide: the knob blends glides of 60 ms, 250 ms, and 1 s, while the
    ;; glide switch is on.
    (let ((key    (mapper note (fn (f) (* (log (max f 8.1758)) 1.442695))))
          (glide  (mapper (product [(control "/glide") (control "/glide/on")]) (fn (g) (* g 3))))
          (glided (sum [(product [key (near glide 0)]) (product [slow (near glide 1)])
                        (product [slower (near glide 2)]) (product [slowest (near glide 3)])]))
          ;; Tune, two semitones each way, and the pitch wheel, half an octave.
          (pitch  (sum [glided
                        (mapper (control "/tune") (fn (st) (/ st 12)))
                        (mapper bend (fn (st) (/ st 12)))])))
      (smoother :in key :time 60ms :out slow)
      (smoother :in key :time 250ms :out slower)
      (smoother :in key :time 1s :out slowest)

      ;; The modulation: oscillator 3 or noise, as the mix sets it, scaled
      ;; by the mod wheel.
      (crossfade :a osc3 :b hiss :position (control "/modmix") :law :linear :out source)
      (multiply :in [source wheel] :out modulation)

      ;; The oscillator bank: vibrato of up to half an octave on the first two.
      (let ((vibrato (pipe :audio)))
        (multiply :in [modulation (mapper (control "/osc/mod") (fn (s) (* s 0.5)))] :out vibrato)
        (let ((octaves1 (pipe :audio)) (octaves2 (pipe :audio)))
          (add :in [pitch vibrato (range-octaves (control "/osc1/range"))] :out octaves1)
          (add :in [pitch vibrato (range-octaves (control "/osc2/range"))
                    (mapper (control "/osc2/freq") (fn (st) (/ st 12)))]
               :out octaves2)
          (oscillator octaves1 (control "/osc1/wave") #f osc1)
          (oscillator octaves2 (control "/osc2/wave") #f osc2)))
      ;; Oscillator 3 follows the keys, or rests at middle C with its control
      ;; off, where its frequency knob sweeps four times as far.
      (let ((on (control "/osc3/control"))
            (freq (control "/osc3/freq")))
        (oscillator (sum [(product [pitch on]) (mapper on (fn (s) (* (- 1 s) c4)))
                          (range-octaves (control "/osc3/range"))
                          (product [(mapper freq (fn (st) (/ st 12))) on])
                          (product [(mapper freq (fn (st) (/ st 3))) (mapper on (fn (s) (- 1 s)))])])
                    (control "/osc3/wave") #t osc3))

      ;; Noise, white or pink, and the external input.
      (noise :color :white :out white)
      (noise :color :pink :out pink)
      (crossfade :a white :b pink :position (control "/noise/pink") :law :linear :out hiss)
      (audio-in :channel 0 :out external)

      ;; The mixer.
      (mixer-input osc1 "/mix/osc1" "/mix/osc1/on" mixer)
      (mixer-input external "/mix/ext" "/mix/ext/on" mixer)
      (mixer-input osc2 "/mix/osc2" "/mix/osc2/on" mixer)
      (mixer-input hiss "/mix/noise" "/mix/noise/on" mixer)
      (mixer-input osc3 "/mix/osc3" "/mix/osc3/on" mixer)

      ;; The filter: its knob, keyboard control 1 (a third) and 2 (two
      ;; thirds), its contour of up to 5 octaves, and the modulation.
      (let ((decay (control "/filter/decay")) (decaying (control "/decay/on")))
        (adsr :gate notes :attack (control "/filter/attack") :decay decay
              :sustain (control "/filter/sustain") :release (release decay decaying) :out contour)
        (let ((octaves (pipe :audio)) (swept (pipe :audio)))
          ;; Filter modulation: up to two octaves, while its switch is on.
          (multiply :in [modulation (mapper (control "/filter/mod") (fn (s) (* s 2)))] :out swept)
          (add :in [(mapper (control "/filter/cutoff") (fn (k) (+ k 8.7813597)))  ; 440 Hz at 0
                    (product [(mapper pitch (fn (p) (- p c4)))
                              (sum [(mapper (control "/filter/kb1") (fn (s) (/ s 3)))
                                    (mapper (control "/filter/kb2") (fn (s) (/ (* s 2) 3)))])])
                    (product [contour (control "/filter/amount")])
                    swept]
               :out octaves)
          (ladder :in mixer
                  :cutoff (mapper octaves (fn (o) (clamp (exp (* o ln2)) 20 20000)))
                  :resonance (mapper (control "/filter/emphasis") (fn (e) (* e 0.95)))
                  :drive 0.8
                  :out filtered))

        ;; The loudness contour.
        (let ((amp-decay (control "/amp/decay")))
          (adsr :gate notes :in filtered :attack (control "/amp/attack") :decay amp-decay
                :sustain (control "/amp/sustain") :release (release amp-decay decaying)
                :out voice))))

    ;; The A-440 reference tone, and the output volume while the main output
    ;; and the power are on.
    (sine-osc :freq 440hz :out tone)
    (gain :in tone :amount (mapper (control "/a440") (fn (a) (* a 0.4))) :out voice)
    (gain :in voice
          :amount (product [(control "/volume") (control "/main") (mapper (control "/power") (fn (p) (* p 0.5)))])
          :out output)
    (pan :in output :position 0 :out out)
    (audio-out :in out :channel 0)

    ;; The level, for the panel.
    (smoother :in (mapper output (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
