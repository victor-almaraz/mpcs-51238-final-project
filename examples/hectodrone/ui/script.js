// hectodrone's screen, beside the Detune knob: the hundred oscillators'
// detuning, each a line at its place, and the knob's unit, the detuning's
// standard deviation in cents.

// The detuning's standard deviation in cents, from the knob's -1 to 1 as
// the piece reads the pitch bend: none at -1, 10 at 0, 200 at 1.
const fine = 10, widest = 200;
const deviation = (u) => (u >= 0 ? fine + u * (widest - fine) : fine * (1 + u));
espressoUI.units.deviation = (u) => `${Math.round(deviation(u))} ct`;

// Each oscillator's place: the standard normal quantile at the middle of
// its hundredth, by Acklam's approximation, as the piece computes it.
const horner = (cs, x) => cs.reduce((sum, c) => sum * x + c, 0);
const cn = [-39.69683028665376, 220.9460984245205, -275.9285104469687, 138.3577518672690, -30.66479806614716, 2.506628277459239];
const cd = [-54.47609879822406, 161.5858368580409, -155.6989798598866, 66.80131188771972, -13.28068155288572, 1];
const tn = [-0.007784894002430293, -0.3223964580411365, -2.400758277161838, -2.549732539343734, 4.374664141464968, 2.938163982698783];
const td = [0.007784695709041462, 0.3224671290700398, 2.445134137142996, 3.754408661907416, 1];
const lower = (p) => { const q = Math.sqrt(-2 * Math.log(p)); return horner(tn, q) / horner(td, q); };
const quantile = (p) => {
  if (p < 0.02425) return lower(p);
  if (p > 0.97575) return -lower(1 - p);
  const q = p - 0.5;
  return (q * horner(cn, q * q)) / horner(cd, q * q);
};
const places = Array.from({ length: 100 }, (_, i) => quantile((i + 0.5) / 100));

// The screen spans 600 cents either side of the note; a line's height is
// the normal density at its place.
const screen = document.createElement("div");
screen.className = "screen";
screen.setAttribute("span", "8 4");
screen.setAttribute("aria-hidden", "true");
screen.innerHTML = `<svg viewBox="-1 0 2 1" preserveAspectRatio="none"></svg><div class="readout"></div>`;
document.querySelector('es-section[label="Oscillators"]').prepend(screen);
const svg = screen.querySelector("svg");
const ns = "http://www.w3.org/2000/svg";
const axis = document.createElementNS(ns, "line");
axis.setAttribute("class", "axis");
axis.setAttribute("x1", 0); axis.setAttribute("x2", 0); axis.setAttribute("y1", 0); axis.setAttribute("y2", 1);
axis.setAttribute("vector-effect", "non-scaling-stroke");
svg.append(axis);
const lines = places.map((z) => {
  const line = document.createElementNS(ns, "line");
  line.setAttribute("y1", 1);
  line.setAttribute("y2", 1 - 0.95 * Math.exp((-z * z) / 2));
  line.setAttribute("vector-effect", "non-scaling-stroke");
  svg.append(line);
  return line;
});
const readout = screen.querySelector(".readout");
const draw = (u) => {
  const cents = deviation(u);
  places.forEach((z, i) => {
    const x = Math.max(-1, Math.min(1, (z * cents) / 600));
    lines[i].setAttribute("x1", x);
    lines[i].setAttribute("x2", x);
  });
  readout.textContent = `σ ${Math.round(cents)} ct`;
};
const detune = document.querySelector('es-knob[label="Detune"]');
detune.setAttribute("unit", "deviation");
detune.addEventListener("input", (e) => draw(e.detail.value));
draw(Number(detune.getAttribute("value")));
addEventListener("DOMContentLoaded", () => { detune.value = detune.value; });  // drawn again in its unit
