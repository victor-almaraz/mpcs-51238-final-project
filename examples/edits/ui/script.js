// edits' screen, in the Stream section: the edits a second, large, a light
// at each edit, and the last edit's size under them.
const screen = document.querySelector('es-screen[label="Edits"]');
screen.classList.add("screen");
screen.setAttribute("role", "group");
screen.setAttribute("aria-label", "Edits");
screen.innerHTML = `
  <div class="rate"><span id="rate">–</span><small>EDITS / S</small></div>
  <es-lamp id="edit" label="Edit"></es-lamp>
  <div class="last"><span>Last edit</span><b id="size">–</b></div>`;

// The edits a second, over the last 5 s: the piece counts edits from 1 to
// 8, and each message adds the edits since the last.
const arrivals = [];
let count = null;  // unknown until the first message
const lamp = document.getElementById("edit");
let dark = null;
espresso.on("/edits", (n) => {
  if (count !== null) arrivals.push([performance.now(), (n - count + 8) % 8 || 8]);
  count = n;
  lamp.value = 1;
  clearTimeout(dark);
  dark = setTimeout(() => { lamp.value = 0; }, 60);
});
setInterval(() => {
  const now = performance.now();
  while (arrivals.length > 0 && arrivals[0][0] < now - 5000) arrivals.shift();
  const edits = arrivals.reduce((sum, a) => sum + a[1], 0);
  document.getElementById("rate").textContent = arrivals.length > 0 ? (edits / 5).toFixed(1) : "–";
}, 500);
espresso.on("/size", (n) => {
  document.getElementById("size").textContent = Math.round(n).toLocaleString("en-US") + " bytes";
});
