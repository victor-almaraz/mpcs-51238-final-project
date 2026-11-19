;; Instruments built from primitives (interpreter.md §7). Each is a class
;; whose voice method builds one voice; play splits the notes among the
;; instrument's voices, sums them, and sets the level once.

(module instruments)

(import "std/voices")

;; Every instrument: its number of voices and the level of a voice at
;; full velocity.
(defclass instrument ()
  ((voices :initarg :voices :initform 8     :accessor voice-count)
   (level  :initarg :level  :initform -12db :accessor level)))

;; (play instrument notes out) plays the notes on the event pipe notes
;; into the audio pipe out, one voice per sounding note.
(defgeneric play (instrument notes out))

;; (voice instrument notes out) builds one voice: it plays the notes on
;; notes, one at a time, into out, at full scale; play applies the level.
(defgeneric voice (instrument notes out))

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
  ((attack  :initarg :attack  :initform 5ms   :accessor attack)
   (decay   :initarg :decay   :initform 300ms :accessor decay)
   (sustain :initarg :sustain :initform 0.5   :accessor sustain)
   (release :initarg :release :initform 300ms :accessor release)))

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
   (cutoff     :initarg :cutoff     :initform 400hz :accessor cutoff)
   (env-amount :initarg :env-amount :initform 3khz  :accessor env-amount)
   (resonance  :initarg :resonance  :initform 0.3   :accessor resonance)))

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
  ((ratio :initarg :ratio :initform 2 :accessor ratio)
   (index :initarg :index :initform 3 :accessor index)))

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
  ((feedback :initarg :feedback :initform 0.99 :accessor feedback)
   (damping  :initarg :damping  :initform 0.3  :accessor damping)))

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
