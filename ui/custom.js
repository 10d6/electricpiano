// The wear map: what age has done to each key of this instrument (its serial
// number decides which keys; the Condition knobs decide how far). The engine
// works it out and sends it key by key (messages, API.md §9b), so the page
// shows exactly what plays. Click a key to have it serviced (its faults
// removed, saved with the patch); "New instrument" picks another serial.
const LOW = 21, HIGH = 108;
const WATCH = ["wear.age", "wear.serial", "wear.tuning", "wear.voicing", "wear.hammers",
  "wear.dampers", "wear.action", "wear.rattle"];

export function register(sk) {
  sk.widget("wearMap", (el, ctx) => {
    el.classList.add("wearmap");
    const head = document.createElement("div");
    head.className = "wearmap-head";
    const title = document.createElement("span");
    title.className = "wearmap-title";
    const btns = document.createElement("span");
    btns.className = "wearmap-btns";
    const mk = (text, tip, fn) => {
      const b = document.createElement("button");
      b.type = "button"; b.className = "btn"; b.textContent = text; b.title = tip;
      b.addEventListener("click", fn);
      btns.append(b);
      return b;
    };
    const canvas = document.createElement("canvas");
    canvas.className = "wearmap-canvas";
    canvas.setAttribute("role", "img");
    const legend = document.createElement("div");
    legend.className = "wearmap-legend";
    legend.innerHTML = '<span class="lg tune">tuning</span><span class="lg voice">voicing</span>' +
      '<span class="lg mech">mechanics</span><span class="lg svc">serviced</span>';
    head.append(title, btns);
    el.append(head, canvas, legend);

    const keys = new Map();   // note -> { cents, voicing, mech }
    let hover = -1;

    const serviced = () => new Set(((ctx.custom("service") || {}).keys) || []);
    const setServiced = (set) => ctx.setCustom("service", { keys: [...set].sort((a, b) => a - b) });

    mk("New instrument", "Another instrument of the same age: a new serial number", () => {
      ctx.set("wear.serial", 1 + Math.floor(Math.random() * 9999));
    });
    mk("Service all", "Fix every key (the electronics stay as they are)", () => {
      const s = new Set(); for (let n = LOW; n <= HIGH; n++) s.add(n); setServiced(s);
    });
    mk("Unservice", "Let every key's faults back", () => setServiced(new Set()));

    let timer = 0;
    const request = () => { clearTimeout(timer); timer = setTimeout(() => ctx.send("wear.map", 1), 80); };

    ctx.onMessage("wear.key", (note, cents, voicing, mech) => {
      keys.set(Math.round(note), { cents, voicing, mech });
      draw();
    });
    for (const id of WATCH) ctx.onParam(id, () => { request(); updateTitle(); });
    ctx.onCustom("service", () => { request(); draw(); });

    function updateTitle() {
      const serial = Math.round(ctx.get("wear.serial"));
      const age = ctx.get("wear.age");
      const word = age < 0.08 ? "showroom" : age < 0.3 ? "well kept" : age < 0.55 ? "gigged" : age < 0.8 ? "tired" : "barn find";
      title.textContent = `Serial ${serial} · ${word}`;
    }

    function keyAt(x, w) {
      const n = LOW + Math.floor((x / w) * (HIGH - LOW + 1));
      return Math.max(LOW, Math.min(HIGH, n));
    }
    canvas.addEventListener("click", (e) => {
      const r = canvas.getBoundingClientRect();
      const n = keyAt(e.clientX - r.left, r.width);
      const s = serviced();
      if (s.has(n)) s.delete(n); else s.add(n);
      setServiced(s);
    });
    canvas.addEventListener("mousemove", (e) => {
      const r = canvas.getBoundingClientRect();
      hover = keyAt(e.clientX - r.left, r.width);
      const k = keys.get(hover);
      const name = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][hover % 12] + (Math.floor(hover / 12) - 1);
      canvas.title = k ? `${name}: ${k.cents >= 0 ? "+" : ""}${k.cents.toFixed(1)} ct, voicing ${(k.voicing * 100).toFixed(0)}%, ` +
        `mechanics ${(k.mech * 100).toFixed(0)}%${serviced().has(hover) ? " (serviced)" : ""} — click to ${serviced().has(hover) ? "unservice" : "service"}` : name;
      draw();
    });
    canvas.addEventListener("mouseleave", () => { hover = -1; draw(); });

    function css(name, fallback) {
      return getComputedStyle(el).getPropertyValue(name).trim() || fallback;
    }

    function draw() {
      const dpr = window.devicePixelRatio || 1;
      const w = Math.max(200, canvas.clientWidth), h = 96;
      if (canvas.width !== Math.round(w * dpr)) { canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr); }
      const g = canvas.getContext("2d");
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, w, h);
      const ink = css("--ink-soft", "#5a6561"), edge = css("--edge", "#c4c9c1"), accent = css("--accent", "#b5532a");
      const count = HIGH - LOW + 1, kw = w / count;
      const svc = serviced();
      // a keyboard strip along the bottom
      for (let n = LOW; n <= HIGH; n++) {
        const x = (n - LOW) * kw;
        const black = [1, 3, 6, 8, 10].includes(n % 12);
        g.fillStyle = svc.has(n) ? "rgba(70,150,90,0.55)" : black ? css("--key-black", "#262b2e") : css("--key", "#f7f8f4");
        g.fillRect(x + 0.5, h - 14, Math.max(1, kw - 1), 14);
        if (n === hover) { g.strokeStyle = accent; g.lineWidth = 1.5; g.strokeRect(x + 0.5, 0.5, kw - 1, h - 1); }
      }
      // tuning: bars up (sharp) and down (flat) around a centre line, ±30 ct full scale
      const mid = 24;
      g.strokeStyle = edge; g.lineWidth = 1;
      g.beginPath(); g.moveTo(0, mid + 0.5); g.lineTo(w, mid + 0.5); g.stroke();
      for (const [n, k] of keys) {
        const x = (n - LOW) * kw;
        const c = Math.max(-1, Math.min(1, k.cents / 30));
        g.fillStyle = c >= 0 ? "#c0502a" : "#2a72c0";
        g.fillRect(x + 1, c >= 0 ? mid - c * 20 : mid, Math.max(1, kw - 2), Math.abs(c) * 20 + 0.5);
        // voicing and mechanics as two shaded rows
        g.fillStyle = `rgba(181,83,42,${0.12 + 0.88 * k.voicing})`;
        g.globalAlpha = k.voicing > 0.01 ? 1 : 0.15;
        g.fillRect(x + 1, 50, Math.max(1, kw - 2), 13);
        g.fillStyle = `rgba(120,90,160,${0.12 + 0.88 * k.mech})`;
        g.globalAlpha = k.mech > 0.01 ? 1 : 0.15;
        g.fillRect(x + 1, 65, Math.max(1, kw - 2), 13);
        g.globalAlpha = 1;
      }
      if (keys.size === 0) {
        g.fillStyle = ink; g.font = "12px system-ui, sans-serif";
        g.fillText("Start the sound (click anywhere) to read this instrument's keys", 8, 44);
      }
    }

    new ResizeObserver(() => draw()).observe(canvas);
    updateTitle();
    request();
    draw();
  });
}
