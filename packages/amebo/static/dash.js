// dash.js - amebo-specific UI behaviours layered over dg.js (window.dj):
// multi-select + bulk actions, show/hide columns (persisted), list query
// (search + filter + pagination), and the cmd/ctrl-K spotlight. Server stays
// the source of truth; this only manages ephemeral client UI state.
(function () {
  const COLS_LS = 'amebo-cols';
  const cfg = () => { try { return JSON.parse(localStorage.getItem(COLS_LS) || '{}'); } catch (e) { return {}; } };
  const save = (c) => localStorage.setItem(COLS_LS, JSON.stringify(c));
  const table = (tkey) => document.querySelector('table.data[data-tkey="' + tkey + '"]');
  const selbar = (tkey) => document.querySelector('[data-selbar="' + tkey + '"]');

  // ---- show / hide columns (persisted per table, re-applied after swaps) ----
  function applyCols() {
    const c = cfg();
    let css = '';
    Object.keys(c).forEach((tkey) => (c[tkey] || []).forEach((col) => {
      css += 'table.data[data-tkey="' + tkey + '"] [data-col="' + col + '"]{display:none}';
    }));
    let s = document.getElementById('dash-colhide');
    if (!s) { s = document.createElement('style'); s.id = 'dash-colhide'; document.head.appendChild(s); }
    s.textContent = css;
    document.querySelectorAll('.pop input[data-colkey]').forEach((cb) => {
      cb.checked = !((c[cb.dataset.tkey] || []).includes(cb.dataset.colkey));
    });
  }
  window.dashCol = function (tkey, col, show) {
    const c = cfg(); const hidden = new Set(c[tkey] || []);
    show ? hidden.delete(col) : hidden.add(col);
    c[tkey] = Array.from(hidden); save(c); applyCols();
  };
  window.dashToggleCols = function (btn) {
    const pop = btn.nextElementSibling; const open = pop.hidden;
    document.querySelectorAll('.pop').forEach((p) => p.hidden = true);
    pop.hidden = !open;
  };

  // ---- selection + bulk actions --------------------------------------------
  function refreshSel(tkey) {
    const t = table(tkey); if (!t) return;
    const n = t.querySelectorAll('.cb-row:checked').length;
    const bar = selbar(tkey);
    if (bar) { bar.hidden = n === 0; const c = bar.querySelector('.sel-n'); if (c) c.textContent = n + ' selected'; }
    const all = t.querySelector('.cb-all');
    if (all) { const rows = t.querySelectorAll('.cb-row').length; all.checked = rows > 0 && n === rows; }
  }
  window.dashClear = function (tkey) {
    const t = table(tkey); if (t) t.querySelectorAll('.cb-row,.cb-all').forEach((cb) => cb.checked = false);
    refreshSel(tkey);
  };
  window.dashBulk = function (tkey, url) {
    const t = table(tkey); if (!t) return;
    const ids = Array.from(t.querySelectorAll('.cb-row:checked')).map((cb) => cb.value);
    if (!ids.length) return;
    const fd = new FormData(); fd.append('ids', ids.join(','));
    window.dj.send(url, { method: 'POST', target: '#list-' + tkey, body: fd });
  };

  // ---- list query: search + filters + page, rebuilt from the toolbar -------
  window.dashList = function (tkey, page) {
    const tb = document.querySelector('[data-toolbar="' + tkey + '"]');
    let url = '/dash/' + tkey + '/list?';
    if (tb) {
      const q = tb.querySelector('.q'); if (q) url += 'q=' + encodeURIComponent(q.value) + '&';
      tb.querySelectorAll('select[data-filter]').forEach((s) => url += s.dataset.filter + '=' + encodeURIComponent(s.value) + '&');
    }
    if (page) url += 'page=' + page;
    window.dj.get(url, '#list-' + tkey, { debounce: 180 });
  };

  // ---- spotlight (cmd/ctrl+K or ctrl+/) ------------------------------------
  function openSpot() {
    const s = document.getElementById('spotlight'); if (!s) return;
    s.hidden = false;
    const i = s.querySelector('input'); if (i) { i.value = ''; i.focus(); }
    const r = document.getElementById('spot-results'); if (r) r.innerHTML = '';
  }
  window.dashCloseSpot = function () { const s = document.getElementById('spotlight'); if (s) s.hidden = true; };

  // ---- full-height list grids -----------------------------------------------
  // Resource list tables fill the available viewport height (measured from the
  // grid's own bounding rect down to the bottom, minus the pager) and scroll
  // internally under their sticky header, so the grid always reaches the
  // bottom of the page regardless of row count.
  function fillAvailableHeight() {
    document.querySelectorAll('.list-page[data-fill] .table-wrap').forEach((el) => {
      const top = el.getBoundingClientRect().top;
      const pager = el.parentElement ? el.parentElement.querySelector('.pagination') : null;
      const reserve = (pager ? pager.offsetHeight + 12 : 0) + 24;
      let h = Math.max(220, window.innerHeight - top - reserve);
      el.style.height = h + 'px';
      // Second pass: whatever still overflows the viewport (the stage's own
      // bottom padding, borders) comes off the grid so the page never scrolls.
      const over = document.documentElement.scrollHeight - window.innerHeight;
      if (over > 0) el.style.height = Math.max(220, h - over) + 'px';
    });
  }
  // dg.js swaps fragments inside startViewTransition, whose callback runs a
  // frame AFTER dj:after fires - measure now, next frame, and once settled.
  function refillHeights() {
    fillAvailableHeight();
    requestAnimationFrame(() => requestAnimationFrame(fillAvailableHeight));
    setTimeout(fillAvailableHeight, 150);
  }
  window.addEventListener('resize', fillAvailableHeight);

  // ---- pinned page headers: show the divider once content scrolls under ----
  function markStuck() {
    const on = window.scrollY > 8;
    document.querySelectorAll('.section-head.pin').forEach((el) => el.classList.toggle('stuck', on));
  }
  window.addEventListener('scroll', markStuck, { passive: true });

  // ---- delegated events -----------------------------------------------------
  document.addEventListener('change', (e) => {
    if (e.target.classList.contains('cb-all')) {
      const t = e.target.closest('table'); t.querySelectorAll('.cb-row').forEach((cb) => cb.checked = e.target.checked);
      refreshSel(t.dataset.tkey);
    } else if (e.target.classList.contains('cb-row')) {
      refreshSel(e.target.closest('table').dataset.tkey);
    }
  });
  document.addEventListener('click', (e) => { if (!e.target.closest('.pos')) document.querySelectorAll('.pop').forEach((p) => p.hidden = true); });
  document.addEventListener('keydown', (e) => {
    if ((e.metaKey || e.ctrlKey) && (e.key === 'k' || e.key === '/')) { e.preventDefault(); openSpot(); }
    else if (e.key === 'Escape') { window.dashCloseSpot(); }
  });
  document.addEventListener('dj:after', () => { applyCols(); refillHeights(); markStuck(); }); // re-apply after any fragment swap
  applyCols();
  refillHeights();
  markStuck();
})();
