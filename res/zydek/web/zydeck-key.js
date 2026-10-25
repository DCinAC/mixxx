// ZyDeck: another ZyDeck's server answers this device only with the key it gave when someone there allowed this one
// (pairing: src/zydek/zydekhub.cpp, pairApi). A page for another ZyDeck has it as ?k= in its address (with ?hub=, the
// other one's address; or the page came from there itself): every request and socket to that ZyDeck carries it.
(() => {
  const params = new URLSearchParams(location.search), key = params.get("k");
  if (!key) return;
  const hub = params.get("hub") || location.host;
  const keyed = u => {
    try {
      const url = new URL(u, location.href);
      if (url.host !== hub) return u;
      url.searchParams.set("k", key);
      return url.toString();
    } catch { return u; }
  };
  const pageFetch = window.fetch.bind(window);
  window.fetch = (u, opts) => pageFetch(typeof u === "string" ? keyed(u) : u, opts);
  const PageSocket = window.WebSocket;
  function KeyedSocket(u, protocols) { return protocols === undefined ? new PageSocket(keyed(u)) : new PageSocket(keyed(u), protocols); }
  KeyedSocket.prototype = PageSocket.prototype;
  Object.assign(KeyedSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 });
  window.WebSocket = KeyedSocket;
  window.zydeckKeyed = keyed;   // (for addresses that aren't fetched: an iframe's, an image's)
})();
