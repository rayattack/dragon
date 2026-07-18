// builder.js - visual spell-chain builder for Wands with a Visual <-> YAML
// toggle. The visual builder renders step editors (add/remove/reorder + {{
// autocomplete of the event's payload fields). The YAML view is the spellbook
// `then:` dialect. Both convert to the canonical chain JSON on submit, so the
// server never needs a YAML parser. dashBuilder(rootEl) wires one instance.
(function () {
  const TYPES = ['fetch', 'map', 'post', 'log'];
  const esc = (s) => String(s == null ? '' : s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/"/g, '&quot;');
  // YAML scalar: quote when it holds YAML-special chars, else emit bare.
  const yv = (v) => { v = v || ''; return (/[:{}#]/.test(v) || v === '' || /^\s|\s$/.test(v)) ? JSON.stringify(v) : v; };
  const uq = (v) => { v = (v || '').trim(); if (v.length >= 2 && (v[0] === '"' || v[0] === "'")) { try { return JSON.parse(v); } catch (e) { return v.slice(1, -1); } } return v; };

  function toYaml(steps) {
    const out = [];
    steps.forEach((s) => {
      if (s.type === 'fetch') { out.push('- fetch: ' + yv(s.url)); if (s.as) out.push('  as: ' + s.as); }
      else if (s.type === 'post') { out.push('- post: ' + yv(s.url)); out.push('  with:'); s.pairs.forEach((p) => { if (p.k) out.push('    ' + p.k + ': ' + yv(p.v)); }); }
      else if (s.type === 'map') { out.push('- map:'); s.pairs.forEach((p) => { if (p.k) out.push('    ' + p.k + ': ' + yv(p.v)); }); if (s.as) out.push('  as: ' + s.as); }
      else { out.push('- log: ' + yv(s.text)); }
    });
    return out.join('\n');
  }
  function fromYaml(text) {
    const steps = []; let cur = null; let inMap = false;
    (text || '').split('\n').forEach((raw) => {
      if (!raw.trim()) return;
      const line = raw.trim();
      if (line[0] === '-') {
        const m = line.slice(1).trim().match(/^(\w+):\s*(.*)$/);
        cur = { type: 'log', url: '', as: '', text: '', pairs: [] }; inMap = false;
        if (m) {
          const verb = m[1], val = uq(m[2]);
          cur.type = TYPES.indexOf(verb) >= 0 ? verb : 'log';
          if (verb === 'fetch' || verb === 'post') cur.url = val;
          else if (verb === 'log') cur.text = val;
          else if (verb === 'map') inMap = true;
        }
        steps.push(cur);
      } else if (cur) {
        const m2 = line.match(/^(\w+):\s*(.*)$/); if (!m2) return;
        const key = m2[1], val = m2[2];
        if (key === 'as') { cur.as = uq(val); inMap = false; }
        else if (key === 'with' || key === 'map') { inMap = true; }
        else if (inMap) { cur.pairs.push({ k: key, v: uq(val) }); }
      }
    });
    steps.forEach((s) => { if ((s.type === 'map' || s.type === 'post') && !s.pairs.length) s.pairs = [{ k: '', v: '' }]; });
    return steps;
  }

  // chain JSON <-> spellbook YAML, so a saved spell can be viewed/edited as YAML.
  function pairsOf(obj) { const ps = Object.keys(obj || {}).map((k) => ({ k: k, v: String(obj[k]) })); return ps.length ? ps : [{ k: '', v: '' }]; }
  function chainToSteps(chain) {
    return (chain || []).map((it) => {
      if ('fetch' in it) return { type: 'fetch', url: it.fetch || '', as: it.as || '', text: '', pairs: [{ k: '', v: '' }] };
      if ('post' in it) return { type: 'post', url: it.post || '', as: '', text: '', pairs: pairsOf(it.with) };
      if ('map' in it) return { type: 'map', url: '', as: it.as || '', text: '', pairs: pairsOf(it.map) };
      return { type: 'log', url: '', as: '', text: it.log != null ? it.log : '', pairs: [] };
    });
  }
  window.chainToYaml = function (source) {
    let obj; try { obj = typeof source === 'string' ? JSON.parse(source) : (source || {}); } catch (e) { return String(source || ''); }
    return toYaml(chainToSteps((obj && obj.chain) || []));
  };
  window.yamlToChain = function (text) {
    const chain = fromYaml(text).map((s) => {
      if (s.type === 'fetch') return { fetch: s.url, as: s.as || 'result' };
      if (s.type === 'post') { const w = {}; s.pairs.forEach((p) => { if (p.k) w[p.k] = p.v; }); return { post: s.url, with: w }; }
      if (s.type === 'map') { const m = {}; s.pairs.forEach((p) => { if (p.k) m[p.k] = p.v; }); return { map: m, as: s.as || 'map' }; }
      return { log: s.text };
    });
    return JSON.stringify({ chain: chain }, null, 2);
  };

  window.dashBuilder = function (root) {
    const stepsEl = root.querySelector('.b-steps');
    const actionSel = root.querySelector('.b-action');
    const hidden = root.querySelector('input[name="source"]');
    let steps = [];
    let fields = [];
    let mode = 'visual';
    let yamlEd = null;

    const vars = (upto) => {
      const vs = fields.map((f) => 'event.' + f);
      for (let i = 0; i < upto; i++) if (steps[i] && steps[i].as) vs.push(steps[i].as);
      return vs;
    };
    function addStep(type) { steps.push({ type: type || 'log', url: '', as: '', text: '', pairs: [{ k: '', v: '' }] }); render(); }
    function move(i, d) { const j = i + d; if (j < 0 || j >= steps.length) return; const t = steps[i]; steps[i] = steps[j]; steps[j] = t; render(); }
    function del(i) { steps.splice(i, 1); render(); }

    function pairRows(s, i) {
      return s.pairs.map((p, pi) => `
        <div class="b-pair">
          <input class="b-tmpl" data-si="${i}" data-pk="${pi}" data-f="k" placeholder="key" value="${esc(p.k)}">
          <input class="b-tmpl" data-si="${i}" data-pk="${pi}" data-f="v" placeholder="value / {{event.field}}" value="${esc(p.v)}">
          <button type="button" class="icon-btn b-sm" data-act="delpair" data-si="${i}" data-pk="${pi}">&times;</button>
        </div>`).join('') + `<button type="button" class="btn btn-sm btn-ghost" data-act="addpair" data-si="${i}">+ field</button>`;
    }
    function stepBody(s, i) {
      if (s.type === 'fetch') return `
        <input class="b-tmpl b-grow" data-si="${i}" data-f="url" placeholder="https://api.example.com/x?id={{event.field}}" value="${esc(s.url)}">
        <input class="b-as" data-si="${i}" data-f="as" placeholder="save as" value="${esc(s.as)}">`;
      if (s.type === 'post') return `
        <input class="b-tmpl b-grow" data-si="${i}" data-f="url" placeholder="https://api.example.com/webhook" value="${esc(s.url)}">
        <div class="b-pairs">${pairRows(s, i)}</div>`;
      if (s.type === 'map') return `
        <input class="b-as" data-si="${i}" data-f="as" placeholder="save as" value="${esc(s.as)}">
        <div class="b-pairs">${pairRows(s, i)}</div>`;
      return `<input class="b-tmpl b-grow" data-si="${i}" data-f="text" placeholder="log message {{event.field}}" value="${esc(s.text)}">`;
    }
    function render() {
      stepsEl.innerHTML = steps.map((s, i) => `
        <div class="b-step">
          <div class="b-step-head">
            <span class="b-num">${i + 1}</span>
            <select class="b-type" data-si="${i}">${TYPES.map((t) => `<option value="${t}"${t === s.type ? ' selected' : ''}>${t}</option>`).join('')}</select>
            <div class="grow"></div>
            <button type="button" class="icon-btn b-sm" data-act="up" data-si="${i}">&uarr;</button>
            <button type="button" class="icon-btn b-sm" data-act="down" data-si="${i}">&darr;</button>
            <button type="button" class="icon-btn b-sm" data-act="del" data-si="${i}">&times;</button>
          </div>
          <div class="b-step-body">${stepBody(s, i)}</div>
        </div>`).join('') || '<p class="hint">No steps yet. Add one below.</p>';
      serialize();
    }
    function serialize() { hidden.value = JSON.stringify({ chain: steps.map((s) => {
      if (s.type === 'fetch') return { fetch: s.url, as: s.as || 'result' };
      if (s.type === 'post') { const w = {}; s.pairs.forEach((p) => { if (p.k) w[p.k] = p.v; }); return { post: s.url, with: w }; }
      if (s.type === 'map') { const m = {}; s.pairs.forEach((p) => { if (p.k) m[p.k] = p.v; }); return { map: m, as: s.as || 'map' }; }
      return { log: s.text };
    }) }, null, 2); }

    // sync current mode into `steps` + hidden source (called before submit)
    root.__sync = function () { if (mode === 'yaml' && yamlEd) steps = fromYaml(yamlEd.getValue()); serialize(); };

    // ---- Visual <-> YAML toggle ----
    function ensureYaml() {
      if (yamlEd) return;
      yamlEd = ace.edit(root.querySelector('.b-yaml-ide'));
      yamlEd.session.setMode('ace/mode/yaml');
      const dark = document.documentElement.getAttribute('data-theme') === 'dark';
      yamlEd.setTheme(dark ? 'ace/theme/one_dark' : 'ace/theme/chrome');
      yamlEd.setOptions({ fontSize: 14, showPrintMargin: false, tabSize: 2, useWorker: false });
      (window.__ace = window.__ace || []).push(yamlEd);
    }
    function showMode(m) {
      if (m === 'yaml') { ensureYaml(); yamlEd.setValue(toYaml(steps), -1); yamlEd.clearSelection(); root.querySelector('.b-visual').hidden = true; root.querySelector('.b-yaml-pane').hidden = false; yamlEd.resize(); }
      else { if (yamlEd) { steps = fromYaml(yamlEd.getValue()); render(); } root.querySelector('.b-visual').hidden = false; root.querySelector('.b-yaml-pane').hidden = true; }
      mode = m;
      root.querySelectorAll('[data-bmode]').forEach((b) => b.classList.toggle('active', b.dataset.bmode === m));
    }
    root.querySelectorAll('[data-bmode]').forEach((b) => b.addEventListener('click', () => showMode(b.dataset.bmode)));

    // ---- step editing (delegated) ----
    stepsEl.addEventListener('click', (e) => {
      const b = e.target.closest('[data-act]'); if (!b) return;
      const i = +b.dataset.si, act = b.dataset.act;
      if (act === 'up') move(i, -1); else if (act === 'down') move(i, 1); else if (act === 'del') del(i);
      else if (act === 'addpair') { steps[i].pairs.push({ k: '', v: '' }); render(); }
      else if (act === 'delpair') { steps[i].pairs.splice(+b.dataset.pk, 1); render(); }
    });
    stepsEl.addEventListener('change', (e) => { if (e.target.classList.contains('b-type')) { steps[+e.target.dataset.si].type = e.target.value; render(); } });
    stepsEl.addEventListener('input', (e) => {
      const t = e.target, i = t.dataset.si; if (i == null) return;
      const s = steps[+i], f = t.dataset.f;
      if (t.dataset.pk != null) s.pairs[+t.dataset.pk][f] = t.value; else if (f) s[f] = t.value;
      serialize();
      if (t.classList.contains('b-tmpl')) autocomplete(t, +i);
    });
    root.querySelector('.b-add').addEventListener('click', () => addStep('log'));

    // ---- {{ autocomplete ----
    let pop = null;
    const closePop = () => { if (pop) { pop.remove(); pop = null; } };
    function autocomplete(input, stepIdx) {
      const val = input.value, caret = input.selectionStart;
      const open = val.lastIndexOf('{{', caret);
      if (open < 0) { closePop(); return; }
      const close = val.indexOf('}}', open); if (close !== -1 && close < caret) { closePop(); return; }
      const typed = val.slice(open + 2, caret).trim();
      const opts = vars(stepIdx).filter((v) => v.indexOf(typed) === 0);
      closePop(); if (!opts.length) return;
      pop = document.createElement('div'); pop.className = 'b-autoc';
      pop.innerHTML = opts.map((o) => `<div class="b-opt" data-v="${o}">${o}</div>`).join('');
      const r = input.getBoundingClientRect();
      pop.style.cssText = `left:${r.left}px;top:${r.bottom + 2}px;width:${Math.max(180, r.width)}px`;
      document.body.appendChild(pop);
      pop.addEventListener('mousedown', (e) => {
        const o = e.target.closest('.b-opt'); if (!o) return; e.preventDefault();
        input.value = val.slice(0, open) + '{{' + o.dataset.v + '}}' + val.slice(caret);
        const s = steps[stepIdx], f = input.dataset.f;
        if (input.dataset.pk != null) s.pairs[+input.dataset.pk][f] = input.value; else s[f] = input.value;
        closePop(); serialize(); input.focus();
      });
    }
    document.addEventListener('click', (e) => { if (!e.target.closest('.b-autoc') && !e.target.classList.contains('b-tmpl')) closePop(); });

    // ---- action -> load payload fields ----
    function loadFields() {
      const a = actionSel.value; if (!a) { fields = []; return; }
      fetch('/dash/action-fields?name=' + encodeURIComponent(a), { headers: { 'X-Dragon-Request': 'true' } })
        .then((r) => r.json()).then((d) => { fields = d.fields || []; }).catch(() => { fields = []; });
    }
    actionSel.addEventListener('change', loadFields);
    loadFields();
    addStep('fetch');
  };

  // Submit hook: sync the active editor into the hidden chain JSON, then post.
  window.dashBuilderSubmit = function (form) {
    const root = form.closest('.spell-builder');
    if (root && root.__sync) root.__sync();
    return window.dj.submit(form, '#list-wands');
  };
})();
