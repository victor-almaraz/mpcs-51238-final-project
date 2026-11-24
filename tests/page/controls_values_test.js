// The value mapping of espresso-ui.js (ui.md §5.2), without a DOM, in
// JavaScriptCore: osascript -l JavaScript controls_values_test.js ESPRESSO_UI_JS.
// Throws, and exits non-zero, at the first expectation that fails.

ObjC.import("Foundation");

function run(argv) {
  const source = $.NSString.stringWithContentsOfFileEncodingError(argv[0], $.NSUTF8StringEncoding, null).js;
  (0, eval)(source);
  const ui = globalThis.espressoUI;
  const expect = (what, ok) => { if (!ok) throw new Error("espresso-ui.js: " + what); };
  const near = (a, b) => Math.abs(a - b) <= 1e-9 * Math.max(1, Math.abs(b));

  const linear = { min: -1, max: 1, scale: "linear", step: 0 };
  expect("linear ends", ui.valueAt(0, linear) === -1 && ui.valueAt(1, linear) === 1);
  expect("linear middle", ui.valueAt(0.5, linear) === 0 && ui.positionOf(0, linear) === 0.5);
  expect("positions are clamped", ui.valueAt(-3, linear) === -1 && ui.valueAt(7, linear) === 1);

  const exp = { min: 20, max: 20000, scale: "exp", step: 0 };
  expect("exp: equal ratios over the travel", near(ui.valueAt(0.5, exp), Math.sqrt(20 * 20000)));
  expect("exp: the position of a value", near(ui.positionOf(632.4555320336759, exp), 0.5));
  expect("exp without a positive range is linear", ui.valueAt(0.5, { min: 0, max: 10, scale: "exp", step: 0 }) === 5);

  const db = { min: 0, max: 1, scale: "db", step: 0 };
  expect("db: 0 only at the bottom", ui.valueAt(0, db) === 0 && ui.valueAt(0.001, db) > 0);
  expect("db: -30 dB halfway down from 0 dB to -60 dB", near(ui.valueAt(0.5, db), Math.pow(10, -30 / 20)));
  expect("db: the position of -30 dB", near(ui.positionOf(Math.pow(10, -30 / 20), db), 0.5));

  const stepped = { min: 0, max: 10, scale: "linear", step: 2.5 };
  expect("a step rounds the value", ui.fit(3.4, stepped) === 2.5 && ui.fit(3.8, stepped) === 5);
  expect("fit keeps values in the range", ui.fit(12, stepped) === 10 && ui.fit(-1, stepped) === 0);
  expect("a value that is not a number has position 0", ui.positionOf(NaN, linear) === 0);

  expect("hz", ui.format(440, "hz") === "440 Hz" && ui.format(1500, "hz") === "1.5 kHz");
  expect("s", ui.format(0.25, "s") === "250 ms" && ui.format(2, "s") === "2 s");
  expect("db", ui.format(1, "db") === "0 dB" && ui.format(0, "db") === "−∞ dB" && ui.format(0.5, "db") === "-6.02 dB");
  expect("%", ui.format(0.5, "%") === "50%");
  expect("plain numbers keep 3 significant digits", ui.format(3.14159, "") === "3.14" && ui.format(1234.5, "") === "1235");
  expect("an unknown unit reads as a plain number", ui.format(2.5, "furlongs") === "2.5" && ui.format(1, "toString") === "1");
  ui.units.cents = (v) => Math.round(v * 100) + " cents";
  expect("a page adds units", ui.format(0.25, "cents") === "25 cents");

  return "espresso-ui.js values: ok";
}
