// charts.js - the dashboard's Apache ECharts layer over /dash/metrics.json.
// Reads the app's CSS custom properties so every chart follows the light/dark
// theme, polls while the dashboard fragment is mounted, and setOption-merges
// each refresh so charts never flash or lose their zoom/hover state.
//
// Series slots are fixed and validated (dataviz palette checks, both modes):
//   slot 1 #a2517d (brand mauve) · slot 2 #3b6fd4 (info blue)
//   slot 1b #bb6295 (lighter step of slot 1, for a second stat of the SAME
//   measure - e.g. max next to avg latency - dashed as secondary encoding)
// Status series (ok/failed/error, response classes) wear the app's status
// tokens, never the categorical slots.
window.AmeboCharts = (function () {
  const POLL_MS = 5000;
  const IDS = ['ch-flow', 'ch-outcomes', 'ch-latency', 'ch-status', 'ch-actions', 'ch-io', 'ch-spells'];
  const APP_IDS = ['ch-app-pub', 'ch-app-cons', 'ch-app-lat'];
  const S1 = '#a2517d', S2 = '#3b6fd4', S1B = '#bb6295';
  let charts = {};
  let timer = null;

  const cssv = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  // Two chart surfaces share this module: the engine dashboard and the
  // application detail page. They never coexist in #view.
  const mode = () => document.getElementById('ch-flow') ? 'dash'
    : document.getElementById('ch-app-pub') ? 'app' : null;
  const mounted = () => mode() !== null;
  const fmtBytes = (n) => {
    if (n < 1024) return n + ' B';
    const u = ['KB', 'MB', 'GB', 'TB']; let i = -1;
    do { n /= 1024; i++; } while (n >= 1024 && i < 3);
    return (n >= 100 ? n.toFixed(0) : n.toFixed(1)) + ' ' + u[i];
  };
  const fmtPct = (ok, total) => total ? (Math.floor(ok * 1000 / total) / 10).toFixed(1) + '%' : '-';

  function theme() {
    return {
      ok: cssv('--ok'), warn: cssv('--warn'), bad: cssv('--bad'), info: cssv('--info'),
      text: cssv('--text'), muted: cssv('--muted'), faint: cssv('--faint'),
      grid: cssv('--border-2'), border: cssv('--border'), surface: cssv('--surface'),
      font: getComputedStyle(document.body).fontFamily,
    };
  }

  // ---- shared chart chrome ---------------------------------------------------
  function tip(t, opts) {
    return Object.assign({
      backgroundColor: t.surface, borderColor: t.border, borderWidth: 1, padding: [8, 12],
      textStyle: { color: t.text, fontSize: 12, fontFamily: t.font },
      extraCssText: 'box-shadow: 0 6px 20px rgba(0,0,0,.12);',
    }, opts || {});
  }
  function legend(t) {
    return { top: 0, right: 0, icon: 'roundRect', itemWidth: 10, itemHeight: 10, itemGap: 14,
             textStyle: { color: t.muted, fontSize: 11, fontFamily: t.font } };
  }
  function timeAxes(t, yFormatter) {
    return {
      grid: { left: 6, right: 10, top: 30, bottom: 2, containLabel: true },
      xAxis: { type: 'time', axisLine: { lineStyle: { color: t.grid } }, axisTick: { show: false },
               axisLabel: { color: t.faint, fontSize: 10.5, hideOverlap: true, formatter: '{HH}:{mm}' },
               splitLine: { show: false } },
      yAxis: { type: 'value', minInterval: 1, splitNumber: 3,
               axisLabel: { color: t.faint, fontSize: 10.5, formatter: yFormatter },
               splitLine: { lineStyle: { color: t.grid, width: 1, type: 'solid' } } },
    };
  }
  // 2px line, round caps, optional 10%-opacity wash; surface-ringed markers.
  function line(t, name, color, data, o) {
    o = o || {};
    return {
      name: name, type: 'line', data: data, showSymbol: false, symbol: 'circle', symbolSize: 8,
      lineStyle: { width: 2, color: color, type: o.dash ? 'dashed' : 'solid', cap: 'round', join: 'round' },
      itemStyle: { color: color, borderColor: t.surface, borderWidth: 2 },
      areaStyle: o.area ? { color: color, opacity: 0.1 } : null,
      z: o.z || 2,
    };
  }
  // Stacked column: thin, surface gap between segments, top segment rounded.
  function stackBar(t, name, color, data, top) {
    return {
      name: name, type: 'bar', stack: 's', data: data, barMaxWidth: 18,
      itemStyle: { color: color, borderColor: t.surface, borderWidth: 1,
                   borderRadius: top ? [3, 3, 0, 0] : 0 },
    };
  }
  const zip = (b, v) => b.map((s, i) => [s * 1000, v[i]]);

  // ---- per-chart options -------------------------------------------------------
  function render(m) {
    const t = theme();
    const s = m.series, b = s.buckets;
    const base = { animationDuration: 260, animationDurationUpdate: 260, textStyle: { fontFamily: t.font } };

    set('ch-flow', Object.assign({}, base, timeAxes(t), {
      legend: legend(t),
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'line', lineStyle: { color: t.border } } }),
      series: [line(t, 'published', S1, zip(b, s.published), { area: true }),
               line(t, 'delivered', S2, zip(b, s.ok), { area: true })],
    }));

    set('ch-outcomes', Object.assign({}, base, timeAxes(t), {
      legend: legend(t),
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'shadow' } }),
      series: [stackBar(t, 'delivered', t.ok, zip(b, s.ok), false),
               stackBar(t, 'failed', t.bad, zip(b, s.failed), true)],
    }));

    set('ch-latency', Object.assign({}, base, timeAxes(t, (v) => v + 'ms'), {
      legend: legend(t),
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'line', lineStyle: { color: t.border } },
                        valueFormatter: (v) => v + ' ms' }),
      series: [line(t, 'avg', S1, zip(b, s.avg_ms)),
               line(t, 'max', S1B, zip(b, s.max_ms), { dash: true })],
    }));

    const mix = m.status_mix;
    const mixTotal = mix.s2xx + mix.s3xx + mix.s4xx + mix.s5xx + mix.net;
    set('ch-status', Object.assign({}, base, {
      legend: Object.assign(legend(t), { orient: 'vertical', top: 'middle', right: 6 }),
      tooltip: tip(t, { trigger: 'item' }),
      title: { text: mixTotal ? String(mixTotal) : 'no attempts', subtext: mixTotal ? 'attempts' : '',
               left: '32%', top: '40%', textAlign: 'center',
               textStyle: { color: t.text, fontSize: mixTotal ? 22 : 12, fontWeight: 650, fontFamily: t.font },
               subtextStyle: { color: t.faint, fontSize: 11, fontFamily: t.font } },
      series: [{
        type: 'pie', radius: ['58%', '82%'], center: ['33%', '50%'],
        itemStyle: { borderColor: t.surface, borderWidth: 2 },
        label: { show: false }, emphasis: { scaleSize: 3 },
        data: [
          { name: '2xx', value: mix.s2xx, itemStyle: { color: t.ok } },
          { name: '3xx', value: mix.s3xx, itemStyle: { color: t.info } },
          { name: '4xx', value: mix.s4xx, itemStyle: { color: t.warn } },
          { name: '5xx', value: mix.s5xx, itemStyle: { color: t.bad } },
          { name: 'no conn', value: mix.net, itemStyle: { color: t.faint } },
        ].filter((d) => d.value > 0),
      }],
    }));

    const acts = (m.top_actions || []).slice().reverse();
    set('ch-actions', Object.assign({}, base, {
      grid: { left: 6, right: 44, top: 8, bottom: 2, containLabel: true },
      tooltip: tip(t, { trigger: 'item',
        formatter: (p) => p.name + '<br>' + p.value + ' events · ' + (acts[p.dataIndex] || {}).delivered + ' delivered' }),
      xAxis: { type: 'value', minInterval: 1, axisLabel: { color: t.faint, fontSize: 10.5 },
               splitLine: { lineStyle: { color: t.grid, width: 1, type: 'solid' } } },
      yAxis: { type: 'category', data: acts.map((a) => a.action),
               axisLine: { lineStyle: { color: t.grid } }, axisTick: { show: false },
               axisLabel: { color: t.muted, fontSize: 11 } },
      series: [{
        type: 'bar', data: acts.map((a) => a.events), barMaxWidth: 16,
        itemStyle: { color: S1, borderRadius: [0, 3, 3, 0] },
        label: { show: true, position: 'right', color: t.muted, fontSize: 11 },
      }],
    }));

    set('ch-io', Object.assign({}, base, timeAxes(t, fmtBytes), {
      legend: legend(t),
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'line', lineStyle: { color: t.border } },
                        valueFormatter: fmtBytes }),
      series: [line(t, 'bytes out', S1, zip(b, s.bytes_out), { area: true }),
               line(t, 'bytes in', S2, zip(b, s.bytes_in), { area: true })],
    }));

    set('ch-spells', Object.assign({}, base, timeAxes(t), {
      legend: legend(t),
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'shadow' } }),
      series: [stackBar(t, 'ok', t.ok, zip(b, s.spell_ok), false),
               stackBar(t, 'failed', t.bad, zip(b, s.spell_failed), false),
               stackBar(t, 'error', t.warn, zip(b, s.spell_error), true)],
    }));
  }

  function set(id, option) {
    const c = charts[id];
    if (c) c.setOption(option);
  }

  // ---- application detail charts ----------------------------------------------
  function renderApp(m) {
    const t = theme();
    const s = m.series, b = s.buckets;
    const base = { animationDuration: 260, animationDurationUpdate: 260, textStyle: { fontFamily: t.font } };

    set('ch-app-pub', Object.assign({}, base, timeAxes(t), {
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'line', lineStyle: { color: t.border } } }),
      series: [line(t, 'published', S1, zip(b, s.published), { area: true })],
    }));

    set('ch-app-cons', Object.assign({}, base, timeAxes(t), {
      legend: legend(t),
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'shadow' } }),
      series: [stackBar(t, 'delivered', t.ok, zip(b, s.ok), false),
               stackBar(t, 'failed', t.bad, zip(b, s.failed), true)],
    }));

    set('ch-app-lat', Object.assign({}, base, timeAxes(t, (v) => v + 'ms'), {
      legend: legend(t),
      tooltip: tip(t, { trigger: 'axis', axisPointer: { type: 'line', lineStyle: { color: t.border } },
                        valueFormatter: (v) => v + ' ms' }),
      series: [line(t, 'avg', S1, zip(b, s.avg_ms)),
               line(t, 'max', S1B, zip(b, s.max_ms), { dash: true })],
    }));
  }

  function appKpis(k) {
    put('app-kpi-evh', k.ev_h);
    put('app-kpi-delivered', k.delivered);
    put('app-kpi-rate', fmtPct(k.attempts_ok, k.attempts));
    put('app-kpi-avg', k.attempts ? k.avg_ms + ' ms' : '-');
    put('app-kpi-backlog', k.backlog);
  }

  // ---- KPI tiles (server-rendered values kept live by the same poll) ----------
  const put = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v; };
  function kpis(k) {
    put('kpi-published', k.published);
    put('kpi-delivered', k.delivered);
    put('kpi-pending', k.pending);
    put('kpi-retrying', k.retrying);
    put('kpi-failed', k.failed);
    put('kpi-rate', fmtPct(k.attempts_ok, k.attempts));
    put('kpi-p95', k.attempts ? k.p95 + ' ms' : '-');
    put('kpi-due', k.due);
  }

  // ---- lifecycle ---------------------------------------------------------------
  async function refresh() {
    const md = mode();
    if (!md) { stop(); return; }
    let url = '/dash/metrics.json';
    if (md === 'app') {
      const holder = document.querySelector('[data-app]');
      if (!holder) { stop(); return; }
      url = '/dash/apps/metrics.json?app=' + encodeURIComponent(holder.dataset.app);
    }
    let m;
    try {
      const r = await fetch(url, { headers: { 'X-Dragon-Request': 'true' } });
      const ct = r.headers.get('content-type') || '';
      if (!r.ok || ct.indexOf('json') < 0) { stop(); return; } // session gone: quit polling
      m = await r.json();
    } catch (e) { return; } // transient fetch error: keep the timer, try next tick
    if (mode() !== md) { stop(); return; }
    if (md === 'dash') { render(m); kpis(m.kpis); }
    else { renderApp(m); appKpis(m.kpis); }
  }

  function dispose() {
    Object.values(charts).forEach((c) => { try { c.dispose(); } catch (e) {} });
    charts = {};
  }
  function stop() {
    if (timer) { clearInterval(timer); timer = null; }
    dispose();
  }
  function boot() {
    stop();
    const md = mode();
    if (!window.echarts || !md) return;
    (md === 'dash' ? IDS : APP_IDS).forEach((id) => {
      const el = document.getElementById(id);
      if (el) charts[id] = echarts.init(el);
    });
    refresh();
    timer = setInterval(refresh, POLL_MS);
  }

  window.addEventListener('resize', () => Object.values(charts).forEach((c) => c.resize()));
  // Fragment navigation away from the dashboard tears the containers out.
  document.addEventListener('dj:after', () => { if (!mounted()) stop(); });
  // Theme toggle: palette comes from CSS vars, so re-read and re-render.
  new MutationObserver(() => { if (mounted() && timer) { dispose(); boot(); } })
    .observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  // Full page load lands after this script; fragment swaps call boot() inline.
  if (mounted()) boot();

  return { boot: boot, stop: stop };
})();
