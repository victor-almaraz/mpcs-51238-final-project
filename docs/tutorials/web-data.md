# Live web data

This tutorial builds a piece that plays the weather: a tone whose pitch follows the temperature in Chicago, a noise whose level follows the wind, a page that shows both, and, last, plucked notes driven by a stream of Wikipedia edits. It assumes the project and page of [A first piece](../guide/first-piece.md) and a network connection.

## Polling an endpoint

Open-Meteo answers a forecast request with a JSON document, and its current conditions need no API key. Create a folder `weather` holding `main.lisp`:

```lisp
;; The temperature in Chicago as the pitch of a sine.

(module main)

(define forecast
  "https://api.open-meteo.com/v1/forecast?latitude=41.88&longitude=-87.63&current=temperature_2m,wind_speed_10m")

(defpiece main ()
  (let ((temp   (pipe :control))
        (warmth (pipe :control))
        (tone   (pipe :audio))
        (out    (pipe :audio)))
    (web-in :url forecast :every 60s :fields ["/current/temperature_2m"] :value temp)
    (smoother :in temp :time 2s :out warmth)
    (sine-osc :freq (mapper warmth (fn (t) (scale-exp t -20 40 110hz 880hz))) :out tone)
    (gain :in tone :amount -18db :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
```

- `web-in` requests `forecast` when the piece starts and then every 60 s of wall-clock time, counted from the start of the previous request. A request that falls due while the previous one runs is skipped. `:every` takes a time from 100 ms to 3600 s; a beat value is an error.
- `:fields` holds JSON Pointers into the response. Field 0, `/current/temperature_2m`, sets channel 0 of `temp`, the control pipe on `:value`. A field that is absent, or holds no number, leaves the pipe unchanged.
- `smoother` follows `temp` with an exponential curve whose time constant is `:time`, so a new reading moves the pitch over seconds instead of jumping to it.
- The mapper turns degrees Celsius into a frequency: `scale-exp` maps -20 to 40 degrees onto 110 Hz to 880 Hz, an octave for every 20 degrees.

Until the first response arrives, `temp` holds 0, and the tone plays 220 Hz. A request that fails is reported in the console as `server: <what failed>`, at most once a second for each node, with the count of failures since the last report. An offline render makes no request, so `temp` keeps its initial value there.

## A second field

The same document carries the wind speed in km/h. Add a second `web-in` for it, and a pink noise whose level follows it:

```lisp
;; The temperature in Chicago as the pitch of a sine, the wind as a noise.

(module main)

(define forecast
  "https://api.open-meteo.com/v1/forecast?latitude=41.88&longitude=-87.63&current=temperature_2m,wind_speed_10m")

(defpiece main ()
  (let ((temp   (pipe :control))
        (wind   (pipe :control))
        (warmth (pipe :control))
        (gusts  (pipe :control))
        (tone   (pipe :audio))
        (air    (pipe :audio))
        (mix    (pipe :audio))
        (out    (pipe :audio)))
    (web-in :url forecast :every 60s :fields ["/current/temperature_2m"] :value temp)
    (web-in :url forecast :every 60s :fields ["/current/wind_speed_10m"] :value wind)
    (smoother :in temp :time 2s :out warmth)
    (smoother :in wind :time 2s :out gusts)

    ;; The tone: 110 Hz at -20 degrees, doubling every 20 degrees.
    (sine-osc :freq (mapper warmth (fn (t) (scale-exp t -20 40 110hz 880hz))) :out tone)
    (gain :in tone :amount -6db :out mix)

    ;; The wind: pink noise, silent when calm, 0.8 at 60 km/h and above.
    (noise :color :pink :out air)
    (gain :in air :amount (mapper gusts (fn (w) (clamp (scale w 0 60 0 0.8) 0 0.8))) :out mix)

    (gain :in mix :amount -12db :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)))
```

The two `web-in` nodes request one URL with the same headers and interval, so they share each request: the piece makes one request a minute, and each node reads its own field from the response. A single node with two fields and a two-channel pipe reads the same values: `(pipe :control :channels 2)` and `:fields ["/current/temperature_2m" "/current/wind_speed_10m"]`.

