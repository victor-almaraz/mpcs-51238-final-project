;; Instruments built from primitives (interpreter.md §7). Each is a class
;; whose voice method builds one voice; play splits the notes among the
;; instrument's voices, sums them, and sets the level once.

(module instruments)

(import "std/voices")
(import "std/modulators")

;; Every instrument: its number of voices and the level of a voice at
;; full velocity, and the modulations modulate records.
(defclass instrument ()
  ((voices      :initarg :voices :initform 8     :accessor voice-count)
   (level       :initarg :level  :initform -12db :accessor level :control (knob :min -40db :max 0db))
   (modulations :initform [])))

;; (play instrument notes out) plays the notes on the event pipe notes
;; into the audio pipe out, one voice per sounding note.
(defgeneric play (instrument notes out))

;; (voice instrument notes out) builds one voice: it plays the notes on
;; notes, one at a time, into out, at full scale; play applies the level.
(defgeneric voice (instrument notes out))

;; The modulations modulate recorded move their slots' knobs, which the
;; page has drawn by now, before the voices are built; the pipe it writes
;; is recorded for the routing a page's tabs show.
(defmethod play :before ((i instrument) notes out)
  (apply-modulations i)
  (route i #f out))

(defmethod play ((i instrument) notes out)
  (let ((sum (pipe :audio)))
    (voices (voice-count i) notes (fn (n) (voice i n sum)))
    (gain :in sum :amount (level i) :out out)))

;; The velocity of each note on notes, from 0 to 1.
(define (note-velocity notes)
  (let ((vel (pipe :control)))
    (velocity :in notes :out vel)
    vel))

;; An envelope's attack, decay, sustain, and release.
(defclass enveloped ()
  ((attack  :initarg :attack  :initform 5ms   :accessor attack :control (knob :min 1ms :max 2s :scale :exp))
   (decay   :initarg :decay   :initform 300ms :accessor decay)
   (sustain :initarg :sustain :initform 0.5   :accessor sustain :control (knob :unit :percent))
   (release :initarg :release :initform 300ms :accessor release :control (knob :min 5ms :max 4s :scale :exp))))

;; The amplitude envelope of a voice of i, an enveloped instrument: gated
;; by notes and scaled by each note's velocity, applied to in.
(define (amplitude i notes in out)
  (adsr :gate notes :in in
        :attack (attack i) :decay (decay i) :sustain (sustain i) :release (release i)
        :gain (note-velocity notes)
        :out out))

;; Subtractive synthesis: a sawtooth or pulse wave through a resonant
;; low-pass ladder filter, which a second envelope opens from cutoff by
;; up to env-amount at each note.
(defclass subtractive (instrument enveloped)
  ((wave       :initarg :wave       :initform :saw  :accessor wave)
   (cutoff     :initarg :cutoff     :initform 400hz :accessor cutoff :control (knob :min 50hz :max 8khz))
   (env-amount :initarg :env-amount :initform 3khz  :accessor env-amount :control (knob :max 8khz :scale :linear))
   (resonance  :initarg :resonance  :initform 0.3   :accessor resonance :control (knob :max 0.95 :unit :percent))
   (decay      :control (knob :min 5ms :max 4s :scale :exp))))

(defmethod voice ((s subtractive) notes out)
  (let ((freq     (pipe :control))
        (osc      (pipe :audio))
        (opening  (pipe :control))
        (filtered (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (if (= (wave s) ':pulse)
        (pulse-osc :freq freq :width 0.4 :out osc)
        (saw-osc :freq freq :out osc))
    (adsr :gate notes :attack 2ms :decay (decay s) :sustain 0.1 :release (release s) :out opening)
    (ladder :in osc
            :cutoff (mapper opening (fn (e) (scale e 0 1 (cutoff s) (+ (cutoff s) (env-amount s)))))
            :resonance (resonance s)
            :out filtered)
    (amplitude s notes filtered out)))

;; Two-operator frequency modulation: a sine modulator at ratio times the
;; note's frequency moves a sine carrier's frequency by up to index times
;; the note's frequency. The modulation depth decays with its own
;; envelope, so the tone darkens as the note sounds.
(defclass fm (instrument enveloped)
  ((ratio :initarg :ratio :initform 2 :accessor ratio :control (knob :min 0.5 :max 8 :step 0.5))
   (index :initarg :index :initform 3 :accessor index :control (knob :max 10))))

(defmethod voice ((f fm) notes out)
  (let ((freq      (pipe :control))
        (modulator (pipe :audio))
        (depth     (pipe :control))
        (deviation (pipe :audio))
        (carrier   (pipe :audio))
        (tone      (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (sine-osc :freq (mapper freq (fn (x) (* x (ratio f)))) :out modulator)
    ;; The depth envelope peaks at index, so the deviation is modulator × depth × freq.
    (adsr :gate notes :attack 1ms :decay (* 2 (decay f)) :sustain 0.3 :release (release f)
          :gain (index f) :out depth)
    (multiply :in [modulator depth freq] :out deviation)
    (add :in [freq deviation] :out carrier)
    (sine-osc :freq carrier :out tone)
    (amplitude f notes tone out)))

;; A plucked string (Karplus-Strong): a burst of noise at each note
;; excites a feedback comb whose delay is the note's period. Feedback sets
;; how long the string rings; damping, how fast its high partials fade.
(defclass pluck (instrument)
  ((feedback :initarg :feedback :initform 0.99 :accessor feedback :control (knob :min 0.9 :max 0.999))
   (damping  :initarg :damping  :initform 0.3  :accessor damping :control (knob :unit :percent))))

(defmethod voice ((p pluck) notes out)
  (let ((freq  (pipe :control))
        (noisy (pipe :audio))
        (burst (pipe :audio)))
    (note-to-freq :in notes :out freq)
    (noise :color :white :out noisy)
    (adsr :gate notes :in noisy :attack 0.5ms :decay 10ms :sustain 0 :release 5ms
          :gain (note-velocity notes)
          :out burst)
    ;; The period of the note, at least 50 ms (20 Hz) before the first note.
    (comb :in burst
          :time (mapper freq (fn (x) (pow (max x 20) -1)))
          :max-time 50ms
          :feedback (feedback p)
          :damping (damping p)
          :out out)))

;; Sample playback (samples.md §7.3): each note plays the buffer from
;; start, at the rate of its interval from root, under the amplitude
;; envelope. Its voices have the buffer's channels.
(defclass sampler (instrument enveloped)
  ((buffer   :initarg :buffer                   :accessor sample-buffer)
   (root     :initarg :root     :initform 60    :accessor root)
   (start    :initarg :start    :initform 0s    :accessor sample-start)
   (loop     :initarg :loop     :initform ':off :accessor loop-mode)
   (sustain  :initform 1)))

;; As instrument's play, with voices and a sum of the sampler's channels.
(defmethod play ((s sampler) notes out)
  (let ((sum (pipe :audio :channels (channel-count (sample-buffer s)))))
    (voices (voice-count s) notes (fn (n) (voice s n sum)))
    (gain :in sum :amount (level s) :out out)))

(defmethod voice ((s sampler) notes out)
  (let ((played (pipe :audio :channels (channel-count (sample-buffer s)))))
    (sample-player :buffer (sample-buffer s) :trigger notes :root (root s)
                   :start (sample-start s) :loop (loop-mode s)
                   :out played)
    (amplitude s notes played out)))

;; A drum machine (canvas.md §7): five voices, each on its General MIDI
;; note, on any channel: a kick (36), a snare (38), a clap (39), and a
;; closed (42) and an open (46) hi-hat, the open one ringing six times as
;; long. Each voice's level and decay, and the kit's tone, which brightens
;; the noise of the snare, the clap, and the hats, are :control slots.
(defclass drum-kit (instrument)
  ((kick-level  :initarg :kick-level  :initform 0db   :accessor kick-level  :control (knob :min -40db :max 6db))
   (kick-decay  :initarg :kick-decay  :initform 400ms :accessor kick-decay  :control (knob :min 50ms :max 1.5s :scale :exp))
   (snare-level :initarg :snare-level :initform -3db  :accessor snare-level :control (knob :min -40db :max 6db))
   (snare-decay :initarg :snare-decay :initform 180ms :accessor snare-decay :control (knob :min 30ms :max 1s :scale :exp))
   (clap-level  :initarg :clap-level  :initform -3db  :accessor clap-level  :control (knob :min -40db :max 6db))
   (clap-decay  :initarg :clap-decay  :initform 200ms :accessor clap-decay  :control (knob :min 30ms :max 1s :scale :exp))
   (hat-level   :initarg :hat-level   :initform -9db  :accessor hat-level   :control (knob :min -40db :max 6db))
   (hat-decay   :initarg :hat-decay   :initform 60ms  :accessor hat-decay   :control (knob :min 10ms :max 500ms :scale :exp))
   (tone        :initarg :tone        :initform 0.5   :accessor tone        :control (knob :unit :percent))
   (level       :initform -6db)))

;; v, a number or a pipe, times k.
(define (drum-times v k)
  (if (pipe? v) (mapper v (fn (x) (* x k))) (* v k)))

;; The kit's tone mapped from low to high, a frequency.
(define (drum-brightness d low high)
  (let ((bright (tone d)))
    (if (pipe? bright) (mapper bright (fn (x) (scale-exp x 0 1 low high))) (scale-exp bright 0 1 low high))))

;; The note-ons and note-offs of note n among notes.
(define (drum-hits notes n)
  (let ((hits (pipe :event)))
    (event-filter :in notes :type :note :low n :high n :out hits)
    hits))

;; in, struck by each hit: an envelope that falls over decay from the
;; hit's velocity, then level, added into out.
(define (drum-hit hits in decay level out)
  (let ((struck (pipe :audio)))
    (adsr :gate hits :in in :attack 0.5ms :decay decay :sustain 0 :release decay
          :gain (note-velocity hits) :out struck)
    (gain :in struck :amount level :out out)))

;; A sine that falls from about 200 Hz to 48 Hz in the first 45 ms.
(define (drum-kick d hits out)
  (let ((sweep (pipe :control)) (body (pipe :audio)))
    (adsr :gate hits :attack 0.5ms :decay 45ms :sustain 0 :release 45ms :out sweep)
    (sine-osc :freq (mapper sweep (fn (e) (+ 48hz (* e 150hz)))) :out body)
    (drum-hit hits body (kick-decay d) (kick-level d) out)))

;; Band-passed noise over a short 185 Hz tone.
(define (drum-snare d hits out)
  (let ((hiss (pipe :audio)) (band (pipe :audio)) (body (pipe :audio)))
    (noise :color :white :out hiss)
    (biquad :mode :band-pass :freq (drum-brightness d 1200hz 4khz) :q 0.8 :in hiss :out band)
    (sine-osc :freq 185hz :out body)
    (drum-hit hits band (snare-decay d) (snare-level d) out)
    (drum-hit hits body (drum-times (snare-decay d) 0.5) (snare-level d) out)))

;; Band-passed noise, lower and narrower than the snare's.
(define (drum-clap d hits out)
  (let ((hiss (pipe :audio)) (band (pipe :audio)))
    (noise :color :white :out hiss)
    (biquad :mode :band-pass :freq (drum-brightness d 900hz 2500hz) :q 1.5 :in hiss :out band)
    (drum-hit hits band (clap-decay d) (clap-level d) out)))

;; High-passed noise that falls over decay.
(define (drum-hat d hits decay out)
  (let ((hiss (pipe :audio)) (high (pipe :audio)))
    (noise :color :white :out hiss)
    (biquad :mode :high-pass :freq (drum-brightness d 5khz 10khz) :in hiss :out high)
    (drum-hit hits high decay (hat-level d) out)))

(defmethod play ((d drum-kit) notes out)
  (let ((sum (pipe :audio)))
    (drum-kick d (drum-hits notes 36) sum)
    (drum-snare d (drum-hits notes 38) sum)
    (drum-clap d (drum-hits notes 39) sum)
    (drum-hat d (drum-hits notes 42) (hat-decay d) sum)
    (drum-hat d (drum-hits notes 46) (drum-times (hat-decay d) 6) sum)
    (gain :in sum :amount (level d) :out out)))
