// wavetable's screen, above the Sweep section's knobs: the eight tables'
// shapes, from a sine through brighter saws to a hollow square, side by
// side, and a marker where the sweep is.
const shapes = [
  (x) => Math.sin(x),
  (x) => Math.sin(x) + 0.3 * Math.sin(2 * x),
  (x) => Math.sin(x) + 0.5 * Math.sin(2 * x) + 0.2 * Math.sin(3 * x),
  (x) => [1, 2, 3, 4, 5].reduce((s, k) => s + Math.sin(k * x) / k, 0),
  (x) => [1, 2, 3, 4, 5, 6, 7, 8].reduce((s, k) => s + Math.sin(k * x) / k, 0),
  (x) => [1, 3, 5].reduce((s, k) => s + Math.sin(k * x) / k, 0) + 0.3 * Math.sin(2 * x),
  (x) => [1, 3, 5, 7].reduce((s, k) => s + Math.sin(k * x) / k, 0),
  (x) => [1, 3, 5, 7, 9, 11].reduce((s, k) => s + Math.sin(k * x) / k, 0),
];
const screen = document.querySelector('es-screen[label="Tables"]');
screen.classList.add("screen");
screen.setAttribute("aria-hidden", "true");
const tables = document.createElementNS("http://www.w3.org/2000/svg", "svg");
tables.setAttribute("viewBox", "0 0 320 40");
tables.setAttribute("preserveAspectRatio", "none");
shapes.forEach((shape, n) => {
  const points = [];
  for (let i = 0; i <= 40; ++i) {
    const x = (i / 40) * 2 * Math.PI;
    points.push(`${n * 40 + 2 + i * 0.9},${20 - (14 * shape(x)) / 1.3}`);
  }
  const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
  path.setAttribute("d", "M" + points.join(" L"));
  tables.append(path);
});
const marker = document.createElement("div");
marker.className = "marker";
screen.append(tables, marker);

// The marker follows the sweep's position, 0 at the sine and 1 at the square.
const place = (p) => { marker.style.left = `calc(8px + ${Math.min(Math.max(p, 0), 1)} * (100% - 18px))`; };
place(0.5);
espresso.on("/position", place);
