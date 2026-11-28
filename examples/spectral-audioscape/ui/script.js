// The loop's speed as a multiple of the arpeggio's own: a rate of 1/6 Hz
// plays it as recorded. A unit the language does not name, added here.
espressoUI.units.speed = (v) => `${(v * 6).toFixed(2)}×`;
const speed = document.querySelector('es-pedal[label="Loop"] es-knob[label="Speed"]');
speed.setAttribute("unit", "speed");
// Controls take their first values once the document is parsed; the knob
// is drawn again in its new unit after that.
addEventListener("DOMContentLoaded", () => { speed.value = speed.value; });

// Each band's delay time, over its feedback slider in the Spectral pedal,
// as bands in spectral-audioscape.lisp gives them.
const delays = { "250": "4.75 s", "400": "2.83 s", "630": "3.08 s", "1k": "2.76 s",
                 "1.6k": "2.11 s", "2.5k": "4.59 s", "4k": "1.87 s", "6.3k": "3.92 s" };
for (const [band, time] of Object.entries(delays)) {
  const caption = document.querySelector(`es-screen[label="${band} delay"]`);
  if (caption) caption.textContent = time;
}
