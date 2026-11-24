// espresso.js against a scripted WebSocket and timer (io.md §12.9), in
// JavaScriptCore: osascript -l JavaScript espresso_test.js ESPRESSO_JS.
// Throws, and exits non-zero, at the first expectation that fails.

ObjC.import("Foundation");

function run(argv) {
  const source = $.NSString.stringWithContentsOfFileEncodingError(argv[0], $.NSUTF8StringEncoding, null).js;

  const sockets = [];
  const timers = [];
  class FakeSocket {
    constructor(url) {
      this.url = url;
      this.readyState = FakeSocket.CONNECTING;
      this.sent = [];
      sockets.push(this);
    }
    send(text) { this.sent.push(text); }
    open() { this.readyState = FakeSocket.OPEN; this.onopen(); }
    close() { this.readyState = FakeSocket.CLOSED; this.onclose(); }
  }
  FakeSocket.CONNECTING = 0;
  FakeSocket.OPEN = 1;
  FakeSocket.CLOSED = 3;

  globalThis.WebSocket = FakeSocket;
  globalThis.location = { protocol: "http:", host: "localhost:8000" };
  globalThis.setTimeout = (fn, ms) => timers.push({ fn, ms });

  const expect = (what, ok) => { if (!ok) throw new Error("espresso.js: " + what); };
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

  (0, eval)(source);
  const espresso = globalThis.espresso;

  expect("one socket to /osc on the page's own host", sockets.length === 1 && sockets[0].url === "ws://localhost:8000/osc");
  expect("a send while connecting is dropped", espresso.send("/cutoff", 0.4) === false && espresso.dropped === 1);

  sockets[0].open();
  expect("a send once open goes as a batch of one", espresso.send("/note", 60, 100, true) === true);
  expect("the batch's text", same(sockets[0].sent, ['[["/note",60,100,true]]']));

  const heard = [];
  espresso.on("/level", (...args) => heard.push(args));
  espresso.on("/level", () => heard.push("second"));
  sockets[0].onmessage({ data: '[["/level",0.25],["/other",1],["/level",0.5,"x"]]' });
  sockets[0].onmessage({ data: "not json" });
  sockets[0].onmessage({ data: '{"not": "a batch"}' });
  sockets[0].onmessage({ data: '[[1, 2], "loose"]' });
  expect("each message reaches its address's handlers in order", same(heard, [[0.25], "second", [0.5, "x"], "second"]));

  // Reconnection: 1 s, doubling to 30 s while failures repeat.
  const delays = [];
  let current = sockets[0];
  for (let i = 0; i < 7; ++i) {
    current.close();
    const t = timers.shift();
    delays.push(t.ms);
    t.fn();
    current = sockets[sockets.length - 1];
  }
  expect("delays double up to 30 s", same(delays, [1000, 2000, 4000, 8000, 16000, 30000, 30000]));
  expect("a closed socket drops sends", espresso.send("/x") === false && espresso.dropped === 2);
  current.open();  // a connection that opens resets the delay
  current.close();
  expect("an opened socket resets the delay", timers.shift().ms === 1000);

  return "espresso.js: ok";
}
