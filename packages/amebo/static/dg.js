// dg.js - Dragon fragments client. Server is the single source of truth.
// Make one handle, then call it from native on* attributes:
//   const dj = DragonFragments({ name: 'dj' });
//   <button onclick="dj.get('/x1/components/user-settings', '#panel')">Settings</button>
// The name you pass is installed as a global so inline handlers can see it
// (inline on* resolves globals only; a module-scoped const would be invisible).
function DragonFragments(config = {}) {
  const cfg = { name: 'dg', header: 'X-Dragon-Request', suffix: 'dg', ...config };
  const OOB = `[data-${cfg.suffix}-oob]`; // the lone attribute left, and it is configurable

  const EV = cfg.name || 'dg';    // lifecycle event prefix: dg:before / dg:after / dg:error
  const q = (s) => (s == null ? null : typeof s === 'string' ? document.querySelector(s) : s);
  const inflight = new WeakMap(); // target element -> its live AbortController
  const timers = new Map();       // debounce key    -> pending timer
  let region = null;              // lazily-made aria-live region

  const emit = (phase, detail) => document.dispatchEvent(new CustomEvent(EV + ':' + phase, { detail }));
  function matches(f, d) {
    if (!f) return true;
    if (f.verb && f.verb.toUpperCase() !== d.verb) return false;
    if (f.url && !(f.url instanceof RegExp ? f.url.test(d.url) : d.url.includes(f.url))) return false;
    if (f.target && q(f.target) !== d.target) return false;
    return true;
  }
  // dj.watch({verb:'post', url:'/todos'}, {before, after, error}); returns an unsubscribe fn
  function watch(filter, handlers) {
    const offs = Object.keys(handlers).map((phase) => {
      const fn = (e) => matches(filter, e.detail) && handlers[phase](e.detail);
      document.addEventListener(EV + ':' + phase, fn);
      return () => document.removeEventListener(EV + ':' + phase, fn);
    });
    return () => offs.forEach((off) => off());
  }

  // serialize `with: this` (an input -> name=value) or `with: form` (every field)
  function toData(src) {
    if (!src) return null;
    if (src.tagName === 'FORM') return new FormData(src);
    const fd = new FormData();
    if (src.name) fd.append(src.name, src.value);
    return fd;
  }

  function apply(el, html, swap) {
    if (swap === 'outer')        el.outerHTML = html;
    else if (swap === 'append')  el.insertAdjacentHTML('beforeend', html);
    else if (swap === 'prepend') el.insertAdjacentHTML('afterbegin', html);
    else if (swap !== 'none')    el.innerHTML = html; // 'inner' default
  }

  // out-of-band: one response updates several regions (a list AND its badge)
  function oob(html) {
    const t = document.createElement('template');
    t.innerHTML = html;
    t.content.querySelectorAll(OOB).forEach((n) => {
      const cur = n.id && document.getElementById(n.id);
      if (cur) cur.replaceWith(n); else n.remove();
    });
    return t.innerHTML; // remainder is the primary swap
  }

  // innerHTML does not run <script>; re-create them so injected setup executes
  function scripts(el) {
    el.querySelectorAll('script').forEach((old) => {
      const s = document.createElement('script');
      for (const a of old.attributes) s.setAttribute(a.name, a.value);
      s.textContent = old.textContent;
      old.replaceWith(s);
    });
  }

  // a11y: move focus into new content and announce it politely
  function focusIn(el) {
    if (!region) {
      region = document.createElement('div');
      region.setAttribute('aria-live', 'polite');
      region.style.cssText = 'position:absolute;width:1px;height:1px;overflow:hidden;clip:rect(0 0 0 0)';
      document.body.appendChild(region);
    }
    const f = el.querySelector('[autofocus]') || el.querySelector('a,button,input,select,textarea,[tabindex]');
    if (f) f.focus();
    region.textContent = (el.getAttribute('aria-label') || el.textContent || 'Updated').trim().slice(0, 120);
  }

  // built-in skeleton: recolor the target's own contents into shimmer bars,
  // so the placeholder is automatically the shape of what is already there.
  const SK = cfg.skeleton === true ? { loading: true } : cfg.skeleton || null;
  const skOn = !!(SK && SK.loading !== false);
  const skAttr = 'data-' + EV + '-sk';
  let skStyle = null;
  function skeletonInit() {
    if (skStyle) return;
    const [a, b] = SK.colors || ['#e6e6e6', '#f2f2f2'];
    const sp = SK.speed || '1.4s';
    skStyle = document.createElement('style');
    skStyle.textContent =
      `[${skAttr}]{pointer-events:none}[${skAttr}] *{color:transparent!important;text-shadow:none!important}` +
      `[${skAttr}] :where(p,a,span,h1,h2,h3,h4,h5,h6,li,td,th,button,img,svg,label,strong,em,b,i,code,small,input){` +
      `background:linear-gradient(90deg,${a} 25%,${b} 37%,${a} 63%)!important;background-size:400% 100%!important;` +
      `border-radius:6px!important;border-color:transparent!important;box-shadow:none!important;animation:${EV}-sk ${sp} ease infinite}` +
      `@keyframes ${EV}-sk{0%{background-position:100% 0}100%{background-position:0 0}}`;
    document.head.appendChild(skStyle);
  }
  // only replace-swaps skeletonize; opt out per call with {skeleton:false} (e.g. a counter poll)
  const skeletonable = (d) =>
    d.target && d.opts.skeleton !== false && (!d.opts.swap || d.opts.swap === 'inner' || d.opts.swap === 'outer');

  // the one primitive everything funnels through
  async function send(url, o = {}) {
    const target = q(o.target);
    const detail = { verb: (o.method || 'GET').toUpperCase(), url, target, opts: o };
    emit('before', detail);               // dg:before -> skeletons, spinners, disable buttons
    const ind = q(o.indicator) || target;
    if (ind) ind.setAttribute('aria-busy', 'true');
    let ctrl = null;
    if (target) {                         // cancel a prior request aimed at the same slot
      const prev = inflight.get(target);
      if (prev) prev.abort();
      ctrl = new AbortController();
      inflight.set(target, ctrl);
    }
    const init = { method: detail.verb, headers: { [cfg.header]: 'true' }, signal: ctrl && ctrl.signal };
    if (o.body) init.body = o.body;       // FormData sets its own content-type + boundary
    let res;
    try { res = await fetch(url, init); }
    catch (e) {
      if (ind) ind.removeAttribute('aria-busy');
      if (e.name === 'AbortError') return; // superseded: the newer request owns the lifecycle
      detail.error = e; emit('error', detail); emit('after', detail); return; // event, not a throw
    }
    if (target && inflight.get(target) === ctrl) inflight.delete(target);
    if (ind) ind.removeAttribute('aria-busy');
    detail.res = res; detail.ok = res.ok;
    const to = res.headers.get('X-Dragon-Redirect');
    if (to) { emit('after', detail); return void (location.href = to); } // server steers a full nav
    if (res.headers.get('X-Dragon-Refresh')) { emit('after', detail); return location.reload(); }
    const html = oob(await res.text());
    if (!res.ok) emit('error', detail);
    const dest = q(res.headers.get('X-Dragon-Retarget')) || (res.ok ? target : q(o.error) || target);
    const run = () => { if (dest) { apply(dest, html, o.swap || 'inner'); scripts(dest); focusIn(dest); } };
    document.startViewTransition && o.transition !== false ? document.startViewTransition(run) : run();
    const push = res.headers.get('X-Dragon-Push') || (o.push === true ? url : o.push);
    if (push) history.pushState({ f: 1 }, '', push);
    emit('after', detail);                // dg:after -> hide skeleton, re-enable, toast
    return html;                          // escape hatch: caller can keep going
  }

  // verb helpers: what you type in on* attributes
  function request(method, url, target, o) {
    let u = url, body = null;
    const fd = toData(o.with);
    if (fd) method === 'GET'
      ? (u += (u.includes('?') ? '&' : '?') + new URLSearchParams(fd))
      : (body = fd);
    const fire = () => send(u, { ...o, method, target, body });
    if (!o.debounce) return fire();
    const key = typeof target === 'string' ? target : (target && target.id) || url;
    clearTimeout(timers.get(key));
    timers.set(key, setTimeout(fire, o.debounce));
  }
  const get  = (url, target, o = {}) => request('GET', url, target, o);
  const post = (url, target, o = {}) => request('POST', url, target, o);

  // <form onsubmit="return dj.submit(this, '#todos', {swap:'append'})">
  function submit(form, target, o = {}) {
    // Read the attribute, not form.action / form.method: a control named
    // `action` (or `method`) shadows the form's property and would otherwise
    // make the request target the element instead of the URL.
    const url = form.getAttribute('action') || form.action;
    const method = (form.getAttribute('method') || 'POST').toUpperCase();
    request(method, url, target, { ...o, with: form });
    return false; // cancels the native submit
  }

  // no native inline event for these two, so they are imperative one-liners
  function reveal(elOrSel, url, target, o = {}) {
    const el = q(elOrSel);
    if (!el) return;
    const io = new IntersectionObserver((xs) => xs.forEach((x) => {
      if (x.isIntersecting) { io.disconnect(); get(url, target, o); }
    }));
    io.observe(el);
  }
  function poll(url, target, ms, o = {}) {
    const id = setInterval(() => get(url, target, o), ms);
    return () => clearInterval(id); // caller keeps the handle to stop it
  }

  // whole-site boost: opt in with dj.boost('#main') or { boost: '#main' } at init
  function boost(target = 'main') {
    const ext = (a) => !a || !a.href || a.origin !== location.origin || a.target || a.hasAttribute('download');
    document.addEventListener('click', (e) => {
      if (e.defaultPrevented || e.button || e.metaKey || e.ctrlKey || e.shiftKey) return;
      const a = e.target.closest('a[href]');
      if (ext(a)) return;
      e.preventDefault();
      get(a.href, target, { push: true });
    });
    document.addEventListener('submit', (e) => {
      const f = e.target;
      if (e.defaultPrevented || !f.action) return;
      e.preventDefault();
      submit(f, target, { push: (f.method || 'get').toUpperCase() === 'GET' });
    });
    window.addEventListener('popstate', () => send(location.href, { target }));
  }

  // batteries-included skeleton, built on the same lifecycle events
  if (skOn) {
    skeletonInit();
    watch({}, {
      before: (d) => { if (skeletonable(d)) d.target.setAttribute(skAttr, ''); },
      after:  (d) => { if (d.target) d.target.removeAttribute(skAttr); },
      error:  (d) => { if (d.target) d.target.removeAttribute(skAttr); },
    });
  }

  const api = { send, get, post, submit, reveal, poll, boost, watch, apply, config: cfg };
  if (cfg.name) window[cfg.name] = api; // installed global == the name you call inline
  if (cfg.boost) boost(cfg.boost === true ? 'main' : cfg.boost);
  return api;
}
