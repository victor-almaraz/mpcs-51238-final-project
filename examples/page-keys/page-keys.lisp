;; A synthesizer played from a web page (ui/index.html).
;;
;; With project.lisp's :page-port 8000, the server serves ui/ at
;; http://localhost:8000/ while the piece plays. The page's keyboard sends
;; ["/note", note, velocity] and its slider ["/cutoff", 0 to 1]; osc-notes
;; turns the notes into note events for eight subtractive voices, and the
;; slider moves a low-pass filter after them from 200 Hz to 12 kHz. The
;; piece's level, averaged over 100 ms and rounded to hundredths, goes back
;; to the page's meter as ["/level", x].

(module page-keys)

(import "std/instruments")
(import "std/effects")

(defpiece main ()
  (let ((messages (pipe :event))
        (notes    (pipe :event))
        (cutoff   (pipe :control))
        (smooth   (pipe :control))
        (synth    (pipe :audio))
        (filtered (pipe :audio))
        (out      (pipe :audio))
        (level    (pipe :control)))
    (osc-in :port :page :out messages)
    (osc-notes :in messages :out notes)
    (osc-in :port :page :address "/cutoff" :value cutoff)
    (smoother :in cutoff :time 30ms :out smooth)

    (play (make subtractive :voices 8 :cutoff 1200hz :env-amount 2khz :resonance 0.25
                            :attack 5ms :release 400ms :level -9db)
          notes synth)
    (svf :in synth :mode :low
         :cutoff (mapper smooth (fn (c) (scale-exp c 0 1 200hz 12khz)))
         :q 0.8
         :out filtered)
    (process (make reverb :size 0.8 :mix 0.2) filtered out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)

    (smoother :in (mapper out (fn (x) (abs x))) :time 100ms :out level)
    (osc-out :port :page :address "/level" :value (mapper level (fn (l) (quantize l 0.01))))))
