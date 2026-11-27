// phasing's screen, above the Pattern section's knobs: the twelve steps, a
// row for each marimba, its place lit and the steps the fill has not
// joined dimmed; and the units of the tempo and the phasing, which the
// language does not name.
espressoUI.units.bpm = (rate) => `${Math.round(rate * 15)} BPM`;  // sixteenths: four steps a beat
espressoUI.units.phasing = (w) => `+${(w * 3).toFixed(2)}%`;      // the right marimba up to 3% faster
const units = [
  [document.querySelector('es-knob[label="Tempo"]'), "bpm"],
  [document.querySelector('es-knob[label="Phasing"]'), "phasing"],
];
for (const [knob, unit] of units) knob.setAttribute("unit", unit);
addEventListener("DOMContentLoaded", () => { for (const [knob] of units) knob.value = knob.value; });

// The pattern, as in loops.lisp: each step's held note (from 0), octave,
// and the fill at which it joins.
const pattern = [[0, 0, 0], [1, 0, 0.5], [2, 0, 0.2], [3, 0, 0.75], [1, 0, 0.1], [4, 0, 0.6],
                 [0, 1, 0.3], [3, 0, 0.85], [2, 0, 0.15], [1, 0, 0.65], [4, 0, 0.4], [3, -1, 0.95]];
const screen = document.querySelector('es-screen[label="Steps"]');
screen.classList.add("screen");
screen.setAttribute("aria-hidden", "true");
const rows = ["left", "right"].map((side) => {
  const track = document.createElement("div");
  track.className = `track ${side}`;
  track.innerHTML = `<b>${side === "left" ? "L" : "R"}</b>`;
  const cells = pattern.map(([note, octave]) => {
    const cell = document.createElement("div");
    cell.className = "cell";
    cell.textContent = `${note + 1}${octave > 0 ? "↑" : octave < 0 ? "↓" : ""}`;
    track.append(cell);
    return cell;
  });
  screen.append(track);
  return cells;
});
// A step sounds once the fill reaches its entry.
const fill = (f) => rows.forEach((cells) => cells.forEach((cell, k) => cell.classList.toggle("out", f < pattern[k][2])));
fill(0);
espresso.on("/fill/now", fill);
// Each marimba's step, 0 to 11.
rows.forEach((cells, side) => {
  let now = -1;
  const place = (step) => {
    if (now >= 0) cells[now].classList.remove("now");
    now = Math.round(step) % 12;
    cells[now].classList.add("now");
  };
  place(0);
  espresso.on(side === 0 ? "/step/left" : "/step/right", place);
});
