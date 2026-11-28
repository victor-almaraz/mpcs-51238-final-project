// firehose's screen, in the Stream section: the posts a second, large, a
// light at each post, and the last post's note under them.
const screen = document.querySelector('es-screen[label="Posts"]');
screen.classList.add("screen");
screen.setAttribute("role", "group");
screen.setAttribute("aria-label", "Posts");
screen.innerHTML = `
  <div class="rate"><span id="rate">–</span><small>POSTS / S</small></div>
  <es-lamp id="post" label="Post"></es-lamp>
  <div class="last"><span>Last note</span><b id="note">–</b></div>`;

// The posts a second, over the last 5 s: the piece counts posts from 1 to
// 8, and each message adds the posts since the last.
const arrivals = [];
let count = null;  // unknown until the first message
const lamp = document.getElementById("post");
let dark = null;
espresso.on("/posts", (n) => {
  if (count !== null) arrivals.push([performance.now(), (n - count + 8) % 8 || 8]);
  count = n;
  lamp.value = 1;
  clearTimeout(dark);
  dark = setTimeout(() => { lamp.value = 0; }, 60);
});
setInterval(() => {
  const now = performance.now();
  while (arrivals.length > 0 && arrivals[0][0] < now - 5000) arrivals.shift();
  const posts = arrivals.reduce((sum, a) => sum + a[1], 0);
  document.getElementById("rate").textContent = arrivals.length > 0 ? (posts / 5).toFixed(1) : "–";
}, 500);
// The last post's note, by name: C4 is 60.
const names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
espresso.on("/pitch", (n) => {
  if (n <= 0) return;
  const note = Math.round(n);
  document.getElementById("note").textContent = names[note % 12] + (Math.floor(note / 12) - 1);
});
