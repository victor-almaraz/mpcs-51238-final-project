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

  // Each unit writes a value as text: "hz" (Hz, kHz), "s" (s, ms below
  // 1 s), "db" (an amplitude in decibels), "%" (0 to 1 as a percent). A
  // page adds its own to espressoUI.units, and its scales to
  // espressoUI.scales, in a script before the document finishes parsing.
  const units = {
    hz: (v) => (Math.abs(v) >= 1000 ? short(v / 1000) + " kHz" : short(v) + " Hz"),
    s: (v) => (Math.abs(v) < 1 ? short(v * 1000) + " ms" : short(v) + " s"),
    db: (v) => (v <= 0 ? "−∞ dB" : short(20 * Math.log10(v)) + " dB"),
    "%": (v) => short(v * 100) + "%",
  };

  // Value v as text in unit, or as a number without one.
  function format(v, unit) {
    if (!Number.isFinite(v)) return String(v);
    return (Object.hasOwn(units, unit) ? units[unit] : short)(v);
  }

  const ui = { scales, units, fit, valueAt, positionOf, format };
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

  // An HTML element of class className, with the part names a page styles
  // it by (es-knob::part(pointer)).
  function div(className, part) {
    const node = document.createElement("div");
    node.className = className;
    if (part) node.setAttribute("part", part);
    return node;
  }

  // A comma-separated list of numbers, or [] when an item is not one.
  function numbers(element, name) {
    const items = (element.getAttribute(name) || "").split(",").filter((t) => t.trim() !== "").map(Number);
    return items.every(Number.isFinite) ? items : [];
  }

  // Keeps a pressed pointer's events on element c, and focuses it.
  function capture(c, e) {
    try { c.setPointerCapture(e.pointerId); } catch (error) { /* a pointer the browser does not track */ }
    c.focus();
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
      this.waiting = false;
    }

    connectedCallback() {
      // A control takes its first value once the document is parsed, so the
      // scales, units, and options a page adds after it are in place.
      if (document.readyState === "loading") {
        if (!this.waiting) {
          this.waiting = true;
          document.addEventListener("DOMContentLoaded", () => {
            this.waiting = false;
            if (this.isConnected) this.connectedCallback();
          }, { once: true });
        }
        return;
      }
      if (!this.shadowRoot) {
        const root = this.attachShadow({ mode: "open" });
        const style = document.createElement("style");
        style.textContent = css + this.styles();
        root.appendChild(style);
        this.control = this.build(root);
        this.setup();
      }
      const address = this.getAttribute("address");
      // The address shows on hover, for wiring an outside OSC controller.
      if (address && !this.hasAttribute("title")) this.setAttribute("title", address);
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
      const label = div("label", "label");
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

    // The text a value reads as: its option (the first for the lowest step
    // of a stepped range), or the value in its unit.
    text(v) {
      const options = (this.getAttribute("options") || "").split(",").filter((s) => s !== "");
      const range = this.range();
      const i = range.step > 0 ? Math.round((v - range.min) / range.step) : v;
      if (options.length > 0 && Number.isInteger(i) && i >= 0 && i < options.length) return options[i];
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

    // Returns it to its value attribute, and sends that, as a double click
    // does: for a page's own reset.
    reset() { this.take(this.startValue(), true); }

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

    // The keyboard's step: one step of a linear stepped range, at least 1%.
    keyStep() {
      const range = this.range();
      const span = Math.abs(range.max - range.min);
      return range.step > 0 && range.scale === "linear" && span > 0 ? Math.max(0.01, range.step / span) : 0.01;
    }

    listen(c) {
      c.setAttribute("tabindex", "0");
      c.setAttribute("role", "slider");
      c.setAttribute("aria-label", this.name());
      if (typeof this.vertical === "function" && this.vertical()) c.setAttribute("aria-orientation", "vertical");
      c.addEventListener("pointerdown", (e) => {
        capture(c, e);
        this.drag = this.dragStart(e);
        e.preventDefault();
      });
      c.addEventListener("pointermove", (e) => { if (this.drag) this.dragTo(this.drag, e); });
      for (const type of ["pointerup", "pointercancel"]) c.addEventListener(type, () => { this.drag = null; });
      c.addEventListener("dblclick", () => this.reset());
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
      this.readout = div("value", "value");
      return this.readout;
    }
  }

  // --- es-knob ------------------------------------------------------------

  const defaultSweep = 135;  // degrees either side of the top

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
  // sweep is the travel either side of the top, in degrees (135); angles,
  // a list of degrees, makes a selector that stops at each and sends its
  // index.
  class Knob extends ValueControl {
    static get observedAttributes() { return ["mod", "sweep"]; }
    attributeChangedCallback() { if (this.mod) this.render(); }

    angles() {
      const angles = numbers(this, "angles");
      return angles.length >= 2 ? angles : null;
    }

    range() {
      const angles = this.angles();
      return angles ? { min: 0, max: angles.length - 1, scale: "linear", step: 1 } : super.range();
    }

    // The pointer's angle at the current value, and the ends of its travel.
    angle() {
      const angles = this.angles();
      if (angles) return angles[Math.round(this.current)];
      const sweep = number(this, "sweep", defaultSweep);
      return -sweep + this.position * 2 * sweep;
    }
    ends() {
      const angles = this.angles();
      const sweep = number(this, "sweep", defaultSweep);
      return angles ? [angles[0], angles[angles.length - 1]] : [-sweep, sweep];
    }

    styles() {
      return `
        svg { width: var(--es-knob-size, 56px); height: var(--es-knob-size, 56px); }
        .control { cursor: ns-resize; }
        .track { fill: none; stroke: var(--line); stroke-width: 6; stroke-linecap: round; }
        .fill { fill: none; stroke: var(--accent); stroke-width: 6; stroke-linecap: round; }
        .mod { fill: none; stroke: var(--es-mod, var(--text)); stroke-width: 2.5; stroke-linecap: round; }
        .label { max-width: calc(3 * var(--es-u, 24px)); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
        .body { fill: var(--surface); stroke: var(--line); }
        .pointer { stroke: var(--text); stroke-width: 4; stroke-linecap: round; }
        .focus { fill: none; stroke: transparent; stroke-width: 2; }
      `;
    }

    build(root) {
      const picture = svg("svg", { viewBox: "0 0 100 100", class: "control", part: "knob" });
      svg("circle", { cx: 50, cy: 50, r: 48, class: "focus", part: "focus" }, picture);
      const [from, to] = this.ends();
      svg("path", { d: arc(40, from, to), class: "track", part: "track" }, picture);
      this.fill = svg("path", { d: "", class: "fill", part: "fill" }, picture);
      // The span its modulations reach, around the track (mod="low,high").
      this.mod = svg("path", { d: "", class: "mod", part: "mod" }, picture);
      svg("circle", { cx: 50, cy: 50, r: 30, class: "body", part: "body" }, picture);
      this.pointer = svg("line", { x1: 50, y1: 42, x2: 50, y2: 24, class: "pointer", part: "pointer" }, picture);
      root.append(this.label(), picture, this.readoutBelow());
      return picture;
    }

    render() {
      const angle = this.angle();
      const from = this.ends()[0];
      this.pointer.setAttribute("transform", `rotate(${angle.toFixed(2)} 50 50)`);
      this.fill.setAttribute("d", angle > from ? arc(40, from, angle) : "");
      // The modulations' span, from the knob's position, kept in its travel.
      const mod = numbers(this, "mod");
      const [low, high] = mod.length === 2 && !this.angles() ? mod : [0, 0];
      const sweep = number(this, "sweep", defaultSweep);
      const at = (p) => -sweep + clamp01(this.position + p) * 2 * sweep;
      this.mod.setAttribute("d", at(high) > at(low) ? arc(45.5, at(low), at(high)) : "");
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
    box.setAttribute("part", "track");
    box.append(...parts);
    return box;
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
      this.fillBar = div("fill", "fill");
      this.thumb = div("thumb", "thumb");
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
        :host { --length: var(--es-toggle-length, 36px); --thickness: var(--es-toggle-thickness, 18px);
                --inset: var(--es-toggle-inset, 2px); }
        .control { position: relative; cursor: pointer; border-radius: 9px; background: var(--line); outline-offset: 3px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .horizontal { width: var(--length); height: var(--thickness); }
        .vertical { width: var(--thickness); height: var(--length); }
        .thumb { position: absolute; width: calc(var(--thickness) - 2 * var(--inset));
                 height: calc(var(--thickness) - 2 * var(--inset)); border-radius: 50%; background: var(--muted); }
        .horizontal .thumb { top: var(--inset); left: var(--inset); }
        .horizontal.on .thumb { left: auto; right: var(--inset); }
        .vertical .thumb { left: var(--inset); bottom: var(--inset); }
        .vertical.on .thumb { bottom: auto; top: var(--inset); }
        .control.on { background: var(--accent); }
        .on .thumb { background: var(--text); }
      `;
    }

    build(root) {
      this.thumb = div("thumb", "thumb");
      const box = track(this, this.thumb);
      root.append(this.label(), box);
      return box;
    }

    // "on" joins the part names while it is on: es-toggle::part(thumb on).
    render() {
      const on = this.current === 1;
      this.control.classList.toggle("on", on);
      this.control.setAttribute("part", on ? "track on" : "track");
      this.thumb.setAttribute("part", on ? "thumb on" : "thumb");
    }
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
      list.setAttribute("part", "select");
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
      const face = div("control", "button");
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
        capture(c, e);
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
      this.control.setAttribute("part", down ? "button down" : "button");
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
      this.fillBar = div("fill", "fill");
      this.peakLine = div("peak", "peak");
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
      const light = div("control", "light");
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
      const shown = div("control value", "value");
      root.append(this.label(), shown);
      return shown;
    }

    render() { this.control.textContent = this.known ? this.text(this.current) : ""; }
  }

  // --- es-keys ------------------------------------------------------------

  const whites = [0, 2, 4, 5, 7, 9, 11];  // the white keys of an octave
  const noteNames = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
  const noteName = (n) => noteNames[n % 12] + (Math.floor(n / 12) - 1);  // C4 is 60

  // The computer keys that play an octave and a third, by position: the
  // home row is the white keys, the row above the black ones.
  const typed = ["KeyA", "KeyW", "KeyS", "KeyE", "KeyD", "KeyF", "KeyT", "KeyG", "KeyY", "KeyH", "KeyU", "KeyJ",
                 "KeyK", "KeyO", "KeyL", "KeyP", "Semicolon"];

  // A keyboard that sends [address, note, velocity] when a key is pressed
  // and [address, note, 0] when it is let go, as oscNotes reads them (io.md
  // §12.10). Attributes: address (default /note), low and high (the range,
  // default 48 to 71), velocity (default 100). Each pointer holds one key and
  // slides from key to key; while it has focus, the computer keyboard plays
  // it from the lowest C (A to ;), and Z and X move that an octave.
  class Keys extends Control {
    constructor() {
      super();
      this.held = new Map();  // a pointer, or a computer key, to the note it holds
      this.octave = 0;        // the computer keyboard's shift, in octaves
    }

    bind() {}  // it sends notes; it shows nothing
    address() { return this.getAttribute("address") || "/note"; }
    low() { return Math.round(number(this, "low", 48)); }
    high() { return Math.max(this.low(), Math.round(number(this, "high", 71))); }

    styles() {
      return `
        :host { display: flex; flex-direction: row; align-items: flex-start; }
        .control { position: relative; display: flex; flex: 1; height: var(--es-keys-height, 120px); outline-offset: 3px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .key { box-sizing: border-box; border: 1px solid var(--line); border-radius: 0 0 3px 3px; }
        .white { flex: 1; background: var(--es-key-white, #ece9e2); margin-right: -1px; }
        .black { position: absolute; top: 0; height: 60%; z-index: 1; background: var(--es-key-black, #161616); }
        .key.down { background: var(--accent); }
      `;
    }

    build(root) {
      const board = div("control", "keys");
      const low = this.low();
      const high = this.high();
      const isWhite = (n) => whites.includes(n % 12);
      let count = 0;
      for (let n = low; n <= high; ++n) if (isWhite(n)) ++count;
      const width = 100 / Math.max(1, count);  // one white key, in percent
      let before = 0;                           // white keys left of n
      this.keyOf = new Map();
      for (let n = low; n <= high; ++n) {
        const key = div(isWhite(n) ? "key white" : "key black", isWhite(n) ? "key white" : "key black");
        key.dataset.note = String(n);
        if (isWhite(n)) {
          ++before;
        } else {
          key.style.left = Math.max(0, before * width - 0.3 * width).toFixed(3) + "%";
          key.style.width = (0.6 * width).toFixed(3) + "%";
        }
        this.keyOf.set(n, key);
        board.appendChild(key);
      }
      root.appendChild(board);
      return board;
    }

    setup() {
      const c = this.control;
      c.setAttribute("tabindex", "0");
      c.setAttribute("role", "group");
      c.setAttribute("aria-label",
                     `${this.getAttribute("label") || "Keyboard"}, ${noteName(this.low())} to ${noteName(this.high())}`);
      const under = (e) => {
        const key = this.shadowRoot.elementFromPoint(e.clientX, e.clientY);
        return key && key.dataset && key.dataset.note !== undefined ? Number(key.dataset.note) : null;
      };
      c.addEventListener("pointerdown", (e) => {
        capture(c, e);
        this.hold(e.pointerId, under(e));
        e.preventDefault();
      });
      c.addEventListener("pointermove", (e) => {
        if (!this.held.has(e.pointerId)) return;
        const note = under(e);
        if (note !== null && note !== this.held.get(e.pointerId)) this.hold(e.pointerId, note);
      });
      for (const type of ["pointerup", "pointercancel"]) c.addEventListener(type, (e) => this.hold(e.pointerId, null));
      c.addEventListener("keydown", (e) => {
        if (e.code === "KeyZ" || e.code === "KeyX") {
          if (!e.repeat) this.octave = Math.min(10, Math.max(-10, this.octave + (e.code === "KeyZ" ? -1 : 1)));
          return;
        }
        const step = typed.indexOf(e.code);
        if (step < 0 || e.ctrlKey || e.metaKey || e.altKey) return;
        e.preventDefault();
        if (e.repeat || this.held.has(e.code)) return;
        const note = Math.ceil(this.low() / 12) * 12 + 12 * this.octave + step;
        if (note >= this.low() && note <= this.high()) this.hold(e.code, note);
      });
      c.addEventListener("keyup", (e) => this.hold(e.code, null));
      c.addEventListener("blur", () => { for (const who of [...this.held.keys()]) this.hold(who, null); });
    }

    // Makes who (a pointer or a computer key) hold note, or nothing when
    // note is null, letting go of what it held.
    hold(who, note) {
      const before = this.held.get(who);
      if (before !== undefined) {
        this.held.delete(who);
        this.sound(before, 0);
      }
      if (note === null) return;
      this.held.set(who, note);
      this.sound(note, Math.round(number(this, "velocity", 100)));
    }

    // A note sounds from its first holder's press to its last one's release.
    sound(note, velocity) {
      const holders = [...this.held.values()].filter((n) => n === note).length;
      const key = this.keyOf.get(note);
      if (key) {
        key.classList.toggle("down", holders > 0);
        key.setAttribute("part", key.className);
      }
      if (holders !== (velocity > 0 ? 1 : 0)) return;
      if (globalThis.espresso) espresso.send(this.address(), note, velocity);
      this.dispatchEvent(new CustomEvent("input", { detail: { note, velocity, fromPage: true } }));
    }
  }

  // --- es-xy --------------------------------------------------------------

  // A square that sends two values, [address, x, y], as a point moves on it:
  // a press or a drag puts the point under the pointer, the arrow keys move
  // it, and a double click returns it to its value ("x,y"). min, max, scale,
  // and step apply to both; y rises upward. It takes the values the piece
  // sends and sends its own again each time the socket opens.
  class XY extends Control {
    constructor() {
      super();
      this.current = [0, 0];
      this.positions = [0, 0];
      this.dragging = false;
    }

    get value() { return this.current.slice(); }
    set value(v) { if (Array.isArray(v)) this.take(Number(v[0]), Number(v[1]), false); }

    bind(address) {
      espresso.on(address, (x, y) => { if (typeof x === "number" && typeof y === "number") this.take(x, y, false); });
      espresso.onOpen(() => { if (this.isConnected) espresso.send(address, ...this.current); });
    }

    startValue() {
      const text = (this.getAttribute("value") || "").trim();
      const parts = text === "" ? [] : text.split(",").map(Number);
      const min = this.range().min;
      return [0, 1].map((i) => (Number.isFinite(parts[i]) ? parts[i] : min));
    }

    styles() {
      return `
        .control { position: relative; width: var(--es-xy-size, 160px); height: var(--es-xy-size, 160px);
                   box-sizing: border-box; border: 1px solid var(--line); border-radius: 3px; background: var(--surface);
                   cursor: crosshair; outline-offset: 3px; overflow: hidden; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .h, .v { position: absolute; background: var(--line); }
        .h { left: 0; right: 0; height: 1px; }
        .v { top: 0; bottom: 0; width: 1px; }
        .point { position: absolute; width: 14px; height: 14px; margin: -7px 0 0 -7px; border-radius: 50%;
                 background: var(--accent); }
      `;
    }

    build(root) {
      this.lines = [div("v", "line"), div("h", "line")];
      this.point = div("point", "point");
      const pad = div("control", "pad");
      pad.append(...this.lines, this.point);
      this.readout = div("value", "value");
      root.append(this.label(), pad, this.readout);
      return pad;
    }

    setup() {
      const c = this.control;
      c.setAttribute("tabindex", "0");
      c.setAttribute("role", "slider");
      c.setAttribute("aria-roledescription", "xy pad");
      c.setAttribute("aria-label", this.name());
      const at = (e) => {
        const box = c.getBoundingClientRect();
        return [box.width > 0 ? (e.clientX - box.left) / box.width : 0,
                box.height > 0 ? (box.bottom - e.clientY) / box.height : 0];
      };
      c.addEventListener("pointerdown", (e) => {
        capture(c, e);
        this.dragging = true;
        this.moveTo(at(e));
        e.preventDefault();
      });
      c.addEventListener("pointermove", (e) => { if (this.dragging) this.moveTo(at(e)); });
      for (const type of ["pointerup", "pointercancel"]) c.addEventListener(type, () => { this.dragging = false; });
      c.addEventListener("dblclick", () => this.take(...this.startValue(), true));
      c.addEventListener("keydown", (e) => {
        const moves = { ArrowRight: [0.01, 0], ArrowLeft: [-0.01, 0], ArrowUp: [0, 0.01], ArrowDown: [0, -0.01] };
        if (!(e.key in moves)) return;
        e.preventDefault();
        this.moveTo(this.positions.map((p, i) => p + moves[e.key][i]));
      });
      this.take(...this.startValue(), false);
    }

    moveTo(ps) {
      const range = this.range();
      this.take(valueAt(clamp01(ps[0]), range), valueAt(clamp01(ps[1]), range), true);
    }

    take(x, y, send) {
      if (!Number.isFinite(x) || !Number.isFinite(y)) return;
      const range = this.range();
      const next = [fit(x, range), fit(y, range)];
      const changed = next[0] !== this.current[0] || next[1] !== this.current[1];
      this.current = next;
      this.positions = next.map((v) => positionOf(v, range));
      if (this.control) this.render();
      if (!changed) return;
      const address = this.getAttribute("address");
      if (send && address && globalThis.espresso) espresso.send(address, ...next);
      this.dispatchEvent(new CustomEvent("input", { detail: { value: next.slice(), fromPage: send } }));
    }

    render() {
      const [x, y] = this.positions;
      this.point.style.left = this.lines[0].style.left = percent(x);
      this.point.style.top = this.lines[1].style.top = percent(1 - y);
      const text = this.current.map((v) => this.text(v)).join(", ");
      this.control.setAttribute("aria-valuetext", text);
      this.readout.textContent = text;
    }
  }

  // --- Layout (dsl-ui.md §3, patterns.md §28.7) ---------------------------
  // A page is laid out on one unit, --es-u (24 px). es-panel holds sections
  // side by side (flow="column" stacks them), and its other children (a
  // header, a footer) across their width. es-section is a grid of units,
  // titled by its label, in which each control fills its footprint: a knob
  // 3 x 4 units, a big one (size="big") 4 x 5. Controls flow in the order
  // written, in one row, a control that fits the section's height below the
  // one before stacking under it; columns="n" makes rows n units wide, filled
  // in turn. span="w h" and at="column row" (from 1) place a child by hand.
  // A control sits centred in its footprint; any other child fills its cells.
  // Neither element has a shadow root: a page styles both, and what they
  // hold, as its own HTML.

  const footprints = [
    // selector, columns, rows, and the sizes that fit the control in them
    ["es-knob", 3, 4], ["es-knob[size=big]", 4, 5, "--es-knob-size: 80px"],
    ["es-toggle", 3, 3], ["es-button", 3, 3], ["es-lamp", 3, 2], ["es-value", 3, 2], ["es-select", 4, 2],
    ["es-slider", 6, 2], ["es-slider[orientation=vertical]", 2, 6],
    ["es-meter", 6, 2], ["es-meter[orientation=vertical]", 2, 6],
    ["es-xy", 8, 8, "--es-xy-size: calc(8 * var(--es-u) - 40px)"], ["es-keys", 7, 5, "justify-self: stretch"],
    ["es-footswitch", 3, 3], ["es-drawbar", 2, 6],
  ];
  const layoutCss = `
    es-panel, es-section { --es-u: 24px; box-sizing: border-box; }
    es-panel { display: grid; gap: var(--es-u); padding: var(--es-u); align-items: start; justify-content: start; }
    es-panel > :not(es-section) { grid-column: 1 / -1; }
    es-section {
      --es-knob-size: 56px; --es-slider-length: calc(6 * var(--es-u) - 38px);
      --es-meter-length: calc(6 * var(--es-u) - 38px); --es-keys-height: calc(5 * var(--es-u));
      --es-drawbar-length: calc(6 * var(--es-u) - 62px);
      position: relative; display: grid; grid-auto-flow: column; grid-auto-columns: var(--es-u);
      grid-auto-rows: var(--es-u); align-content: start; justify-content: start;
    }
    es-section[columns] { grid-auto-flow: row; }
    es-section[label] { padding-top: var(--es-u); }
    es-section[label]::before {
      content: attr(label); position: absolute; inset: 0 0 auto 0; height: var(--es-u);
      padding-inline: inherit; box-sizing: border-box;
      font: var(--es-heading-font, 700 10px/var(--es-u) system-ui, sans-serif); letter-spacing: 0.2em;
      text-transform: uppercase; color: var(--es-heading, var(--es-muted, #8f8b83)); white-space: nowrap;
    }
    es-section > * { justify-self: stretch; align-self: stretch; }
    es-screen { display: block; position: relative; box-sizing: border-box; }
    es-pedalboard { --es-u: 24px; display: flex; flex-wrap: wrap; align-items: flex-start; gap: var(--es-u);
                    padding: var(--es-u) calc(2 * var(--es-u)); border-radius: 12px;
                    background: repeating-linear-gradient(90deg, #1d1d1f 0 3px, #202022 3px 6px);
                    box-shadow: inset 0 0 0 1px #ffffff0d, 0 24px 60px #000c; }
    es-pedalboard > es-pedal:nth-child(6n + 1) { --es-paint: #e0622f; }
    es-pedalboard > es-pedal:nth-child(6n + 2) { --es-paint: #3f8fb8; }
    es-pedalboard > es-pedal:nth-child(6n + 3) { --es-paint: #7d66d9; }
    es-pedalboard > es-pedal:nth-child(6n + 4) { --es-paint: #3aa76d; }
    es-pedalboard > es-pedal:nth-child(6n + 5) { --es-paint: #d9a62e; }
    es-pedalboard > es-pedal:nth-child(6n + 6) { --es-paint: #c4486b; }
    es-synth > es-section:not([slot="keybed"]) ~ es-section:not([slot="keybed"]) {
      margin-left: var(--es-u); padding-left: var(--es-u); border-left: 1px solid #ffffff14; }
    /* The face is dark in every theme: its text is light. */
    es-synth { --es-heading: var(--es-accent, #4fd1c5); --es-text: #ece7dc; --es-muted: #b4ad9f; }
    es-synth > es-section[slot="keybed"] { display: block; margin: 0; padding: 8px 8px 0; border: 0; border-radius: 4px;
                                           background: #121110; box-shadow: inset 0 2px 6px #000c; }
    es-synth > es-section[slot="keybed"] > es-keys { display: block; width: 100%; }
    es-pedal > es-lamp[slot="led"]::part(label) { display: none; }
    es-pedal:has(> es-lamp[slot="led"]) > es-footswitch { --es-footswitch-on: #ffd84a; }
    es-section > :is(${[...new Set(footprints.map(([selector]) => selector.split("[")[0]))].join(", ")}) {
      justify-self: center; align-self: center;
    }
    ${footprints.map(([selector, w, h, more]) =>
      `es-section > ${selector} { grid-column-end: span ${w}; grid-row-end: span ${h}; ${more || ""} }`).join("\n    ")}
  `;
  {
    const style = document.createElement("style");
    style.id = "es-layout";
    style.textContent = layoutCss;
    document.head.prepend(style);  // first, so the page's own rules win
  }

  // Two whole numbers from attribute name ("3 4", "3,4"), or null.
  function pair(element, name) {
    const parts = (element.getAttribute(name) || "").split(/[\s,]+/).filter((t) => t !== "").map(Number);
    return parts.length === 2 && parts.every((n) => Number.isInteger(n) && n > 0) ? parts : null;
  }

  // A layout element: lays out its children once the document is parsed,
  // and again when they or its attributes change.
  class Layout extends HTMLElement {
    static get observedAttributes() { return ["columns", "label", "flow", "size"]; }
    connectedCallback() {
      if (!this.observer) {
        this.observer = new MutationObserver(() => this.layout());
        this.observer.observe(this, { childList: true, subtree: true, attributes: true, attributeFilter: ["span", "at", "low", "high", "label"] });
      }
      if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", () => this.layout(), { once: true });
      } else {
        this.layout();
      }
    }
    disconnectedCallback() { this.observer?.disconnect(); this.observer = null; }
    attributeChangedCallback() { if (this.isConnected) this.layout(); }
  }

  // es-panel: sections side by side, or stacked with flow="column".
  class Panel extends Layout {
    layout() {
      const sections = [...this.children].filter((c) => c.localName === "es-section").length;
      const row = this.getAttribute("flow") !== "column";
      this.style.gridTemplateColumns = row && sections > 0 ? `repeat(${sections}, max-content)` : "";
      sized(this);
      if (!this.hasAttribute("role")) this.setAttribute("role", "group");
      if (this.hasAttribute("label")) this.setAttribute("aria-label", this.getAttribute("label"));
    }
  }

  // es-section: a titled grid of units (see above).
  // Sizes an element by its size="w h" attribute, in units: its width and
  // height (or the properties given, min-width and min-height for a group
  // that grows with what it holds).
  function sized(element, width = "minWidth", height = "minHeight") {
    const size = pair(element, "size");
    element.style[width === "width" ? "width" : "minWidth"] = size ? `calc(${size[0]} * var(--es-u, 24px))` : "";
    element.style[height === "height" ? "height" : "minHeight"] = size ? `calc(${size[1]} * var(--es-u, 24px))` : "";
  }

  // The width of a section's title as drawn, so the section is at least as
  // wide and its title never runs into the next.
  const titleRuler = document.createElement("canvas").getContext("2d");
  function titleWidth(section) {
    const label = section.getAttribute("label");
    if (!label || !titleRuler) return 0;
    const style = getComputedStyle(section, "::before");
    titleRuler.font = style.font;
    const spacing = parseFloat(style.letterSpacing) || 0;
    return titleRuler.measureText(label.toUpperCase()).width + spacing * label.length;
  }

  class Section extends Layout {
    layout() {
      // Its columns: as many as it says, or as wide as the size the
      // compiler computed, so a child a script adds wraps within it.
      const size = pair(this, "size");
      const given = Number(this.getAttribute("columns"));
      const columns = Number.isInteger(given) && given > 0 ? given : size ? size[0] : 0;
      this.style.gridTemplateColumns = columns > 0 ? `repeat(${columns}, var(--es-u))` : "";
      // At least as wide as its title, in whole units.
      const unit = parseFloat(getComputedStyle(this).getPropertyValue("--es-u")) || 24;
      const title = titleWidth(this);
      this.style.minWidth = title > 0 ? `${Math.ceil(title / unit) * unit}px` : "";
      this.setAttribute("role", "group");
      if (this.hasAttribute("label")) this.setAttribute("aria-label", this.getAttribute("label"));
      for (const child of this.children) {
        const span = pair(child, "span");
        const at = pair(child, "at");
        let width = span ? span[0] : null;
        // A keyboard is as many units wide as it has white keys.
        if (!width && child.localName === "es-keys") {
          const low = Math.round(number(child, "low", 48));
          const high = Math.max(low, Math.round(number(child, "high", 71)));
          width = 0;
          for (let n = low; n <= high; ++n) if (whites.includes(((n % 12) + 12) % 12)) ++width;
        }
        child.style.gridColumnEnd = width ? `span ${width}` : "";
        child.style.gridRowEnd = span ? `span ${span[1]}` : "";
        child.style.gridColumnStart = at ? String(at[0]) : "";
        child.style.gridRowStart = at ? String(at[1]) : "";
      }
    }
  }

  customElements.define("es-panel", Panel);
  customElements.define("es-section", Section);
  // --- Kits: synthesizer panels and pedals (dsl-ui.md §6) -----------------
  // es-footswitch, a pedal's stomp switch: a toggle drawn as a chrome
  // button, 1 while the effect is engaged (the default). es-drawbar, an
  // organ's drawbar: a slider of 0 to 8, pulled down to raise it.
  // es-status, the connection's state as text. es-synth, a panel between
  // wooden cheeks under its model's name (label) and maker; es-pedal, an
  // enclosure with its knobs on a centred grid over its name, an LED, and
  // its footswitch; es-pedalboard, pedals in a row in the order of their
  // chain.

  class Footswitch extends Toggle {
    startValue() { return number(this, "value", 1); }

    styles() {
      return `
        :host { --size: var(--es-footswitch-size, 64px); }
        .control { position: relative; width: var(--size); height: var(--size); border-radius: 50%; cursor: pointer;
                   background: radial-gradient(circle at 40% 35%, #fff, #b9bcc0 35%, #6b6f74 70%, #3a3d41);
                   box-shadow: 0 0 0 6px #2b2d30, 0 0 0 7px #0008, 0 6px 10px #000a; outline-offset: 8px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .control.down { transform: translateY(2px); box-shadow: 0 0 0 6px #2b2d30, 0 0 0 7px #0008, 0 2px 4px #000a; }
        /* Engaged, on a pedal whose LED is a lamp of something else: a ring of light. */
        .control.on { box-shadow: 0 0 0 6px #2b2d30, 0 0 0 8px var(--es-footswitch-on, #2b2d30), 0 6px 10px #000a; }
        .label { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); }
      `;
    }

    build(root) {
      const box = document.createElement("div");
      box.className = "control";
      box.setAttribute("part", "track");
      for (const type of ["pointerdown", "pointerup", "pointerleave", "pointercancel"])
        box.addEventListener(type, () => box.classList.toggle("down", type === "pointerdown"));
      root.append(this.label(), box);
      return box;
    }

    render() {
      const on = this.current === 1;
      this.control.setAttribute("part", on ? "track on" : "track");
      this.control.classList.toggle("on", on);
    }
  }

  class Drawbar extends Slider {
    vertical() { return true; }

    range() {
      return { min: number(this, "min", 0), max: number(this, "max", 8), scale: "linear", step: number(this, "step", 1) };
    }

    styles() {
      // Room above and below the track for the tip, which overhangs its ends.
      return barStyles("drawbar") + `
        .control { cursor: pointer; outline-offset: 3px; margin: 12px 0; background: #0006; border-radius: 4px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .fill { top: 0; bottom: auto; border-radius: 4px; background: var(--es-drawbar-rod, #8a8780); }
        .thumb { position: absolute; left: 50%; width: 26px; height: 22px; border-radius: 4px; transform: translate(-50%, -50%);
                 background: var(--es-drawbar-tip, #f2efe6); box-shadow: 0 2px 4px #000a; }
      `;
    }

    // Pulled down from the top: 0 at the top, the most at the bottom.
    render() {
      const at = percent(this.position);
      this.fillBar.style.height = at;
      this.thumb.style.bottom = "auto";
      this.thumb.style.top = at;
    }

    at(e) {
      const box = this.control.getBoundingClientRect();
      return box.height > 0 ? (e.clientY - box.top) / box.height : 0;
    }
  }

  class Status extends HTMLElement {
    connectedCallback() {
      this.setAttribute("role", "status");
      if (!this.textContent) this.textContent = "Connecting…";
      if (globalThis.espresso && !this.bound) {
        this.bound = true;
        espresso.onOpen(() => { this.textContent = "Connected"; });
        if (espresso.onClose) espresso.onClose(() => { this.textContent = "Disconnected · reconnecting…"; });
      }
    }
  }

  // A kit drawn in a shadow root around its children, which keep the
  // page's styles: label and the attributes observed name its text.
  class Kit extends HTMLElement {
    static get observedAttributes() { return ["label", "maker"]; }
    connectedCallback() {
      if (!this.shadowRoot) {
        const root = this.attachShadow({ mode: "open" });
        root.innerHTML = `<style>${this.styles()}</style>${this.markup()}`;
        this.setup(root);
      }
      this.setAttribute("role", "group");
      this.update();
    }
    attributeChangedCallback() { if (this.shadowRoot) this.update(); }
    setup() {}
    update() {
      const label = this.getAttribute("label") || "";
      if (label) this.setAttribute("aria-label", label);
      for (const node of this.shadowRoot.querySelectorAll("[data-text]"))
        node.textContent = this.getAttribute(node.dataset.text) || "";
    }
  }

  class Synth extends Kit {
    static get observedAttributes() { return ["label", "maker", "size"]; }
    update() {
      super.update();
      sized(this);
    }

    styles() {
      return `
        :host { display: inline-grid; grid-template-columns: var(--es-u, 24px) auto var(--es-u, 24px);
                border-radius: 8px; box-shadow: 0 28px 70px #000d; color: var(--es-text, #ece7dc); }
        .cheek { background: var(--es-cheek, linear-gradient(90deg, #4a2c18, #7b4d2c 35%, #5c371e 70%, #3b2313)); }
        .cheek:first-child { border-radius: 8px 0 0 8px; }
        .cheek:last-child { border-radius: 0 8px 8px 0; }
        .face { padding: var(--es-u, 24px); background: var(--es-face, linear-gradient(#34322f, #24231f));
                box-shadow: inset 0 1px 0 #ffffff22, inset 0 -2px 0 #00000055; }
        header { height: calc(2 * var(--es-u, 24px)); margin-bottom: var(--es-u, 24px); display: flex;
                 align-items: center; justify-content: space-between; gap: calc(2 * var(--es-u, 24px));
                 border-bottom: 1px solid #ffffff1c; }
        .model { font: 800 24px/1 system-ui, sans-serif; letter-spacing: 0.24em; text-transform: uppercase; }
        .maker { font: 600 10px/1 system-ui, sans-serif; letter-spacing: 0.32em; text-transform: uppercase;
                 color: var(--es-muted, #a39c8f); }
        .sections { display: flex; align-items: flex-start; }
        .keybed { display: block; margin-top: var(--es-u, 24px); }
        :host(:not([keybed])) .keybed { display: none; }
      `;
    }
    markup() {
      return `<div class="cheek"></div><div class="face" part="face">
        <header part="header"><div class="model" part="model" data-text="label"></div>
        <div class="maker" part="maker" data-text="maker"></div></header>
        <div class="sections"><slot></slot></div><slot class="keybed" name="keybed"></slot></div>
        <div class="cheek"></div>`;
    }
    // A section that holds a keyboard is the keybed: under the other
    // sections, as wide as the panel. Watched as sections and keyboards
    // come and go.
    setup(root) {
      const place = () => {
        for (const child of this.children) {
          if (child.localName !== "es-section") continue;
          const keys = Boolean(child.querySelector(":scope > es-keys"));
          if (keys && !child.slot) child.slot = "keybed";
          else if (!keys && child.slot === "keybed") child.slot = "";
        }
        this.toggleAttribute("keybed", [...this.children].some((c) => c.slot === "keybed"));
      };
      root.querySelector("slot:not([name])").addEventListener("slotchange", place);
      new MutationObserver(place).observe(this, { childList: true, subtree: true });
      place();
    }
  }

  class Pedal extends Kit {
    styles() {
      return `
        :host { --u: var(--es-u, 24px); --paint: var(--es-paint, #3f8fb8); --ink: var(--es-ink, #0d2a38);
                position: relative; display: inline-flex; flex-direction: column; align-items: center;
                box-sizing: border-box; width: calc(10 * var(--u)); min-height: calc(18 * var(--u));
                padding: var(--u); border-radius: 16px; color: var(--ink);
                background: linear-gradient(160deg, color-mix(in srgb, var(--paint), #fff 30%), var(--paint) 40%,
                                            color-mix(in srgb, var(--paint), #000 40%));
                box-shadow: inset 0 2px 0 #ffffff55, inset 0 -4px 0 #00000066, 0 14px 26px #000a; }
        :host([wide]) { width: calc(14 * var(--u)); }
        /* A size the compiler computed (size="w h") or the page gave. */
        :host([size]) { min-height: 0; }
        :host([size]) .knobs { min-height: 0; }
        :host([size]:not([footswitch])) .foot { display: none; }
        .screw { position: absolute; width: 9px; height: 9px; border-radius: 50%;
                 background: radial-gradient(circle at 35% 35%, #f2f2f2, #8a8a8a 60%, #4a4a4a); }
        .screw:nth-of-type(1) { top: 9px; left: 9px; } .screw:nth-of-type(2) { top: 9px; right: 9px; }
        .screw:nth-of-type(3) { bottom: 9px; left: 9px; } .screw:nth-of-type(4) { bottom: 9px; right: 9px; }
        /* At a fixed height from the top, so pedals of every size on a board connect level. */
        .jack { position: absolute; top: calc(2 * var(--u)); width: 10px; height: 26px;
                background: linear-gradient(#5a5a5a, #2a2a2a); }
        .rows { display: flex; flex-direction: column; width: 100%; }
        .row { display: flex; justify-content: center; align-items: flex-start; column-gap: var(--u); width: 100%; }
        .jack.in { left: -10px; border-radius: 3px 0 0 3px; }
        .jack.out { right: -10px; border-radius: 0 3px 3px 0; }
        .knobs { display: flex; flex-wrap: wrap; justify-content: center; align-content: flex-start;
                 column-gap: var(--u); width: 100%; min-height: calc(8 * var(--u)); }
        ::slotted(*) { display: inline-flex; justify-content: center; align-items: center;
                       --es-accent: #f3efe2; --es-surface: #f3efe2; --es-line: #00000040;
                       --es-text: var(--ink); --es-muted: var(--ink); --es-focus: #111;
                       --es-slider-length: calc(6 * var(--u) - 32px); --es-meter-length: calc(6 * var(--u) - 32px); }
        /* A knob's footprint; other controls keep their own size. */
        ::slotted(es-knob) { width: calc(3 * var(--u)); height: calc(4 * var(--u)); }
        .name { margin-top: auto; height: calc(2 * var(--u)); display: flex; align-items: center;
                font: 900 26px/1 system-ui, sans-serif; letter-spacing: 0.08em; text-transform: uppercase; }
        .light { height: var(--u); display: flex; align-items: center; justify-content: center; }
        .led { width: 12px; height: 12px; border-radius: 50%; background: #4a1210; box-shadow: inset 0 0 0 1px #0008; }
        .light ::slotted(es-lamp) { width: auto; height: auto; --es-lamp-size: 12px; --es-accent: #ff3b30;
                                    --es-surface: #4a1210; --es-line: #0008; }
        .led.on { background: #ff3b30; box-shadow: 0 0 10px #ff3b30, inset 0 0 0 1px #0004; }
        .foot { height: calc(4 * var(--u)); display: flex; align-items: center; justify-content: center; }
        .foot ::slotted(*) { width: auto; height: auto; }
      `;
    }
    markup() {
      return `<span class="screw"></span><span class="screw"></span><span class="screw"></span>
        <span class="screw"></span><span class="jack in"></span><span class="jack out"></span>
        <div class="knobs" part="knobs"><div class="rows"></div><slot></slot></div>
        <div class="name" part="name" data-text="label"></div>
        <div class="light"><slot name="led"><div class="led" part="led"></div></slot></div>
        <div class="foot"><slot name="footswitch"></slot></div>`;
    }
    setup(root) {
      this.led = root.querySelector(".led");
      // The footswitch goes under the name, and a lamp, when the pedal has
      // one, is its LED; more than four knobs widen the enclosure, as a
      // 1590BB is wider than a 125B.
      const watched = new WeakSet();
      const place = () => {
        for (const child of this.children) {
          if (child.localName === "es-lamp" && !child.slot) child.slot = "led";
          if (child.localName !== "es-footswitch") continue;
          if (child.slot !== "footswitch") child.slot = "footswitch";
          if (!watched.has(child)) {
            watched.add(child);
            child.addEventListener("input", () => this.light());
          }
        }
        // Controls the compiler placed (at="column row") go in rows by their
        // row, each row centred, each control its span's size.
        const rows = root.querySelector(".rows");
        const tops = new Set();
        for (const child of this.children) {
          if (child.slot === "footswitch" || child.slot === "led") continue;
          const at = pair(child, "at");
          const span = pair(child, "span");
          if (!at) continue;
          tops.add(at[1]);
          if (child.slot !== `row-${at[1]}`) child.slot = `row-${at[1]}`;
          if (span && child.localName !== "es-knob") {
            child.style.width = `calc(${span[0]} * var(--u))`;
            child.style.height = `calc(${span[1]} * var(--u))`;
          }
        }
        const wanted = [...tops].sort((a, b) => a - b).map((t) => `row-${t}`).join(" ");
        if (rows.dataset.rows !== wanted) {
          rows.dataset.rows = wanted;
          rows.replaceChildren(...[...tops].sort((a, b) => a - b).map((t) => {
            const row = document.createElement("div");
            row.className = "row";
            const slot = document.createElement("slot");
            slot.name = `row-${t}`;
            row.append(slot);
            return row;
          }));
        }
        const knobs = [...this.children].filter((c) => c.slot !== "footswitch" && c.slot !== "led").length;
        // A pedal without a size (a page written by hand) takes a 125B's,
        // or a 1590BB's for more than four knobs.
        this.toggleAttribute("wide", !this.hasAttribute("size") && knobs > 4);
        this.toggleAttribute("footswitch", Boolean(this.querySelector(":scope > es-footswitch")));
        this.light();
      };
      root.querySelector("slot:not([name])").addEventListener("slotchange", place);
      place();
    }
    static get observedAttributes() { return ["label", "maker", "size"]; }
    update() {
      super.update();
      sized(this, "width", "height");
    }
    // The LED, without a lamp, is lit while the footswitch engages the effect.
    light() {
      const foot = this.querySelector(":scope > es-footswitch");
      this.led.classList.toggle("on", !foot || foot.value === 1);
    }
  }

  // A row of pedals: a group to assistive technology, named by its label.
  class Pedalboard extends HTMLElement {
    connectedCallback() {
      this.setAttribute("role", "group");
      this.setAttribute("aria-label", this.getAttribute("label") || "Pedalboard");
      sized(this);
    }
  }

  // An area of its span with no address, which a page's own script draws
  // in, finding it by its label: es-screen[label="Tables"].
  class Screen extends HTMLElement {
    connectedCallback() {
      if (this.hasAttribute("label")) this.setAttribute("aria-label", this.getAttribute("label"));
    }
  }

  customElements.define("es-status", Status);
  customElements.define("es-screen", Screen);
  customElements.define("es-pedalboard", Pedalboard);
  customElements.define("es-synth", Synth);
  customElements.define("es-pedal", Pedal);

  const elements = {
    "es-knob": Knob, "es-slider": Slider, "es-toggle": Toggle, "es-select": Select, "es-button": Button,
    "es-meter": Meter, "es-lamp": Lamp, "es-value": Value, "es-keys": Keys, "es-xy": XY,
    "es-footswitch": Footswitch, "es-drawbar": Drawbar,
  };
  for (const [name, type] of Object.entries(elements)) customElements.define(name, type);
  Object.assign(ui, { Control, ValueControl, Display });
})();
