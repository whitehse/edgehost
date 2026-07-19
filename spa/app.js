/* Shared SPA helpers (kept minimal; page scripts own UI wiring). */
(function (global) {
  global.edgehostFetch = async function (url, opts) {
    var r = await fetch(url, Object.assign({ credentials: "same-origin" }, opts || {}));
    var body = await r.text();
    return { status: r.status, body: body, headers: r.headers, ok: r.ok };
  };
})(typeof window !== "undefined" ? window : globalThis);
