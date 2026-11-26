// The editor's 16 px symbolic icons, drawn as 1 px strokes in the text's
// color, square-ended, after the layout sketches (brand/gui/b.py, ICON). The
// design guide's 1.3 px (§6) straddles device pixels at every scale and
// draws soft and uneven; 1 px on the grid draws crisp at 1x and 2x.

const paths = {
  play: '<path d="M4.5 2.5v11l9-5.5z" fill="currentColor" stroke="none"/>',
  stop: '<rect x="3.5" y="3.5" width="9" height="9" fill="currentColor" stroke="none"/>',
  down: '<path d="M4 6l4 4 4-4"/>',
  search: '<circle cx="6.5" cy="6.5" r="4"/><line x1="9.5" y1="9.5" x2="14" y2="14"/>',
  sidebar: '<rect x="1.5" y="2.5" width="13" height="11"/><line x1="6" y1="2.5" x2="6" y2="13.5"/>',
  panel: '<rect x="1.5" y="2.5" width="13" height="11"/><line x1="1.5" y1="9.5" x2="14.5" y2="9.5"/>',
  menu: '<line x1="2" y1="4" x2="14" y2="4"/><line x1="2" y1="8" x2="14" y2="8"/><line x1="2" y1="12" x2="14" y2="12"/>',
  close: '<path d="M4 4l8 8M12 4l-8 8"/>',
  file: '<path d="M3.5 1.5h6l3 3v10h-9z"/><path d="M9.5 1.5v3h3"/>',
  error: '<circle cx="8" cy="8" r="6.5" fill="currentColor" stroke="none"/>' +
         '<path d="M5.8 5.8l4.4 4.4M10.2 5.8l-4.4 4.4" stroke="#000" stroke-width="1.5"/>',
  warning: '<path d="M8 1.8l6.6 12H1.4z" fill="currentColor" stroke="none"/>' +
           '<path d="M8 6v4M8 11.4v.8" stroke="#000" stroke-width="1.5"/>',
  info: '<circle cx="8" cy="8" r="6"/><path d="M8 7v4.5M8 4.6v.8"/>',
  clear: '<path d="M3 4.5h10M6 4.5V2.5h4v2M4.5 4.5l.7 9h5.6l.7-9"/>',
  dot: '<circle cx="8" cy="8" r="4" fill="currentColor" stroke="none"/>',
  ring: '<circle cx="8" cy="8" r="3.5"/>',
  max: '<path d="M4 10l4-4 4 4"/>',
  up: '<path d="M4 10l4-4 4 4"/>',
  right: '<path d="M6 4l4 4-4 4"/>',
  check: '<path d="M3 8.5l3 3 7-7"/>',
  globe: '<circle cx="8" cy="8" r="6.5"/><path d="M1.5 8h13M8 1.5c-2.2 2.3-2.2 10.7 0 13M8 1.5c2.2 2.3 2.2 10.7 0 13"/>',
  reload: '<path d="M13 8a5 5 0 1 1-1.5-3.6"/><path d="M12 1.8v3h-3"/>',
  window: '<rect x="1.5" y="2.5" width="13" height="11"/><line x1="1.5" y1="5.5" x2="14.5" y2="5.5"/>',
  external: '<path d="M9.5 2.5h4v4M13.5 2.5l-6 6M11.5 9.5v4h-9v-9h4"/>',
  minus: '<line x1="3" y1="8.5" x2="13" y2="8.5"/>',
  plus: '<line x1="3" y1="8.5" x2="13" y2="8.5"/><line x1="8.5" y1="3" x2="8.5" y2="14"/>',
  // A line's mark (patterns.md §21.7): a ribbon with a notch, after GNOME's user-bookmarks-symbolic.
  mark: '<path d="M4.5 1.5h7v13l-3.5-3.5-3.5 3.5z" fill="currentColor"/>',
};

/** An icon as an inline SVG element, hidden from assistive technology. */
export function icon(name, className = "") {
  const template = document.createElement("template");
  template.innerHTML =
    `<svg class="icon ${className}" viewBox="0 0 16 16" width="16" height="16" fill="none" ` +
    `stroke="currentColor" stroke-width="1" aria-hidden="true">${paths[name]}</svg>`;
  return template.content.firstChild;
}