The noise's mapper scales 0 to 60 km/h onto a linear amplitude of 0 to 0.8, and `clamp` holds the result within that range, so a wind above 60 km/h plays at 0.8. Until the first response arrives, `wind` holds 0 and the noise is silent. Both sounds write `mix`, an audio pipe whose writers sum, and a last `gain` sets the level of the sum.

## A page

A page shows the readings as they arrive and holds the piece's volume. A project has a page when its `project.lisp` names a port for it. Add `project.lisp` to `weather`:

```lisp
(project :name "weather" :page-port 8000)
```

and replace `main.lisp` with:

```lisp
;; The weather in Chicago as a tone and a wind, with its figures on a page.

(module main)

(define forecast
  "https://api.open-meteo.com/v1/forecast?latitude=41.88&longitude=-87.63&current=temperature_2m,wind_speed_10m")

(defpiece main ()
  (let ((temp   (pipe :control))
        (wind   (pipe :control))
        (warmth (pipe :control))
        (gusts  (pipe :control))
        (tone   (pipe :audio))
        (air    (pipe :audio))
        (mix    (pipe :audio))
        (out    (pipe :audio))
        (volume (knob "Volume" :min -60db :max 0db :initial -18db)))
    (web-in :url forecast :every 60s :fields ["/current/temperature_2m"] :value temp)
    (web-in :url forecast :every 60s :fields ["/current/wind_speed_10m"] :value wind)
    (smoother :in temp :time 2s :out warmth)
    (smoother :in wind :time 2s :out gusts)

    (define pitch (mapper warmth (fn (t) (scale-exp t -20 40 110hz 880hz))))
    (sine-osc :freq pitch :out tone)
    (gain :in tone :amount -6db :out mix)

    (define blowing (mapper gusts (fn (w) (clamp (scale w 0 60 0 0.8) 0 0.8))))
    (noise :color :pink :out air)
    (gain :in air :amount blowing :out mix)

    (gain :in mix :amount volume :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)

    (page
      (panel "WEATHER"
        (section "Chicago"
                 (readout "Temperature" (mapper warmth (fn (t) (quantize t 0.1))))
                 (readout "Wind" (mapper gusts (fn (w) (quantize w 0.1)))))
        (section "Sound"
                 (readout "Pitch" (mapper pitch (fn (p) (quantize p 1hz))) :unit :hz)
                 (meter "Air" blowing :max 0.8 :hold 0))
        (section "Output" volume)))))
```

- `knob` adds a volume control to the page and returns a control pipe, which takes the place of the constant `-12db`. Until the page sends a value, the pipe holds `:initial`.
- `define` binds the two mappers, `pitch` and `blowing`, so the sound and the displays read the same pipes.
- `readout` shows a pipe as text, and `meter` as a bar; each adds the `osc-out` that sends its pipe to the page. `quantize` rounds the temperature and the wind to a tenth and the pitch to 1 Hz before they are shown. `:unit :hz` writes the pitch in Hz or kHz; without a unit, a readout shows a number of 3 significant digits. `:hold 0` makes the meter follow the level without a peak line.
- `page` lays out one panel of three sections. Each control and display takes its address from its label under its section's: `/chicago/temperature`, `/chicago/wind`, `/sound/pitch`, `/sound/air`, and `/output/volume`.

Play the piece. The page is at `http://localhost:8000/`, and the GUI editor shows it in a tab, "Page · weather".

## Event streams

A stream delivers data as it happens, instead of on a schedule. `web-stream` holds a server-sent event stream open and reads each message as a JSON document. Wikimedia's recent changes stream sends a message for each edit to any Wikimedia wiki. Add a plucked note for each edit:

