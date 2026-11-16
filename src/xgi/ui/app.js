// XGI Console: cluster dashboard + code console. Plain JS, no dependencies.
// Talks only to the coordinator that served it (same origin), with X-Cluster-Token on every call.
// Everything that comes from nodes is rendered with textContent, never as HTML.
"use strict";

(() => {
  const POLL_MS = 3000;
  const TOKEN_KEY = "xgi.clusterToken";

  const $ = (id) => document.getElementById(id);
  const state = {
    token: loadToken(),
    info: null,
    nodes: [],
    groups: [],
    lastOk: 0,
    selected: null,       // node_id shown in the detail drawer
    pollTimer: null,
    chat: [],             // [{role: "user", content} | {role: "assistant", box}]
    busy: false,
  };

  // ------------------------------------------------------------------ helpers

  function h(tag, props, ...children) {
    const el = document.createElement(tag);
    if (props) {
      for (const [k, v] of Object.entries(props)) {
        if (v == null || v === false) continue;
        if (k === "class") el.className = v;
        else if (k === "text") el.textContent = v;
        else if (k.startsWith("on")) el.addEventListener(k.slice(2), v);
        else if (k === "dataset") Object.assign(el.dataset, v);
        else el.setAttribute(k, v === true ? "" : String(v));
      }
    }
    for (const c of children.flat()) {
      if (c == null || c === false) continue;
      el.append(c instanceof Node ? c : document.createTextNode(String(c)));
    }
    return el;
  }

  function gb(bytes) {
    if (bytes == null || isNaN(bytes)) return "–";
    return (bytes / 1e9).toFixed(bytes >= 1e11 ? 0 : 1) + " GB";
  }

  function ago(epochSeconds) {
    if (!epochSeconds) return "never";
    const s = Math.max(0, Date.now() / 1000 - epochSeconds);
    if (s < 2) return "just now";
    if (s < 90) return Math.round(s) + " s ago";
    if (s < 5400) return Math.round(s / 60) + " min ago";
    return Math.round(s / 3600) + " h ago";
  }

  function duration(s) {
    if (s == null) return "–";
    if (s < 1) return Math.round(s * 1000) + " ms";
    if (s < 120) return s.toFixed(1) + " s";
    if (s < 7200) return Math.round(s / 60) + " min";
    return (s / 3600).toFixed(1) + " h";
  }

  function shortOs(os) {
    if (!os) return "–";
    const m = /^(Windows|Linux|macOS)-([^-]+)/.exec(os);
    if (!m) return os;
    return m[1] === "Linux" ? "Linux " + m[2].split(".").slice(0, 2).join(".") : m[1] + " " + m[2];
  }

  function loadToken() {
    try {
      return sessionStorage.getItem(TOKEN_KEY) || localStorage.getItem(TOKEN_KEY) || "";
    } catch {
      return "";
    }
  }

  function saveToken(token, remember) {
    state.token = token;
    try {
      sessionStorage.setItem(TOKEN_KEY, token);
      if (remember) localStorage.setItem(TOKEN_KEY, token);
      else localStorage.removeItem(TOKEN_KEY);
    } catch { /* storage blocked: keep it in memory */ }
  }

  function forgetToken() {
    state.token = "";
    try {
      sessionStorage.removeItem(TOKEN_KEY);
      localStorage.removeItem(TOKEN_KEY);
    } catch { /* ignore */ }
  }

  async function api(method, path, body) {
    const headers = { "Accept": "application/json" };
    if (state.token) headers["X-Cluster-Token"] = state.token;
    if (body !== undefined) headers["Content-Type"] = "application/json";
    let res;
    try {
      res = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body), cache: "no-store" });
    } catch (e) {
      return { status: 0, json: null, error: "cannot reach the coordinator (" + e.message + ")" };
    }
    let json = null;
    try { json = await res.json(); } catch { /* not JSON */ }
    const error = res.ok ? null : (json && json.error && json.error.message) || ("HTTP " + res.status);
    if (res.status === 401) askToken("The coordinator rejected the token.");
    return { status: res.status, json, error };
  }

  // ------------------------------------------------------------------ token dialog

  function askToken(message) {
    const dlg = $("token-dialog");
    if (dlg.open) return;
    $("token-input").value = state.token;
    $("token-remember").checked = (() => { try { return !!localStorage.getItem(TOKEN_KEY); } catch { return false; } })();
    $("token-error").hidden = !message;
    $("token-error").textContent = message || "";
    dlg.showModal();
    $("token-input").focus();
  }

  function initTokenDialog() {
    $("token-btn").addEventListener("click", () => askToken(""));
    $("token-form").addEventListener("submit", () => {
      saveToken($("token-input").value.trim(), $("token-remember").checked);
      start();
    });
    $("token-forget").addEventListener("click", () => {
      forgetToken();
      $("token-input").value = "";
      $("token-error").hidden = false;
      $("token-error").textContent = "Token removed from this browser.";
    });
  }

  // ------------------------------------------------------------------ data + polling

  async function start() {
    const info = await api("GET", "/console/info");
    if (info.status !== 200) {
      if (info.status !== 401) setRefreshState(info.error, true);
      return;
    }
    state.info = info.json;
    $("version").textContent = "v" + state.info.version;
    $("target-timeout").value = Math.round(state.info.exec_timeout_s || 60);
    renderExecNotice();
    await poll();
  }

  async function poll() {
    clearTimeout(state.pollTimer);
    const [n, g] = await Promise.all([api("GET", "/nodes"), api("GET", "/groups")]);
    if (n.status === 200 && g.status === 200) {
      state.nodes = (n.json || []).slice().sort((a, b) => String(a.node_id).localeCompare(String(b.node_id), undefined, { numeric: true }));
      state.groups = g.json || [];
      state.lastOk = Date.now();
      renderAll();
      setRefreshState("", false);
    } else if (n.status !== 401 && g.status !== 401) {
      setRefreshState(n.error || g.error, true);
    }
    if (n.status !== 401 && g.status !== 401) state.pollTimer = setTimeout(poll, POLL_MS);
  }

  function setRefreshState(err, stale) {
    const el = $("refresh-state");
    el.classList.toggle("stale", !!stale);
    if (err) el.textContent = "⚠ " + err;
    else el.textContent = "Updated " + new Date(state.lastOk).toLocaleTimeString();
  }

  function nodeById(id) { return state.nodes.find((n) => n.node_id === id) || null; }
  function groupById(id) { return state.groups.find((g) => g.group_id === id) || null; }

  // Health = node liveness + its group's state + its own processes.
  function health(n) {
    if (n.status !== "alive") return { cls: "bad", text: "Dead", detail: "no heartbeat since " + ago(n.last_seen) };
    const procs = n.processes || {};
    const exited = Object.entries(procs).find(([, p]) => p && p.state === "exited");
    if (n.draining) return { cls: "warn", text: "Draining", detail: "not used for new groups" };
    if (n.group_id) {
      const g = groupById(n.group_id);
      const gs = g ? g.status : "unknown";
      if (exited) return { cls: "bad", text: exited[0] + " exited", detail: "exit code " + exited[1].exit_code };
      if (gs === "healthy") return { cls: "ok", text: "Serving", detail: "group " + n.group_id + " is healthy" };
      if (gs === "starting" || gs === "loading") return { cls: "warn", text: gs === "loading" ? "Loading" : "Starting", detail: "group " + n.group_id };
      return { cls: "bad", text: gs, detail: (g && g.error) || "group " + n.group_id };
    }
    return { cls: "ok", text: "Idle", detail: "free node, heartbeat " + ago(n.last_seen) };
  }

  // ------------------------------------------------------------------ rendering

  function renderAll() {
    renderSummary();
    renderNodes();
    renderGroups();
    renderDetail();
    renderTargets();
  }

  function renderSummary() {
    const alive = state.nodes.filter((n) => n.status === "alive");
    const dead = state.nodes.length - alive.length;
    const healthy = state.groups.filter((g) => g.status === "healthy").length;
    const bad = state.groups.filter((g) => ["degraded", "failed", "unplaced"].includes(g.status)).length;
    const exec = alive.filter((n) => n.exec_enabled).length;
    const ramFree = alive.reduce((s, n) => s + (n.ram_available || 0), 0);
    const chip = (dot, label, value) => h("span", { class: "chip" }, dot ? h("span", { class: "dot " + dot }) : null, label, " ", h("b", { text: value }));
    $("summary").replaceChildren(...[
      chip("ok", "Nodes alive", alive.length),
      dead ? chip("bad", "Dead", dead) : null,
      chip(bad ? "warn" : healthy ? "ok" : "", "Groups healthy", healthy + " / " + state.groups.length),
      chip("", "Free RAM", gb(ramFree)),
      chip("", "Exec-enabled", exec),
    ].filter(Boolean));
  }

  function nodeMatches(n, q, f) {
    if (f === "alive" && n.status !== "alive") return false;
    if (f === "dead" && n.status === "alive") return false;
    if (f === "free" && (n.group_id || n.status !== "alive")) return false;
    if (f === "serving" && !n.group_id) return false;
    if (f === "exec" && !n.exec_enabled) return false;
    if (!q) return true;
    const langs = (n.runtimes || []).map((r) => r.language + " " + r.label).join(" ");
    return [n.node_id, n.hostname, n.ip, n.os, n.cpu_model, n.group_id, langs].join(" ").toLowerCase().includes(q);
  }

  function ramCell(n) {
    if (!n.ram_total) return h("td", null, "–");
    const free = n.ram_available || 0;
    const used = 1 - free / n.ram_total;
    const bar = h("span");
    bar.style.width = Math.round((free / n.ram_total) * 100) + "%";
    if (used > 0.9) bar.className = "bad";
    else if (used > 0.75) bar.className = "warn";
    return h("td", { class: "nowrap" }, gb(free) + " / " + gb(n.ram_total), h("div", { class: "bar" }, bar));
  }

  function langBadges(n, limit) {
    const rts = n.runtimes || [];
    const shown = limit ? rts.slice(0, limit) : rts;
    const out = shown.map((r) => h("span", { class: "badge accent", title: r.label + (r.version ? " — " + r.version : ""), text: r.language }));
    if (limit && rts.length > limit) out.push(h("span", { class: "badge", title: rts.slice(limit).map((r) => r.language).join(", "), text: "+" + (rts.length - limit) }));
    if (!rts.length) out.push(h("span", { class: "muted", text: "none detected" }));
    return out;
  }

  function renderNodes() {
    const q = $("node-filter").value.trim().toLowerCase();
    const f = $("node-status-filter").value;
    const rows = state.nodes.filter((n) => nodeMatches(n, q, f)).map((n) => {
      const hl = health(n);
      const role = n.role ? h("span", { class: "badge " + (n.role === "head" ? "info" : ""), text: n.role }) : h("span", { class: "muted", text: "free" });
      const tr = h("tr", { class: n.node_id === state.selected ? "selected" : null, tabindex: 0, onclick: () => openDetail(n.node_id),
        onkeydown: (e) => { if (e.key === "Enter") openDetail(n.node_id); } },
        h("td", null, h("div", { class: "node-name", text: n.node_id }), h("div", { class: "node-sub", text: n.ip + ":" + n.agent_port })),
        h("td", { title: hl.detail }, h("span", { class: "health" }, h("span", { class: "dot " + hl.cls }), hl.text),
          h("div", { class: "node-sub", text: ago(n.last_seen) })),
        h("td", null, role, n.group_id ? h("div", { class: "node-sub", text: n.group_id }) : null,
          n.draining ? h("span", { class: "badge warn", text: "draining" }) : null),
        ramCell(n),
        h("td", { class: "nowrap", title: n.cpu_model || "" }, (n.cpu_physical || "?") + "c / " + (n.cpu_logical || "?") + "t",
          h("div", { class: "node-sub", text: n.arch || "" })),
        h("td", { class: "nowrap", title: n.os || "" }, shortOs(n.os)),
        h("td", null, h("div", { class: "langs" }, langBadges(n, 6), n.exec_enabled ? h("span", { class: "badge ok", title: "xi --allow-exec", text: "exec" }) : null)),
      );
      return tr;
    });
    $("nodes-body").replaceChildren(...rows);
    $("nodes-empty").hidden = state.nodes.length > 0;
  }

  function statusBadge(s) {
    const cls = { healthy: "ok", starting: "warn", loading: "warn", degraded: "bad", failed: "bad", unplaced: "bad", stopped: "" }[s] ?? "";
    return h("span", { class: "badge " + cls, text: s });
  }

  function renderGroups() {
    const rows = state.groups.map((g) => h("tr", null,
      h("td", null, h("div", { class: "node-name", text: g.group_id }), h("div", { class: "node-sub", text: g.pool })),
      h("td", null, statusBadge(g.status), g.healthy_at && g.status === "healthy" ? h("div", { class: "node-sub", text: "since " + ago(g.healthy_at) }) : null),
      h("td", null, g.head || "–", g.head_url ? h("div", { class: "node-sub", text: g.head_url }) : null),
      h("td", null, (g.workers || []).join(", ") || "–"),
      h("td", null, String(g.slots)),
      h("td", null, g.error ? h("span", { class: "err-text", text: g.error }) : ""),
    ));
    $("groups-body").replaceChildren(...rows);
    $("groups-empty").hidden = state.groups.length > 0;
    $("groups-count").textContent = state.groups.length ? state.groups.length + " group(s)" : "";
  }

  // ------------------------------------------------------------------ node detail

  function openDetail(id) {
    state.selected = id;
    $("detail").hidden = false;
    renderDetail();
    renderNodes();
  }

  function closeDetail() {
    state.selected = null;
    $("detail").hidden = true;
    renderNodes();
  }

  function kv(pairs) {
    return h("dl", { class: "kv" }, pairs.filter(Boolean).flatMap(([k, v]) => [h("dt", { text: k }), h("dd", null, v ?? "–")]));
  }

  function renderDetail() {
    if (!state.selected) return;
    const n = nodeById(state.selected);
    $("detail-title").textContent = state.selected;
    if (!n) {
      $("detail-body").replaceChildren(h("p", { class: "empty", text: "This node is no longer registered." }));
      return;
    }
    const hl = health(n);
    const procs = n.processes || {};
    const procRows = ["rpc", "head"].map((role) => {
      const p = procs[role] || { state: "stopped" };
      const cls = p.state === "running" ? "ok" : p.state === "exited" ? "bad" : "";
      return h("tr", null, h("td", { text: role === "rpc" ? "rpc-server" : "llama-server" }), h("td", null, h("span", { class: "badge " + cls, text: p.state })),
        h("td", { text: p.pid ? "pid " + p.pid : "" }), h("td", { text: p.state === "running" ? duration(p.uptime_s) : p.exit_code != null ? "exit " + p.exit_code : "" }));
    });
    const bins = n.binaries || {};
    const binBadge = (ok) => ok == null ? "–" : h("span", { class: "badge " + (ok ? "ok" : "bad"), text: ok ? "found" : "missing" });
    const rts = n.runtimes || [];
    const runtimeTable = rts.length
      ? h("table", { class: "mini" }, h("thead", null, h("tr", null, h("th", { text: "Language" }), h("th", { text: "Version" }), h("th", { text: "Path" }))),
          h("tbody", null, rts.map((r) => h("tr", null, h("td", null, h("span", { class: "badge accent", text: r.language }), " ", r.label),
            h("td", { text: r.version || "" }), h("td", { class: "path", text: r.path || "" })))))
      : h("p", { class: "muted", text: "No interpreters or compilers were found on this node's PATH." });

    const actions = h("div", { class: "detail-actions" },
      h("button", { type: "button", class: "primary", disabled: n.status !== "alive", onclick: () => { selectTarget(n.node_id); closeDetail(); $("prompt").focus(); } }, "Use in code console"),
      h("button", { type: "button", onclick: () => drain(n, !n.draining) }, n.draining ? "Undrain" : "Drain"),
    );

    $("detail-body").replaceChildren(
      h("p", null, h("span", { class: "health" }, h("span", { class: "dot " + hl.cls }), hl.text), " ", h("span", { class: "muted", text: "— " + hl.detail })),
      h("h3", { text: "Identity" }),
      kv([["Hostname", n.hostname], ["IP", n.ip], ["Ports", "agent " + n.agent_port + " · rpc " + n.rpc_port + " · head " + n.head_port],
          ["Agent", "XI " + (n.agent_version || "?")], ["Last heartbeat", ago(n.last_seen)]]),
      h("h3", { text: "System" }),
      kv([["OS", n.os], ["Architecture", n.arch], ["CPU", n.cpu_model || "unknown"], ["Cores", (n.cpu_physical || "?") + " physical / " + (n.cpu_logical || "?") + " logical"],
          ["RAM", gb(n.ram_available) + " free of " + gb(n.ram_total)], ["Disk free", n.disk_free == null ? "–" : gb(n.disk_free) + " (models dir)"]]),
      h("h3", { text: "Cluster" }),
      kv([["Status", n.status + (n.draining ? " (draining)" : "")], ["Group", n.group_id || "free"], ["Role", n.role || "–"]]),
      h("table", { class: "mini" }, h("tbody", null, procRows)),
      h("h3", { text: "llama.cpp" }),
      kv([["rpc-server", binBadge(bins.rpc_server)], ["llama-server", binBadge(bins.llama_server)],
          ["Models", (n.models || []).length ? h("div", null, (n.models || []).map((m) => h("div", { class: "node-sub", text: m }))) : "none"]]),
      h("h3", { text: "Languages & code execution" }),
      kv([["Console exec", h("span", { class: "badge " + (n.exec_enabled ? "ok" : ""), text: n.exec_enabled ? "allowed (--allow-exec)" : "off" })]]),
      runtimeTable,
      actions,
    );
  }

  async function drain(n, on) {
    const verb = on ? "Drain" : "Undrain";
    if (on && n.group_id && !confirm(`Drain ${n.node_id}? Its group ${n.group_id} will be repaired on other nodes.`)) return;
    const r = await api("POST", (on ? "/drain/" : "/undrain/") + encodeURIComponent(n.node_id));
    if (r.status !== 200) alert(verb + " failed: " + r.error);
    poll();
  }

  // ------------------------------------------------------------------ code console

  function selectedNode() { return nodeById($("target-node").value); }

  function selectTarget(id) {
    $("target-node").value = id;
    renderLangs();
    renderExecNotice();
    refreshRunButtons();
  }

  function renderTargets() {
    const sel = $("target-node");
    const current = sel.value;
    const opts = state.nodes.map((n) => {
      const langs = (n.runtimes || []).length;
      const o = h("option", { value: n.node_id, disabled: n.status !== "alive" },
        n.node_id + " — " + shortOs(n.os) + (n.status !== "alive" ? " (dead)" : ` · ${langs} lang${langs === 1 ? "" : "s"}${n.exec_enabled ? "" : " · no exec"}`));
      return o;
    });
    if (!opts.length) opts.push(h("option", { value: "", text: "No nodes" }));
    sel.replaceChildren(...opts);
    const keep = nodeById(current);
    if (keep && keep.status === "alive") sel.value = current;
    else {
      const best = state.nodes.find((n) => n.status === "alive" && n.exec_enabled && (n.runtimes || []).length) ||
                   state.nodes.find((n) => n.status === "alive");
      sel.value = best ? best.node_id : "";
    }
    renderLangs();
    renderExecNotice();
    refreshRunButtons();
  }

  function renderLangs() {
    const sel = $("target-lang");
    const current = sel.value;
    const n = selectedNode();
    const rts = (n && n.runtimes) || [];
    const opts = rts.map((r) => h("option", { value: r.language }, r.label + (r.version ? " — " + r.version : "")));
    if (!opts.length) opts.push(h("option", { value: "", text: n ? "No languages detected" : "—" }));
    sel.replaceChildren(...opts);
    if (rts.some((r) => r.language === current)) sel.value = current;
    else {
      const pref = ["python", "powershell", "pwsh", "javascript", "bash"].find((l) => rts.some((r) => r.language === l));
      sel.value = pref || (rts[0] ? rts[0].language : "");
    }
  }

  function renderExecNotice() {
    const el = $("exec-notice");
    const n = selectedNode();
    let msg = "";
    if (state.info && !state.info.allow_exec) msg = "Running code is turned off on the coordinator (console.allow_exec in cluster.yaml). You can still generate code.";
    else if (n && !n.exec_enabled) msg = `${n.node_id} does not allow code execution. Start its agent with --allow-exec to run code there.`;
    else if (n && n.group_id) msg = `${n.node_id} is serving ${n.group_id} (${n.role}). Programs you run compete with inference for CPU and RAM.`;
    el.hidden = !msg;
    el.textContent = msg;
  }

  function scrollChat() {
    const log = $("chat-log");
    log.scrollTop = log.scrollHeight;
  }

  function addMessage(el) {
    const hint = document.querySelector(".chat-hint");
    if (hint) hint.remove();
    $("chat-log").append(el);
    scrollChat();
    return el;
  }

  function userMessage(text) {
    return addMessage(h("div", { class: "msg user" }, h("div", { class: "msg-meta", text: "You" }), h("div", { class: "bubble", text })));
  }

  function errorMessage(text) {
    return addMessage(h("div", { class: "msg error" }, h("div", { class: "bubble", text })));
  }

  function pendingMessage(label) {
    const started = Date.now();
    const bubble = h("div", { class: "bubble", text: label });
    const el = addMessage(h("div", { class: "msg pending" }, bubble));
    const t = setInterval(() => { bubble.textContent = `${label} ${Math.round((Date.now() - started) / 1000)} s`; }, 1000);
    return { remove() { clearInterval(t); el.remove(); } };
  }

  function langLabel(n, lang) {
    const r = ((n && n.runtimes) || []).find((x) => x.language === lang);
    return r ? r.label : lang;
  }

  function codeBox(lang, code, meta, rawReply) {
    const frag = $("tpl-code").content.firstElementChild.cloneNode(true);
    frag.dataset.lang = lang;
    frag.querySelector(".msg-meta").textContent = meta;
    frag.querySelector(".code-lang").textContent = langLabel(selectedNode(), lang);
    const ta = frag.querySelector("textarea.code");
    ta.value = code;
    ta.rows = Math.min(24, Math.max(6, code.split("\n").length + 1));
    ta.addEventListener("keydown", (e) => {  // Tab indents instead of leaving the editor
      if (e.key !== "Tab" || e.shiftKey) return;
      e.preventDefault();
      const s = ta.selectionStart;
      ta.setRangeText("    ", s, ta.selectionEnd, "end");
    });
    const raw = frag.querySelector("pre.raw");
    const rawBtn = frag.querySelector(".act-raw");
    if (rawReply) raw.textContent = rawReply;
    else rawBtn.hidden = true;
    rawBtn.addEventListener("click", () => {
      raw.hidden = !raw.hidden;
      rawBtn.textContent = raw.hidden ? "Model reply" : "Hide reply";
    });
    frag.querySelector(".act-copy").addEventListener("click", async (e) => {
      try {
        await navigator.clipboard.writeText(ta.value);
        e.target.textContent = "Copied";
      } catch {
        ta.select();
        e.target.textContent = "Press Ctrl+C";
      }
      setTimeout(() => { e.target.textContent = "Copy"; }, 1500);
    });
    frag.querySelector(".act-run").addEventListener("click", () => runCode(frag));
    addMessage(frag);
    refreshRunButtons();
    return frag;
  }

  // Why a code box can't run on the selected node, or "" if it can.
  function runBlocker(lang) {
    const n = selectedNode();
    if (!state.info || !state.info.allow_exec) return "Code execution is disabled on the coordinator (console.allow_exec).";
    if (!n) return "Select a node.";
    if (n.status !== "alive") return n.node_id + " is dead.";
    if (!n.exec_enabled) return n.node_id + " does not allow code execution (xi --allow-exec).";
    if (!(n.runtimes || []).some((r) => r.language === lang)) return n.node_id + " has no " + lang + " runtime.";
    return "";
  }

  function refreshRunButtons() {
    const n = selectedNode();
    for (const box of document.querySelectorAll(".msg.assistant")) {
      const btn = box.querySelector(".act-run");
      const why = runBlocker(box.dataset.lang);
      btn.disabled = !!why || box.dataset.running === "1";
      btn.title = why || "Run this code on " + n.node_id;
      btn.textContent = box.dataset.running === "1" ? "Running…" : "▶ Run on " + (n ? n.node_id : "node");
      box.querySelector(".code-target").textContent = why ? why : `${n.node_id} · ${langLabel(n, box.dataset.lang)}`;
    }
  }

  async function runCode(box) {
    const n = selectedNode();
    const lang = box.dataset.lang;
    if (runBlocker(lang)) return;
    const code = box.querySelector("textarea.code").value;
    if (!code.trim()) return;
    const timeout = Math.max(1, Number($("target-timeout").value) || 60);
    box.dataset.running = "1";
    refreshRunButtons();
    const started = Date.now();
    const head = h("div", { class: "run-head" }, h("span", { class: "dot warn" }), h("b", { text: n.node_id }), h("span", { class: "muted", text: "running…" }));
    const out = h("pre", { text: "" });
    const run = h("div", { class: "run" }, head, out);
    out.hidden = true;
    box.querySelector(".runs").prepend(run);
    const ticker = setInterval(() => { head.lastChild.textContent = `running… ${Math.round((Date.now() - started) / 1000)} s`; }, 1000);

    const r = await api("POST", "/console/exec", { node_id: n.node_id, language: lang, code, timeout_s: timeout });
    clearInterval(ticker);
    box.dataset.running = "";
    refreshRunButtons();
    if (r.status !== 200) {
      head.replaceChildren(h("span", { class: "dot bad" }), h("b", { text: n.node_id }), h("span", { class: "err-text", text: r.error }));
      return;
    }
    const j = r.json;
    const okRun = j.exit_code === 0 && !j.timed_out;
    head.replaceChildren(...[
      h("span", { class: "dot " + (okRun ? "ok" : "bad") }),
      h("b", { text: j.node_id }),
      j.timed_out ? h("span", { class: "badge bad", text: "timed out after " + timeout + " s" })
                  : h("span", { class: "badge " + (okRun ? "ok" : "bad"), text: "exit " + j.exit_code }),
      j.stage === "compile" && !okRun ? h("span", { class: "badge warn", text: "compile failed" }) : null,
      h("span", { class: "muted", text: duration(j.duration_s) + " · " + (j.runtime || lang) }),
      h("span", { class: "spacer" }),
      h("span", { class: "muted", text: new Date().toLocaleTimeString() }),
    ].filter(Boolean));
    const lines = j.output || [];
    out.textContent = lines.length ? lines.join("\n") : "(no output)";
    if (j.output_truncated) out.textContent = "… (only the last " + lines.length + " lines are kept)\n" + out.textContent;
    out.hidden = false;
    scrollChat();
  }

  // Earlier turns for the model; assistant turns carry the code as it is now (including edits).
  function history() {
    return state.chat.map((t) => t.role === "user"
      ? { role: "user", content: t.content }
      : { role: "assistant", content: "```" + t.box.dataset.lang + "\n" + t.box.querySelector("textarea.code").value + "\n```" });
  }

  async function generate(e) {
    e.preventDefault();
    if (state.busy) return;
    const prompt = $("prompt").value.trim();
    const n = selectedNode();
    const lang = $("target-lang").value;
    if (!prompt) return;
    if (!lang) { errorMessage("Pick a node that has at least one language runtime."); return; }
    state.busy = true;
    $("send-btn").disabled = true;
    const hist = history();
    userMessage(prompt);
    $("prompt").value = "";
    const model = (state.info && state.info.model) || "the cluster";
    const pending = pendingMessage(`Generating ${langLabel(n, lang)} with ${model}… (slow pools can take minutes)`);
    const r = await api("POST", "/console/generate", { node_id: n ? n.node_id : null, language: lang, prompt, history: hist });
    pending.remove();
    state.busy = false;
    $("send-btn").disabled = false;
    if (r.status !== 200) {
      errorMessage("Code generation failed: " + r.error);
      $("prompt").value = prompt;
      return;
    }
    const j = r.json;
    const meta = `${j.model}${j.group ? " · " + j.group : ""} · ${duration(j.duration_s)}${j.finish_reason === "length" ? " · cut off at max_tokens" : ""}`;
    state.chat.push({ role: "user", content: prompt });
    const box = codeBox(j.language, j.code || "", meta, j.reply);
    state.chat.push({ role: "assistant", box });
  }

  function initConsole() {
    $("chat-form").addEventListener("submit", generate);
    $("prompt").addEventListener("keydown", (e) => {
      if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) generate(e);
    });
    $("target-node").addEventListener("change", () => { renderLangs(); renderExecNotice(); refreshRunButtons(); });
    $("target-lang").addEventListener("change", refreshRunButtons);
    $("paste-code").addEventListener("click", () => {
      const lang = $("target-lang").value;
      if (!lang) { errorMessage("Pick a node that has at least one language runtime."); return; }
      const box = codeBox(lang, "", "Your code", null);
      box.querySelector("textarea.code").focus();
    });
    $("chat-clear").addEventListener("click", () => {
      state.chat = [];
      $("chat-log").replaceChildren(h("div", { class: "chat-hint" }, h("p", null, h("strong", { text: "New conversation." }), " Describe a task to generate a program.")));
    });
  }

  // ------------------------------------------------------------------ boot

  document.addEventListener("DOMContentLoaded", () => {
    initTokenDialog();
    initConsole();
    $("detail-close").addEventListener("click", closeDetail);
    document.addEventListener("keydown", (e) => { if (e.key === "Escape" && !$("detail").hidden) closeDetail(); });
    $("node-filter").addEventListener("input", renderNodes);
    $("node-status-filter").addEventListener("change", renderNodes);
    start();
  });
})();
