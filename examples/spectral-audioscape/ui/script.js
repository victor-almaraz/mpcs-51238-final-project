// The loop's speed as a multiple of the arpeggio's own: a rate of 1/6 Hz
// plays it as recorded. A unit the language does not name, added here.
espressoUI.units.speed = (v) => `${(v * 6).toFixed(2)}×`;
const speed = document.querySelector('es-pedal[label="Loop"] es-knob[label="Speed"]');
speed.setAttribute("unit", "speed");
// Controls take their first values once the document is parsed; the knob
// is drawn again in its new unit after that.
addEventListener("DOMContentLoaded", () => { speed.value = speed.value; });
