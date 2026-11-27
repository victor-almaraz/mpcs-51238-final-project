// straintest's strips: each note by name, middle C (60) being C4, and a
// light that flashes at each note.
const names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
espressoUI.units.note = (n) => names[((Math.round(n) % 12) + 12) % 12] + (Math.floor(Math.round(n) / 12) - 1);
for (const note of document.querySelectorAll('es-value[label="Note"]')) {
  note.setAttribute("unit", "note");
  const led = document.createElement("i");
  led.className = "led";
  led.setAttribute("aria-hidden", "true");
  // The screen and the light share the strip's first row, the light in
  // the screen's top right corner.
  note.setAttribute("at", "1 1");
  led.setAttribute("at", "5 1");
  note.after(led);
  espresso.on(note.getAttribute("address"), () => {
    led.classList.add("on");
    requestAnimationFrame(() => requestAnimationFrame(() => led.classList.remove("on")));
  });
}
