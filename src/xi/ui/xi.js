// XI Agent page: this node's status and settings. Plain JS, no dependencies.
// Talks only to the agent that served it (same origin), with X-Cluster-Token on every call.
// Everything from the API is rendered with textContent, never as HTML.
"use strict";

(() => {
  const POLL_MS = 3000;
  const TOKEN_KEY = "xi.clusterToken";
  const $ = (id) => document.getElementById(id);
  const state = { token: loadToken(), st: null, set: null, draft: null, dirty: false, timer: null, view: "status" };

  // ------------------------------------------------------------------ helpers

  function h(tag, props, ...children) {
    const el = document.createElement(tag);
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (v == null || v === false) continue;
        if (k === "class") el.className = v;
        else if (k === "text") el.textContent = v;
        else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
        else el.setAttribute(k, v === true ? "" : String(v));
      }
    }
    for (const c of children.flat()) {
      if (c == null || c === false) continue;
      el.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return el;
  }
  const gb = (b) => (b == null || isNaN(b) ? "–" : (b / 1e9).toFixed(b >= 1e11 ? 0 : 1) + " GB");
  function ago(t) {
    if (!t) return "never";
    const s = Math.max(0, Date.now() / 1000 - t);
    if (s < 2) return "just now";
    if (s < 90) return Math.round(s) + " s ago";
    if (s < 5400) return Math.round(s / 60) + " min ago";
    return Math.round(s / 3600) + " h ago";
  }
  function duration(s) {
    if (s == null) return "–";
    if (s < 120) return s.toFixed(1) + " s";
    if (s < 7200) return Math.round(s / 60) + " min";
    return (s / 3600).toFixed(1) + " h";
  }
  function kv(pairs) {
    return h("dl", { class: "kv" }, pairs.filter(Boolean).flatMap(([k, v]) => [h("dt", { text: k }), h("dd", null, v ?? "–")]));
  }
  const badge = (cls, text, title) => h("span", { class: "badge " + cls, text, title });

  function loadToken() {
    try { return sessionStorage.getItem(TOKEN_KEY) || localStorage.getItem(TOKEN_KEY) || ""; } catch { return ""; }
  }
  function saveToken(t, remember) {
    state.token = t;
    try {
      sessionStorage.setItem(TOKEN_KEY, t);
      if (remember) localStorage.setItem(TOKEN_KEY, t);
      else localStorage.removeItem(TOKEN_KEY);
    } catch { /* storage blocked */ }
  }
  function rememberedToken() { try { return !!localStorage.getItem(TOKEN_KEY); } catch { return false; } }

  async function api(method, path, body) {
    const headers = { Accept: "application/json" };
    if (state.token) headers["X-Cluster-Token"] = state.token;
    if (body !== undefined) headers["Content-Type"] = "application/json";
    let res;
    try {
      res = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body), cache: "no-store" });
    } catch (e) {
      return { status: 0, json: null, error: "cannot reach the agent (" + e.message + ")" };
    }
    let json = null;
    try { json = await res.json(); } catch { /* not JSON */ }
    const error = res.ok ? null : (json && json.error && json.error.message) || "HTTP " + res.status;
    if (res.status === 401) askToken("The agent rejected the token.");
    return { status: res.status, json, error };
  }

  // ------------------------------------------------------------------ token dialog

  function askToken(message) {
    const dlg = $("token-dialog");
    if (dlg.open) return;
    $("token-input").value = state.token;
    $("token-remember").checked = rememberedToken();
    $("token-error").hidden = !message;
    $("token-error").textContent = message || "";
    dlg.showModal();
  }

  // ------------------------------------------------------------------ status

  const LINK = {
    registered: ["ok", "Registered"],
    registering: ["warn", "Registering…"],
    error: ["bad", "Not connected"],
    not_configured: ["warn", "No coordinator set"],
    starting: ["warn", "Starting"],
  };

  async function poll() {
    clearTimeout(state.timer);
    const r = await api("GET", "/agent/state");
    if (r.status === 200) {
      state.st = r.json;
      renderStatus();
      $("refresh-state").classList.remove("stale");
      $("refresh-state").textContent = "Updated " + new Date().toLocaleTimeString();
    } else if (r.status !== 401) {
      $("refresh-state").classList.add("stale");
      $("refresh-state").textContent = "⚠ " + r.error;
    }
    if (r.status !== 401) state.timer = setTimeout(poll, POLL_MS);
  }

  function renderStatus() {
    const { node: n, link, version, started_at, exec_jobs } = state.st;
    $("node-title").textContent = n.node_id;
    $("version").textContent = "v" + version;
    document.title = "XI · " + n.node_id;
    const [lcls, ltext] = LINK[link.state] || ["", link.state];
    $("summary").replaceChildren(
      h("span", { class: "chip" }, h("span", { class: "dot " + lcls }), ltext),
      h("span", { class: "chip" }, "Exec ", h("b", { text: n.exec_enabled ? "on" : "off" })),
      h("span", { class: "chip" }, "Free RAM ", h("b", { text: gb(n.ram_available) })),
    );

    $("link-body").replaceChildren(...[
      h("p", null, h("span", { class: "health" }, h("span", { class: "dot " + lcls }), ltext)),
      link.state === "not_configured"
        ? h("p", { class: "notice-inline", text: "Set the coordinator URL on the Settings tab to join the cluster." }) : null,
      kv([["Coordinator", link.coordinator || "–"], ["Registered as", link.node_id || n.node_id],
          ["Advertised IP", n.ip], ["Registered", link.registered_at ? ago(link.registered_at) : "–"],
          ["Last heartbeat", link.last_heartbeat_ok ? ago(link.last_heartbeat_ok) : "–"],
          link.last_error ? ["Last error", h("span", { class: "err-text", text: link.last_error })] : null,
          ["Agent started", ago(started_at)]]),
    ].filter(Boolean));

    $("system-body").replaceChildren(kv([
      ["Hostname", n.hostname], ["OS", n.os], ["Architecture", n.arch], ["CPU", n.cpu_model || "unknown"],
      ["Cores", n.cpu_physical + " physical / " + n.cpu_logical + " logical"],
      ["RAM", gb(n.ram_available) + " free of " + gb(n.ram_total)],
      ["Disk free", n.disk_free == null ? "–" : gb(n.disk_free) + " (models folder)"],
      ["Ports", "agent " + n.agent_port + " · rpc " + n.rpc_port + " · head " + n.head_port],
    ]));

    const procRow = (role, label) => {
      const p = (n.processes || {})[role] || { state: "stopped" };
      const cls = p.state === "running" ? "ok" : p.state === "exited" ? "bad" : "";
      return h("tr", null, h("td", { text: label }), h("td", null, badge(cls, p.state)),
        h("td", { text: p.pid ? "pid " + p.pid : "" }),
        h("td", { text: p.state === "running" ? duration(p.uptime_s) : p.exit_code != null ? "exit " + p.exit_code : "" }));
    };
    const bin = (ok) => badge(ok ? "ok" : "bad", ok ? "found" : "missing");
    const models = n.models || [];
    $("llama-body").replaceChildren(
      h("table", { class: "mini" }, h("tbody", null, procRow("head", "llama-server (head)"), procRow("rpc", "rpc-server (worker)"))),
      kv([["rpc-server", bin(n.binaries && n.binaries.rpc_server)], ["llama-server", bin(n.binaries && n.binaries.llama_server)],
          ["Models", models.length ? h("div", { class: "models" }, models.map((m) => h("span", { class: "node-sub", text: m }))) : "none in the models folder"]]),
      h("p", { class: "cfg-help", text: "The coordinator starts and stops these processes. This page only shows them." }),
    );

    const rts = n.runtimes || [];
    $("lang-body").replaceChildren(
      kv([["Code execution", n.exec_enabled ? badge("ok", "allowed") : badge("", "off")], ["Jobs running", String(exec_jobs)]]),
      rts.length
        ? h("table", { class: "mini" }, h("thead", null, h("tr", null, ["Language", "Version", "Path"].map((t) => h("th", { text: t })))),
            h("tbody", null, rts.map((r) => h("tr", null, h("td", null, badge("accent", r.language), " ", r.label),
              h("td", { text: r.version }), h("td", { class: "path", text: r.path })))))
        : h("p", { class: "muted", text: "No interpreters or compilers were found on PATH." }),
    );
  }

  async function loadLogs() {
    const r = await api("GET", `/logs/${$("log-role").value}?lines=${$("log-lines").value}`);
    $("log-body").textContent = r.status === 200 ? ((r.json.lines || []).join("\n") || "(no output yet)") : "⚠ " + r.error;
  }

  // ------------------------------------------------------------------ settings

  // [key, label, type, help]
  const SECTIONS = [
    { title: "Connection", restart: false, fields: [
      ["coordinator", "Coordinator URL", "text", "e.g. http://192.168.1.10:8100. Changing it registers with the new coordinator at once."],
      ["name", "Node name (node_id)", "text", "Unique in the cluster. Renaming re-registers; the old name goes dead after the node timeout, and its group is repaired."],
      ["advertise_ip", "Advertised IP", "text", "The IP other nodes use to reach this one."],
      ["heartbeat_s", "Heartbeat interval (s)", "num"],
      ["@token", "Cluster token", "password", "Leave empty to keep the current one. A new token works at once but is not saved: set QWEN_CLUSTER_TOKEN to keep it."],
    ] },
    { title: "llama.cpp", restart: false, note: "Used the next time the coordinator starts a process.", fields: [
      ["llama_dir", "llama.cpp folder", "text"],
      ["rpc_server_bin", "rpc-server binary", "text", "Empty = rpc-server in the llama.cpp folder."],
      ["llama_server_bin", "llama-server binary", "text", "Empty = llama-server in the llama.cpp folder."],
      ["models_dir", "Models folder", "text", "Scanned for *.gguf files and reported to the coordinator."],
    ] },
    { title: "Code execution", restart: false, fields: [
      ["allow_exec", "Allow the XGI console to run code here", "bool"],
      ["exec_dir", "Working folder for programs", "text"],
      ["exec_max_timeout_s", "Longest run allowed (s)", "num"],
    ] },
    { title: "Network and logs", restart: true, fields: [
      ["host", "Listen address", "text"], ["port", "Agent port (this page)", "int"], ["log_dir", "Log folder", "text"],
      ["rpc_port", "rpc-server port", "int"], ["head_port", "llama-server port", "int"],
    ] },
  ];

  async function loadSettings(keepResult) {
    const r = await api("GET", "/agent/settings");
    if (r.status !== 200) {
      $("set-status").replaceChildren(h("div", { class: "err-text", text: "Cannot load the settings: " + r.error }));
      return;
    }
    state.set = r.json;
    state.draft = structuredClone(r.json.configured);
    state.draft["@token"] = "";
    state.dirty = false;
    renderSettings();
    if (!keepResult) $("set-result").hidden = true;
  }

  function field(key, label, type, help, restartSection) {
    const s = state.set, d = state.draft;
    const flag = s.cli_flags[key];
    const running = s.running[key];
    let input;
    if (type === "bool") {
      input = h("input", { type: "checkbox" });
      input.checked = !!d[key];
      // Turning execution on is only possible from a browser on this node (or with a flag / xi.yaml).
      if (key === "allow_exec" && !s.running.allow_exec && !s.local_request) {
        input.disabled = true;
        help = "Can only be turned on from a browser on this node (http://127.0.0.1:" + s.running.port + "/ui/), with --allow-exec, or in xi.yaml. It can be turned off from anywhere.";
      } else if (key === "allow_exec") {
        help = "Programs run with this agent's rights (SYSTEM for the scheduled task). The coordinator must also allow it (console.allow_exec).";
      }
      input.addEventListener("change", () => { d[key] = input.checked; state.dirty = true; });
    } else {
      const numeric = type === "int" || type === "num";
      input = h("input", { type: type === "password" ? "password" : numeric ? "number" : "text",
        step: type === "int" ? "1" : numeric ? "any" : null, spellcheck: "false", autocomplete: "off" });
      input.value = d[key] ?? "";
      const def = s.defaults[key];
      if (def) input.placeholder = "default: " + def;
      input.addEventListener("input", () => { d[key] = numeric ? (input.value === "" ? null : Number(input.value)) : input.value; state.dirty = true; });
    }
    const pendingRestart = restartSection && running !== undefined && JSON.stringify(running) !== JSON.stringify(s.configured[key]);
    const isBool = type === "bool";
    return h("label", { class: isBool ? "cfg-field check" : "cfg-field" }, isBool ? input : null,
      h("span", { class: "cfg-label" }, label,
        flag ? badge("info", flag, "Set on the command line: it wins again at the next start") : null,
        pendingRestart ? badge("warn", "running: " + running, "Saved, but the running agent still uses the old value until it restarts") : null),
      isBool ? null : input,
      help ? h("span", { class: "cfg-help", text: help }) : null);
  }

  function renderSettings() {
    const s = state.set;
    const tokenText = { flag: "from --token", env: "from QWEN_CLUSTER_TOKEN", ui: "changed on this page (not saved)", none: "NOT SET: the API is unauthenticated" }[s.token_source] || s.token_source;
    $("set-status").replaceChildren(...[
      h("div", null, h("span", { class: "muted", text: "File " }), h("code", { text: s.path }), " ",
        s.file_exists ? badge("ok", "loaded") : badge("warn", "not created yet")),
      Object.keys(s.cli_flags).length
        ? h("div", { class: "muted", text: "Set on the command line (these win at the next start): " + Object.values(s.cli_flags).join(" ") }) : null,
      h("div", { class: "muted", text: "Cluster token: " + tokenText }),
      s.restart_pending.length ? h("div", { class: "notice-inline", text: "Saved, applies after the agent restarts: " + s.restart_pending.join(", ") }) : null,
    ].filter(Boolean));
    $("set-form").replaceChildren(...SECTIONS.map((sec) => h("fieldset", { class: "cfg-section" },
      h("legend", null, sec.title + " ", badge(sec.restart ? "warn" : "ok", sec.restart ? "after restart" : "applies now")),
      sec.note ? h("p", { class: "cfg-help", text: sec.note }) : null,
      sec.fields.map(([k, l, t, help]) => field(k, l, t, help, sec.restart)))));
  }

  function showResult(r) {
    const box = $("set-result");
    box.hidden = false;
    if (r.status !== 200) {
      box.className = "cfg-result bad";
      box.replaceChildren(h("b", { text: "Not saved. " }), r.error);
      return;
    }
    const j = r.json;
    box.className = "cfg-result ok";
    box.replaceChildren(...[
      h("div", null, h("b", { text: "Saved " }), h("code", { text: j.path })),
      h("div", { text: "Applied now: " + (j.live.length ? j.live.join(", ") : "nothing changed") }),
      j.restart.length ? h("div", { class: "warn-text", text: "Takes effect after the agent restarts: " + j.restart.join(", ") }) : null,
      j.warnings.length ? h("ul", { class: "warn-list" }, j.warnings.map((w) => h("li", { text: w }))) : null,
    ].filter(Boolean));
  }

  async function saveSettings() {
    const d = state.draft, c = state.set.configured;
    const settings = {};
    for (const k of state.set.keys) if (JSON.stringify(d[k]) !== JSON.stringify(c[k])) settings[k] = d[k];
    const newToken = d["@token"];
    if (!Object.keys(settings).length && !newToken) { showResult({ status: 200, json: { path: state.set.path, live: [], restart: [], warnings: [] } }); return; }
    if ((("name" in settings) || ("coordinator" in settings)) &&
        !confirm("Changing the node name or coordinator re-registers this node now. If it serves a group, that group is repaired on other nodes.\n\nSave?")) return;
    $("set-save").disabled = true;
    const r = await api("PUT", "/agent/settings", newToken ? { settings, token: newToken } : { settings });
    $("set-save").disabled = false;
    if (r.status === 200 && newToken) saveToken(newToken, rememberedToken());
    showResult(r);
    if (r.status === 200) { await loadSettings(true); poll(); }
  }

  // ------------------------------------------------------------------ views + boot

  function showView(view) {
    state.view = view;
    for (const t of document.querySelectorAll(".tab")) t.classList.toggle("active", t.dataset.view === view);
    $("status-view").hidden = view !== "status";
    $("settings-view").hidden = view !== "settings";
    if (view === "settings" && !state.set) loadSettings();
    const hash = view === "status" ? "" : "#" + view;
    if (location.hash !== hash) history.replaceState(null, "", location.pathname + hash);
  }

  async function start() {
    await poll();
    loadLogs();
    if (location.hash === "#settings") showView("settings");
  }

  document.addEventListener("DOMContentLoaded", () => {
    $("token-btn").addEventListener("click", () => askToken(""));
    $("token-form").addEventListener("submit", () => { saveToken($("token-input").value.trim(), $("token-remember").checked); start(); });
    $("token-forget").addEventListener("click", () => {
      saveToken("", false);
      $("token-input").value = "";
      $("token-error").hidden = false;
      $("token-error").textContent = "Token removed from this browser.";
    });
    for (const t of document.querySelectorAll(".tab")) t.addEventListener("click", () => showView(t.dataset.view));
    $("reregister-btn").addEventListener("click", async () => { await api("POST", "/agent/register"); setTimeout(poll, 800); });
    $("detect-btn").addEventListener("click", async (e) => {
      e.target.disabled = true;
      e.target.textContent = "Detecting…";
      const r = await api("POST", "/agent/runtimes/detect");
      e.target.disabled = false;
      e.target.textContent = "Detect again";
      if (r.status !== 200) alert("Detection failed: " + r.error);
      poll();
    });
    $("log-refresh").addEventListener("click", loadLogs);
    $("log-role").addEventListener("change", loadLogs);
    $("log-lines").addEventListener("change", loadLogs);
    $("set-save").addEventListener("click", saveSettings);
    $("set-reload").addEventListener("click", () => { if (!state.dirty || confirm("Discard your unsaved changes?")) loadSettings(); });
    window.addEventListener("beforeunload", (e) => { if (state.dirty) e.preventDefault(); });
    start();
  });
})();
