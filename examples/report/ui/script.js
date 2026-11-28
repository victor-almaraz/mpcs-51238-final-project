// report's screens: above the Swell section's knobs, the swell over the
// last 20 s; above the Report section's meter, the last report as it was
// posted, a light at each report, and the requests dropped.
const trace = document.querySelector('es-screen[label="Swell trace"]');
trace.classList.add("screen", "trace");
trace.setAttribute("aria-hidden", "true");
trace.innerHTML = `<canvas width="432" height="144"></canvas>`;

const post = document.querySelector('es-screen[label="Last report"]');
post.classList.add("screen", "post");
post.setAttribute("role", "group");
post.setAttribute("aria-label", "Last report");
post.innerHTML = `
  <div class="line"><span>POST localhost:8080/loudness</span><es-lamp id="sent" label="Sent"></es-lamp></div>
  <div class="body" id="body">{"loudness": –}</div>
  <div class="line"><span>every 4 beats</span><span>dropped <b id="dropped">0</b></span></div>`;

// The swell, sampled ten times a second, 0 at the bottom and 1 at the top.
const canvas = trace.querySelector("canvas");
const pen = canvas.getContext("2d");
const history = [];
let swell = null;
espresso.on("/swell", (s) => { swell = s; });
const draw = () => {
  pen.clearRect(0, 0, canvas.width, canvas.height);
  pen.strokeStyle = "#ffffff14";
  pen.lineWidth = 1;
  for (const y of [0.25, 0.5, 0.75]) {
    pen.beginPath(); pen.moveTo(0, y * canvas.height); pen.lineTo(canvas.width, y * canvas.height); pen.stroke();
  }
  pen.strokeStyle = "#8fd16a";
  pen.lineWidth = 2.5;
  pen.beginPath();
  history.forEach((s, i) => {
    const x = canvas.width - (history.length - 1 - i) * (canvas.width / 199);
    const y = 10 + (1 - Math.min(Math.max(s, 0), 1)) * (canvas.height - 20);
    if (i === 0) pen.moveTo(x, y); else pen.lineTo(x, y);
  });
  pen.stroke();
};
setInterval(() => {
  if (swell === null) return;
  history.push(swell);
  if (history.length > 200) history.shift();
  draw();
}, 100);
draw();

// At each report, the light flashes and the body shows the loudness
// posted, as the piece last sent it to the meter.
let loudness = null;
espresso.on("/report/loudness", (l) => { loudness = l; });
// The piece's count of reports, from 1 to 8, changes at each report.
const lamp = document.getElementById("sent");
let dark = null;
espresso.on("/reports", () => {
  lamp.value = 1;
  clearTimeout(dark);
  dark = setTimeout(() => { lamp.value = 0; }, 250);
  if (loudness !== null) document.getElementById("body").textContent = `{"loudness": ${loudness.toFixed(3)}}`;
});
espresso.on("/dropped", (d) => { document.getElementById("dropped").textContent = String(Math.round(d)); });
