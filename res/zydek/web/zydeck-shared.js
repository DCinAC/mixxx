// ZyDeck: what the two settings menus share, so they're one menu in two layouts. The phone page's Settings
// (System) and the deck page's (Controller) both build these from here: the head (System | Controller tabs,
// search, "n in the other menu"), the search's matching, and Link devices. Sizes are in em (zydeck-shared.css),
// so each takes its menu's scale: the upright page's, or the sideways window's.
(function () {
  const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; };
  // ZyDeck's app (a page in it, or the deck page in the phone page)
  const bridge = () => { try { return window.ZydekAndroid || parent.ZydekAndroid; } catch { return window.ZydekAndroid; } };

  /// The menus' head; active: "system" | "controller". head.tab(name) the tab button, head.q the search box,
  /// head.other the "n in the other menu ›" button.
  function menuHead(active) {
    const head = el("div", "zs-head",
      `<div class="zs-tabs"><button data-menu="system">System</button><button data-menu="controller">Controller</button></div>` +
      `<input class="zs-q" type="search" placeholder="Search settings" autocomplete="off" spellcheck="false">` +
      `<button class="zs-other" hidden></button>`);
    head.querySelector(`[data-menu="${active}"]`).classList.add("on");
    head.tab = name => head.querySelector(`[data-menu="${name}"]`);
    head.q = head.querySelector(".zs-q");
    head.other = head.querySelector(".zs-other");
    return head;
  }

  /// The search: every word typed, anywhere in an item's text.
  function matcher(q) {
    const words = String(q || "").trim().toLowerCase().split(/\s+/).filter(Boolean);
    return { any: words.length > 0, hit: node => words.every(w => node.textContent.toLowerCase().includes(w)) };
  }

  /// Link devices: a status line and Scan, then the ZyDecks found nearby (Android's network discovery), each
  /// with Connect: this device's own page (its version, its Real-time input) talking to that ZyDeck's server,
  /// as the tablet controller on a tablet or the phone layout on a phone.
  function linkDevices(box) {
    box.innerHTML = `<div class="zs-row"><span class="zs-st"></span><button class="zs-scan">Scan</button></div><div class="zs-devs"></div>`;
    const st = box.querySelector(".zs-st"), list = box.querySelector(".zs-devs");
    let scanning = false;
    box.querySelector(".zs-scan").addEventListener("click", async () => {
      const b = bridge();
      if (!b || !b.linkScan) { st.textContent = "Only in the ZyDeck app"; return; }
      if (scanning) return;
      scanning = true;
      b.linkScan();
      st.textContent = "Looking…"; list.innerHTML = "";
      try {
        for (let i = 0; i < 30; i++) {
          await new Promise(r => setTimeout(r, 500));
          let r; try { r = JSON.parse(b.linkDevices()); } catch { continue; }
          const seen = new Set([...list.children].map(x => x.dataset.key));
          for (const d of r.devices) {
            const key = `${d.host}:${d.port}`;
            if (seen.has(key)) continue;
            const row = el("div", "zs-dev", `<span>${esc(d.name)}<small>${esc(d.host)}</small></span><button>Connect</button>`);
            row.dataset.key = key;
            row.querySelector("button").addEventListener("click", () => {
              const page = Math.min(screen.width, screen.height) >= 600 ? "/controller?" : "/controller?phone=1&";
              b.linkOpen(`http://127.0.0.1:8766${page}hub=${encodeURIComponent(key)}`);
            });
            list.appendChild(row);
          }
          st.textContent = list.children.length ? `${list.children.length} found${r.scanning ? " · still looking" : ""}`
            : r.scanning ? "Looking…" : "None found: is the other ZyDeck open, on the same Wi-Fi?";
          if (!r.scanning && i > 4) break;
        }
      } finally { scanning = false; }
    });
  }

  window.ZydeckShared = { menuHead, matcher, linkDevices, bridge };
})();
