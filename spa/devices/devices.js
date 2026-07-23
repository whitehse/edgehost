/* Locations & devices — premise ONT + customer router views. */
(function () {
  function $(id) { return document.getElementById(id); }

  var LOCATIONS = [
    {
      id: "loc-north-12",
      address: "12 North Ridge Rd",
      member: "Rivera household",
      account: "A-10428",
      region: "North ridge",
      installed: "2024-03-12",
      ont: {
        id: "1/1/3/12",
        model: "Calix GS4227E",
        status: "online",
        serial: "CXNK00A1B2C3",
        shelf_mac: "00:02:5d:d9:21:47",
        rx_dbm: -18.4,
        vendor: "Calix"
      },
      router: {
        model: "prplOS CPE",
        status: "online",
        mac: "02:1a:2b:3c:4d:5e",
        software: "cpe_agent · OpenWrt",
        wan: "GPON · DHCP",
        last_seen: "moments ago"
      }
    },
    {
      id: "loc-elm-408",
      address: "408 Elm Court",
      member: "Nguyen household",
      account: "A-10991",
      region: "Elm / town center",
      installed: "2023-11-02",
      ont: {
        id: "1/2/1/08",
        model: "Calix GS4220E",
        status: "degraded",
        serial: "CXNK00D4E5F6",
        shelf_mac: "00:02:5d:d9:21:47",
        rx_dbm: -26.1,
        vendor: "Calix"
      },
      router: {
        model: "OpenWrt CPE",
        status: "online",
        mac: "02:aa:bb:cc:dd:01",
        software: "cpe_agent · OpenWrt",
        wan: "GPON · DHCP",
        last_seen: "2 min ago"
      }
    },
    {
      id: "loc-pine-9",
      address: "9 Pine Hollow",
      member: "Okoye household",
      account: "A-11204",
      region: "Pine hollow",
      installed: "2025-01-18",
      ont: {
        id: "1/1/2/19",
        model: "Calix GS4227E",
        status: "offline",
        serial: "CXNK00G7H8I9",
        shelf_mac: "00:02:5d:aa:10:02",
        rx_dbm: null,
        vendor: "Calix"
      },
      router: {
        model: "prplOS CPE",
        status: "offline",
        mac: "02:11:22:33:44:55",
        software: "cpe_agent · OpenWrt",
        wan: "—",
        last_seen: "6 h ago"
      }
    },
    {
      id: "loc-meadow-77",
      address: "77 Meadow Lane",
      member: "Patel household",
      account: "A-10003",
      region: "Meadows",
      installed: "2022-08-30",
      ont: {
        id: "1/3/1/04",
        model: "Calix GS4227E",
        status: "online",
        serial: "CXNK00J1K2L3",
        shelf_mac: "00:02:5d:aa:10:02",
        rx_dbm: -19.2,
        vendor: "Calix"
      },
      router: {
        model: "prplOS CPE",
        status: "online",
        mac: "02:fe:dc:ba:98:76",
        software: "cpe_agent · OpenWrt",
        wan: "GPON · DHCP",
        last_seen: "moments ago"
      }
    }
  ];

  function escapeHtml(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function statusClass(st) {
    var s = String(st || "").toLowerCase();
    if (s === "online" || s === "up") return "ok";
    if (s === "degraded") return "warn";
    if (s === "offline" || s === "down") return "bad";
    return "muted";
  }

  function kv(pairs) {
    return pairs
      .map(function (p) {
        return (
          '<div><div class="k">' +
          escapeHtml(p[0]) +
          '</div><div class="v">' +
          (p[2] ? p[1] : escapeHtml(p[1])) +
          "</div></div>"
        );
      })
      .join("");
  }

  function filterList(q) {
    q = (q || "").trim().toLowerCase();
    if (!q) return LOCATIONS.slice();
    return LOCATIONS.filter(function (loc) {
      var blob = [
        loc.address,
        loc.member,
        loc.account,
        loc.region,
        loc.ont && loc.ont.id,
        loc.ont && loc.ont.serial,
        loc.ont && loc.ont.model,
        loc.router && loc.router.mac,
        loc.router && loc.router.model
      ]
        .join(" ")
        .toLowerCase();
      return blob.indexOf(q) >= 0;
    });
  }

  function renderList(list) {
    var g = $("deviceGrid");
    var c = $("deviceCount");
    if (c) c.textContent = list.length + " location" + (list.length === 1 ? "" : "s");
    if (!g) return;
    if (!list.length) {
      g.innerHTML =
        '<div class="empty-state" style="grid-column:1/-1">' +
        '<div class="empty-ico">⌕</div>' +
        "<p>No locations match that search.</p></div>";
      return;
    }
    g.innerHTML = list
      .map(function (loc) {
        var st = (loc.ont && loc.ont.status) || "unknown";
        return (
          '<button type="button" class="device-card" data-id="' +
          escapeHtml(loc.id) +
          '" style="text-align:left;width:100%;font:inherit;cursor:pointer">' +
          '<div class="device-head">' +
          "<div>" +
          '<div class="device-kind">Premise</div>' +
          "<h3>" +
          escapeHtml(loc.address) +
          "</h3></div>" +
          '<span class="badge ' +
          statusClass(st) +
          '">' +
          escapeHtml(st) +
          "</span></div>" +
          '<div class="device-meta">' +
          "<span>" +
          escapeHtml(loc.member) +
          " · " +
          escapeHtml(loc.account) +
          "</span>" +
          "<span>ONT <code>" +
          escapeHtml(loc.ont.id) +
          "</code></span>" +
          "<span>Router <code>" +
          escapeHtml(loc.router.mac) +
          "</code></span>" +
          "</div></button>"
        );
      })
      .join("");

    g.querySelectorAll(".device-card").forEach(function (el) {
      el.addEventListener("click", function () {
        showDetail(el.getAttribute("data-id"));
      });
    });
  }

  function findLoc(id) {
    for (var i = 0; i < LOCATIONS.length; i++) {
      if (LOCATIONS[i].id === id) return LOCATIONS[i];
    }
    return null;
  }

  function showDetail(id) {
    var loc = findLoc(id);
    if (!loc) return;
    var list = $("listView");
    var det = $("detailView");
    if (list) list.classList.add("hidden");
    if (det) det.classList.remove("hidden");

    if ($("detailAddress")) $("detailAddress").textContent = loc.address;
    if ($("detailMember")) {
      $("detailMember").textContent =
        loc.member + " · account " + loc.account + " · " + loc.region;
    }
    var overall = loc.ont.status === "online" && loc.router.status === "online"
      ? "online"
      : loc.ont.status === "offline" || loc.router.status === "offline"
        ? "attention"
        : loc.ont.status;
    if ($("detailStatus")) {
      $("detailStatus").textContent = overall;
      $("detailStatus").className = "badge " + statusClass(
        overall === "attention" ? "degraded" : overall
      );
    }
    if ($("detailKv")) {
      $("detailKv").innerHTML = kv([
        ["Installed", loc.installed],
        ["Region", loc.region],
        ["Account", loc.account],
        ["Location id", loc.id]
      ]);
    }

    var ont = loc.ont;
    if ($("ontBadge")) {
      $("ontBadge").textContent = ont.status;
      $("ontBadge").className = "badge " + statusClass(ont.status);
    }
    if ($("ontKv")) {
      $("ontKv").innerHTML = kv([
        ["ONT id", ont.id],
        ["Model", ont.model],
        ["Serial", ont.serial],
        ["Vendor", ont.vendor],
        ["Shelf MAC", ont.shelf_mac],
        ["Rx power", ont.rx_dbm != null ? ont.rx_dbm + " dBm" : "—"]
      ]);
    }
    if ($("ontE7Link")) {
      $("ontE7Link").href = "/e7/";
    }

    var rt = loc.router;
    if ($("routerBadge")) {
      $("routerBadge").textContent = rt.status;
      $("routerBadge").className = "badge " + statusClass(rt.status);
    }
    if ($("routerKv")) {
      $("routerKv").innerHTML = kv([
        ["Model", rt.model],
        ["LAN MAC", rt.mac],
        ["Software", rt.software],
        ["WAN", rt.wan],
        ["Last seen", rt.last_seen]
      ]);
    }

    /* Update URL without full reload */
    try {
      history.replaceState(null, "", "/devices/?id=" + encodeURIComponent(id));
    } catch (e) { /* ignore */ }
  }

  function showList() {
    var list = $("listView");
    var det = $("detailView");
    if (list) list.classList.remove("hidden");
    if (det) det.classList.add("hidden");
    try {
      history.replaceState(null, "", "/devices/");
    } catch (e) { /* ignore */ }
  }

  function refresh() {
    var q = $("deviceSearch") ? $("deviceSearch").value : "";
    renderList(filterList(q));
  }

  if ($("deviceSearch")) {
    $("deviceSearch").addEventListener("input", refresh);
  }
  if ($("btnRefresh")) $("btnRefresh").addEventListener("click", refresh);
  if ($("btnBack")) $("btnBack").addEventListener("click", showList);

  refresh();

  var params = new URLSearchParams(location.search);
  var id = params.get("id");
  if (id && findLoc(id)) {
    showDetail(id);
  }

  /* Soft auth check — still usable in open mode */
  if (window.EdgeShell && window.EdgeShell.refreshAuth) {
    window.EdgeShell.refreshAuth();
  }
})();
