/* Home dashboard — services, health, device preview. */
(function () {
  function $(id) { return document.getElementById(id); }

  var SERVICES = [
    {
      href: "/map/",
      title: "Status map",
      desc: "Live network geography — basemap, fiber, field context.",
      ico: "◎",
      rose: false
    },
    {
      href: "/devices/",
      title: "Locations & devices",
      desc: "ONT and customer router at each member premise.",
      ico: "⌂",
      rose: true
    },
    {
      href: "/e7/",
      title: "E7 Call Home",
      desc: "Access shelves, sessions, ONT inventory, commands.",
      ico: "⬡",
      rose: false
    },
    {
      href: "/junos/",
      title: "Junos Call Home",
      desc: "Juniper outbound-SSH dial-in and allowlist.",
      ico: "▣",
      rose: false
    },
    {
      href: "/explain/",
      title: "Fiber explain",
      desc: "Interactive path stories for training and support.",
      ico: "◇",
      rose: true
    },
    {
      href: "/ca/",
      title: "Certificate Authority",
      desc: "Sign CSRs, manage roots, publish CRL.",
      ico: "🔐",
      rose: false
    },
    {
      href: "/documentation/",
      title: "Documentation",
      desc: "Lessons and operator references.",
      ico: "☰",
      rose: true
    },
    {
      href: "/lab/",
      title: "Lab console",
      desc: "Health, memory, APIs — engineering workspace.",
      ico: "⚙",
      rose: false
    }
  ];

  /** Demo premises when live inventory is empty (lab-friendly). */
  var DEMO_DEVICES = [
    {
      id: "loc-north-12",
      address: "12 North Ridge Rd",
      member: "Rivera household",
      ont: { id: "1/1/3/12", model: "GS4227E", status: "online", serial: "CXNK00A1B2C3" },
      router: { model: "prplOS CPE", status: "online", mac: "02:1a:2b:3c:4d:5e" }
    },
    {
      id: "loc-elm-408",
      address: "408 Elm Court",
      member: "Nguyen household",
      ont: { id: "1/2/1/08", model: "GS4220E", status: "degraded", serial: "CXNK00D4E5F6" },
      router: { model: "OpenWrt CPE", status: "online", mac: "02:aa:bb:cc:dd:01" }
    },
    {
      id: "loc-pine-9",
      address: "9 Pine Hollow",
      member: "Okoye household",
      ont: { id: "1/1/2/19", model: "GS4227E", status: "offline", serial: "CXNK00G7H8I9" },
      router: { model: "prplOS CPE", status: "offline", mac: "02:11:22:33:44:55" }
    }
  ];

  function statusBadge(st) {
    var s = (st || "").toLowerCase();
    if (s === "online" || s === "up" || s === "ok") return "ok";
    if (s === "degraded" || s === "warn") return "warn";
    if (s === "offline" || s === "down" || s === "bad") return "bad";
    return "muted";
  }

  function renderServices() {
    var g = $("serviceGrid");
    if (!g) return;
    g.innerHTML = SERVICES.map(function (s) {
      return (
        '<a class="service-tile" href="' +
        s.href +
        '">' +
        '<div class="tile-ico' +
        (s.rose ? " rose" : "") +
        '">' +
        s.ico +
        "</div>" +
        "<h3>" +
        s.title +
        "</h3>" +
        "<p>" +
        s.desc +
        "</p></a>"
      );
    }).join("");
  }

  function renderDevicePreview(list) {
    var g = $("devicePreview");
    if (!g) return;
    var items = (list || DEMO_DEVICES).slice(0, 3);
    g.innerHTML = items
      .map(function (loc) {
        var ontSt = loc.ont && loc.ont.status ? loc.ont.status : "unknown";
        return (
          '<a class="device-card" href="/devices/?id=' +
          encodeURIComponent(loc.id) +
          '">' +
          '<div class="device-head">' +
          "<div>" +
          '<div class="device-kind">Premise</div>' +
          "<h3>" +
          escapeHtml(loc.address) +
          "</h3></div>" +
          '<span class="badge ' +
          statusBadge(ontSt) +
          '">' +
          escapeHtml(ontSt) +
          "</span></div>" +
          '<div class="device-meta">' +
          "<span>" +
          escapeHtml(loc.member || "") +
          "</span>" +
          "<span>ONT " +
          escapeHtml((loc.ont && loc.ont.id) || "—") +
          " · " +
          escapeHtml((loc.router && loc.router.model) || "router") +
          "</span>" +
          "</div></a>"
        );
      })
      .join("");
  }

  function escapeHtml(s) {
    return String(s || "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  async function fetchText(url, opts) {
    if (typeof window.edgehostFetch === "function") {
      return window.edgehostFetch(url, opts);
    }
    var r = await fetch(url, Object.assign({ credentials: "same-origin" }, opts || {}));
    var body = await r.text();
    return { status: r.status, body: body, ok: r.ok };
  }

  function showAuthed(yes) {
    var gate = $("loginGate");
    var dash = $("dashRoot");
    /* When using shell, hide gate by taking over full layout */
    if (gate) {
      if (yes) {
        gate.classList.add("hidden");
      } else {
        gate.classList.remove("hidden");
        /* Gate sits inside content; make it feel full-page */
        gate.style.minHeight = "70vh";
      }
    }
    if (dash) dash.classList.toggle("hidden", !yes);
    document.body.classList.toggle("signed-out", !yes);
  }

  async function me() {
    var r = await fetchText("/auth/me");
    if (r.ok) {
      showAuthed(true);
      if (window.EdgeShell) window.EdgeShell.refreshAuth();
      return true;
    }
    showAuthed(false);
    return false;
  }

  async function login() {
    var pwEl = $("labPw");
    var pw = pwEl ? pwEl.value : "lab";
    var r = await fetchText("/auth/lab-login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: pw })
    });
    var out = $("authOut");
    if (out) {
      out.classList.remove("hidden");
      out.textContent = "HTTP " + r.status + "\n" + r.body;
    }
    if (r.ok) {
      showAuthed(true);
      if (window.EdgeShell) window.EdgeShell.refreshAuth();
      var params = new URLSearchParams(location.search);
      var next = params.get("next");
      if (next === "map") location.href = "/map/";
      else if (next === "lab") location.href = "/lab/";
      else if (next === "e7") location.href = "/e7/";
      else if (next === "junos") location.href = "/junos/";
      else if (next === "devices") location.href = "/devices/";
      else if (next === "explain") location.href = "/explain/";
      else if (next === "documentation" || next === "docs") {
        location.href = "/documentation/";
      } else {
        await loadDashboard();
      }
    } else {
      showAuthed(false);
    }
  }

  function logoutLocal() {
    showAuthed(false);
    if (window.EdgeShell) {
      window.EdgeShell.setAuthUi(false, "not logged in");
    }
  }

  async function loadHealth() {
    try {
      var r = await fetchText("/health");
      var el = $("statHealth");
      var meta = $("statHealthMeta");
      if (!el) return;
      if (r.ok) {
        el.textContent = "OK";
        el.style.color = "var(--ok)";
        try {
          var j = JSON.parse(r.body);
          if (meta && j.uptime_ms != null) {
            meta.textContent = "uptime " + Math.round(j.uptime_ms / 1000) + "s";
          } else if (meta) {
            meta.textContent = "HTTP " + r.status;
          }
        } catch (e) {
          if (meta) meta.textContent = "HTTP " + r.status;
        }
      } else {
        el.textContent = "Down";
        el.style.color = "var(--bad)";
      }
    } catch (e) {
      var el2 = $("statHealth");
      if (el2) {
        el2.textContent = "—";
        el2.style.color = "";
      }
    }
  }

  async function loadE7Stats() {
    var shelves = 0;
    var onts = 0;
    try {
      var r = await fetchText("/api/v1/e7/shelves");
      if (r.ok) {
        var j = JSON.parse(r.body);
        var list = j.shelves || j.items || j;
        if (Array.isArray(list)) {
          shelves = list.length;
          /* Best-effort ONT counts if embedded */
          list.forEach(function (s) {
            if (s.ont_count != null) onts += Number(s.ont_count) || 0;
            else if (Array.isArray(s.onts)) onts += s.onts.length;
          });
        }
      }
    } catch (e) { /* unauth or disabled */ }
    if ($("statShelves")) $("statShelves").textContent = String(shelves);
    if ($("statOnts")) {
      $("statOnts").textContent = onts > 0 ? String(onts) : String(DEMO_DEVICES.length);
    }
    if ($("statLocations")) {
      $("statLocations").textContent = String(DEMO_DEVICES.length);
    }
  }

  async function loadDashboard() {
    renderServices();
    renderDevicePreview(DEMO_DEVICES);
    await loadHealth();
    await loadE7Stats();
  }

  if ($("btnLogin")) $("btnLogin").addEventListener("click", login);
  if ($("btnLogout")) $("btnLogout").addEventListener("click", logoutLocal);
  if ($("labPw")) {
    $("labPw").addEventListener("keydown", function (e) {
      if (e.key === "Enter") login();
    });
  }

  me()
    .then(function (ok) {
      if (ok) return loadDashboard();
      renderServices();
    })
    .catch(function () {
      showAuthed(false);
    });
})();
