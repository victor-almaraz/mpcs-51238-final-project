;; A fixed modular synthesizer, patched from a web page (ui/index.html)
;; and played from a MIDI keyboard.
;;
;; Thirteen modules stand in the rack in this order: the keyboard, an LFO,
;; noise, two envelopes, sample and hold, two VCOs, a mixer, a VCF, a VCA,
;; a delay, and the output. No module is added or removed. Every cable
;; that can exist is built: for each output and each input, a gain from
;; the output into the input whose amount is the page's value at
;; /cable/OUTPUT/INPUT, 1 while the cable is plugged and 0 while it is
;; not. An input hears the sum of its cables. A cable into a later module
;; joins it in the same block; a cable into the same module or an earlier
;; one passes through a feedback pipe, a block later.
;;
;; Gates are events: the keyboard's notes and the LFO's clock reach the
;; envelopes and the sample and hold through `chance` nodes that pass
;; every event at probability 1 and none at 0. A gate from the clock
;; starts an envelope that does not sustain.
;;
;; The page sends each knob as [/MODULE/KNOB, value] in the knob's unit:
;; seconds, hertz, octaves, semitones, the VCF's Q, or a level from 0 to 1.
;; The output's level, before its volume, returns as ["/level", x].

(module rack)

(define ln2 0.6931472)
(define log2e 1.442695)
(define c4 261.63)  ; middle C, 0 octaves on the pitch inputs

;; The value of the page control at address, on a control pipe.
(define (control address)
  (let ((value (pipe :control)))
    (osc-in :port :page :address address :value value)
    value))

;; The knob of a module: /MODULE/KNOB.
(define (knob module name)
  (control (string-append "/" module "/" name)))

;; An audio pipe holding the sum, or the product, of the signals in parts.
(define (sum parts)
  (let ((out (pipe :audio)))
    (add :in parts :out out)
    out))
(define (product parts)
  (let ((out (pipe :audio)))
    (multiply :in parts :out out)
    out))

