/* Certificate Authority admin SPA */
(function () {
  function $(id) { return document.getElementById(id); }

  async function fetchText(url, opts) {
    var r = await fetch(url, Object.assign({ credentials: "same-origin" }, opts || {}));
    var body = await r.text();
    return { status: r.status, body: body, ok: r.ok };
  }

  function setBadge(ok, label) {
    var b = $("authBadge");
    if (!b) return;
    b.textContent = label;
    b.className = "badge " + (ok ? "ok" : "muted");
  }

  async function login() {
    var r = await fetchText("/auth/lab-login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: $("labPw").value })
    });
    $("authOut").textContent = "HTTP " + r.status + "\n" + r.body;
    if (r.ok) {
      setBadge(true, "authenticated");
      await refreshAll();
    } else setBadge(false, "login failed");
  }

  async function me() {
    var r = await fetchText("/auth/me");
    $("authOut").textContent = "HTTP " + r.status + "\n" + r.body;
    if (r.ok) setBadge(true, "authenticated");
    else setBadge(false, "not logged in");
  }

  async function refreshStatus() {
    var r = await fetchText("/api/v1/ca/status");
    $("statusOut").textContent = "HTTP " + r.status + "\n" + r.body;
  }

  async function listCa() {
    var r = await fetchText("/api/v1/ca/authorities");
    $("caListOut").textContent = "HTTP " + r.status + "\n" + r.body;
  }

  async function createCa() {
    var r = await fetchText("/api/v1/ca/authorities", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        name: $("caName").value,
        cn: $("caCn").value,
        days: parseInt($("caDays").value, 10) || 3650
      })
    });
    $("caListOut").textContent = "HTTP " + r.status + "\n" + r.body;
    await listCa();
    await refreshStatus();
  }

  async function listCerts() {
    var f = $("certFilter").value;
    var url = "/api/v1/ca/certs" + (f ? ("?status=" + encodeURIComponent(f)) : "");
    var r = await fetchText(url);
    var tbody = $("certBody");
    tbody.innerHTML = "";
    if (!r.ok) {
      $("certDetail").textContent = "HTTP " + r.status + "\n" + r.body;
      return;
    }
    try {
      var j = JSON.parse(r.body);
      (j.certificates || []).forEach(function (c) {
        var tr = document.createElement("tr");
        tr.innerHTML =
          "<td>" + c.id + "</td>" +
          "<td>" + c.serial + "</td>" +
          "<td class=\"kind\">" + (c.common_name || "") + "</td>" +
          "<td>" + (c.device_id || "") + "</td>" +
          "<td>" + (c.status || "") + "</td>" +
          "<td>" + (c.not_after || "") + "</td>" +
          "<td></td>";
        var td = tr.querySelector("td:last-child");
        var b1 = document.createElement("button");
        b1.type = "button";
        b1.className = "ghost";
        b1.textContent = "View";
        b1.onclick = function () { viewCert(c.id); };
        td.appendChild(b1);
        if (c.status === "valid") {
          var b2 = document.createElement("button");
          b2.type = "button";
          b2.textContent = "Revoke";
          b2.onclick = function () { revokeCert(c.id); };
          td.appendChild(b2);
        }
        tbody.appendChild(tr);
      });
      if (!(j.certificates || []).length) {
        tbody.innerHTML = "<tr><td colspan=\"7\" class=\"muted-cell\">No certificates</td></tr>";
      }
    } catch (e) {
      $("certDetail").textContent = r.body;
    }
  }

  async function viewCert(id) {
    var r = await fetchText("/api/v1/ca/certs/" + id);
    $("certDetail").textContent = "HTTP " + r.status + "\n" + r.body;
  }

  async function revokeCert(id) {
    if (!confirm("Revoke certificate " + id + "?")) return;
    var r = await fetchText("/api/v1/ca/certs/" + id + "/revoke", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ reason: "admin" })
    });
    $("certDetail").textContent = "HTTP " + r.status + "\n" + r.body;
    await listCerts();
    await refreshStatus();
  }

  async function signCsr() {
    var r = await fetchText("/api/v1/ca/sign", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        ca_id: parseInt($("signCaId").value, 10) || 1,
        device_id: $("signDevice").value,
        days: parseInt($("signDays").value, 10) || 825,
        csr_pem: $("csrPem").value
      })
    });
    $("signOut").textContent = "HTTP " + r.status + "\n" + r.body;
    await listCerts();
  }

  async function issueLeaf() {
    var r = await fetchText("/api/v1/ca/issue", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        ca_id: parseInt($("issueCaId").value, 10) || 1,
        cn: $("issueCn").value,
        device_id: $("issueDevice").value,
        days: parseInt($("issueDays").value, 10) || 825
      })
    });
    $("issueOut").textContent = "HTTP " + r.status + "\n" + r.body;
    await listCerts();
  }

  async function crlRebuild() {
    var r = await fetchText("/api/v1/ca/crl/rebuild", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ca_id: parseInt($("crlCaId").value, 10) || 1 })
    });
    $("crlOut").textContent = "HTTP " + r.status + "\n" + r.body;
  }

  async function refreshAll() {
    await refreshStatus();
    await listCa();
    await listCerts();
  }

  if ($("btnLogin")) $("btnLogin").addEventListener("click", login);
  if ($("btnMe")) $("btnMe").addEventListener("click", me);
  if ($("btnStatus")) $("btnStatus").addEventListener("click", refreshStatus);
  if ($("btnCreateCa")) $("btnCreateCa").addEventListener("click", createCa);
  if ($("btnListCa")) $("btnListCa").addEventListener("click", listCa);
  if ($("btnListCerts")) $("btnListCerts").addEventListener("click", listCerts);
  if ($("certFilter")) $("certFilter").addEventListener("change", listCerts);
  if ($("btnSign")) $("btnSign").addEventListener("click", signCsr);
  if ($("btnIssue")) $("btnIssue").addEventListener("click", issueLeaf);
  if ($("btnCrlRebuild")) $("btnCrlRebuild").addEventListener("click", crlRebuild);

  me().then(refreshAll).catch(function () { /* ignore */ });
})();
