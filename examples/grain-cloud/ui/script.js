// grain-cloud's screen, above the Scan section's knobs: the recording,
// the span the scan sweeps (the centre, less and more the depth), and the
// read head where the grains read.
const peaks = [0.59,0.58,0.53,0.49,0.46,0.47,0.48,0.45,0.44,0.42,0.39,0.37,0.36,0.36,0.36,0.35,0.55,0.53,0.54,0.55,
  0.53,0.51,0.48,0.43,0.41,0.40,0.41,0.41,0.39,0.39,0.36,0.34,0.40,0.37,0.37,0.37,0.36,0.34,0.33,0.29,0.27,0.26,
  0.27,0.26,0.25,0.25,0.23,0.21,0.73,0.65,0.64,0.59,0.54,0.61,0.54,0.54,0.48,0.49,0.49,0.49,0.49,0.43,0.45,0.40,
  0.72,0.85,0.76,0.76,0.73,0.63,0.63,0.68,0.57,0.61,0.56,0.52,0.62,0.53,0.52,0.48,0.60,0.63,0.61,0.64,0.64,0.49,
  0.52,0.53,0.51,0.58,0.49,0.44,0.44,0.45,0.47,0.43,0.82,0.73,0.85,0.68,0.68,0.72,0.70,0.68,0.64,0.57,0.57,0.61,
  0.56,0.59,0.45,0.47,1.00,0.89,0.76,0.81,0.77,0.80,0.68,0.73];  // 120 over its 3 s
const upper = peaks.map((p, i) => `${i + 0.5},${20 - 18 * p}`);
const lower = peaks.map((p, i) => `${i + 0.5},${20 + 18 * p}`).reverse();
const section = document.querySelector('es-section[label="Scan"]');
const screen = document.createElement("div");
screen.className = "screen scan";
screen.setAttribute("span", "9 3");
screen.setAttribute("aria-hidden", "true");
screen.innerHTML = `<svg viewBox="0 0 120 40" preserveAspectRatio="none">
  <path d="M${upper.join(" L")} L${lower.join(" L")} Z"></path></svg><div class="span"></div><div class="marker"></div>`;
section.prepend(screen);

// A time in the recording, 0 to 3 s, as a place across the screen.
const across = (t) => `calc(8px + ${Math.min(Math.max(t / 3, 0), 1)} * (100% - 18px))`;
const marker = screen.querySelector(".marker");
espresso.on("/position", (t) => { marker.style.left = across(t); });
const centre = section.querySelector('es-knob[label="Centre"]');
const depth = section.querySelector('es-knob[label="Depth"]');
const span = screen.querySelector(".span");
const shade = () => {
  span.style.left = across(centre.value - depth.value);
  span.style.right = `calc(100% - ${across(centre.value + depth.value)} - 2px)`;
};
for (const knob of [centre, depth]) knob.addEventListener("input", shade);
// The knobs take their first values once the document is parsed; until the
// piece sends a position, the read head sits at the centre.
addEventListener("DOMContentLoaded", () => {
  shade();
  marker.style.left = across(centre.value);
});
