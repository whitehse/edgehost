/* Lab console: actions, hierarchical memory, CPU flame graph. */
(function () {
  var MEM_BASELINE_KEY = "edgehost.lab.memBaseline";
  var memTimer = null;
  var lastMem = null;
  var lastDebug = null;
  var cpuPoll = null;
  var flameRoot = null;
  var flameRects = [];

  var MOD_COLORS = {
    process: "#8b9bb4",
    http: "#4c8dff",
    ws: "#7c6af7",
    e7: "#3ecf8e",
    state: "#e6b450",
    edgecore: "#f07178",
    plugin: "#c792ea",
    other: "#89ddff"
  };

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

  async function fetchText(url, opts) {
    var r = await fetch(url, Object.assign({ credentials: "same-origin" }, opts || {}));
    var body = await r.text();
    return { status: r.status, body: body, headers: r.headers, ok: r.ok };
  }

  function fmtBytes(n) {
    if (n == null || isNaN(n)) return "—";
    var v = Number(n);
    var neg = v < 0;
    v = Math.abs(v);
    var units = ["B", "KiB", "MiB", "GiB"];
    var i = 0;
    while (v >= 1024 && i < units.length - 1) {
      v /= 1024;
      i++;
    }
    var s = (i === 0 ? String(Math.round(v)) : v.toFixed(v >= 10 ? 1 : 2)) + " " + units[i];
    return neg ? ("-" + s) : s;
  }

  function fmtKb(n) {
    if (n == null || isNaN(n)) return "—";
    return fmtBytes(Number(n) * 1024);
  }

  function deltaClass(d) {
    if (d == null || d === 0) return "delta-flat";
    return d > 0 ? "delta-up" : "delta-down";
  }

  function loadBaseline() {
    try {
      var raw = localStorage.getItem(MEM_BASELINE_KEY);
      return raw ? JSON.parse(raw) : null;
    } catch (e) {
      return null;
    }
  }

  function saveBaseline(snap) {
    try {
      localStorage.setItem(MEM_BASELINE_KEY, JSON.stringify(snap));
    } catch (e) { /* ignore */ }
  }

  function clearBaseline() {
    try {
      localStorage.removeItem(MEM_BASELINE_KEY);
    } catch (e) { /* ignore */ }
  }

  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function moduleBytes(m) {
    return Number(m && m.bytes != null ? m.bytes : 0);
  }

  function drawModuleBars(modules) {
    var canvas = $("memBars");
    if (!canvas || !modules) return;
    var ctx = canvas.getContext("2d");
    var dpr = window.devicePixelRatio || 1;
    var cssW = canvas.clientWidth || 900;
    var cssH = 220;
    canvas.width = Math.floor(cssW * dpr);
    canvas.height = Math.floor(cssH * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);

    var rows = modules.filter(function (m) {
      return m.id !== "process";
    });
    var max = 1;
    rows.forEach(function (m) {
      if (moduleBytes(m) > max) max = moduleBytes(m);
    });

    var padL = 108;
    var padR = 72;
    var padT = 12;
    var rowH = Math.min(28, (cssH - padT - 8) / Math.max(rows.length, 1));
    rows.forEach(function (m, i) {
      var y = padT + i * rowH;
      var bw = ((cssW - padL - padR) * moduleBytes(m)) / max;
      var color = MOD_COLORS[m.id] || "#4c8dff";
      ctx.fillStyle = "#8b9bb4";
      ctx.font = "12px ui-sans-serif, system-ui, sans-serif";
      ctx.textAlign = "right";
      ctx.fillText(m.name || m.id, padL - 8, y + rowH * 0.65);
      ctx.fillStyle = color;
      ctx.fillRect(padL, y + 4, Math.max(bw, 2), rowH - 8);
      ctx.textAlign = "left";
      ctx.fillStyle = "#e8eef7";
      ctx.fillText(fmtBytes(moduleBytes(m)), padL + Math.max(bw, 2) + 6, y + rowH * 0.65);
    });
  }

  function itemRowsHtml(items, baseItems) {
    if (!items || !items.length) {
      return "<tr><td colspan=\"4\" class=\"hint\">No items</td></tr>";
    }
    var baseMap = {};
    if (baseItems) {
      baseItems.forEach(function (it) {
        if (it && it.id) baseMap[it.id] = it.bytes || 0;
      });
    }
    return items.map(function (it) {
      var b = it.bytes || 0;
      var d = baseMap[it.id] != null ? b - baseMap[it.id] : null;
      var extra = "";
      if (it.count != null) extra += " · n=" + it.count;
      if (it.keys_used != null) extra += " · keys=" + it.keys_used + "/" + (it.capacity || 0);
      if (it.enabled === false) extra += " · disabled";
      if (it.kind === "estimate") extra += " · estimate";
      return "<tr>" +
        "<td class=\"kind\">" + esc(it.label || it.id) + extra + "</td>" +
        "<td class=\"num\">" + fmtBytes(b) + "</td>" +
        "<td class=\"num\">" + (it.count != null ? it.count : "—") + "</td>" +
        "<td class=\"num " + deltaClass(d) + "\">" +
        (d == null ? "—" : ((d >= 0 ? "+" : "") + fmtBytes(d))) +
        "</td></tr>";
    }).join("");
  }

  function sessionsTableHtml(sessions) {
    if (!sessions || !sessions.length) {
      return "<p class=\"hint\">No live E7 sessions.</p>";
    }
    var rows = sessions.map(function (s) {
      var comps = (s.components || []).map(function (c) {
        return esc(c.label) + " " + fmtBytes(c.bytes);
      }).join("; ");
      return "<tr>" +
        "<td class=\"kind\">" + esc(s.mac || s.label || s.id) + "</td>" +
        "<td>" + esc(s.state) + "</td>" +
        "<td class=\"num\">" + fmtBytes(s.bytes) + "</td>" +
        "<td class=\"hint\">" + esc(comps) + "</td>" +
        "</tr>";
    }).join("");
    return "<table class=\"mem-table\"><thead><tr>" +
      "<th>Session</th><th>State</th><th>Bytes</th><th>Components</th>" +
      "</tr></thead><tbody>" + rows + "</tbody></table>";
  }

  function shelvesTableHtml(shelves) {
    if (!shelves || !shelves.length) {
      return "<p class=\"hint\">No runtime shelves configured.</p>";
    }
    var total = shelves.reduce(function (a, s) { return a + (s.bytes || 0); }, 0);
    var rows = shelves.map(function (s) {
      return "<tr>" +
        "<td class=\"kind\">" + esc(s.mac || s.label || s.id) + "</td>" +
        "<td>" + esc(s.label || "") + "</td>" +
        "<td>" + (s.enabled ? "yes" : "no") + "</td>" +
        "<td>" + (s.has_session ? esc(s.session_state) : "—") + "</td>" +
        "<td class=\"num\">" + fmtBytes(s.session_bytes || 0) + "</td>" +
        "<td class=\"num\">" + fmtBytes(s.bytes || 0) + "</td>" +
        "</tr>";
    }).join("");
    return "<p class=\"hint\">All shelves combined (allowlist row + attached session): <strong>" +
      fmtBytes(total) + "</strong></p>" +
      "<table class=\"mem-table\"><thead><tr>" +
      "<th>MAC / id</th><th>Label</th><th>Enabled</th><th>Session</th>" +
      "<th>Session bytes</th><th>Shelf total</th>" +
      "</tr></thead><tbody>" + rows + "</tbody></table>";
  }

  function renderModules(debug, base) {
    var root = $("memModules");
    if (!root) return;
    var modules = (debug && debug.modules) || [];
    var baseMods = {};
    if (base && base.modules) {
      base.modules.forEach(function (m) { baseMods[m.id] = m; });
    }
    drawModuleBars(modules);
    root.innerHTML = modules.map(function (m) {
      var bm = baseMods[m.id];
      var d = bm ? moduleBytes(m) - moduleBytes(bm) : null;
      var color = MOD_COLORS[m.id] || "var(--accent)";
      var body = "";
      body += "<div class=\"mod-head\">" +
        "<span class=\"mod-swatch\" style=\"background:" + color + "\"></span>" +
        "<strong>" + esc(m.name || m.id) + "</strong>" +
        "<span class=\"num\">" + fmtBytes(moduleBytes(m)) + "</span>" +
        "<span class=\"num " + deltaClass(d) + "\">" +
        (d == null ? "" : ((d >= 0 ? "+" : "") + fmtBytes(d))) +
        "</span></div>";
      if (m.host_alloc_bytes != null) {
        body += "<p class=\"hint\">host_alloc[" + esc(m.id) + "] " +
          fmtBytes(m.host_alloc_bytes) +
          (m.live_sessions != null
            ? " · live " + m.live_sessions + "/" + (m.max_sessions || "?") +
              " · open " + (m.open_sessions || 0)
            : "") +
          (m.per_session_budget_bytes
            ? " · per-session budget " + fmtBytes(m.per_session_budget_bytes)
            : "") +
          "</p>";
      }
      body += "<table class=\"mem-table\"><thead><tr>" +
        "<th>Item</th><th>Bytes</th><th>Count</th><th>Δ baseline</th>" +
        "</tr></thead><tbody>" +
        itemRowsHtml(m.items, bm && bm.items) +
        "</tbody></table>";
      if (m.id === "e7") {
        body += "<h4 class=\"subhead\">Live sessions (each connected shelf peer)</h4>";
        body += sessionsTableHtml(m.sessions);
        body += "<h4 class=\"subhead\">Runtime shelves (allowlist impact)</h4>";
        body += shelvesTableHtml(m.shelves);
      }
      if (m.id === "http" && m.metrics) {
        body += "<p class=\"hint\">accepts " + (m.metrics.accepts || 0) +
          " · requests " + (m.metrics.requests || 0) +
          " · max_conns " + (m.metrics.max_conns || 0) +
          " · send_cap " + fmtBytes(m.metrics.send_cap || 0) + "</p>";
      }
      return "<div class=\"mod-card\" data-mod=\"" + esc(m.id) + "\">" + body + "</div>";
    }).join("");
  }

  function snapshotFromDebug(j) {
    return {
      at: new Date().toISOString(),
      process: j.process || {},
      host_alloc: j.host_alloc || {},
      modules: j.modules || [],
      summary: j.summary || {}
    };
  }

  function renderDebugMemory(j) {
    lastDebug = j;
    var snap = snapshotFromDebug(j);
    lastMem = snap;
    var base = loadBaseline();
    var summary = $("memSummary");
    var updated = $("memUpdated");
    var raw = $("memRaw");
    var proc = j.process || {};
    var sum = j.summary || {};

    if (updated) {
      updated.textContent = "updated " + new Date().toISOString().slice(11, 19) +
        (j.uptime_s != null ? " · uptime " + j.uptime_s + "s" : "");
      updated.className = "badge ok";
    }

    if (summary) {
      var rss = proc.vm_rss_kb || 0;
      var hwm = proc.vm_hwm_kb || 0;
      var hostB = (j.host_alloc && j.host_alloc.bytes) || 0;
      var baseRss = base && base.process ? base.process.vm_rss_kb : null;
      var baseHost = base && base.host_alloc ? base.host_alloc.bytes : null;
      var rssD = baseRss != null ? rss - baseRss : null;
      var hostD = baseHost != null ? hostB - baseHost : null;
      var lines = [];
      lines.push("<strong>Process RSS</strong> " + fmtKb(rss) +
        " · high-water " + fmtKb(hwm));
      if (rssD != null) {
        lines.push("RSS Δ baseline: <span class=\"" + deltaClass(rssD * 1024) + "\">" +
          (rssD >= 0 ? "+" : "") + fmtKb(rssD) + "</span>");
      }
      lines.push("<strong>host_alloc</strong> " + fmtBytes(hostB) +
        " · state eager " + fmtBytes(sum.state_eager_bytes || 0) +
        " · unaccounted vs RSS " +
        "<span class=\"" + deltaClass(sum.unaccounted_vs_rss) + "\">" +
        fmtBytes(sum.unaccounted_vs_rss || 0) + "</span>");
      if (hostD != null) {
        lines.push("host_alloc Δ: <span class=\"" + deltaClass(hostD) + "\">" +
          (hostD >= 0 ? "+" : "") + fmtBytes(hostD) + "</span>");
      }
      if (sum.note) {
        lines.push("<em>" + esc(sum.note) + "</em>");
      }
      summary.innerHTML = lines.join("<br/>");
    }

    renderModules(j, base);
    if (raw) {
      try {
        raw.textContent = JSON.stringify(j, null, 2);
      } catch (e) {
        raw.textContent = String(j);
      }
    }
  }

  async function refreshHealth() {
    try {
      var r = await fetchText("/health");
      setText("healthOut", "HTTP " + r.status + "\n" + r.body);
    } catch (e) {
      setText("healthOut", "error: " + e);
    }
  }

  async function refreshMemoryOnly() {
    try {
      var r = await fetchText("/api/v1/debug/memory");
      if (!r.ok) {
        if ($("memSummary")) {
          $("memSummary").textContent =
            "HTTP " + r.status + " (login required for debug memory) — " + r.body;
        }
        /* fall back to /health surface */
        var h = await fetchText("/health");
        try {
          var hj = JSON.parse(h.body);
          if (hj.memory) {
            if ($("memSummary")) {
              $("memSummary").innerHTML =
                "Using /health fallback (login for full module breakdown).<br/>" +
                "RSS " + fmtKb(hj.memory.process && hj.memory.process.vm_rss_kb) +
                " · host_alloc " + fmtBytes(hj.memory.host_alloc && hj.memory.host_alloc.bytes);
            }
          }
        } catch (e2) { /* ignore */ }
        return;
      }
      var j = JSON.parse(r.body);
      renderDebugMemory(j);
    } catch (e) {
      if ($("memSummary")) $("memSummary").textContent = "error: " + e;
    }
  }

  function setMemAuto(on) {
    if (memTimer) {
      clearInterval(memTimer);
      memTimer = null;
    }
    if (on) {
      memTimer = setInterval(function () {
        refreshMemoryOnly().catch(function () { /* ignore */ });
      }, 5000);
    }
  }

  /* ---- CPU flame graph ---- */

  function colorForName(name) {
    var h = 0;
    var i;
    for (i = 0; i < name.length; i++) h = (h * 33 + name.charCodeAt(i)) >>> 0;
    var hue = h % 360;
    return "hsl(" + hue + " 55% 42%)";
  }

  function layoutFlame(node, depth, x0, x1, rows) {
    if (!node) return;
    var value = node.value || 0;
    rows.push({ node: node, depth: depth, x0: x0, x1: x1, value: value });
    var children = node.children || [];
    var total = children.reduce(function (a, c) { return a + (c.value || 0); }, 0) || 1;
    var x = x0;
    children.forEach(function (c) {
      var w = (x1 - x0) * ((c.value || 0) / total);
      layoutFlame(c, depth + 1, x, x + w, rows);
      x += w;
    });
  }

  function drawFlame(root) {
    var canvas = $("flameCanvas");
    if (!canvas) return;
    flameRoot = root;
    flameRects = [];
    var ctx = canvas.getContext("2d");
    var dpr = window.devicePixelRatio || 1;
    var cssW = canvas.clientWidth || 960;
    var cssH = 360;
    canvas.width = Math.floor(cssW * dpr);
    canvas.height = Math.floor(cssH * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);
    ctx.fillStyle = "#0c1017";
    ctx.fillRect(0, 0, cssW, cssH);

    if (!root || !(root.value > 0) && !(root.children && root.children.length)) {
      ctx.fillStyle = "#8b9bb4";
      ctx.font = "14px ui-sans-serif, system-ui, sans-serif";
      ctx.fillText("No profile data yet — start a capture above.", 16, 32);
      return;
    }

    var rows = [];
    layoutFlame(root, 0, 0, 1, rows);
    var maxDepth = 0;
    rows.forEach(function (r) {
      if (r.depth > maxDepth) maxDepth = r.depth;
    });
    var rowH = Math.max(16, Math.min(22, (cssH - 8) / (maxDepth + 1)));

    rows.forEach(function (r) {
      var x = r.x0 * cssW;
      var w = Math.max((r.x1 - r.x0) * cssW, 1);
      var y = cssH - (r.depth + 1) * rowH;
      var name = (r.node && r.node.name) || "?";
      ctx.fillStyle = colorForName(name);
      ctx.fillRect(x, y, w, rowH - 1);
      flameRects.push({ x: x, y: y, w: w, h: rowH - 1, node: r.node, depth: r.depth });
      if (w > 40) {
        ctx.fillStyle = "#fff";
        ctx.font = "11px ui-monospace, monospace";
        ctx.save();
        ctx.beginPath();
        ctx.rect(x, y, w, rowH - 1);
        ctx.clip();
        ctx.fillText(name + " (" + (r.node.value || 0) + ")", x + 4, y + rowH * 0.7);
        ctx.restore();
      }
    });
  }

  function onFlameMove(ev) {
    var canvas = $("flameCanvas");
    var tip = $("flameTip");
    if (!canvas || !tip) return;
    var rect = canvas.getBoundingClientRect();
    var x = ev.clientX - rect.left;
    var y = ev.clientY - rect.top;
    var hit = null;
    for (var i = flameRects.length - 1; i >= 0; i--) {
      var r = flameRects[i];
      if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
        hit = r;
        break;
      }
    }
    if (!hit) {
      tip.textContent = "Hover a frame for detail.";
      return;
    }
    var n = hit.node;
    var pct = flameRoot && flameRoot.value
      ? ((100 * (n.value || 0)) / flameRoot.value).toFixed(1)
      : "?";
    tip.textContent = n.name + " — samples " + (n.value || 0) + " (" + pct + "% of root)";
  }

  async function loadCpuCaps() {
    try {
      var r = await fetchText("/api/v1/debug/cpu/capabilities");
      if (r.ok) {
        var j = JSON.parse(r.body);
        setText("cpuCaps", JSON.stringify(j, null, 2));
      } else {
        setText("cpuCaps", "HTTP " + r.status + " " + r.body);
      }
    } catch (e) {
      setText("cpuCaps", "error: " + e);
    }
  }

  async function refreshCpuStatus() {
    try {
      var r = await fetchText("/api/v1/debug/cpu/profile");
      var j = JSON.parse(r.body);
      var st = $("cpuStatus");
      if (st) {
        st.textContent = j.state + (j.samples != null ? " · " + j.samples + " samples" : "");
        st.className = "badge " + (j.state === "done" ? "ok" : (j.state === "running" ? "ok" : "muted"));
      }
      if (j.state === "done") {
        if (cpuPoll) {
          clearInterval(cpuPoll);
          cpuPoll = null;
        }
        var fl = await fetchText("/api/v1/debug/cpu/profile/flame");
        if (fl.ok) {
          var tree = JSON.parse(fl.body);
          drawFlame(tree);
        }
        var fo = await fetchText("/api/v1/debug/cpu/profile/folded");
        if (fo.ok) setText("cpuFolded", fo.body || "(empty)");
      }
    } catch (e) {
      if ($("cpuStatus")) $("cpuStatus").textContent = "error";
    }
  }

  async function startCpuProfile() {
    var secs = parseInt(($("cpuSecs") && $("cpuSecs").value) || "10", 10);
    var mode = ($("cpuMode") && $("cpuMode").value) || "auto";
    var r = await fetchText(
      "/api/v1/debug/cpu/profile?seconds=" + secs + "&mode=" + encodeURIComponent(mode),
      { method: "POST" }
    );
    if (!r.ok) {
      if ($("cpuStatus")) {
        $("cpuStatus").textContent = "HTTP " + r.status;
        $("cpuStatus").className = "badge muted";
      }
      setText("cpuFolded", r.body);
      return;
    }
    if ($("cpuStatus")) {
      $("cpuStatus").textContent = "running…";
      $("cpuStatus").className = "badge ok";
    }
    if (cpuPoll) clearInterval(cpuPoll);
    cpuPoll = setInterval(function () {
      refreshCpuStatus().catch(function () { /* ignore */ });
    }, 1000);
    refreshCpuStatus();
  }

  async function login() {
    var pw = $("labPw").value;
    var r = await fetchText("/auth/lab-login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: pw })
    });
    setText("authOut", "HTTP " + r.status + "\n" + r.body);
    if (r.ok) {
      setBadge(true, "session cookie set");
      await me();
      await refreshMemoryOnly();
      await loadCpuCaps();
    } else {
      setBadge(false, "login failed");
    }
  }

  async function me() {
    var r = await fetchText("/auth/me");
    setText("authOut", "HTTP " + r.status + "\n" + r.body);
    if (r.ok) setBadge(true, "authenticated");
    else setBadge(false, "not logged in");
  }

  function stateUrl() {
    var ns = $("stateNs").value;
    var key = $("stateKey").value.replace(/^\/+/, "");
    return "/api/v1/state/" + ns + "/" + key;
  }

  async function putState() {
    var r = await fetchText(stateUrl(), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: $("stateBody").value
    });
    setText("stateOut", "PUT " + r.status + "\n" + r.body);
  }

  async function getState() {
    var r = await fetchText(stateUrl());
    setText("stateOut", "GET " + r.status + "\n" + r.body);
  }

  async function listState() {
    var ns = $("stateNs").value;
    var r = await fetchText("/api/v1/state/" + ns);
    setText("stateOut", "LIST " + r.status + "\n" + r.body);
  }

  var ws = null;

  function wsConnect() {
    if (ws) {
      try { ws.close(); } catch (e) { /* ignore */ }
      ws = null;
    }
    $("wsOut").textContent = "";
    var proto = location.protocol === "https:" ? "wss:" : "ws:";
    var url = proto + "//" + location.host + "/api/v1/stream?topics=state";
    appendLog("wsOut", "connecting " + url);
    ws = new WebSocket(url);
    ws.onopen = function () { appendLog("wsOut", "open"); };
    ws.onclose = function (ev) {
      appendLog("wsOut", "close code=" + ev.code);
      ws = null;
    };
    ws.onerror = function () { appendLog("wsOut", "error"); };
    ws.onmessage = function (ev) { appendLog("wsOut", "msg " + ev.data); };
  }

  function wsClose() {
    if (ws) ws.close();
  }

  async function loadPackages() {
    var r = await fetchText("/packages/index.json");
    setText("pkgOut", "HTTP " + r.status + "\n" + r.body);
  }

  if ($("btnHealth")) $("btnHealth").addEventListener("click", refreshHealth);
  if ($("btnLogin")) $("btnLogin").addEventListener("click", login);
  if ($("btnMe")) $("btnMe").addEventListener("click", me);
  if ($("btnPut")) $("btnPut").addEventListener("click", putState);
  if ($("btnGet")) $("btnGet").addEventListener("click", getState);
  if ($("btnList")) $("btnList").addEventListener("click", listState);
  if ($("btnWsConnect")) $("btnWsConnect").addEventListener("click", wsConnect);
  if ($("btnWsClose")) $("btnWsClose").addEventListener("click", wsClose);
  if ($("btnPkgIndex")) $("btnPkgIndex").addEventListener("click", loadPackages);
  if ($("btnMemRefresh")) $("btnMemRefresh").addEventListener("click", refreshMemoryOnly);
  if ($("btnMemBaseline")) {
    $("btnMemBaseline").addEventListener("click", function () {
      if (lastMem) {
        saveBaseline(lastMem);
        refreshMemoryOnly();
      } else {
        refreshMemoryOnly().then(function () {
          if (lastMem) saveBaseline(lastMem);
          refreshMemoryOnly();
        });
      }
    });
  }
  if ($("btnMemClearBaseline")) {
    $("btnMemClearBaseline").addEventListener("click", function () {
      clearBaseline();
      refreshMemoryOnly();
    });
  }
  if ($("memAuto")) {
    $("memAuto").addEventListener("change", function () {
      setMemAuto($("memAuto").checked);
    });
    setMemAuto($("memAuto").checked);
  }
  if ($("btnCpuStart")) $("btnCpuStart").addEventListener("click", startCpuProfile);
  if ($("btnCpuRefresh")) $("btnCpuRefresh").addEventListener("click", refreshCpuStatus);
  if ($("flameCanvas")) {
    $("flameCanvas").addEventListener("mousemove", onFlameMove);
  }

  refreshHealth();
  me().then(function () {
    return refreshMemoryOnly();
  }).then(function () {
    return loadCpuCaps();
  }).catch(function () { /* ignore */ });
})();
