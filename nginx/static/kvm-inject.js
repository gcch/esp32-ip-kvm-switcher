(function () {
  // -----------------------------------------
  // Theme definitions
  // -----------------------------------------
  const THEMES = {
    light: {
      bar:         "display:flex;flex-wrap:wrap;gap:0.5rem 0;align-items:center;padding:0.4rem 1rem;background:rgba(255,255,255,0.8);border-top:1px solid #BCBFC4;min-width:0;width:100%;box-sizing:border-box;",
      group:       "display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:0.5rem;padding:0 1rem;min-width:0;flex-shrink:1;",
      title:       "padding:0.25rem 0.5rem; color:#000; font-size:12px; font-family:Circular,sans-serif; font-weight:600;",
      divider:     "width:0px;height:1.25rem;background:#BCBFC4;margin:0;flex-shrink:0;",
      btnBase:     "padding:0.25rem 0.5rem; border-radius:4px; border:1px solid #BCBFC4; background:#fff; color:#000; font-size:12px;font-weight:500;cursor:pointer;transition:background 0.15s,color 0.15s,border-color 0.15s;white-space:nowrap;flex-shrink:0;",
      btnHover:    "#F3F7FE",
      btnActive:   { bg: "#1a56db", color: "#fff",   border: "#1a56db" },
      btnInactive: { bg: "#fff",    color: "#000",    border: "#BCBFC4" },
      btnPress:    "#EBF2FD",
      btnError:    { bg: "#fee2e2", border: "#f87171" },
    },
    dark: {
      bar:         "display:flex;flex-wrap:wrap;gap:0.5rem 0;align-items:center;padding:0.4rem 1rem;background:rgba(15,23,42,0.85);border-top:1px solid #334155;min-width:0;width:100%;box-sizing:border-box;",
      group:       "display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:0.5rem;padding:0 1rem;min-width:0;flex-shrink:1;",
      title:       "padding:0.25rem 0.5rem; color:#fff; font-size:12px; font-family:Circular,sans-serif; font-weight:600;",
      divider:     "width:0px;height:1.25rem;background:#334155;margin:0;flex-shrink:0;",
      btnBase:     "padding:0.25rem 0.5rem; border-radius:4px; border:1px solid #334155; background:#1e293b; color:#fff; font-size:12px;font-weight:500;cursor:pointer;transition:background 0.15s,color 0.15s,border-color 0.15s;white-space:nowrap;flex-shrink:0;",
      btnHover:    "#273549",
      btnActive:   { bg: "#1d4ed8", color: "#fff",   border: "#3b82f6" },
      btnInactive: { bg: "#1e293b", color: "#fff",    border: "#334155" },
      btnPress:    "#1e3a5f",
      btnError:    { bg: "#450a0a", border: "#dc2626" },
    },
  };

  let activeChannel = null;
  const inputButtons = {};
  const allButtons   = [];  // All buttons (INPUT + EDID)

  // -----------------------------------------
  // Dark mode detection
  // -----------------------------------------
  function isDark() {
    return document.documentElement.classList.contains("dark");
  }

  function getTheme() {
    return isDark() ? THEMES.dark : THEMES.light;
  }

  // -----------------------------------------
  // Apply theme to the entire bar
  // -----------------------------------------
  function applyTheme() {
    const t   = getTheme();
    const bar = document.getElementById("kvm-switch-bar");
    if (!bar) return;

    bar.style.cssText = t.bar;

    const titleEl = document.getElementById("kvm-switch-title");
    if (titleEl) titleEl.style.cssText = t.title;

    document.querySelectorAll(".kvm-group").forEach(g => {
      g.style.cssText = t.group;
    });

    document.querySelectorAll(".kvm-divider").forEach(d => {
      d.style.cssText = t.divider;
    });

    allButtons.forEach(btn => {
      btn.style.cssText = t.btnBase;
    });

    // Re-apply active state after theme change
    setActiveChannel(activeChannel, true);
  }

  // -----------------------------------------
  // Highlight the active channel button
  // -----------------------------------------
  function setActiveChannel(ch, themeChangeOnly = false) {
    if (!themeChangeOnly) activeChannel = ch;
    const t = getTheme();

    for (let i = 1; i <= 4; i++) {
      const btn = inputButtons[i];
      if (!btn) continue;
      const isActive = i === (themeChangeOnly ? activeChannel : ch);
      const s = isActive ? t.btnActive : t.btnInactive;
      btn.style.background  = s.bg;
      btn.style.color       = s.color;
      btn.style.borderColor = s.border;
      btn.dataset.active    = isActive ? "1" : "";
      btn.onmouseleave      = isActive ? () => {} : () => { btn.style.background = getTheme().btnInactive.bg; };
    }
  }

  // -----------------------------------------
  // Button factory helper
  // -----------------------------------------
  function makeButton(label, onClick) {
    const t   = getTheme();
    const btn = document.createElement("button");
    btn.textContent   = label;
    btn.style.cssText = t.btnBase;
    btn.onmouseenter  = () => {
      if (!btn.dataset.active) btn.style.background = getTheme().btnHover;
    };
    btn.onmouseleave  = () => {
      if (!btn.dataset.active) btn.style.background = getTheme().btnInactive.bg;
    };
    btn.onclick = onClick;
    allButtons.push(btn);
    return btn;
  }

  // -----------------------------------------
  // Build a flex group wrapper
  // -----------------------------------------
  function makeGroup() {
    const t = getTheme();
    const g = document.createElement("div");
    g.className    = "kvm-group";
    g.style.cssText = t.group;
    return g;
  }

  // -----------------------------------------
  // Build a vertical divider
  // -----------------------------------------
  function makeDivider() {
    const t = getTheme();
    const d = document.createElement("div");
    d.className    = "kvm-divider";
    d.style.cssText = t.divider;
    return d;
  }

  // -----------------------------------------
  // Setup layout
  // -----------------------------------------
  function setupLayout(bar) {
    const root = document.getElementById("root");
    if (!root) return;

    document.body.style.display = "flex";
    document.body.style.flexDirection = "column";
    document.body.style.height = "100vh";
    document.body.style.overflow = "hidden";

    root.style.flex = "1 1 auto";
    root.style.minHeight = "0";
    root.style.overflow = "hidden";

    bar.style.flex = "0 0 auto";
  }
  
  // -----------------------------------------
  // Build and inject the toolbar bar
  // -----------------------------------------
  function injectButtons() {
    if (document.getElementById("kvm-switch-bar")) return;
    const toolbar = document.querySelector('[class*="ActionBar"], [class*="Toolbar"], header, [class*="header"]');
    if (!toolbar) return;

    const t   = getTheme();
    const bar = document.createElement("div");
    bar.id            = "kvm-switch-bar";
    bar.style.cssText = t.bar;

    // Title label
    //const title = document.createElement("div");
    //title.id            = "kvm-switch-title";
    //title.textContent   = "KVM Switcher";
    //title.style.cssText = t.title;
    //bar.appendChild(title);

    bar.appendChild(makeDivider());

    // INPUT 1-4 channel buttons (group)
    const inputGroup = makeGroup();
    [1, 2, 3, 4].forEach(ch => {
      const btn = makeButton(`INPUT ${ch}`, async () => {
        try {
          const res = await fetch("/esp32/api/input", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ channel: ch }),
          });
          if (!res.ok) throw new Error();
          setActiveChannel(ch);
        } catch {
          const tt = getTheme();
          btn.style.background  = tt.btnError.bg;
          btn.style.borderColor = tt.btnError.border;
          setTimeout(() => {
            const s = activeChannel === ch ? tt.btnActive : tt.btnInactive;
            btn.style.background  = s.bg;
            btn.style.borderColor = s.border;
          }, 1500);
        }
      });
      inputButtons[ch] = btn;
      inputGroup.appendChild(btn);
    });
    bar.appendChild(inputGroup);

    bar.appendChild(makeDivider());

    // EDID / Reset buttons (group)
    const edidGroup = makeGroup();
    [
      { label: "EDID ON/OFF",  path: "/api/edid/toggle",  resetToInput1: false },
      { label: "EDID Refresh", path: "/api/edid/refresh", resetToInput1: true  },
      { label: "Reset",        path: "/api/edid/factory", resetToInput1: true  },
    ].forEach(fn => {
      const btn = makeButton(fn.label, async () => {
        const tt = getTheme();
        btn.style.background = tt.btnPress;
        btn.disabled = true;
        try {
          const res = await fetch(`/esp32${fn.path}`, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
          });
          if (!res.ok) throw new Error();
          if (fn.resetToInput1) setActiveChannel(1);
        } catch {
          const ttt = getTheme();
          btn.style.background  = ttt.btnError.bg;
          btn.style.borderColor = ttt.btnError.border;
          setTimeout(() => {
            btn.style.background  = ttt.btnInactive.bg;
            btn.style.borderColor = ttt.btnInactive.border;
          }, 1500);
        } finally {
          setTimeout(() => {
            btn.style.background = getTheme().btnInactive.bg;
            btn.disabled = false;
          }, 500);
        }
      });
      edidGroup.appendChild(btn);
    });
    bar.appendChild(edidGroup);

    document.body.appendChild(bar);
    setupLayout(bar);

    // Fetch current channel and highlight on load
    fetch("/esp32/api/input")
      .then(r => r.json())
      .then(d => { if (d.channel) setActiveChannel(d.channel); })
      .catch(() => {});
  }

  // -----------------------------------------
  // Watch <html class="dark"> changes for auto theme switching
  // -----------------------------------------
  const themeObserver = new MutationObserver(() => applyTheme());
  themeObserver.observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["class"],
  });

  // Wait for React DOM to render
  const domObserver = new MutationObserver(() => injectButtons());
  domObserver.observe(document.body, { childList: true, subtree: true });
  injectButtons();
})();
