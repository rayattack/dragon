// Dragon Docs - minimal client-side JS.
//
// Single responsibility: theme toggle. Persists user choice in localStorage
// and applies it before first paint by reading the stored value as early as
// possible. Everything else (layout, navigation, sidebar) is server-rendered
// and works fine without JS.

(function () {
    var KEY = 'dragon-docs-theme';
    var root = document.documentElement;

    // Restore saved theme as soon as the script runs (script tag is at the
    // end of <body>, so first paint already happened with the default
    // light theme - for stricter no-flash we'd inline this in <head>).
    try {
        var saved = localStorage.getItem(KEY);
        if (saved === 'light' || saved === 'dark') {
            root.setAttribute('data-theme', saved);
        }
    } catch (_) {}

    var btn = document.getElementById('theme-toggle');
    if (!btn) return;

    function refreshIcon() {
        // Sun for light mode (you're in it; icon hints "press for dark"
        // wouldn't be intuitive, so use the current state's icon - Rust
        // Book uses a paintbrush, we use the literal sun/moon glyph).
        var current = root.getAttribute('data-theme') || 'light';
        btn.textContent = current === 'dark' ? '☽' : '☀';
    }

    btn.addEventListener('click', function () {
        var current = root.getAttribute('data-theme') || 'light';
        var next = current === 'dark' ? 'light' : 'dark';
        root.setAttribute('data-theme', next);
        try { localStorage.setItem(KEY, next); } catch (_) {}
        refreshIcon();
    });

    refreshIcon();
})();

// Sidebar chapter folds. The server renders every chapter expanded and tags
// the chapter containing the current page with data-active. We persist the
// reader's manual collapses in localStorage (keyed by chapter index) and
// reapply them on each page - but never collapse the active chapter, so the
// page you're reading is always visible in the sidebar.
(function () {
    var FKEY = 'dragon-docs-folds';
    function load() {
        try { return JSON.parse(localStorage.getItem(FKEY) || '{}') || {}; }
        catch (_) { return {}; }
    }
    function save(f) {
        try { localStorage.setItem(FKEY, JSON.stringify(f)); } catch (_) {}
    }

    var folds = load();
    var chapters = document.querySelectorAll('details.sb-chapter');
    Array.prototype.forEach.call(chapters, function (d) {
        var id = d.getAttribute('data-ch');
        var isActive = d.hasAttribute('data-active');
        // Reapply a saved collapse for non-active chapters; the active chapter
        // stays open regardless of saved state.
        if (!isActive && Object.prototype.hasOwnProperty.call(folds, id)) {
            d.open = !!folds[id];
        }
        d.addEventListener('toggle', function () {
            folds[id] = d.open;
            save(folds);
        });
    });

    // Bring the active section into view on long sidebars.
    var active = document.querySelector('.sidebar .sb-active');
    if (active && active.scrollIntoView) {
        active.scrollIntoView({ block: 'nearest' });
    }
})();

// Download page: highlight the card matching the visitor's OS. The server
// already picks one from the User-Agent; this refines it client-side using
// navigator.platform (more reliable behind proxies) and reveals the
// "Recommended for your system" badge on the matched card.
(function () {
    var grid = document.querySelector('.dl-grid');
    if (!grid) return;
    try {
        var hint = (navigator.platform || '') + ' ' + (navigator.userAgent || '');
        var os = '';
        if (/Win/i.test(hint)) os = 'windows';
        else if (/Mac|Darwin|iP(hone|ad|od)/i.test(hint)) os = 'macos';
        else if (/Linux|X11|Android|CrOS/i.test(hint)) os = 'linux';
        if (!os) return;

        var match = grid.querySelector('.dl-card[data-os="' + os + '"]');
        if (!match) return;

        Array.prototype.forEach.call(grid.querySelectorAll('.dl-card'), function (card) {
            var isMatch = card === match;
            card.classList.toggle('featured', isMatch);
            var badge = card.querySelector('.dl-rec');
            if (badge) badge.style.display = isMatch ? '' : 'none';
        });
    } catch (_) {}
})();
