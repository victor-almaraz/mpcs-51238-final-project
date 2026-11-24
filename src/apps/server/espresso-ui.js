// espresso-ui.js: page controls bound to a piece's OSC addresses (ui.md),
// served by the page port at /espresso-ui.js. A page loads it after
// /espresso.js:
//
//   <script src="/espresso.js"></script>
//   <script src="/espresso-ui.js"></script>
//   <es-knob address="/cutoff" label="Cutoff" min="20" max="20000"
//            scale="exp" value="800" unit="hz"></es-knob>
//
// A control sends its value to its address when the user changes it, shows
// the values the piece sends to that address, and sends its value again
// each time the socket opens, so the piece starts where the page is. Every
// control also fires an "input" event, with the value in event.detail, as
// it changes. Colors and sizes come from CSS custom properties (--es-*).
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
    .control { outline: none; cursor: ns-resize; }
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

  // --- The base of a value control (ui.md §5) ----------------------------

  // A control with a value on an address: subclasses draw it and say how a
  // pointer moves it. Attributes: address, label, min, max, scale, step,
  // value (the start, and what a reset returns to), unit, and steps (a
  // selector of that many positions, which sends its index).
  class ValueControl extends HTMLElement {
    constructor() {
      super();
      this.position = 0;
      this.current = 0;
      this.drag = null;
      this.bound = false;
    }

    // The range the attributes give.
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

    // The value, as the piece last heard it or the page last set it.
    get value() { return this.current; }
    set value(v) { this.take(Number(v), false); }

    connectedCallback() {
      if (!this.shadowRoot) {
        const root = this.attachShadow({ mode: "open" });
        const style = document.createElement("style");
        style.textContent = css + this.styles();
        root.appendChild(style);
        this.control = this.build(root);
        this.listen(this.control);
        this.take(number(this, "value", this.range().min), false);
      }
      if (!this.bound && globalThis.espresso) {
        this.bound = true;
        const address = this.getAttribute("address");
        if (address) {
          espresso.on(address, (v) => { if (typeof v === "number") this.take(v, false); });
          espresso.onOpen(() => { if (this.isConnected) espresso.send(address, this.current); });
        }
      }
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
      this.render();
      this.describe();
      if (!changed) return;
      if (send) {
        const address = this.getAttribute("address");
        if (address && globalThis.espresso) espresso.send(address, next);
      }
      this.dispatchEvent(new CustomEvent("input", { detail: { value: next, fromPage: send } }));
    }

    // Moves to position p, as the user does.
    moveTo(p) { this.take(valueAt(clamp01(p), this.range()), true); }

    // The text a value reads as.
    text(v) {
      const options = (this.getAttribute("options") || "").split(",").filter((s) => s !== "");
      if (options.length > 0 && Number.isInteger(v) && v >= 0 && v < options.length) return options[v];
      return format(v, this.getAttribute("unit") || "");
    }

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
      c.setAttribute("aria-label", this.getAttribute("label") || this.getAttribute("address") || "");
      c.addEventListener("pointerdown", (e) => {
        try { c.setPointerCapture(e.pointerId); } catch (error) { /* a pointer the browser does not track */ }
        c.focus();
        this.drag = this.dragStart(e);
        e.preventDefault();
      });
      c.addEventListener("pointermove", (e) => { if (this.drag) this.dragTo(this.drag, e); });
      for (const type of ["pointerup", "pointercancel"]) c.addEventListener(type, () => { this.drag = null; });
      c.addEventListener("dblclick", () => this.take(number(this, "value", this.range().min), true));
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
        .track { fill: none; stroke: var(--line); stroke-width: 6; stroke-linecap: round; }
        .fill { fill: none; stroke: var(--accent); stroke-width: 6; stroke-linecap: round; }
        .body { fill: var(--surface); stroke: var(--line); }
        .pointer { stroke: var(--text); stroke-width: 4; stroke-linecap: round; }
        .focus { fill: none; stroke: transparent; stroke-width: 2; }
      `;
    }

    build(root) {
      const label = document.createElement("div");
      label.className = "label";
      label.textContent = this.getAttribute("label") || "";
      const picture = svg("svg", { viewBox: "0 0 100 100", class: "control" });
      svg("circle", { cx: 50, cy: 50, r: 48, class: "focus" }, picture);
      svg("path", { d: arc(40, -sweep, sweep), class: "track" }, picture);
      this.fill = svg("path", { d: "", class: "fill" }, picture);
      svg("circle", { cx: 50, cy: 50, r: 30, class: "body" }, picture);
      this.pointer = svg("line", { x1: 50, y1: 42, x2: 50, y2: 24, class: "pointer" }, picture);
      this.readout = document.createElement("div");
      this.readout.className = "value";
      root.append(label, picture, this.readout);
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

  // A linear control: a press or a drag puts it where the pointer is; keys
  // and a double click as es-knob. orientation="vertical" stands it up.
  class Slider extends ValueControl {
    vertical() { return this.getAttribute("orientation") === "vertical"; }

    styles() {
      return `
        .control { position: relative; cursor: pointer; border-radius: 3px;
                   background: var(--line); outline-offset: 3px; }
        .control:focus-visible { outline: 2px solid var(--focus); }
        .control.horizontal { width: var(--es-slider-length, 160px); height: var(--es-slider-thickness, 8px); }
        .control.vertical { height: var(--es-slider-length, 160px); width: var(--es-slider-thickness, 8px); }
        .fill { position: absolute; left: 0; bottom: 0; background: var(--accent); border-radius: 3px; }
        .horizontal .fill { top: 0; }
        .vertical .fill { right: 0; }
        .thumb { position: absolute; width: 14px; height: 14px; border-radius: 50%; background: var(--text); }
        .horizontal .thumb { top: 50%; transform: translate(-50%, -50%); }
        .vertical .thumb { left: 50%; transform: translate(-50%, 50%); }
      `;
    }

    build(root) {
      const label = document.createElement("div");
      label.className = "label";
      label.textContent = this.getAttribute("label") || "";
      const track = document.createElement("div");
      track.className = "control " + (this.vertical() ? "vertical" : "horizontal");
      this.fillBar = document.createElement("div");
      this.fillBar.className = "fill";
      this.thumb = document.createElement("div");
      this.thumb.className = "thumb";
      track.append(this.fillBar, this.thumb);
      this.readout = document.createElement("div");
      this.readout.className = "value";
      root.append(label, track, this.readout);
      return track;
    }

    render() {
      const percent = (this.position * 100).toFixed(3) + "%";
      if (this.vertical()) {
        this.fillBar.style.height = percent;
        this.thumb.style.bottom = percent;
      } else {
        this.fillBar.style.width = percent;
        this.thumb.style.left = percent;
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

  customElements.define("es-knob", Knob);
  customElements.define("es-slider", Slider);
  ui.ValueControl = ValueControl;
})();
