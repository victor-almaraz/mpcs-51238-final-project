# Inputs and outputs

A piece reads and writes the world through its sources and sinks: audio and MIDI devices, OSC over UDP, serial devices, web endpoints, and its page. The [language reference](../language.md) describes each primitive and its parameters; this chapter describes what each needs from the machine.

## Audio

The server opens one audio device at startup: the system's default, or the one `--device NAME` names, with the sample rate, block size, and channel counts its options request ([Playing](playing.md#the-server)). `espresso-server --list-devices` lists the devices of each type, and the line the server prints at startup names the type and the device it opened.

On Linux the server opens its audio device through JACK: PipeWire's JACK interface (`pipewire-jack`) or a JACK server, which runs the device callback on a real-time thread. When no JACK server lists a device, the server reports it and opens the device through ALSA, whose callback thread runs without real-time priority. A name that JACK does not list selects an ALSA device. JACK and PipeWire list the server as the client `espresso-server`, so `pw-top` shows its load and xruns.

On macOS the server uses Core Audio. Audio input needs microphone permission for the terminal application (System Settings › Privacy & Security › Microphone); without it, the server runs with no inputs.

`audio-in` reads device inputs into its pipe and `audio-out` writes its pipe to device outputs, one device channel for each channel of the pipe, from `:channel`, counted from 0: a stereo pipe on `:channel 0` plays on outputs 0 and 1. Every `audio-out` limits its signal to its `:ceiling`, full scale by default, and mutes itself if it receives infinite or NaN samples, reporting the node in the editor's console.

## MIDI

`midi-in` and `midi-out` select a device by a case-insensitive substring of its name, given as `:device`; an empty `:device` matches every device. MIDI inputs are opened as they appear, so a keyboard plugged in while a piece plays is heard at once.

## OSC

`osc-in` and `osc-out` send and receive OSC over UDP on `127.0.0.1`, on the ports a piece names. Another program on the same machine, such as a controller application, talks to a piece through them.

## Serial devices

`serial-in` and `serial-out` open the serial devices pieces name, rescanning every second, and open a device again when it returns. On Linux, opening `/dev/ttyACM*` or `/dev/ttyUSB*` needs membership in the `dialout` group (`uucp` on some distributions); log in again after joining it. [Serial devices](../language.md#serial-devices) in the language reference describes the line formats.

## Web data

`web-in`, `web-stream`, `web-socket`, and `web-out` reach HTTP, HTTPS, WS, and WSS endpoints through libcurl, with TLS from the system's OpenSSL. Failures appear in the editor's console, at most once a second for each node. [Web data](../language.md#web-data) in the language reference describes polling, streams, and requests.

## Pages

A project with `:page-port P` in `project.lisp` has a page. While it plays, the server serves at `http://localhost:P/` the page drawn from the controls and layout the piece declares, or the project's own `ui/index.html` in its place, with the rest of `ui/`. The server carries OSC between its pages and the piece over `POST /osc`, server-sent events on `GET /osc`, and a WebSocket on `/osc`. Pages load `/espresso.js` for `espresso.send`, `espresso.on`, `espresso.onOpen`, and `espresso.onClose`, and `/espresso-ui.js` for page controls: knobs, sliders, toggles, buttons, lists, a keyboard, an xy pad, footswitches, drawbars, clip rows, meters, lamps, and readouts, bound to the piece's addresses. The routes of the page port, the clip banks' among them, are in [Pages](../language.md#pages).

The listener answers only its own host names and takes messages only from its own pages. With `--page-lan`, the server also answers other machines on the network that open the pairing link it prints, such as a phone. Its traffic, the pairing token included, is plain HTTP without encryption, so `--page-lan` belongs on a trusted network only.
