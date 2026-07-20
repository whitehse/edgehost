/* E7 Call Home admin UI. Depends on /app.js (edgehostFetch). Vanilla JS. */
(function () {
  function $(id) { return document.getElementById(id); }

  function setText(id, text) {
    var el = $(id);
    if (el) el.textContent = text;
  }

  function appendLog(id, line) {
    var el = $(id);
    if (!el) return;
    var t = new Date().toISOString().slice(11, 19);
    el.textContent += "[" + t + "] " + line + "\n";
    el.scrollTop = el.scrollHeight;
  }

  function setBadge(ok, label) {
    var b = $("authBadge");
    if (!b) return;
    b.textContent = label;
    b.className = "badge " + (ok ? "ok" : "muted");
  }

  function setWsBadge(on) {
    var b = $("wsBadge");
    if (!b) return;
    b.textContent = on ? "ws on" : "ws off";
    b.className = "badge " + (on ? "ok" : "muted");
  }

  async function fetchText(url, opts) {
    if (typeof window.edgehostFetch === "function") {
      return window.edgehostFetch(url, opts);
    }
    var r = await fetch(url, Object.assign({ credentials: "same-origin" }, opts || {}));
    var body = await r.text();
    return { status: r.status, body: body, headers: r.headers, ok: r.ok };
  }

  function esc(s) {
    if (s == null) return "";
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  /** Path segment for MAC — hyphens avoid colon issues; normalize accepts both. */
  function macPath(mac) {
    return String(mac || "").trim().toLowerCase().replace(/:/g, "-");
  }

  /** State key segment: colon → hyphen (matches edge_e7_mac_to_key_seg). */
  function macKeySeg(mac) {
    return macPath(mac);
  }

  function selectedMac() {
    var d = $("detailMac");
    var c = $("cmdMac");
    var s = $("shelfMac");
    if (d && d.value.trim()) return d.value.trim();
    if (c && c.value.trim()) return c.value.trim();
    if (s && s.value.trim()) return s.value.trim();
    return "";
  }

  function setSelectedMac(mac) {
    if (!mac) return;
    if ($("detailMac")) $("detailMac").value = mac;
    if ($("cmdMac") && !$("cmdMac").value.trim()) $("cmdMac").value = mac;
    if ($("shelfMac") && !$("shelfMac").value.trim()) $("shelfMac").value = mac;
  }

  /* --- auth --- */

  async function me() {
    var r = await fetchText("/auth/me");
    setText("authOut", "HTTP " + r.status + "\n" + r.body);
    if (r.ok) setBadge(true, "authenticated");
    else setBadge(false, "not logged in");
    return r.ok;
  }

  async function login() {
    var pw = $("labPw") ? $("labPw").value : "lab";
    var r = await fetchText("/auth/lab-login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: pw })
    });
    setText("authOut", "HTTP " + r.status + "\n" + r.body);
    if (r.ok) {
      setBadge(true, "session cookie set");
      await me();
      refreshStatus().catch(function () { /* ignore */ });
      refreshShelves().catch(function () { /* ignore */ });
      startEventsPoll();
    } else {
      setBadge(false, "login failed");
      stopEventsPoll();
    }
  }

  /* --- status --- */

  var STATUS_KEYS = [
    "enabled",
    "transport",
    "listen",
    "e7_accepts",
    "e7_sessions_open",
    "e7_sessions_opened",
    "e7_sessions_error",
    "e7_notifications",
    "e7_state_puts",
    "e7_ws_fanouts",
    "e7_ws_coalesce_flush",
    "e7_ws_drop_oldest",
    "e7_ws_format_fail",
    "e7_coalesce_overflow",
    "e7_commands_ok",
    "e7_commands_err",
    "e7_rejects",
    "e7_unconfigured",
    "e7_rejects_bad_identity",
    "e7_rejects_disabled",
    "e7_rejects_capacity",
    "e7_rejects_other",
    "e7_rss_estimate",
    "e7_subscriptions_ok",
    "max_sessions",
    "runtime_shelves",
    "events_seq"
  ];

  /* --- connection progress log --- */

  var eventsSinceId = 0;
  var eventsTimer = null;
  var eventsPollMs = 1000;

  function setEventsBadge(on, label) {
    var b = $("eventsBadge");
    if (!b) return;
    b.textContent = label || (on ? "watching" : "log off");
    b.className = "badge " + (on ? "ok" : "muted");
  }

  function formatAge(ms) {
    if (ms == null || ms < 0) return "—";
    if (ms < 1000) return ms + " ms";
    if (ms < 60000) return (ms / 1000).toFixed(1) + " s";
    return (ms / 60000).toFixed(1) + " min";
  }

  function sessionLabel(state) {
    var s = String(state || "empty").toLowerCase();
    if (s === "empty" || s === "") return "Inactive";
    if (s === "open") return "Open (active)";
    if (s === "accepted") return "Accepted (TCP)";
    if (s === "identity") return "Identity preamble";
    if (s === "ssh") return "SSH handshake";
    if (s === "hello") return "NETCONF hello";
    if (s === "error") return "Error";
    if (s === "closing") return "Closing";
    return state;
  }

  function sessionClass(state) {
    var s = String(state || "").toLowerCase();
    if (s === "open") return "sess-open";
    if (s === "error" || s === "closing") return "sess-bad";
    if (s === "ssh" || s === "hello" || s === "identity" || s === "accepted") {
      return "sess-warn";
    }
    return "";
  }

  function stageClass(stage) {
    var s = String(stage || "");
    if (/reject|fail|timeout|error|overflow|bad|closed/.test(s)) return "sess-bad";
    if (/open|subscribed|allowlist_ok|identity_ok/.test(s)) return "sess-open";
    return "sess-warn";
  }

  function renderLiveSessions(list) {
    var tbody = $("liveSessBody");
    if (!tbody) return;
    if (!list || !list.length) {
      tbody.innerHTML =
        "<tr><td colspan=\"6\" class=\"muted-cell\">No in-flight sessions " +
        "(nothing currently past TCP accept)</td></tr>";
      return;
    }
    tbody.innerHTML = list
      .map(function (s) {
        var st = s.state || "";
        return (
          "<tr>" +
          "<td class=\"" +
          sessionClass(st) +
          "\">" +
          esc(sessionLabel(st)) +
          " <code>" +
          esc(st) +
          "</code></td>" +
          "<td><code>" +
          esc(s.mac || "—") +
          "</code></td>" +
          "<td><code>" +
          esc(s.peer || "") +
          "</code></td>" +
          "<td>" +
          esc(formatAge(s.age_ms)) +
          "</td>" +
          "<td>" +
          (s.use_ssh ? "yes" : "no") +
          "</td>" +
          "<td>" +
          (s.allowlisted ? "yes" : "no") +
          "</td>" +
          "</tr>"
        );
      })
      .join("");
  }

  function appendEventLines(events) {
    if (!events || !events.length) return;
    var el = $("eventsLog");
    if (!el) return;
    var i;
    for (i = 0; i < events.length; i++) {
      var e = events[i];
      var line =
        "#" +
        (e.id != null ? e.id : "?") +
        "  " +
        String(e.stage || "") +
        (e.mac ? "  mac=" + e.mac : "") +
        (e.peer ? "  peer=" + e.peer : "") +
        (e.detail ? "  — " + e.detail : "");
      var t = new Date().toISOString().slice(11, 19);
      el.textContent += "[" + t + "] " + line + "\n";
      if (e.id != null && e.id > eventsSinceId) {
        eventsSinceId = e.id;
      }
    }
    el.scrollTop = el.scrollHeight;
  }

  async function refreshEvents(forceFull) {
    var url = "/api/v1/e7/events";
    if (forceFull) {
      eventsSinceId = 0;
      if ($("eventsLog")) $("eventsLog").textContent = "";
    }
    if (!forceFull && eventsSinceId > 0) {
      url += "?since=" + eventsSinceId;
    }
    var r = await fetchText(url);
    if (!r.ok) {
      setEventsBadge(false, "HTTP " + r.status);
      if ($("eventsMeta")) {
        $("eventsMeta").textContent = "events poll failed HTTP " + r.status;
      }
      return;
    }
    var j;
    try {
      j = JSON.parse(r.body);
    } catch (err) {
      setEventsBadge(false, "parse err");
      return;
    }
    renderLiveSessions(j.sessions || []);
    appendEventLines(j.events || []);
    if (j.next_id != null && j.next_id > eventsSinceId && !(j.events && j.events.length)) {
      /* keep cursor even when no new events in this slice */
      eventsSinceId = j.next_id;
    }
    setEventsBadge(true, "watching");
    if ($("eventsMeta")) {
      $("eventsMeta").textContent =
        (j.transport || "?") +
        " @ " +
        (j.listen || "?") +
        " · live=" +
        (j.live_sessions != null ? j.live_sessions : (j.sessions || []).length) +
        " · seq=" +
        (j.next_id != null ? j.next_id : eventsSinceId);
    }
  }

  function stopEventsPoll() {
    if (eventsTimer) {
      clearInterval(eventsTimer);
      eventsTimer = null;
    }
    setEventsBadge(false, "log off");
  }

  function startEventsPoll() {
    stopEventsPoll();
    if ($("eventsAuto") && !$("eventsAuto").checked) {
      setEventsBadge(false, "paused");
      return;
    }
    refreshEvents(false).catch(function () { /* ignore */ });
    eventsTimer = setInterval(function () {
      if ($("eventsAuto") && !$("eventsAuto").checked) return;
      refreshEvents(false).catch(function () { /* ignore */ });
      /* Keep shelf session column fresh while watching */
      refreshShelves().catch(function () { /* ignore */ });
    }, eventsPollMs);
    setEventsBadge(true, "watching");
  }

  function clearEventsView() {
    if ($("eventsLog")) $("eventsLog").textContent = "";
    /* keep server cursor so we only show new events after clear */
  }

  async function refreshStatus() {
    var r = await fetchText("/api/v1/e7/status");
    var tbody = $("statusBody");
    if (!r.ok) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"2\" class=\"muted-cell\">HTTP " +
          r.status +
          "</td></tr>";
      }
      setText("statusOut", "HTTP " + r.status + "\n" + r.body);
      return;
    }
    var j;
    try {
      j = JSON.parse(r.body);
    } catch (e) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"2\" class=\"muted-cell\">parse error</td></tr>";
      }
      setText("statusOut", r.body);
      return;
    }
    if (tbody) {
      var html = "";
      var i;
      for (i = 0; i < STATUS_KEYS.length; i++) {
        var k = STATUS_KEYS[i];
        if (Object.prototype.hasOwnProperty.call(j, k)) {
          html +=
            "<tr><td><code>" +
            esc(k) +
            "</code></td><td>" +
            esc(j[k]) +
            "</td></tr>";
        }
      }
      tbody.innerHTML = html || "<tr><td colspan=\"2\" class=\"muted-cell\">(empty)</td></tr>";
    }
  }

  /* --- shelves --- */

  function shelfRowHtml(s) {
    var mac = s.mac || s.device_id || "";
    var vendor = s.vendor || (s.device_id && !s.mac ? "junos" : "calix");
    var st = s.session_state || "empty";
    var stLabel = sessionLabel(st);
    var stCls = sessionClass(st);
    return (
      "<tr data-mac=\"" +
      esc(mac) +
      "\">" +
      "<td><button type=\"button\" class=\"ghost btn-select-shelf\" data-mac=\"" +
      esc(mac) +
      "\">Select</button></td>" +
      "<td><code>" +
      esc(mac) +
      "</code>" +
      (s.device_id && s.mac
        ? "<br/><span class=\"hint\">id=" + esc(s.device_id) + "</span>"
        : "") +
      "</td>" +
      "<td>" +
      esc(s.label || "") +
      " <span class=\"badge muted\">" +
      esc(vendor) +
      "</span>" +
      (s.has_secret ? " <span class=\"badge\">secret</span>" : "") +
      "</td>" +
      "<td>" +
      (s.enabled ? "<span class=\"badge ok\">yes</span>" : "<span class=\"badge muted\">no</span>") +
      "</td>" +
      "<td class=\"" +
      stCls +
      "\" title=\"" +
      esc(st) +
      (s.peer ? " peer=" + s.peer : "") +
      "\">" +
      esc(stLabel) +
      (st !== "empty" && st !== ""
        ? " <code>" + esc(st) + "</code>"
        : "") +
      (s.peer ? "<br/><code class=\"key-cell\">" + esc(s.peer) + "</code>" : "") +
      "</td>" +
      "<td>" +
      esc(s.serial || "") +
      "</td>" +
      "<td>" +
      esc(s.model || "") +
      "</td>" +
      "<td class=\"row-actions\">" +
      "<button type=\"button\" class=\"ghost btn-disconnect\" data-mac=\"" +
      esc(mac) +
      "\">Disconnect</button> " +
      "<button type=\"button\" class=\"ghost btn-delete\" data-mac=\"" +
      esc(mac) +
      "\">Delete</button>" +
      "</td>" +
      "</tr>"
    );
  }

  async function refreshShelves() {
    var r = await fetchText("/api/v1/e7/shelves");
    var tbody = $("shelvesBody");
    if (!r.ok) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"8\" class=\"muted-cell\">HTTP " +
          r.status +
          " " +
          esc(r.body.slice(0, 120)) +
          "</td></tr>";
      }
      return;
    }
    var j;
    try {
      j = JSON.parse(r.body);
    } catch (e) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"8\" class=\"muted-cell\">parse error</td></tr>";
      }
      return;
    }
    var list = j.shelves || [];
    if (!list.length) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"8\" class=\"muted-cell\">No shelves (YAML seed empty / none added)</td></tr>";
      }
      return;
    }
    if (tbody) {
      tbody.innerHTML = list.map(shelfRowHtml).join("");
    }
  }

  async function putShelf() {
    var mac = $("shelfMac") ? $("shelfMac").value.trim() : "";
    if (!mac) {
      setText("shelfFormOut", "MAC / DEVICE-ID required");
      return;
    }
    var label = $("shelfLabel") ? $("shelfLabel").value.trim() : "";
    var enabled = $("shelfEnabled") ? !!$("shelfEnabled").checked : true;
    var vendor = $("shelfVendor") ? $("shelfVendor").value : "calix";
    var deviceId = $("shelfDeviceId") ? $("shelfDeviceId").value.trim() : "";
    var secret = $("shelfSecret") ? $("shelfSecret").value : "";
    var clearSecret = $("shelfSecretClear")
      ? !!$("shelfSecretClear").checked
      : false;
    var body = { enabled: enabled, vendor: vendor };
    if (label) body.label = label;
    if (deviceId) body.device_id = deviceId;
    if (clearSecret) body.secret = "";
    else if (secret) body.secret = secret;
    var r = await fetchText("/api/v1/e7/shelves/" + macPath(mac), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    });
    setText("shelfFormOut", "PUT HTTP " + r.status + "\n" + r.body);
    if (r.ok) {
      setSelectedMac(mac);
      if ($("shelfSecret")) $("shelfSecret").value = "";
      if ($("shelfSecretClear")) $("shelfSecretClear").checked = false;
      await refreshShelves();
    }
  }

  async function disconnectShelf(mac) {
    if (!mac) return;
    var r = await fetchText(
      "/api/v1/e7/shelves/" + macPath(mac) + "/disconnect",
      { method: "POST" }
    );
    setText("shelfFormOut", "DISCONNECT HTTP " + r.status + "\n" + r.body);
    await refreshShelves();
  }

  async function deleteShelf(mac) {
    if (!mac) return;
    if (!window.confirm("Delete shelf " + mac + " from runtime allowlist and disconnect?")) {
      return;
    }
    var r = await fetchText("/api/v1/e7/shelves/" + macPath(mac), {
      method: "DELETE"
    });
    setText(
      "shelfFormOut",
      "DELETE HTTP " + r.status + (r.body ? "\n" + r.body : " (no content)")
    );
    if (r.ok || r.status === 204) {
      await refreshShelves();
      if ($("detailMac") && $("detailMac").value.trim() === mac) {
        setText("detailOut", "");
        clearOntsTable("Shelf deleted");
      }
    }
  }

  function loadFormFromSelected() {
    var mac = selectedMac();
    if (!mac) {
      setText("shelfFormOut", "No selected MAC");
      return;
    }
    if ($("shelfMac")) $("shelfMac").value = mac;
    /* optional: fetch detail for label/enabled */
    fetchText("/api/v1/e7/shelves/" + macPath(mac)).then(function (r) {
      if (!r.ok) {
        setText("shelfFormOut", "Load HTTP " + r.status + "\n" + r.body);
        return;
      }
      try {
        var j = JSON.parse(r.body);
        if ($("shelfLabel")) $("shelfLabel").value = j.label || "";
        if ($("shelfEnabled")) $("shelfEnabled").checked = !!j.enabled;
        if ($("shelfVendor")) {
          $("shelfVendor").value = j.vendor || "calix";
        }
        if ($("shelfDeviceId")) {
          $("shelfDeviceId").value = j.device_id || "";
        }
        setText(
          "shelfFormOut",
          "Loaded " +
            (j.mac || j.device_id || mac) +
            (j.has_secret ? " (secret set)" : "")
        );
      } catch (e) {
        setText("shelfFormOut", r.body);
      }
    });
  }

  /* --- detail / ONTs --- */

  var ontCursor = "";
  var ontLastKey = "";
  var ontPageLimit = 64;
  var ontRows = {}; /* key -> row data for live update */
  var ontCount = 0;

  function clearOntsTable(msg) {
    ontCursor = "";
    ontLastKey = "";
    ontRows = {};
    ontCount = 0;
    var tbody = $("ontsBody");
    if (tbody) {
      tbody.innerHTML =
        "<tr><td colspan=\"5\" class=\"muted-cell\">" +
        esc(msg || "Select a shelf") +
        "</td></tr>";
    }
    setText("ontsPageHint", "");
  }

  function parseOntValue(val) {
    if (val == null) return {};
    if (typeof val === "object") return val;
    try {
      return JSON.parse(val);
    } catch (e) {
      return {};
    }
  }

  function ontRowHtml(key, val) {
    var v = parseOntValue(val);
    var st = v.oper_state || "";
    var badge =
      st === "up" || st === "enabled"
        ? "ok"
        : st === "down" || st === "disabled"
          ? "bad"
          : "muted";
    return (
      "<tr data-ont-key=\"" +
      esc(key) +
      "\">" +
      "<td><code class=\"key-cell\">" +
      esc(key) +
      "</code></td>" +
      "<td>" +
      esc(v.ont_id || "") +
      "</td>" +
      "<td>" +
      esc(v.pon_id || "") +
      "</td>" +
      "<td><span class=\"badge " +
      badge +
      "\">" +
      esc(st || "—") +
      "</span></td>" +
      "<td>" +
      esc(v.event_time || "") +
      "</td>" +
      "</tr>"
    );
  }

  function renderOntsFromMap() {
    var tbody = $("ontsBody");
    if (!tbody) return;
    var keys = Object.keys(ontRows).sort();
    if (!keys.length) {
      tbody.innerHTML =
        "<tr><td colspan=\"5\" class=\"muted-cell\">No ONTs</td></tr>";
      return;
    }
    tbody.innerHTML = keys
      .map(function (k) {
        return ontRowHtml(k, ontRows[k]);
      })
      .join("");
  }

  function upsertOntRow(key, val) {
    ontRows[key] = val;
    var tbody = $("ontsBody");
    if (!tbody) return;
    var existing = tbody.querySelector('tr[data-ont-key="' + key.replace(/"/g, "") + '"]');
    /* Prefer full re-render for simplicity when small; patch when present */
    if (existing) {
      var tmp = document.createElement("tbody");
      tmp.innerHTML = ontRowHtml(key, val);
      existing.replaceWith(tmp.firstChild);
    } else {
      /* if placeholder row, clear */
      if (tbody.querySelector(".muted-cell")) {
        tbody.innerHTML = "";
      }
      tbody.insertAdjacentHTML("beforeend", ontRowHtml(key, val));
      ontCount++;
    }
  }

  async function loadDetail() {
    var mac = selectedMac();
    if (!mac) {
      setText("detailOut", "MAC required");
      return;
    }
    setSelectedMac(mac);
    var r = await fetchText("/api/v1/e7/shelves/" + macPath(mac));
    setText("detailOut", "GET shelf HTTP " + r.status + "\n" + r.body);
  }

  async function loadOnts(append) {
    var mac = selectedMac();
    if (!mac) {
      setText("detailOut", "MAC required for ONTs");
      return;
    }
    setSelectedMac(mac);
    if (!append) {
      ontCursor = "";
      ontLastKey = "";
      ontRows = {};
      ontCount = 0;
    }
    var url =
      "/api/v1/e7/shelves/" +
      macPath(mac) +
      "/onts?limit=" +
      ontPageLimit;
    if (append && ontCursor) {
      url += "&cursor=" + ontCursor;
    }
    var r = await fetchText(url);
    if (!r.ok) {
      setText("detailOut", "GET onts HTTP " + r.status + "\n" + r.body);
      if (!append) clearOntsTable("HTTP " + r.status);
      return;
    }
    var j;
    try {
      j = JSON.parse(r.body);
    } catch (e) {
      setText("detailOut", "onts parse error\n" + r.body);
      return;
    }
    var list = j.onts || [];
    var i;
    for (i = 0; i < list.length; i++) {
      var item = list[i];
      var key = item.key || "";
      if (!key) continue;
      ontRows[key] = item.value != null ? item.value : item;
      ontLastKey = key;
    }
    ontCount = Object.keys(ontRows).length;
    renderOntsFromMap();

    var fullPage = list.length >= ontPageLimit;
    if (fullPage && ontLastKey) {
      ontCursor = ontLastKey;
      setText(
        "ontsPageHint",
        "Showing " +
          ontCount +
          " ONT(s) (page size " +
          ontPageLimit +
          "). Full page — more may exist; use “Load more ONTs” (cursor=" +
          ontLastKey +
          ")."
      );
    } else {
      ontCursor = ontLastKey || "";
      setText(
        "ontsPageHint",
        "Showing " + ontCount + " ONT(s)" + (append ? " (with more pages)" : "") + "."
      );
    }
  }

  async function loadOntsMore() {
    if (!ontLastKey) {
      await loadOnts(false);
      return;
    }
    ontCursor = ontLastKey;
    await loadOnts(true);
  }

  /* --- WebSocket live ONT updates --- */

  var ws = null;

  function ontPrefixForMac(mac) {
    return "e7/" + macKeySeg(mac) + "/ont/";
  }

  function wsConnect() {
    if (ws) {
      try {
        ws.close();
      } catch (e) {
        /* ignore */
      }
      ws = null;
    }
    if ($("wsOut")) $("wsOut").textContent = "";
    var proto = location.protocol === "https:" ? "wss:" : "ws:";
    var url = proto + "//" + location.host + "/api/v1/stream?topics=state";
    appendLog("wsOut", "connecting " + url);
    ws = new WebSocket(url);
    ws.onopen = function () {
      appendLog("wsOut", "open");
      setWsBadge(true);
    };
    ws.onclose = function (ev) {
      appendLog("wsOut", "close code=" + ev.code);
      setWsBadge(false);
      ws = null;
    };
    ws.onerror = function () {
      appendLog("wsOut", "error");
    };
    ws.onmessage = function (ev) {
      var raw = ev.data;
      var msg;
      try {
        msg = JSON.parse(raw);
      } catch (e) {
        appendLog("wsOut", "msg " + String(raw).slice(0, 160));
        return;
      }
      if (!msg || msg.type !== "STATE_CHANGED") {
        return;
      }
      if (msg.ns !== "net.pon") {
        return;
      }
      var mac = selectedMac();
      if (!mac) return;
      var prefix = ontPrefixForMac(mac);
      var key = msg.key || "";
      if (key.indexOf(prefix) !== 0) {
        return;
      }
      appendLog(
        "wsOut",
        "ont " + msg.op + " " + key + " oper=" + (msg.value && msg.value.oper_state
          ? msg.value.oper_state
          : "?")
      );
      if (msg.op === "delete" || msg.value == null) {
        delete ontRows[key];
        renderOntsFromMap();
        return;
      }
      upsertOntRow(key, msg.value);
    };
  }

  function wsClose() {
    if (ws) ws.close();
  }

  /* --- commands --- */

  async function submitCommand() {
    var mac = ($("cmdMac") && $("cmdMac").value.trim()) || selectedMac();
    if (!mac) {
      setText("cmdOut", "MAC required");
      return;
    }
    var bodyText = $("cmdBody") ? $("cmdBody").value : "{\"op\":\"get-config\"}";
    var r = await fetchText("/api/v1/e7/shelves/" + macPath(mac) + "/commands", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: bodyText
    });
    setText("cmdOut", "POST HTTP " + r.status + "\n" + r.body);
    if (r.ok || r.status === 202) {
      try {
        var j = JSON.parse(r.body);
        if (j.cmd_id && $("cmdId")) $("cmdId").value = j.cmd_id;
      } catch (e) {
        /* ignore */
      }
    }
  }

  async function pollCommand() {
    var mac = ($("cmdMac") && $("cmdMac").value.trim()) || selectedMac();
    var cmdId = $("cmdId") ? $("cmdId").value.trim() : "";
    if (!mac || !cmdId) {
      setText("cmdOut", "MAC and cmd_id required");
      return;
    }
    var r = await fetchText(
      "/api/v1/e7/shelves/" + macPath(mac) + "/commands/" + encodeURIComponent(cmdId)
    );
    setText("cmdOut", "GET cmd HTTP " + r.status + "\n" + r.body);
  }

  /* --- events --- */

  function onShelvesClick(ev) {
    var t = ev.target;
    if (!t || !t.getAttribute) return;
    var mac = t.getAttribute("data-mac");
    if (!mac) return;
    if (t.classList.contains("btn-select-shelf")) {
      setSelectedMac(mac);
      if ($("shelfMac")) $("shelfMac").value = mac;
      loadDetail();
      loadOnts(false);
      return;
    }
    if (t.classList.contains("btn-disconnect")) {
      disconnectShelf(mac);
      return;
    }
    if (t.classList.contains("btn-delete")) {
      deleteShelf(mac);
    }
  }

  if ($("btnLogin")) $("btnLogin").addEventListener("click", login);
  if ($("btnMe")) $("btnMe").addEventListener("click", me);
  if ($("labPw")) {
    $("labPw").addEventListener("keydown", function (e) {
      if (e.key === "Enter") login();
    });
  }
  if ($("btnStatus")) $("btnStatus").addEventListener("click", refreshStatus);
  if ($("btnShelves")) $("btnShelves").addEventListener("click", refreshShelves);
  if ($("btnShelfPut")) $("btnShelfPut").addEventListener("click", putShelf);
  if ($("btnShelfLoad")) $("btnShelfLoad").addEventListener("click", loadFormFromSelected);
  if ($("btnDetail")) $("btnDetail").addEventListener("click", loadDetail);
  if ($("btnOnts")) $("btnOnts").addEventListener("click", function () { loadOnts(false); });
  if ($("btnOntsMore")) $("btnOntsMore").addEventListener("click", loadOntsMore);
  if ($("btnWsConnect")) $("btnWsConnect").addEventListener("click", wsConnect);
  if ($("btnWsClose")) $("btnWsClose").addEventListener("click", wsClose);
  if ($("btnCmdSubmit")) $("btnCmdSubmit").addEventListener("click", submitCommand);
  if ($("btnCmdPoll")) $("btnCmdPoll").addEventListener("click", pollCommand);
  if ($("shelvesBody")) $("shelvesBody").addEventListener("click", onShelvesClick);
  if ($("btnEventsRefresh")) {
    $("btnEventsRefresh").addEventListener("click", function () {
      refreshEvents(true).catch(function () { /* ignore */ });
    });
  }
  if ($("btnEventsClear")) {
    $("btnEventsClear").addEventListener("click", clearEventsView);
  }
  if ($("eventsAuto")) {
    $("eventsAuto").addEventListener("change", function () {
      if ($("eventsAuto").checked) startEventsPoll();
      else stopEventsPoll();
    });
  }

  me()
    .then(function (ok) {
      if (ok) {
        return Promise.all([refreshStatus(), refreshShelves()]).then(function () {
          startEventsPoll();
        });
      }
    })
    .catch(function () {
      /* ignore */
    });
})();