```lisp
;; The weather in Chicago as a tone and a wind, and Wikipedia's edits as
;; plucked notes over them.

(module main)

(import "std/instruments")

(define forecast
  "https://api.open-meteo.com/v1/forecast?latitude=41.88&longitude=-87.63&current=temperature_2m,wind_speed_10m")

(defpiece main ()
  (let ((temp   (pipe :control))
        (wind   (pipe :control))
        (warmth (pipe :control))
        (gusts  (pipe :control))
        (edits  (pipe :event))
        (size   (pipe :control))
        (step   (pipe :control))
        (notes  (pipe :event))
        (tone   (pipe :audio))
        (air    (pipe :audio))
        (mix    (pipe :audio))
        (out    (pipe :audio))
        (volume (knob "Volume" :min -60db :max 0db :initial -18db)))
    (web-in :url forecast :every 60s :fields ["/current/temperature_2m"] :value temp)
    (web-in :url forecast :every 60s :fields ["/current/wind_speed_10m"] :value wind)
    (smoother :in temp :time 2s :out warmth)
    (smoother :in wind :time 2s :out gusts)

    (define pitch (mapper warmth (fn (t) (scale-exp t -20 40 110hz 880hz))))
    (sine-osc :freq pitch :out tone)
    (gain :in tone :amount -6db :out mix)

    (define blowing (mapper gusts (fn (w) (clamp (scale w 0 60 0 0.8) 0 0.8))))
    (noise :color :pink :out air)
    (gain :in air :amount blowing :out mix)

    ;; The edits: an event for each edit that gives the page's new length.
    (web-stream :url "https://stream.wikimedia.org/v2/stream/recentchange"
                :headers ["User-Agent: espresso-weather-tutorial"]
                :fields ["/length/new"] :value size :out edits)
    (sequencer :in edits :values [57 60 62 64 67 69 72] :order :random :out step)
    (define velocity (mapper size (fn (n) (clamp (scale (log (+ n 1)) 4 12 0.15 1) 0.15 1))))
    (trigger-notes :in edits :pitch step :velocity velocity :length 120ms :out notes)
    (play (make pluck :voices 8 :level -12db) notes mix)

    (gain :in mix :amount volume :out out)
    (audio-out :in out :channel 0)
    (audio-out :in out :channel 1)

    (page
      (panel "WEATHER"
        (section "Chicago"
                 (readout "Temperature" (mapper warmth (fn (t) (quantize t 0.1))))
                 (readout "Wind" (mapper gusts (fn (w) (quantize w 0.1)))))
        (section "Sound"
                 (readout "Pitch" (mapper pitch (fn (p) (quantize p 1hz))) :unit :hz)
                 (meter "Air" blowing :max 0.8 :hold 0))
        (section "Output" volume)))))
```

- Each message that holds field 0, `/length/new`, the edited page's new length in bytes, sets `size` and sends a value event on `edits`. A message without the field, such as a log entry, sends no event and plays nothing. With no `:fields`, a stream reads each whole document as one number, so a stream of JSON objects sends no events.
- `sequencer` draws a note of a pentatonic scale at each event, and `trigger-notes` turns each event into a note of 120 ms at that pitch. The velocity mapper scales the logarithm of the length, so a stub plays softly and a long article at full velocity.
- `play` plays the notes on a `pluck` instrument from `std/instruments` into `mix`, beside the tone and the noise.
- Wikimedia asks clients to name themselves in a `User-Agent` header. Add a contact to the header before sharing a piece that uses the stream. Header values are part of the IR, so a header is as readable as the IR and the file that writes it.

A stream that closes or fails connects again after 3 s, or after the stream's own retry time, doubling while failures repeat, up to 60 s, and sends the last event ID it received.

## Next

- [Web data](../language.md#web-data) in the language reference describes `web-socket`, which reads a WebSocket, and `web-out`, which sends requests from a piece.
- [Inputs and outputs](../guide/io.md#web-data) describes what web sources and sinks need from the machine.
- [Primitives](../reference/primitives.md) lists every parameter of `web-in`, `web-stream`, `smoother`, and `noise`, with their ranges and defaults.
- The examples `weather` and `edits`, in `examples/`, are complete versions of these pieces, each with a screen of its own drawn by `ui/script.js`.