;; An input jack: its name, its module's position in the rack, the pipe
;; that cables from earlier modules write, and the feedback pipe that
;; cables from its own module and later ones write.
(define (input name position)
  (list name position (pipe :audio) (pipe :audio :feedback #t)))

;; What an input hears: the sum of its cables.
(define (heard jack)
  (sum [(nth jack 2) (nth jack 3)]))

;; An output jack: its name, its module's position, and its pipe.
(define (output name position signal)
  (list name position signal))

;; The cable from output out to input jack.
(define (cable out jack)
  (gain :in (nth out 2)
        :amount (control (string-append "/cable/" (nth out 0) "/" (nth jack 0)))
        :out (if (< (nth out 1) (nth jack 1)) (nth jack 2) (nth jack 3))))

;; The gate cable from gate output out to gate input jack, each a name and
;; an event pipe.
(define (gate-cable out jack)
  (chance :in (nth out 1)
          :probability (control (string-append "/cable/" (nth out 0) "/" (nth jack 0)))
          :out (nth jack 1)))

;; Frequency in hertz from a pitch in octaves above middle C, as one
;; exponential: a frequency, not a level, so not a gain of 261.63 on the
;; path to the output.
(define ln-c4 5.566924)
(define (hertz octaves)
  (mapper octaves (fn (o) (exp (+ (* o ln2) ln-c4)))))

;; A VCO at its octave and tune knobs, plus its pitch input and its FM
;; input scaled by its FM knob, with a pulse whose width its PWM input
;; moves as far as its PWM knob allows.
(define (vco name pitch fm pwm saw pulse tri)
  (let ((freq (hertz (sum [(knob name "octave")
                           (mapper (knob name "tune") (fn (st) (/ st 12)))
                           (heard pitch)
                           (product [(heard fm) (knob name "fm")])]))))
    (saw-osc :freq freq :out saw)
    (pulse-osc :freq freq
               :width (mapper (sum [(knob name "width") (product [(heard pwm) (knob name "pwm")])])
                              (fn (w) (clamp w 0.05 0.95)))
               :out pulse)
    (triangle-osc :freq freq :out tri)))

;; An envelope on its gate jack.
(define (envelope name gate out)
  (adsr :gate gate :attack (knob name "attack") :decay (knob name "decay") :sustain (knob name "sustain")
        :release (knob name "release") :out out))

(defpiece main ()
  (let (;; Input jacks, by module position.
        (sh-in      (input "sh-in" 5))
        (vco1-pitch (input "vco1-pitch" 6))
        (vco1-fm    (input "vco1-fm" 6))
        (vco1-pwm   (input "vco1-pwm" 6))
        (vco2-pitch (input "vco2-pitch" 7))
        (vco2-fm    (input "vco2-fm" 7))
        (vco2-pwm   (input "vco2-pwm" 7))
        (mix-1      (input "mix-1" 8))
        (mix-2      (input "mix-2" 8))
        (mix-3      (input "mix-3" 8))
        (vcf-in     (input "vcf-in" 9))
        (vcf-cv     (input "vcf-cv" 9))
        (vca-in     (input "vca-in" 10))
        (vca-cv     (input "vca-cv" 10))
        (delay-in   (input "delay-in" 11))
        (out-l      (input "out-l" 12))
        (out-r      (input "out-r" 12))
        ;; Gate input jacks.
        (env1-gate  (list "env1-gate" (pipe :event)))
        (env2-gate  (list "env2-gate" (pipe :event)))
        (sh-trig    (list "sh-trig" (pipe :event)))
        ;; Module outputs.
        (notes      (pipe :event))
        (freq       (pipe :control))
        (bend       (pipe :control))
        (wheel      (pipe :control))
        (lfo-tri    (pipe :control))
        (lfo-sqr    (pipe :control))
        (clock      (pipe :event))
        (white      (pipe :audio))
        (pink       (pipe :audio))
        (env1       (pipe :control))
        (env2       (pipe :control))
        (held       (pipe :control))
        (saw1       (pipe :audio))
        (pulse1     (pipe :audio))
        (tri1       (pipe :audio))
        (saw2       (pipe :audio))
        (pulse2     (pipe :audio))
        (tri2       (pipe :audio))
        (mixed      (pipe :audio))
        (low        (pipe :audio))
        (band       (pipe :audio))
        (high       (pipe :audio))
        (amplified  (pipe :audio))
        (echoes     (pipe :audio))
        (delayed    (pipe :audio))
        (stereo     (pipe :audio :channels 2))
        (level      (pipe :control)))
    ;; 0. The keyboard: pitch in octaves above middle C with the pitch
    ;; wheel's two semitones; the modulation wheel; and the notes as a gate.
    (midi-in :device "" :out notes)
    (note-to-freq :in notes :out freq)
    (pitch-bend :in notes :range 2st :out bend)
    (control-change :in notes :cc 1 :out wheel)

    ;; 1. The LFO: a triangle and a square, both from -1 to 1, and a clock
    ;; on the square's rising edges.
    (let ((rate (knob "lfo" "rate")))
      (lfo :shape :triangle :rate rate :out lfo-tri)
      (lfo :shape :square :rate rate :out lfo-sqr))
    (threshold :in lfo-sqr :level 0 :direction :rising :out clock)

    ;; 2. Noise, white and pink.
    (noise :color :white :out white)
    (noise :color :pink :out pink)

    ;; 3, 4. The envelopes.
    (envelope "env1" (nth env1-gate 1) env1)
    (envelope "env2" (nth env2-gate 1) env2)

    ;; 5. Sample and hold.
    (sample-and-hold :trigger (nth sh-trig 1) :in (heard sh-in) :out held)

    ;; 6, 7. The VCOs.
    (vco "vco1" vco1-pitch vco1-fm vco1-pwm saw1 pulse1 tri1)
    (vco "vco2" vco2-pitch vco2-fm vco2-pwm saw2 pulse2 tri2)

    ;; 8. The mixer: three inputs at their levels.
    (gain :in (heard mix-1) :amount (knob "mix" "1") :out mixed)
    (gain :in (heard mix-2) :amount (knob "mix" "2") :out mixed)
    (gain :in (heard mix-3) :amount (knob "mix" "3") :out mixed)

    ;; 9. The VCF: its cutoff knob in hertz, moved by its CV input in octaves
    ;; as far as its CV knob allows, and its Q.
    (svf :in (heard vcf-in)
         :cutoff (mapper (sum [(mapper (knob "vcf" "cutoff") (fn (f) (* (log f) log2e)))
                               (product [(heard vcf-cv) (knob "vcf" "cv")])])
                         (fn (o) (clamp (exp (* o ln2)) 20 20000)))
         :q (knob "vcf" "q")
         :low low :band band :high high)

    ;; 10. The VCA: its input at its level knob plus its CV input, from 0 to 1.
    (multiply :in [(heard vca-in)
                   (mapper (sum [(knob "vca" "level") (heard vca-cv)]) (fn (g) (clamp g 0 1)))]
              :out amplified)

    ;; 11. The delay: echoes from a feedback comb, blended with the input.
    (let ((dry (heard delay-in)))
      (comb :in dry :time (knob "delay" "time") :max-time 1s :feedback (knob "delay" "feedback") :damping 0.2
            :out echoes)
      (crossfade :a dry :b echoes :position (knob "delay" "mix") :law :linear :out delayed))

    ;; 12. The output: left and right at the volume, and the level for the
    ;; page.
    (let ((left (heard out-l))
          (right (heard out-r))
          (volume (knob "out" "volume")))
      (pan :in (product [left volume]) :position -1 :out stereo)
      (pan :in (product [right volume]) :position 1 :out stereo)
      (smoother :in (mapper (sum [left right]) (fn (x) (abs (* x 0.5)))) :time 100ms :out level))
    (audio-out :in stereo :channel 0)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))

    ;; Every cable.
    (let ((outputs [(output "kbd-pitch" 0 (sum [(mapper freq (fn (f) (* (log (/ (max f 8.1758) c4)) log2e)))
                                                (mapper bend (fn (st) (/ st 12)))]))
                    (output "kbd-mod" 0 wheel)
                    (output "lfo-tri" 1 lfo-tri)
                    (output "lfo-sqr" 1 lfo-sqr)
                    (output "noise-white" 2 white)
                    (output "noise-pink" 2 pink)
                    (output "env1-out" 3 env1)
                    (output "env2-out" 4 env2)
                    (output "sh-out" 5 held)
                    (output "vco1-saw" 6 saw1)
                    (output "vco1-pulse" 6 pulse1)
                    (output "vco1-tri" 6 tri1)
                    (output "vco2-saw" 7 saw2)
                    (output "vco2-pulse" 7 pulse2)
                    (output "vco2-tri" 7 tri2)
                    (output "mix-out" 8 mixed)
                    (output "vcf-low" 9 low)
                    (output "vcf-band" 9 band)
                    (output "vcf-high" 9 high)
                    (output "vca-out" 10 amplified)
                    (output "delay-out" 11 delayed)])
          (inputs [sh-in vco1-pitch vco1-fm vco1-pwm vco2-pitch vco2-fm vco2-pwm mix-1 mix-2 mix-3
                   vcf-in vcf-cv vca-in vca-cv delay-in out-l out-r])
          (gates [(list "kbd-gate" notes) (list "lfo-clock" clock)]))
      (map (fn (out) (map (fn (jack) (cable out jack)) inputs)) outputs)
      (map (fn (out) (map (fn (jack) (gate-cable out jack)) [env1-gate env2-gate sh-trig])) gates))))
