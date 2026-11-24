// espresso-ui.js: page controls bound to a piece's OSC addresses (ui.md),
// served by the page port at /espresso-ui.js. A page loads it after
// /espresso.js:
//
//   <script src="/espresso.js"></script>
//   <script src="/espresso-ui.js"></script>
//   <es-knob address="/cutoff" label="Cutoff" min="20" max="20000"
//            scale="exp" value="800" unit="hz"></es-knob>
//
// Controls that send (es-knob, es-slider, es-toggle, es-select) send their
// value to their address when the user changes it, show the values the
// piece sends to that address, and send their value again each time the
// socket opens, so the piece starts where the page is. es-button sends 1
// while held and 0 when let go. Displays (es-meter, es-lamp, es-value) show
// what the piece sends and send nothing. Every control fires an "input"
// event, with { value, fromPage } in event.detail, as it changes. Colors
// and sizes come from CSS custom properties (--es-*).
(function () {
  "use strict";

  // --- Values (ui.md §5.2) ----------------------------------------------
  // A control holds a position from 0 to 1 and maps it to a value.

  const clamp01 = (x) => Math.min(1, Math.max(0, x));
  const dbFloor = -60;  // the bottom of a "db" scale whose minimum is 0

  // Each scale maps a position to a value between lo and hi, and back.
  const scales = {
    linear: {
      value: (p, lo, hi) => lo + p * (hi - lo),
      position: (v, lo, hi) => (hi === lo ? 0 : (v - lo) / (hi - lo)),
    },
    // Equal ratios over the travel, for frequencies and times; needs
    // lo and hi above 0, and is linear otherwise.
    exp: {
      value: (p, lo, hi) => (lo > 0 && hi > 0 ? lo * Math.pow(hi / lo, p) : scales.linear.value(p, lo, hi)),
      position: (v, lo, hi) =>
        lo > 0 && hi > 0 && v > 0 ? Math.log(v / lo) / Math.log(hi / lo) : scales.linear.position(v, lo, hi),
    },
    // Equal decibels, for levels: values are amplitudes from lo to hi; a
    // lo of 0 is reached only at position 0, the rest from -60 dB.
    db: {
      value(p, lo, hi) {
        if (p <= 0 || !(hi > 0)) return lo;
        const bottom = lo > 0 ? 20 * Math.log10(lo) : dbFloor;
        const top = 20 * Math.log10(hi);
        return Math.pow(10, (bottom + p * (top - bottom)) / 20);
      },
      position(v, lo, hi) {
        if (!(v > lo) || !(hi > 0)) return 0;
        const bottom = lo > 0 ? 20 * Math.log10(lo) : dbFloor;
        const top = 20 * Math.log10(hi);
        return top === bottom ? 0 : (20 * Math.log10(v) - bottom) / (top - bottom);
      },
    },
  };

  // Value v made one of a range { min, max, scale, step }: within it, and
  // a multiple of step from min when step is above 0.
  function fit(v, range) {
    if (range.step > 0) v = range.min + Math.round((v - range.min) / range.step) * range.step;
    const lo = Math.min(range.min, range.max);
    const hi = Math.max(range.min, range.max);
    return Math.min(hi, Math.max(lo, v));
  }

  // The value at position p of a range.
  function valueAt(p, range) {
    const scale = scales[range.scale] || scales.linear;
    return fit(scale.value(clamp01(p), range.min, range.max), range);
  }

  // The position of value v in a range, from 0 to 1.
  function positionOf(v, range) {
    const scale = scales[range.scale] || scales.linear;
    const p = scale.position(v, range.min, range.max);
    return Number.isFinite(p) ? clamp01(p) : 0;
  }

  // Up to 3 significant digits, without trailing zeros.
  function short(x) {
    if (x === 0) return "0";
    const digits = Math.max(0, 2 - Math.floor(Math.log10(Math.abs(x))));
    return String(Number(x.toFixed(Math.min(digits, 3))));
  }

  // Value v as text in unit: "hz" (Hz, kHz), "s" (s, ms below 1 s), "db"
  // (an amplitude in decibels), "%" (0 to 1 as a percent), or none.
  function format(v, unit) {
    if (!Number.isFinite(v)) return String(v);
    switch (unit) {
      case "hz": return Math.abs(v) >= 1000 ? short(v / 1000) + " kHz" : short(v) + " Hz";
      case "s": return Math.abs(v) < 1 ? short(v * 1000) + " ms" : short(v) + " s";
      case "db": return v <= 0 ? "−∞ dB" : short(20 * Math.log10(v)) + " dB";
      case "%": return short(v * 100) + "%";
      default: return short(v);
    }
  }

  const ui = { scales, fit, valueAt, positionOf, format };
  globalThis.espressoUI = ui;
  if (typeof customElements === "undefined") return;  // no DOM: the values alone, for tests

  // --- Shared look -------------------------------------------------------

  const css = `
    :host {
      --accent: var(--es-accent, #e8a33d);
      --surface: var(--es-surface, #262626);
      --line: var(--es-line, #4a4a4a);
      --text: var(--es-text, #ece9e2);
      --muted: var(--es-muted, #8f8b83);
      --focus: var(--es-focus, #6fa8dc);
      display: inline-flex; flex-direction: column; align-items: center; gap: 4px;
      font: var(--es-font, 12px/1.2 system-ui, sans-serif); color: var(--text);
      user-select: none; -webkit-user-select: none; touch-action: none;
    }
    .control { outline: none; }
    .control:focus-visible .focus { stroke: var(--focus); }
    .label { color: var(--muted); }
    .value { font-variant-numeric: tabular-nums; }
    svg { display: block; overflow: visible; }
  `;

  const svgNs = "http://www.w3.org/2000/svg";
  function svg(name, attributes, parent) {
    const node = document.createElementNS(svgNs, name);
    for (const [k, v] of Object.entries(attributes)) node.setAttribute(k, v);
    if (parent) parent.appendChild(node);
    return node;
  }

  function number(element, name, fallback) {
    const text = element.getAttribute(name);
    const v = text === null || text === "" ? NaN : Number(text);
    return Number.isFinite(v) ? v : fallback;
  }


  // --- The base of every control ------------------------------------------

  // A custom element drawn in a shadow root and bound to an address.
  // Subclasses give styles(), build(root), which draws the control and
  // returns the element that takes focus and events, and setup(). By
  // default a control shows the numbers the piece sends to its address.
  class Control extends HTMLElement {
    constructor() {
      super();
      this.bound = false;
    }

    connectedCallback() {
      if (!this.shadowRoot) {
        const root = this.attachShadow({ mode: "open" });
        const style = document.createElement("style");
        style.textContent = css + this.styles();
        root.appendChild(style);
        this.control = this.build(root);
        this.setup();
      }
      const address = this.getAttribute("address");
      if (!this.bound && address && globalThis.espresso) {
        this.bound = true;
        this.bind(address);
      }
    }

    bind(address) { espresso.on(address, (v) => { if (typeof v === "number") this.value = v; }); }
    styles() { return ""; }
    setup() {}

    vertical() { return this.getAttribute("orientation") === "vertical"; }

    // The label above the control, from the label attribute.
    label() {
      const label = document.createElement("div");
      label.className = "label";
      label.textContent = this.getAttribute("label") || "";
      return label;
    }

    // The name assistive technology reads.
    name() { return this.getAttribute("label") || this.getAttribute("address") || ""; }

    // The range the attributes give: min, max, scale, and step, or steps,
    // a selector of that many positions.
    range() {
      const steps = Math.round(number(this, "steps", 0));
      if (steps >= 2) return { min: 0, max: steps - 1, scale: "linear", step: 1 };
      return {
        min: number(this, "min", 0),
        max: number(this, "max", 1),
        scale: this.getAttribute("scale") || "linear",
        step: number(this, "step", 0),
      };
    }

    // The text a value reads as: its option, or the value in its unit.
    text(v) {
      const options = (this.getAttribute("options") || "").split(",").filter((s) => s !== "");
      if (options.length > 0 && Number.isInteger(v) && v >= 0 && v < options.length) return options[v];
      return format(v, this.getAttribute("unit") || "");
    }

    // Sends v to the address, and reports it with an "input" event.
    report(v, fromPage) {
      const address = this.getAttribute("address");
      if (fromPage && address && globalThis.espresso) espresso.send(address, v);
      this.dispatchEvent(new CustomEvent("input", { detail: { value: v, fromPage } }));
    }
  }

  // --- The base of a value control (ui.md §5) ----------------------------

  // A control with a value the user sets and the piece reports: subclasses
  // draw it and say how a pointer moves it. Attributes: address, label,
  // min, max, scale, step, value (the start, and what a reset returns to),
  // unit, options, and steps. Its value is sent again each time the socket
  // opens.
  class ValueControl extends Control {
    constructor() {
      super();
      this.position = 0;
      this.current = 0;
      this.drag = null;
      this.moved = false;  // set once the page or the piece has changed the value
    }

    // The value, as the piece last reported it or the page last set it.
    get value() { return this.current; }
    set value(v) {
      this.moved = true;
      this.take(Number(v), false);
    }

    startValue() { return number(this, "value", this.range().min); }

    setup() {
      this.listen(this.control);
      this.take(this.moved ? this.current : this.startValue(), false);
    }

    bind(address) {
      super.bind(address);
      espresso.onOpen(() => { if (this.isConnected) espresso.send(address, this.current); });
    }

    // Sets the value (a received one when send is false), redraws, and
    // reports a change; the page sends only what the user changed.
    take(v, send) {
      if (!Number.isFinite(v)) return;
      const range = this.range();
      const next = fit(v, range);
      this.position = positionOf(next, range);
      const changed = next !== this.current;
      this.current = next;
      if (send) this.moved = true;
      if (this.control) {
        this.render();
        this.describe();
      }
      if (changed) this.report(next, send);
    }

    // Moves to position p, as the user does.
    moveTo(p) { this.take(valueAt(clamp01(p), this.range()), true); }

    describe() {
      const c = this.control;
      const range = this.range();
      c.setAttribute("aria-valuemin", String(Math.min(range.min, range.max)));
      c.setAttribute("aria-valuemax", String(Math.max(range.min, range.max)));
      c.setAttribute("aria-valuenow", String(this.current));
      c.setAttribute("aria-valuetext", this.text(this.current));
      if (this.readout) this.readout.textContent = this.text(this.current);
    }

    // The keyboard's step: one position of a selector, 1% otherwise.
    keyStep() {
      const steps = Math.round(number(this, "steps", 0));
      return steps >= 2 ? 1 / (steps - 1) : 0.01;
    }

    listen(c) {
      c.setAttribute("tabindex", "0");
      c.setAttribute("role", "slider");
      c.setAttribute("aria-label", this.name());
      c.addEventListener("pointerdown", (e) => {
        try { c.setPointerCapture(e.pointerId); } catch (error) { /* a pointer the browser does not track */ }
        c.focus();
        this.drag = this.dragStart(e);
        e.preventDefault();
      });
      c.addEventListener("pointermove", (e) => { if (this.drag) this.dragTo(this.drag, e); });
      for (const type of ["pointerup", "pointercancel"]) c.addEventListener(type, () => { this.drag = null; });
      c.addEventListener("dblclick", () => this.take(this.startValue(), true));
      c.addEventListener("keydown", (e) => {
        const step = this.keyStep();
        const moves = {
          ArrowUp: step, ArrowRight: step, ArrowDown: -step, ArrowLeft: -step,
          PageUp: 0.1, PageDown: -0.1,
        };
        if (e.key in moves) this.moveTo(this.position + moves[e.key]);
        else if (e.key === "Home") this.moveTo(0);
        else if (e.key === "End") this.moveTo(1);
        else return;
        e.preventDefault();
      });
    }

    readoutBelow() {
      this.readout = document.createElement("div");
      this.readout.className = "value";
      return this.readout;
    }
  }

  // --- es-knob ------------------------------------------------------------

  const sweep = 135;  // degrees either side of the top

  function arc(r, from, to) {
    const at = (a) => {
      const t = (a * Math.PI) / 180;
      return [50 + r * Math.sin(t), 50 - r * Math.cos(t)];
    };
    const [x1, y1] = at(from);
    const [x2, y2] = at(to);
    const large = Math.abs(to - from) > 180 ? 1 : 0;
    return `M ${x1.toFixed(2)} ${y1.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x2.toFixed(2)} ${y2.toFixed(2)}`;
  }

  // A rotary control: drag up or down (Shift for fine), arrow keys, Page
  // Up and Down, Home and End; a double click returns it to its value.
  class Knob extends ValueControl {
    styles() {
      return `
        svg { width: var(--es-knob-size, 56px); height: var(--es-knob-size, 56px); }
        .control { cursor: ns-resize; }
        .track { fill: none; stroke: var(--line); stroke-width: 6; stroke-linecap: round; }
        .fill { fill: none; stroke: var(--accent); stroke-width: 6; stroke-linecap: round; }
        .body { fill: var(--surface); stroke: var(--line); }
        .pointer { stroke: var(--text); stroke-width: 4; stroke-linecap: round; }
        .focus { fill: none; stroke: transparent; stroke-width: 2; }
      `;
    }

    build(root) {
      const picture = svg("svg", { viewBox: "0 0 100 100", class: "control" });
      svg("circle", { cx: 50, cy: 50, r: 48, class: "focus" }, picture);
      svg("path", { d: arc(40, -sweep, sweep), class: "track" }, picture);
      this.fill = svg("path", { d: "", class: "fill" }, picture);
      svg("circle", { cx: 50, cy: 50, r: 30, class: "body" }, picture);
      this.pointer = svg("line", { x1: 50, y1: 42, x2: 50, y2: 24, class: "pointer" }, picture);
      root.append(this.label(), picture, this.readoutBelow());
      return picture;
    }

    render() {
      const angle = -sweep + this.position * 2 * sweep;
      this.pointer.setAttribute("transform", `rotate(${angle.toFixed(2)} 50 50)`);
      this.fill.setAttribute("d", this.position > 0 ? arc(40, -sweep, angle) : "");
    }

    dragStart(e) { return { y: e.clientY, position: this.position }; }
    dragTo(start, e) {
      const travel = e.shiftKey ? 1500 : 150;  // pixels for the whole range
      this.moveTo(start.position + (start.y - e.clientY) / travel);
    }
  }

  // --- es-slider ----------------------------------------------------------

  // The styles of a bar along a track, for es-slider and es-meter: its
  // length and thickness come from --es-<prefix>-length and -thickness.
  const barStyles = (prefix) => `
    .control { position: relative; border-radius: 3px; background: var(--line); }
    .control.horizontal { width: var(--es-${prefix}-length, 160px); height: var(--es-${prefix}-thickness, 8px); }
    .control.vertical { height: var(--es-${prefix}-length, 160px); width: var(--es-${prefix}-thickness, 8px); }
    .fill { position: absolute; left: 0; bottom: 0; background: var(--accent); border-radius: 3px; }
    .horizontal .fill { top: 0; }
    .vertical .fill { right: 0; }
  `;

  function track(element, ...parts) {
    const box = document.createElement("div");
    box.className = "control " + (element.vertical() ? "vertical" : "horizontal");
    box.append(...parts);
    return box;
  }

  function div(className) {
    const node = document.createElement("div");
    node.className = className;
    return node;
  }

  // A percentage for a style, from a position from 0 to 1.
  const percent = (p) => (p * 100).toFixed(3) + "%";

  // A linear control: a press or a drag puts it where the pointer is; keys
  // and a double click as es-knob. orientation="vertical" stands it up.
  class Slider extends ValueControl {
    styles() {
      return barStyles("slider") + `
        .control { cursor: pointer; outline-offset: 3px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .thumb { position: absolute; width: 14px; height: 14px; border-radius: 50%; background: var(--text); }
        .horizontal .thumb { top: 50%; transform: translate(-50%, -50%); }
        .vertical .thumb { left: 50%; transform: translate(-50%, 50%); }
      `;
    }

    build(root) {
      this.fillBar = div("fill");
      this.thumb = div("thumb");
      const bar = track(this, this.fillBar, this.thumb);
      root.append(this.label(), bar, this.readoutBelow());
      return bar;
    }

    render() {
      const at = percent(this.position);
      if (this.vertical()) {
        this.fillBar.style.height = at;
        this.thumb.style.bottom = at;
      } else {
        this.fillBar.style.width = at;
        this.thumb.style.left = at;
      }
    }

    // The position of the pointer along the track.
    at(e) {
      const box = this.control.getBoundingClientRect();
      if (this.vertical()) return box.height > 0 ? (box.bottom - e.clientY) / box.height : 0;
      return box.width > 0 ? (e.clientX - box.left) / box.width : 0;
    }

    dragStart(e) {
      this.moveTo(this.at(e));
      return {};
    }
    dragTo(start, e) { this.moveTo(this.at(e)); }
  }

  // --- es-toggle ----------------------------------------------------------

  // A switch that sends 1 when on and 0 when off: a click, Space, or Enter
  // flips it; value="1" starts it on; orientation="vertical" stands it up,
  // on at the top.
  class Toggle extends ValueControl {
    range() { return { min: 0, max: 1, scale: "linear", step: 1 }; }

    styles() {
      return `
        .control { position: relative; cursor: pointer; border-radius: 9px; background: var(--line); outline-offset: 3px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .horizontal { width: 36px; height: 18px; }
        .vertical { width: 18px; height: 36px; }
        .thumb { position: absolute; left: 2px; top: 20px; width: 14px; height: 14px; border-radius: 50%; background: var(--muted); }
        .horizontal .thumb { top: 2px; }
        .control.on { background: var(--accent); }
        .on .thumb { background: var(--text); }
        .horizontal.on .thumb { left: 20px; }
        .vertical.on .thumb { top: 2px; }
      `;
    }

    build(root) {
      const box = track(this, div("thumb"));
      root.append(this.label(), box);
      return box;
    }

    render() { this.control.classList.toggle("on", this.current === 1); }
    describe() { this.control.setAttribute("aria-checked", String(this.current === 1)); }

    listen(c) {
      c.setAttribute("tabindex", "0");
      c.setAttribute("role", "switch");
      c.setAttribute("aria-label", this.name());
      c.addEventListener("click", () => this.take(1 - this.current, true));
      c.addEventListener("keydown", (e) => {
        if (e.key !== " " && e.key !== "Enter") return;
        e.preventDefault();
        if (!e.repeat) this.take(1 - this.current, true);
      });
    }
  }

  // --- es-select ----------------------------------------------------------

  // A choice among its <option>s that sends the chosen one's index; value
  // or an option's selected attribute picks the first.
  class Select extends ValueControl {
    choices() { return Array.from(this.querySelectorAll("option")); }
    range() { return { min: 0, max: Math.max(0, this.choices().length - 1), scale: "linear", step: 1 }; }

    startValue() {
      if (this.hasAttribute("value")) return number(this, "value", 0);
      return Math.max(0, this.choices().findIndex((o) => o.hasAttribute("selected")));
    }

    styles() {
      return `
        select { font: inherit; color: var(--text); background: var(--surface); border: 1px solid var(--line);
                 border-radius: 3px; padding: 3px 6px; }
        select:focus-visible { outline: 2px solid var(--focus); outline-offset: 1px; }
      `;
    }

    build(root) {
      const list = document.createElement("select");
      list.className = "control";
      root.append(this.label(), list);
      return list;
    }

    // Options the parser adds after the element connects are taken too.
    setup() {
      this.control.setAttribute("aria-label", this.name());
      this.control.addEventListener("change", () => this.take(this.control.selectedIndex, true));
      new MutationObserver(() => this.rebuild()).observe(this, { childList: true, subtree: true, characterData: true });
      this.rebuild();
    }

    rebuild() {
      this.control.replaceChildren(...this.choices().map((o) => new Option(o.textContent)));
      this.take(this.moved ? this.current : this.startValue(), false);
    }

    render() { this.control.selectedIndex = this.current; }
    describe() {}
  }

  // --- es-button ----------------------------------------------------------

  // A button that sends 1 while it is held down, by a pointer, Space, or
  // Enter, and 0 when it is let go. Its text is its content, or its label.
  class Button extends Control {
    constructor() {
      super();
      this.pressed = false;
    }

    get value() { return this.pressed ? 1 : 0; }
    bind() {}  // a button sends; it shows nothing

    styles() {
      return `
        .control { padding: 6px 14px; border: 1px solid var(--line); border-radius: 3px; background: var(--surface);
                   cursor: pointer; outline-offset: 2px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .control.down { background: var(--accent); border-color: var(--accent); color: var(--surface); }
      `;
    }

    build(root) {
      const face = div("control");
      const content = document.createElement("slot");
      content.textContent = this.getAttribute("label") || "";
      face.append(content);
      root.append(face);
      return face;
    }

    setup() {
      const c = this.control;
      c.setAttribute("tabindex", "0");
      c.setAttribute("role", "button");
      if (this.hasAttribute("label")) c.setAttribute("aria-label", this.getAttribute("label"));
      c.addEventListener("pointerdown", (e) => {
        try { c.setPointerCapture(e.pointerId); } catch (error) { /* a pointer the browser does not track */ }
        c.focus();
        this.press(true);
        e.preventDefault();
      });
      for (const type of ["pointerup", "pointercancel", "lostpointercapture", "blur"]) {
        c.addEventListener(type, () => this.press(false));
      }
      const held = (e) => e.key === " " || e.key === "Enter";
      c.addEventListener("keydown", (e) => {
        if (!held(e)) return;
        e.preventDefault();
        this.press(true);
      });
      c.addEventListener("keyup", (e) => { if (held(e)) this.press(false); });
    }

    press(down) {
      if (down === this.pressed) return;
      this.pressed = down;
      this.control.classList.toggle("down", down);
      this.report(down ? 1 : 0, true);
    }
  }

  // --- Displays: es-meter, es-lamp, es-value -----------------------------

  // A control that only shows a value: from the piece, or from the page
  // through .value. It sends nothing.
  class Display extends Control {
    constructor() {
      super();
      this.current = 0;
      this.position = 0;
      this.known = false;  // set once it has a value
    }

    get value() { return this.current; }
    set value(v) {
      v = Number(v);
      if (!Number.isFinite(v)) return;
      this.current = v;
      this.position = positionOf(v, this.range());
      this.known = true;
      if (this.control) this.render();
    }

    setup() {
      if (this.hasAttribute("value")) this.value = number(this, "value", 0);
      else this.render();
    }
  }

  // A bar that shows the value it receives through min, max, and scale (0
  // to 1 by default), with a line at the highest value of the last hold
  // milliseconds (1000 by default).
  class Meter extends Display {
    constructor() {
      super();
      this.peak = 0;
      this.peakUntil = 0;
      this.timer = null;
    }

    styles() {
      return barStyles("meter") + `
        .control { overflow: hidden; }
        .peak { position: absolute; background: var(--text); }
        .horizontal .peak { top: 0; bottom: 0; width: 2px; margin-left: -2px; }
        .vertical .peak { left: 0; right: 0; height: 2px; margin-bottom: -2px; }
      `;
    }

    build(root) {
      this.fillBar = div("fill");
      this.peakLine = div("peak");
      const bar = track(this, this.fillBar, this.peakLine);
      bar.setAttribute("role", "meter");
      bar.setAttribute("aria-label", this.name());
      root.append(this.label(), bar);
      return bar;
    }

    render() {
      const now = performance.now();
      if (this.position >= this.peak || now >= this.peakUntil) {
        this.peak = this.position;
        this.peakUntil = now + Math.max(0, number(this, "hold", 1000));
      }
      if (this.peak > this.position && this.timer === null) {
        this.timer = setTimeout(() => {
          this.timer = null;
          this.render();
        }, this.peakUntil - now);
      }
      const side = this.vertical() ? "height" : "width";
      this.fillBar.style[side] = percent(this.position);
      this.peakLine.style[this.vertical() ? "bottom" : "left"] = percent(this.peak);
      this.peakLine.hidden = this.peak <= 0;
      const range = this.range();
      this.control.setAttribute("aria-valuemin", String(range.min));
      this.control.setAttribute("aria-valuemax", String(range.max));
      this.control.setAttribute("aria-valuenow", String(this.current));
    }
  }

  // A light lit by the value it receives through min, max, and scale: off
  // at the bottom of the range and full at the top, in the accent color.
  class Lamp extends Display {
    styles() {
      return `
        .control { width: var(--es-lamp-size, 14px); height: var(--es-lamp-size, 14px); box-sizing: border-box;
                   border-radius: 50%; border: 1px solid var(--line); background: var(--surface); }
      `;
    }

    build(root) {
      const light = div("control");
      light.setAttribute("role", "img");
      root.append(this.label(), light);
      return light;
    }

    render() {
      const p = this.position;
      this.control.style.background =
        p > 0 ? `color-mix(in srgb, var(--accent) ${Math.round(25 + 75 * p)}%, var(--surface))` : "";
      this.control.setAttribute("aria-label", `${this.name()}, ${p > 0 ? "on" : "off"}`);
    }
  }

  // The value it receives as text, in its unit or as one of its options;
  // empty until it has a value.
  class Value extends Display {
    build(root) {
      const shown = div("control value");
      root.append(this.label(), shown);
      return shown;
    }

    render() { this.control.textContent = this.known ? this.text(this.current) : ""; }
  }

  const elements = {
    "es-knob": Knob, "es-slider": Slider, "es-toggle": Toggle, "es-select": Select, "es-button": Button,
    "es-meter": Meter, "es-lamp": Lamp, "es-value": Value,
  };
  for (const [name, type] of Object.entries(elements)) customElements.define(name, type);
  Object.assign(ui, { Control, ValueControl, Display });
})();
