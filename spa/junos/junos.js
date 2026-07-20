/* Junos Call Home configuration UI. Depends on /app.js (edgehostFetch). */
(function () {
  function $(id) {
    return document.getElementById(id);
  }

  function setText(id, text) {
    var el = $(id);
    if (el) el.textContent = text;
  }

  function setHtml(id, html) {
    var el = $(id);
    if (el) el.innerHTML = html;
  }

  function setBadge(ok, label) {
    var b = $("authBadge");
    if (!b) return;
    b.textContent = label;
    b.className = "badge " + (ok ? "ok" : "muted");
  }

  async function fetchText(url, opts) {
    if (typeof window.edgehostFetch === "function") {
      return window.edgehostFetch(url, opts);
    }
    var r = await fetch(
      url,
      Object.assign({ credentials: "same-origin" }, opts || {})
    );
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

  /** API path segment for DEVICE-ID (preserve case; encode specials). */
  function devicePath(id) {
    return encodeURIComponent(String(id || "").trim());
  }

  function isJunosShelf(s) {
    if (!s) return false;
    var v = (s.vendor || "").toLowerCase();
    if (v === "junos" || v === "juniper") return true;
    if (s.device_id && (!s.mac || s.mac === s.device_id)) return true;
    return false;
  }

  function selectedId() {
    return window.__junosSelectedId || "";
  }

  function setSelectedId(id) {
    window.__junosSelectedId = id || "";
    var rows = document.querySelectorAll("#devicesBody tr[data-id]");
    var i;
    for (i = 0; i < rows.length; i++) {
      rows[i].classList.toggle(
        "row-selected",
        rows[i].getAttribute("data-id") === id
      );
    }
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
      refreshStatus().catch(function () {});
      refreshDevices().catch(function () {});
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
    "e7_notifications",
    "e7_subscriptions_ok",
    "e7_rejects_not_allowlisted",
    "e7_rejects_bad_identity",
    "e7_rejects_other",
    "runtime_shelves",
    "max_sessions"
  ];

  async function refreshStatus() {
    var r = await fetchText("/api/v1/e7/status");
    var tbody = $("statusBody");
    var badge = $("statusBadge");
    if (!r.ok) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"2\" class=\"muted-cell\">HTTP " +
          r.status +
          "</td></tr>";
      }
      if (badge) {
        badge.textContent = "error";
        badge.className = "badge bad";
      }
      setText("statusSummary", "Failed to load status (HTTP " + r.status + ").");
      return;
    }
    var j;
    try {
      j = JSON.parse(r.body);
    } catch (e) {
      setText("statusSummary", "Parse error");
      return;
    }
    var en = !!j.enabled;
    if (badge) {
      badge.textContent = en ? "Call Home on" : "disabled";
      badge.className = "badge " + (en ? "ok" : "muted");
    }
    setHtml(
      "statusSummary",
      "Listener <code>" +
        esc(j.listen || "?") +
        "</code> · transport <code>" +
        esc(j.transport || "?") +
        "</code> · open sessions <strong>" +
        esc(j.e7_sessions_open != null ? j.e7_sessions_open : "—") +
        "</strong> · accepts " +
        esc(j.e7_accepts != null ? j.e7_accepts : "—")
    );
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
      tbody.innerHTML =
        html ||
        "<tr><td colspan=\"2\" class=\"muted-cell\">(empty)</td></tr>";
    }
  }

  /* --- devices --- */

  function sessionLabel(state) {
    var s = String(state || "empty").toLowerCase();
    if (s === "empty" || s === "") return "Inactive";
    if (s === "open") return "Open";
    if (s === "accepted") return "Accepted";
    if (s === "identity") return "Identity";
    if (s === "post_identity") return "Post-identity";
    if (s === "ssh") return "SSH";
    if (s === "hello") return "Hello";
    if (s === "error") return "Error";
    if (s === "closing") return "Closing";
    return state;
  }

  function sessionClass(state) {
    var s = String(state || "").toLowerCase();
    if (s === "open") return "sess-open";
    if (s === "error" || s === "closing") return "sess-bad";
    if (
      s === "ssh" ||
      s === "hello" ||
      s === "identity" ||
      s === "post_identity" ||
      s === "accepted"
    ) {
      return "sess-warn";
    }
    return "";
  }

  function deviceIdOf(s) {
    return (s.device_id && s.device_id) || s.mac || "";
  }

  function deviceRowHtml(s) {
    var id = deviceIdOf(s);
    var st = s.session_state || "empty";
    return (
      "<tr data-id=\"" +
      esc(id) +
      "\"" +
      (selectedId() === id ? " class=\"row-selected\"" : "") +
      ">" +
      "<td><button type=\"button\" class=\"ghost btn-select\" data-id=\"" +
      esc(id) +
      "\">Select</button></td>" +
      "<td><code>" +
      esc(id) +
      "</code></td>" +
      "<td>" +
      esc(s.label || "") +
      "</td>" +
      "<td>" +
      (s.has_secret
        ? "<span class=\"badge ok\">set</span>"
        : "<span class=\"badge muted\">none</span>") +
      "</td>" +
      "<td>" +
      (s.enabled
        ? "<span class=\"badge ok\">yes</span>"
        : "<span class=\"badge muted\">no</span>") +
      "</td>" +
      "<td class=\"" +
      sessionClass(st) +
      "\" title=\"" +
      esc(st) +
      "\">" +
      esc(sessionLabel(st)) +
      " <code>" +
      esc(st) +
      "</code></td>" +
      "<td><code class=\"key-cell\">" +
      esc(s.peer || "") +
      "</code></td>" +
      "<td class=\"row-actions\">" +
      "<button type=\"button\" class=\"ghost btn-edit\" data-id=\"" +
      esc(id) +
      "\">Edit</button> " +
      "<button type=\"button\" class=\"ghost btn-disconnect\" data-id=\"" +
      esc(id) +
      "\">Disconnect</button> " +
      "<button type=\"button\" class=\"ghost btn-delete\" data-id=\"" +
      esc(id) +
      "\">Delete</button>" +
      "</td>" +
      "</tr>"
    );
  }

  async function refreshDevices() {
    var r = await fetchText("/api/v1/e7/shelves");
    var tbody = $("devicesBody");
    if (!r.ok) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"8\" class=\"muted-cell\">HTTP " +
          r.status +
          " " +
          esc(r.body.slice(0, 120)) +
          "</td></tr>";
      }
      setText("devicesMeta", "");
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
    var all = j.shelves || [];
    var list = all.filter(isJunosShelf);
    setText(
      "devicesMeta",
      list.length +
        " Junos system(s)" +
        (all.length !== list.length
          ? " (" + (all.length - list.length) + " other vendor hidden)"
          : "")
    );
    if (!list.length) {
      if (tbody) {
        tbody.innerHTML =
          "<tr><td colspan=\"8\" class=\"muted-cell\">No Junos systems yet — use the form above</td></tr>";
      }
      return;
    }
    if (tbody) {
      tbody.innerHTML = list.map(deviceRowHtml).join("");
    }
  }

  function updateRouterSketch() {
    var id = $("devId") ? $("devId").value.trim() : "";
    var secret = $("devSecret") ? $("devSecret").value : "";
    var pre = $("routerSketch");
    if (!pre) return;
    var did = id || "pe1.lab";
    var lines = [
      "set system services netconf ssh",
      "set system services outbound-ssh client edgehost device-id " + did,
      "set system services outbound-ssh client edgehost services netconf",
      "set system services outbound-ssh client edgehost servers <nms-ip> port 4334"
    ];
    if (secret) {
      lines.push(
        'set system services outbound-ssh client edgehost secret "' +
          secret +
          '"'
      );
    } else {
      lines.push(
        '# optional: set system services outbound-ssh client edgehost secret "shared-secret"'
      );
    }
    pre.textContent = lines.join("\n");
  }

  function clearForm() {
    if ($("devId")) $("devId").value = "";
    if ($("devLabel")) $("devLabel").value = "";
    if ($("devSecret")) $("devSecret").value = "";
    if ($("devSecretClear")) $("devSecretClear").checked = false;
    if ($("devEnabled")) $("devEnabled").checked = true;
    setSelectedId("");
    setText("formOut", "");
    updateRouterSketch();
  }

  async function loadIntoForm(id) {
    if (!id) {
      setText("formOut", "No system selected");
      return;
    }
    var r = await fetchText("/api/v1/e7/shelves/" + devicePath(id));
    if (!r.ok) {
      setText("formOut", "Load HTTP " + r.status + "\n" + r.body);
      return;
    }
    try {
      var j = JSON.parse(r.body);
      var did = j.device_id || j.mac || id;
      if ($("devId")) $("devId").value = did;
      if ($("devLabel")) $("devLabel").value = j.label || "";
      if ($("devEnabled")) $("devEnabled").checked = !!j.enabled;
      if ($("devSecret")) $("devSecret").value = "";
      if ($("devSecretClear")) $("devSecretClear").checked = false;
      setSelectedId(did);
      setText(
        "formOut",
        "Loaded " + did + (j.has_secret ? " (secret is set — leave blank to keep)" : "")
      );
      updateRouterSketch();
    } catch (e) {
      setText("formOut", r.body);
    }
  }

  async function saveDevice() {
    var id = $("devId") ? $("devId").value.trim() : "";
    if (!id) {
      setText("formOut", "DEVICE-ID is required");
      return;
    }
    var label = $("devLabel") ? $("devLabel").value.trim() : "";
    var enabled = $("devEnabled") ? !!$("devEnabled").checked : true;
    var secret = $("devSecret") ? $("devSecret").value : "";
    var clearSecret = $("devSecretClear")
      ? !!$("devSecretClear").checked
      : false;
    var body = {
      enabled: enabled,
      vendor: "junos",
      device_id: id
    };
    if (label) body.label = label;
    if (clearSecret) body.secret = "";
    else if (secret) body.secret = secret;

    var r = await fetchText("/api/v1/e7/shelves/" + devicePath(id), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    });
    setText("formOut", "PUT HTTP " + r.status + "\n" + r.body);
    if (r.ok) {
      setSelectedId(id);
      if ($("devSecret")) $("devSecret").value = "";
      if ($("devSecretClear")) $("devSecretClear").checked = false;
      await refreshDevices();
      updateRouterSketch();
    }
  }

  async function disconnectDevice(id) {
    if (!id) return;
    var r = await fetchText(
      "/api/v1/e7/shelves/" + devicePath(id) + "/disconnect",
      { method: "POST" }
    );
    setText("formOut", "DISCONNECT HTTP " + r.status + "\n" + r.body);
    await refreshDevices();
  }

  async function deleteDevice(id) {
    if (!id) return;
    if (
      !window.confirm(
        "Delete Junos system “" + id + "” from allowlist and disconnect?"
      )
    ) {
      return;
    }
    var r = await fetchText("/api/v1/e7/shelves/" + devicePath(id), {
      method: "DELETE"
    });
    setText(
      "formOut",
      "DELETE HTTP " + r.status + (r.body ? "\n" + r.body : "")
    );
    if (r.ok || r.status === 204) {
      if (selectedId() === id) clearForm();
      await refreshDevices();
    }
  }

  /* --- connection events --- */

  var eventsSinceId = 0;
  var eventsTimer = null;
  var eventsPollMs = 1000;

  function setEventsBadge(on, label) {
    var b = $("eventsBadge");
    if (!b) return;
    b.textContent = label || (on ? "watching" : "log off");
    b.className = "badge " + (on ? "ok" : "muted");
  }

  function stageClass(stage) {
    var s = String(stage || "");
    if (/reject|fail|timeout|error|overflow|bad|closed|hmac_fail|hmac_missing/.test(s)) {
      return "sess-bad";
    }
    if (
      /open|subscribed|allowlist_ok|identity_ok|junos_hmac_ok|ssh_ready|ssh_client/.test(
        s
      )
    ) {
      return "sess-open";
    }
    return "sess-warn";
  }

  function formatAge(ms) {
    if (ms == null || ms < 0) return "—";
    if (ms < 1000) return ms + " ms";
    if (ms < 60000) return (ms / 1000).toFixed(1) + " s";
    return (ms / 60000).toFixed(1) + " min";
  }

  function renderLiveSessions(list) {
    var tbody = $("liveSessBody");
    if (!tbody) return;
    if (!list || !list.length) {
      tbody.innerHTML =
        "<tr><td colspan=\"6\" class=\"muted-cell\">No in-flight sessions</td></tr>";
      return;
    }
    tbody.innerHTML = list
      .map(function (s) {
        var st = s.state || "";
        var id = s.mac || s.device_id || s.id || "";
        return (
          "<tr>" +
          "<td class=\"" +
          sessionClass(st) +
          "\">" +
          esc(sessionLabel(st)) +
          "</td>" +
          "<td><code>" +
          esc(id) +
          "</code></td>" +
          "<td><code class=\"key-cell\">" +
          esc(s.peer || "") +
          "</code></td>" +
          "<td>" +
          esc(formatAge(s.age_ms)) +
          "</td>" +
          "<td>" +
          esc(s.ssh ? "yes" : "—") +
          "</td>" +
          "<td>" +
          esc(s.allowlisted ? "yes" : s.auto_unknown ? "auto" : "—") +
          "</td>" +
          "</tr>"
        );
      })
      .join("");
  }

  function appendEventLines(events) {
    var el = $("eventsLog");
    if (!el || !events || !events.length) return;
    var i;
    for (i = 0; i < events.length; i++) {
      var e = events[i];
      var stage = e.stage || "";
      var isJ =
        /junos|DEVICE-CONN|device_id|hmac/i.test(stage + " " + (e.detail || "")) ||
        (e.mac && selectedId() && e.mac === selectedId());
      var line =
        "[" +
        (e.id != null ? e.id : "?") +
        "] " +
        stage +
        (e.mac ? " " + e.mac : "") +
        (e.peer ? " @" + e.peer : "") +
        (e.detail ? " — " + e.detail : "");
      var span = document.createElement("div");
      span.className = stageClass(stage) + (isJ ? " evt-junos" : "");
      span.textContent = line;
      el.appendChild(span);
    }
    el.scrollTop = el.scrollHeight;
  }

  async function pollEvents(forceAll) {
    var url =
      "/api/v1/e7/events?limit=64" +
      (forceAll || !eventsSinceId ? "" : "&since=" + eventsSinceId);
    var r = await fetchText(url);
    if (!r.ok) {
      setEventsBadge(false, "log err " + r.status);
      return;
    }
    var j;
    try {
      j = JSON.parse(r.body);
    } catch (e) {
      return;
    }
    if (j.sessions) renderLiveSessions(j.sessions);
    var events = j.events || j.trace || [];
    if (events.length) {
      var maxId = eventsSinceId;
      var i;
      for (i = 0; i < events.length; i++) {
        if (events[i].id != null && events[i].id > maxId) {
          maxId = events[i].id;
        }
      }
      appendEventLines(events);
      eventsSinceId = maxId;
    }
    setEventsBadge(true, "watching");
    setText(
      "eventsMeta",
      "since_id=" +
        eventsSinceId +
        (j.events_seq != null ? " seq=" + j.events_seq : "")
    );
  }

  function startEventsPoll() {
    stopEventsPoll();
    var auto = $("eventsAuto");
    if (auto && !auto.checked) {
      setEventsBadge(false, "paused");
      return;
    }
    setEventsBadge(true, "watching");
    pollEvents(true).catch(function () {});
    eventsTimer = setInterval(function () {
      pollEvents(false).catch(function () {});
    }, eventsPollMs);
  }

  function stopEventsPoll() {
    if (eventsTimer) {
      clearInterval(eventsTimer);
      eventsTimer = null;
    }
    setEventsBadge(false, "log off");
  }

  /* --- wire UI --- */

  function onDevicesClick(ev) {
    var t = ev.target;
    if (!t || !t.getAttribute) return;
    var id = t.getAttribute("data-id");
    if (!id) return;
    if (t.classList.contains("btn-select") || t.classList.contains("btn-edit")) {
      setSelectedId(id);
      loadIntoForm(id);
      return;
    }
    if (t.classList.contains("btn-disconnect")) {
      disconnectDevice(id);
      return;
    }
    if (t.classList.contains("btn-delete")) {
      deleteDevice(id);
    }
  }

  function wire() {
    if ($("btnLogin")) $("btnLogin").addEventListener("click", login);
    if ($("btnMe")) $("btnMe").addEventListener("click", me);
    if ($("btnStatus")) $("btnStatus").addEventListener("click", refreshStatus);
    if ($("btnDevices")) $("btnDevices").addEventListener("click", refreshDevices);
    if ($("btnSave")) $("btnSave").addEventListener("click", saveDevice);
    if ($("btnFormClear")) $("btnFormClear").addEventListener("click", clearForm);
    if ($("btnFormLoad")) {
      $("btnFormLoad").addEventListener("click", function () {
        loadIntoForm(selectedId());
      });
    }
    if ($("devicesBody")) {
      $("devicesBody").addEventListener("click", onDevicesClick);
    }
    if ($("devId")) {
      $("devId").addEventListener("input", updateRouterSketch);
    }
    if ($("devSecret")) {
      $("devSecret").addEventListener("input", updateRouterSketch);
    }
    if ($("btnEventsRefresh")) {
      $("btnEventsRefresh").addEventListener("click", function () {
        pollEvents(false);
      });
    }
    if ($("btnEventsClear")) {
      $("btnEventsClear").addEventListener("click", function () {
        if ($("eventsLog")) $("eventsLog").textContent = "";
      });
    }
    if ($("eventsAuto")) {
      $("eventsAuto").addEventListener("change", function () {
        if ($("eventsAuto").checked) startEventsPoll();
        else stopEventsPoll();
      });
    }

    updateRouterSketch();
    me().then(function (ok) {
      if (ok) {
        refreshStatus();
        refreshDevices();
        startEventsPoll();
      }
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", wire);
  } else {
    wire();
  }
})();
