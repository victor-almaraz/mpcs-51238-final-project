// The loop's speed as a multiple of the arpeggio's own: a rate of 1/6 Hz
// plays it as recorded. A unit the language does not name, added here.
espressoUI.units.speed = (v) => `${(v * 6).toFixed(2)}×`;
const speed = document.querySelector('es-pedal[label="Loop"] es-knob[label="Speed"]');
speed.setAttribute("unit", "speed");
// Controls take their first values once the document is parsed; the knob
// is drawn again in its new unit after that.
addEventListener("DOMContentLoaded", () => { speed.value = speed.value; });

// Each band's delay time, above its feedback slider, as bands in
// spectral-audioscape.lisp gives them.
const delays = ["4.75 s", "2.83 s", "3.08 s", "2.76 s", "2.11 s", "4.59 s", "1.87 s", "3.92 s"];
const feedback = document.querySelector('es-section[label="Feedback"]');
[...feedback.querySelectorAll(":scope > es-slider")].forEach((slider, i) => {
  const time = document.createElement("span");
  time.className = "delay";
  time.textContent = delays[i];
  time.setAttribute("span", "2 1");
  time.setAttribute("at", `${2 * i + 1} 1`);
  slider.setAttribute("at", `${2 * i + 1} 2`);
  slider.before(time);
});
