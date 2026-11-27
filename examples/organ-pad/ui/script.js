// organ-pad's units, which the language does not name: the registration
// as the piece morphs it, the flute at 0 and the full chorus at 1; the
// tape's feedback, the knob's 0 to 1 being 0 to 0.96.
espressoUI.units.registration = (m) => (m <= 0.005 ? "Flute" : m >= 0.995 ? "Full" : `${Math.round(m * 100)}%`);
espressoUI.units.feedback = (w) => `${Math.round(w * 96)}%`;
const units = [
  [document.querySelector('es-knob[label="Registration"]'), "registration"],
  [document.querySelector('es-pedal[label="Tape loop"] es-knob[label="Repeats"]'), "feedback"],
];
for (const [knob, unit] of units) knob.setAttribute("unit", unit);
// Controls take their first values once the document is parsed; the knobs
// are drawn again in their units after that.
addEventListener("DOMContentLoaded", () => { for (const [knob] of units) knob.value = knob.value; });
