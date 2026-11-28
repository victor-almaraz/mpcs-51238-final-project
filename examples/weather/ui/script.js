// weather's screen, in the Chicago now section: the temperature and the
// wind as they glide, each with a scale and a marker at the reading.
const screen = document.querySelector('es-screen[label="Weather"]');
screen.classList.add("screen");
screen.setAttribute("role", "group");
screen.setAttribute("aria-label", "Weather");
screen.innerHTML = `
  <div class="reading">
    <span class="what">Temperature</span><span class="now"><span id="temp">–</span><small>°C</small></span>
    <div class="scale warm"><i id="temp-mark"></i></div>
    <div class="ends"><span>−20</span><span>40 °C</span></div>
  </div>
  <div class="reading">
    <span class="what">Wind</span><span class="now"><span id="wind">–</span><small>km/h</small></span>
    <div class="scale"><i id="wind-mark"></i></div>
    <div class="ends"><span>calm</span><span>60 km/h</span></div>
  </div>`;

const clamp = (x, lo, hi) => Math.min(Math.max(x, lo), hi);
const mark = (id, p) => { document.getElementById(id).style.left = `${100 * clamp(p, 0, 1)}%`; };
espresso.on("/temperature", (t) => {
  document.getElementById("temp").textContent = t.toFixed(1).replace("-", "−");
  mark("temp-mark", (t + 20) / 60);
});
espresso.on("/wind", (w) => {
  document.getElementById("wind").textContent = w.toFixed(1);
  mark("wind-mark", w / 60);
});
mark("temp-mark", 1 / 3);
mark("wind-mark", 0);
