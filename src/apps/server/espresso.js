// espresso.js: a page's link to its piece, served by the page port at
// /espresso.js (io.md §12.9). One WebSocket on /osc carries OSC messages
// both ways, as JSON arrays of an address and its arguments.
//
//   espresso.send("/cutoff", 0.4);
//   espresso.on("/level", (level) => { meter.value = level; });
//
// While the socket is closed, sent messages are dropped and counted in
// espresso.dropped, as a gesture is not worth replaying late. The socket
// reconnects after 1 s, doubling up to 30 s while failures repeat.
(function () {
  "use strict";

  const firstDelay = 1000;
  const longestDelay = 30000;
  const handlers = new Map();
  let socket = null;
  let delay = firstDelay;

  const espresso = {
    // Messages sent while the socket was closed.
    dropped: 0,

    // Sends one message: an address starting with /, then numbers,
    // strings, and booleans. Returns whether it was sent.
    send(address, ...args) {
      if (socket === null || socket.readyState !== WebSocket.OPEN) {
        espresso.dropped += 1;
        return false;
      }
      socket.send(JSON.stringify([[address, ...args]]));
      return true;
    },

    // Calls fn(...args) for each message to the page with this exact
    // address.
    on(address, fn) {
      if (!handlers.has(address)) handlers.set(address, []);
      handlers.get(address).push(fn);
    },
  };

  function receive(event) {
    let batch;
    try {
      batch = JSON.parse(event.data);
    } catch (error) {
      return;  // not a batch of messages
    }
    if (!Array.isArray(batch)) return;
    for (const message of batch) {
      if (!Array.isArray(message) || typeof message[0] !== "string") continue;
      for (const fn of handlers.get(message[0]) || []) fn(...message.slice(1));
    }
  }

  function connect() {
    const scheme = location.protocol === "https:" ? "wss://" : "ws://";
    const ws = new WebSocket(scheme + location.host + "/osc");
    ws.onopen = () => {
      delay = firstDelay;
    };
    ws.onmessage = receive;
    ws.onclose = () => {
      if (socket === ws) socket = null;
      setTimeout(connect, delay);
      delay = Math.min(delay * 2, longestDelay);
    };
    socket = ws;
  }

  globalThis.espresso = espresso;
  connect();
})();
