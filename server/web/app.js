/* ===========================================================================
   app.js – Logik der Verwaltungsseite des Mondschürfer-Servers

   WAS PASSIERT HIER?
     1. Alle 2 Sekunden fragt die Seite GET /api/status ab (siehe
        server/src/WebServer.cpp, Aufbau des JSON: DESIGN Abschnitt 6) und
        zeichnet daraus Auslastung, Spielerliste, Bestenliste und Formular neu.
     2. Das Formular "Spielparameter" schickt Änderungen per POST an
        /api/settings bzw. /api/settings/reset.

   WOHER KOMMEN DIE DATEN?
     Der Spiel-Thread des Servers legt einmal pro Sekunde eine Kopie seines
     Zustands ab (SharedState). /api/status liefert diese Kopie als JSON.
     Gespeicherte Einstellungen übernimmt der Spiel-Thread erst zu Beginn
     seines nächsten Schritts – daher kann es nach dem Speichern bis zu etwa
     einer Sekunde dauern, bis /api/status die neuen Werte zeigt (siehe
     state.savedPending unten).

   SICHERHEIT
     - Anmeldung: Der Browser hat beim ersten Aufruf Benutzername und Passwort
       abgefragt (HTTP Basic Auth) und schickt sie bei jeder Anfrage mit
       (credentials: 'same-origin').
     - POST-Anfragen tragen den Header "X-Requested-With: mondschuerfer";
       ohne ihn lehnt der Server ab (Schutz gegen fremde Webseiten, CSRF).
     - Texte vom Server (Spielernamen, Servername) werden NUR über textContent
       eingesetzt, nie über innerHTML. Ein Name wie "<img onerror=...>" bleibt
       so harmloser Text.
     - Die Content-Security-Policy erlaubt nur diese Datei als Skript; deshalb
       gibt es im HTML keine onclick-Attribute, alles wird hier verbunden.
   =========================================================================== */
(function () {
    'use strict';

    // --- Feste Werte (müssen zu shared/Config.hpp passen) ----------------------
    const POLL_MS = 2000;            // Abfrage-Intervall, solange die Seite sichtbar ist
    const POLL_HIDDEN_MS = 10000;    // im Hintergrund-Tab seltener abfragen
    const FETCH_TIMEOUT_MS = 5000;   // danach gilt eine Anfrage als gescheitert
    const GOAL = 100000;             // cfg::GOAL_POINTS – Kontostand für das Spielziel
    const UPGRADE_MAX = 5;           // cfg::UPGRADE_LEVELS
    const UPGRADES = [               // Reihenfolge wie cfg::Upgrade (UP_SPEED, UP_ARMOR, UP_WEAPON, UP_CARGO)
        { letter: 'A', name: 'Antrieb' },
        { letter: 'P', name: 'Panzerung' },
        { letter: 'W', name: 'Waffen' },
        { letter: 'L', name: 'Lager' },
    ];
    const MAX_STAGE = 7;             // cfg::STATION_STAGES - 1 (Stufen 0..7)
    const PRESET_ENEMIES = { easy: 0, medium: 3, hard: 20 };   // EnemiesForDifficulty()
    const DIFFICULTY_NAMES = { easy: 'Leicht', medium: 'Mittel', hard: 'Schwer', custom: 'Benutzerdefiniert' };
    const MOON_NAMES = { grey: 'Grau', orange: 'Orange-Rot', blue: 'Blau' };
    const REBUILD_TEXT = 'Die Welt wird neu aufgebaut, alle Spieler starten an einer Station neu, ihr Fortschritt bleibt erhalten.';
    const SAVE_SETTLE_MS = 6000;     // so lange nach dem Speichern auf die neuen Werte im Status warten

    // Zahlen deutsch formatieren (12.345 und 4,2)
    const nf0 = new Intl.NumberFormat('de-DE', { maximumFractionDigits: 0 });
    const nf1 = new Intl.NumberFormat('de-DE', { minimumFractionDigits: 1, maximumFractionDigits: 1 });
    const nf2 = new Intl.NumberFormat('de-DE', { minimumFractionDigits: 2, maximumFractionDigits: 2 });
    const encoder = new TextEncoder();   // für die Byte-Länge des Servernamens (UTF-8)

    const $ = (id) => document.getElementById(id);

    // --- Zustand der Seite ------------------------------------------------------
    const state = {
        status: null,          // letzte erfolgreiche Antwort von /api/status
        lastOkAt: 0,           // Zeitpunkt (ms) dieser Antwort
        timer: 0,
        inFlight: false,       // läuft gerade eine Statusabfrage?
        baseline: null,        // Einstellungen, die zuletzt ins Formular geladen wurden
        savedPending: null,    // { settings, until }: gerade gespeichert, Spiel-Thread hat noch nicht übernommen
        saving: false,
        playersKey: '',        // Vergleichswert, um die Tabelle nur bei Änderungen neu zu bauen
        highscoreKey: '',
    };

    // =========================================================================
    // Hilfsfunktionen: Formatierung und DOM
    // =========================================================================
    const pad2 = (n) => String(n).padStart(2, '0');

    // Sekunden -> "h:mm:ss" (Bestenliste)
    function fmtHMS(seconds) {
        const s = Math.max(0, Math.floor(Number(seconds) || 0));
        return `${Math.floor(s / 3600)}:${pad2(Math.floor((s % 3600) / 60))}:${pad2(s % 60)}`;
    }

    // Sekunden -> "h:mm:ss", ab einem Tag "2 T 03:04:05" (Laufzeit, Anmeldezeiten)
    function fmtDuration(seconds) {
        const s = Math.max(0, Math.floor(Number(seconds) || 0));
        const days = Math.floor(s / 86400);
        const rest = s % 86400;
        const h = Math.floor(rest / 3600);
        const hms = `${days > 0 ? pad2(h) : h}:${pad2(Math.floor((rest % 3600) / 60))}:${pad2(rest % 60)}`;
        return days > 0 ? `${days} T ${hms}` : hms;
    }

    const fmtClock = (ms) => new Date(ms).toLocaleTimeString('de-DE', { hour: '2-digit', minute: '2-digit', second: '2-digit' });

    // Unix-Zeit -> ["09.10.2025", "08:53"] (Datum und Uhrzeit getrennt, für zwei Zeilen)
    function fmtDate(unixSeconds) {
        const d = new Date(Number(unixSeconds) * 1000);
        if (!Number.isFinite(d.getTime()) || unixSeconds <= 0) return ['–', ''];
        return [d.toLocaleDateString('de-DE', { day: '2-digit', month: '2-digit', year: 'numeric' }),
            d.toLocaleTimeString('de-DE', { hour: '2-digit', minute: '2-digit' })];
    }

    // Zelle mit Haupttext und optionaler zweiter, gedämpfter Zeile
    function twoLineCell(className, first, second) {
        const td = el('td', className, first);
        if (second) td.appendChild(el('span', 'line2', second));
        return td;
    }

    const clampPct = (v) => Math.max(0, Math.min(100, Number(v) || 0));

    // Element mit Klasse und Text anlegen (Text immer über textContent!)
    function el(tag, className, text) {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (text !== undefined && text !== null) node.textContent = String(text);
        return node;
    }

    function setText(id, text) {
        const node = $(id);
        if (node && node.textContent !== text) node.textContent = text;
    }

    // Balken setzen: Breite in Prozent und Farbe nach Grenzwerten
    // (unter warnAt grün, unter badAt gelb, sonst rot)
    function setBar(node, pct, warnAt, badAt) {
        if (!node) return;
        const p = clampPct(pct);
        node.style.width = `${p}%`;   // CSSOM ist trotz CSP erlaubt (kein Inline-Stil im HTML)
        node.classList.remove('lvl-good', 'lvl-warn', 'lvl-bad');
        if (warnAt !== undefined) node.classList.add(p < warnAt ? 'lvl-good' : p < badAt ? 'lvl-warn' : 'lvl-bad');
    }

    // =========================================================================
    // Verbindung und Abfrage
    // =========================================================================
    function setConnection(kind, text) {
        const dot = $('conn-dot');
        dot.className = `dot dot-${kind}`;
        setText('conn-text', text);
    }

    function showBanner(text, kind) {
        const b = $('banner');
        b.textContent = text;
        b.classList.toggle('banner-wait', kind === 'wait');
        b.hidden = false;
    }

    function hideBanner() { $('banner').hidden = true; }

    // Daten veraltet: Inhalt sichtbar lassen, aber gedämpft darstellen
    function markStale(stale) { $('main').classList.toggle('stale', stale); }

    function schedule() {
        clearTimeout(state.timer);
        state.timer = setTimeout(poll, document.hidden ? POLL_HIDDEN_MS : POLL_MS);
    }

    // Eine Statusabfrage. Wird nach jedem Ende (Erfolg oder Fehler) neu geplant,
    // so laufen nie zwei Abfragen gleichzeitig, auch wenn der Server langsam ist.
    async function poll() {
        clearTimeout(state.timer);
        if (state.inFlight) return;
        state.inFlight = true;
        const ctrl = new AbortController();
        const timeout = setTimeout(() => ctrl.abort(), FETCH_TIMEOUT_MS);
        try {
            const res = await fetch('/api/status', {
                credentials: 'same-origin',
                cache: 'no-store',
                headers: { Accept: 'application/json' },
                signal: ctrl.signal,
            });
            if (res.status === 503) {
                setConnection('wait', 'Server startet …');
                showBanner('Der Spielserver startet noch. Die Daten erscheinen in wenigen Sekunden.', 'wait');
            } else if (res.status === 401) {
                setConnection('bad', 'Nicht angemeldet');
                showBanner('Die Anmeldung ist ungültig oder abgelaufen. Bitte die Seite neu laden und erneut anmelden.');
                markStale(state.lastOkAt > 0);
            } else if (res.status === 429) {
                setConnection('bad', 'Gesperrt');
                showBanner('Zu viele falsche Anmeldungen von dieser Adresse. Bitte später erneut versuchen.');
                markStale(state.lastOkAt > 0);
            } else if (!res.ok) {
                throw new Error(`HTTP ${res.status}`);
            } else {
                const data = await res.json();
                state.status = data;
                state.lastOkAt = Date.now();
                setConnection('ok', 'Online');
                hideBanner();
                markStale(false);
                setText('last-update', `Stand: ${fmtClock(state.lastOkAt)}`);
                render(data);
            }
        } catch (err) {
            // Netzwerkfehler, Zeitüberschreitung (abort) oder kaputtes JSON
            setConnection('bad', 'Verbindung verloren');
            showBanner(state.lastOkAt
                ? `Verbindung verloren – angezeigt wird der Stand von ${fmtClock(state.lastOkAt)}. Neuer Versuch läuft …`
                : 'Keine Verbindung zum Server. Neuer Versuch läuft …');
            markStale(state.lastOkAt > 0);
        } finally {
            clearTimeout(timeout);
            state.inFlight = false;
            schedule();
        }
    }

    // =========================================================================
    // Darstellung
    // =========================================================================
    function render(d) {
        renderHeader(d);
        renderLoad(d);
        renderPlayers(d, false);
        renderHighscores(d);
        syncForm(d);
    }

    function renderHeader(d) {
        const name = d.serverName || 'Mondschürfer-Server';
        setText('server-name', name);
        const title = `${name} – Mondschürfer-Verwaltung`;
        if (document.title !== title) document.title = title;
    }

    function renderLoad(d) {
        // Schritt-Last: Anteil der Zeit, die ein Simulationsschritt vom Zeitbudget
        // (1000 ms / Schritte pro Sekunde) braucht. Über 100 % kommt der Server
        // nicht mehr hinterher und das Spiel ruckelt.
        const load = Number(d.load) || 0;
        setText('m-load', `${nf1.format(load)} %`);
        setBar($('m-load-bar'), load, 50, 80);
        const budget = 1000 / (Number(d.tickRate) || 30);
        setText('m-tick', `Ø ${nf2.format(d.avgTickMs || 0)} ms · max ${nf2.format(d.maxTickMs || 0)} ms pro Schritt (Budget ${nf1.format(budget)} ms)`);

        const max = Math.max(1, Number(d.maxPlayers) || 1);
        setText('m-players', `${d.playersOnline} / ${d.maxPlayers}`);
        setBar($('m-players-bar'), (d.playersOnline / max) * 100, 75, 100);
        setText('m-players-sub', `eingestelltes Maximum · Obergrenze ${d.maxPlayersLimit}`);

        setText('m-spectators', String(d.spectators));
        setText('m-spectators-sub', `von ${d.maxSpectators} Zuschauerplätzen`);
        setText('m-cpu', `${nf1.format(d.cpu || 0)} %`);
        setText('m-cores', `${d.cores} ${d.cores === 1 ? 'Kern' : 'Kerne'} · 100 % = 1 Kern`);
        setText('m-ram', `${nf1.format(d.memoryMB || 0)} MB`);
        setText('m-net-in', `${nf1.format(d.netInKBs || 0)} KB/s`);
        setText('m-net-out', `${nf1.format(d.netOutKBs || 0)} KB/s`);
        setText('m-uptime', fmtDuration(d.uptime));
        setText('m-delivered', nf0.format(d.totalDelivered || 0));

        const stage = Math.max(0, Math.min(MAX_STAGE, Number(d.stationStage) || 0));
        setText('m-stage', `${stage} von ${MAX_STAGE}`);
        const pips = $('m-stage-pips');
        if (pips.childElementCount !== MAX_STAGE) {
            pips.replaceChildren();
            for (let i = 0; i < MAX_STAGE; i++) pips.appendChild(el('span', 'pip'));
        }
        Array.from(pips.children).forEach((p, i) => p.classList.toggle('on', i < stage));

        setText('m-enemies', `${d.enemiesAlive} aktiv`);
        const s = d.settings || {};
        setText('m-enemies-sub', `Soll: ${s.enemyShips} (${DIFFICULTY_NAMES[s.difficulty] || '–'})`);
        setText('m-accounts', nf0.format(d.accounts || 0));
        setText('m-port', String(d.gamePort || '–'));
    }

    // --- Spielerliste ---------------------------------------------------------
    function upgradeCell(levels) {
        const wrap = el('span', 'upg');
        const parts = [];
        UPGRADES.forEach((u, i) => {
            const lvl = Math.max(0, Math.min(UPGRADE_MAX, Number(levels && levels[i]) || 0));
            parts.push(`${u.name} ${lvl}/${UPGRADE_MAX}`);
            const group = el('span', 'upg-group');
            group.title = `${u.name}: Stufe ${lvl} von ${UPGRADE_MAX}`;
            group.appendChild(el('span', 'upg-letter', u.letter));
            for (let k = 0; k < UPGRADE_MAX; k++) group.appendChild(el('span', k < lvl ? 'upg-pip on' : 'upg-pip'));
            wrap.appendChild(group);
        });
        wrap.setAttribute('aria-label', parts.join(', '));
        wrap.setAttribute('role', 'img');
        return wrap;
    }

    function statusChip(p) {
        if (!p.alive) return el('span', 'chip chip-wait', 'wartet auf Schiff');
        if (p.docked) return el('span', 'chip chip-dock', 'angedockt');
        return el('span', 'chip chip-fly', 'fliegt');
    }

    // Zelle mit Beschriftung (data-label wird auf dem Handy über dem Wert angezeigt)
    function cell(label, className, content) {
        const td = el('td', className);
        td.dataset.label = label;
        if (content instanceof Node) td.appendChild(content);
        else td.textContent = String(content);
        return td;
    }

    function playerRow(p) {
        const tr = document.createElement('tr');
        tr.appendChild(cell('Name', 'name', p.name));

        // Fortschritt = Kontostand im Verhältnis zum Ziel (100.000)
        const prog = el('div', 'progress');
        const bar = el('div', 'bar');
        const fill = el('div', 'bar-fill');
        setBar(fill, p.progress);
        bar.appendChild(fill);
        prog.appendChild(bar);
        prog.appendChild(el('span', 'progress-pct', `${nf1.format(p.progress || 0)} %`));
        const progCell = cell('Fortschritt', 'cell-progress', prog);
        progCell.appendChild(el('span', 'progress-sub', `${nf0.format(p.balance || 0)} / ${nf0.format(GOAL)}`));
        tr.appendChild(progCell);

        // Runde > 1: der Spieler hat das Ziel mit diesem Schiff schon erreicht
        // und spielt mit seinen Aufrüstungen weiter.
        const round = el('span', 'round', String(p.round));
        if (p.round > 1) round.appendChild(el('span', 'badge badge-accent', 'mit Aufrüstung'));
        tr.appendChild(cell('Runde', '', round));

        tr.appendChild(cell('Aufrüstungen', 'cell-upgrades', upgradeCell(p.upgrades)));
        tr.appendChild(cell('Schiffszeit', 'num', fmtDuration(p.shipTime)));
        tr.appendChild(cell('Angemeldet seit', 'num', fmtDuration(p.session)));
        tr.appendChild(cell('Gesamte Anmeldezeit', 'num', fmtDuration(p.totalLogin)));
        tr.appendChild(cell('Abschüsse / Abstürze / Ziele', 'num', `${p.kills} / ${p.deaths} / ${p.goals}`));
        tr.appendChild(cell('Status', '', statusChip(p)));

        const ping = Number(p.ping) || 0;
        const pingNode = el('span', ping < 80 ? 'text-good' : ping < 150 ? 'text-warn' : 'text-bad', `${ping} ms`);
        tr.appendChild(cell('Ping', 'num', pingNode));
        return tr;
    }

    // force = true: auch neu zeichnen, wenn sich die Daten nicht geändert haben
    // (z. B. weil der Suchtext geändert wurde).
    function renderPlayers(d, force) {
        if (!d) return;
        const players = Array.isArray(d.players) ? d.players : [];
        const query = $('player-search').value.trim().toLowerCase();
        const key = JSON.stringify(players) + '\u0000' + query;
        if (!force && key === state.playersKey) return;
        state.playersKey = key;

        const shown = query ? players.filter((p) => String(p.name).toLowerCase().includes(query)) : players;
        const body = $('players-body');
        const rows = document.createDocumentFragment();
        shown.forEach((p) => rows.appendChild(playerRow(p)));
        body.replaceChildren(rows);

        setText('players-count', players.length === 1 ? '1 Spieler online' : `${players.length} Spieler online`);
        setText('search-hits', query ? (shown.length === 1 ? '1 Treffer' : `${shown.length} Treffer`) : '');

        const empty = $('players-empty');
        if (players.length === 0) {
            empty.textContent = 'Zurzeit ist niemand eingeloggt. Sobald sich jemand im Spiel unter „Mehrspieler“ anmeldet, erscheint er hier.';
            empty.hidden = false;
        } else if (shown.length === 0) {
            empty.textContent = `Kein eingeloggter Spieler passt zu „${$('player-search').value.trim()}“.`;
            empty.hidden = false;
        } else {
            empty.hidden = true;
        }
        body.closest('table').hidden = shown.length === 0;
    }

    // --- Bestenliste ------------------------------------------------------------
    function renderHighscores(d) {
        const list = (Array.isArray(d.highscores) ? d.highscores.slice() : [])
            .sort((a, b) => (a.seconds || 0) - (b.seconds || 0));   // kürzeste Zeit zuerst
        const key = JSON.stringify(list);
        if (key === state.highscoreKey) return;
        state.highscoreKey = key;

        const body = $('hs-body');
        const rows = document.createDocumentFragment();
        list.forEach((h, i) => {
            const tr = document.createElement('tr');
            const rankTd = el('td', 'num hs-rank');
            rankTd.appendChild(el('span', i < 3 ? `rank rank-${i + 1}` : 'rank', String(i + 1)));
            tr.appendChild(rankTd);
            tr.appendChild(el('td', 'hs-name', h.name));
            tr.appendChild(el('td', 'num hs-time', fmtHMS(h.seconds)));
            // Runde 1 = mit einem neuen Schiff geschafft; ab Runde 2 hatte der
            // Spieler schon die Aufrüstungen aus den vorherigen Runden.
            const round = Number(h.round) || 1;
            tr.appendChild(twoLineCell('hs-round', `Runde ${round}`, round > 1 ? ' (mit Aufrüstung)' : ' (neues Schiff)'));
            const [date, time] = fmtDate(h.when);
            tr.appendChild(twoLineCell('num hs-date', date, time ? ` ${time}` : ''));
            rows.appendChild(tr);
        });
        body.replaceChildren(rows);
        $('hs-empty').hidden = list.length > 0;
        body.closest('table').hidden = list.length === 0;
    }

    // =========================================================================
    // Formular "Spielparameter"
    // =========================================================================
    const form = () => $('settings-form');
    const FIELDS = ['serverName', 'difficulty', 'enemyShips', 'tradeStations', 'shipyards', 'moon', 'maxPlayers'];

    // Aktuelle Eingaben als Objekt (gleicher Aufbau wie "settings" im JSON)
    function readForm() {
        const moon = form().querySelector('input[name="moon"]:checked');
        return {
            serverName: $('f-serverName').value.trim(),
            difficulty: $('f-difficulty').value,
            enemyShips: Number($('f-enemyShips').value),
            tradeStations: Number($('f-tradeStations').value),
            shipyards: Number($('f-shipyards').value),
            moon: moon ? moon.value : '',
            maxPlayers: Number($('f-maxPlayers').value),
        };
    }

    function sameSettings(a, b) {
        if (!a || !b) return false;
        return FIELDS.every((k) => String(a[k]) === String(b[k]));
    }

    function fillForm(s) {
        $('f-serverName').value = s.serverName;
        $('f-difficulty').value = s.difficulty;
        $('f-enemyShips').value = s.enemyShips;
        $('f-tradeStations').value = s.tradeStations;
        $('f-shipyards').value = s.shipyards;
        form().querySelectorAll('input[name="moon"]').forEach((r) => { r.checked = r.value === s.moon; });
        $('f-maxPlayers').value = s.maxPlayers;
        updateEnemyField();
        updateNameCounter();
    }

    // Anzahl feindlicher Schiffe nur bei "Benutzerdefiniert" frei; sonst zeigt
    // das Feld den festen Wert des Schwierigkeitsgrads.
    function updateEnemyField() {
        const diff = $('f-difficulty').value;
        const input = $('f-enemyShips');
        const custom = diff === 'custom';
        input.disabled = !custom;
        if (!custom) input.value = PRESET_ENEMIES[diff];
        setText('f-enemyShips-help', custom
            ? `0 bis ${input.max} feindliche Raumschiffe`
            : 'nur bei „Benutzerdefiniert“ einstellbar');
    }

    function nameBytes() { return encoder.encode($('f-serverName').value.trim()).length; }

    function updateNameCounter() {
        const n = nameBytes();
        const max = Number($('f-serverName').dataset.maxBytes || 40);
        const counter = $('f-serverName-count');
        counter.textContent = `${n} / ${max} Byte`;
        counter.classList.toggle('text-bad', n > max);
        $('f-serverName').setAttribute('aria-invalid', n > max || n === 0 ? 'true' : 'false');
    }

    // Grenzen aus dem Status übernehmen (falls der Server andere hat als im HTML)
    function applyLimits(limits) {
        if (!limits) return;
        $('f-enemyShips').max = limits.maxEnemyShips;
        $('f-tradeStations').max = limits.maxTradeStations;
        $('f-shipyards').max = limits.maxShipyards;
        $('f-maxPlayers').max = limits.maxPlayersLimit;
        setText('f-maxPlayers-help', `1 bis ${limits.maxPlayersLimit}`);
    }

    function showDefaults(def) {
        if (!def) return;
        setText('d-serverName', def.serverName);
        setText('d-difficulty', DIFFICULTY_NAMES[def.difficulty] + (def.difficulty === 'custom' ? ` (${def.enemyShips})` : ''));
        setText('d-tradeStations', String(def.tradeStations));
        setText('d-shipyards', String(def.shipyards));
        setText('d-moon', MOON_NAMES[def.moon] || def.moon);
        setText('d-maxPlayers', String(def.maxPlayers));
    }

    const isDirty = () => state.baseline !== null && !sameSettings(readForm(), state.baseline);

    // Bearbeitet der Benutzer gerade ein Feld? Dann darf die automatische
    // Aktualisierung das Formular nicht überschreiben.
    function isEditing() {
        const a = document.activeElement;
        return !!a && form().contains(a) && (a.tagName === 'INPUT' || a.tagName === 'SELECT');
    }

    function updateDirtyUi() {
        const dirty = isDirty();
        $('dirty-note').hidden = !dirty;
        $('btn-save').disabled = !dirty || state.saving;
        $('btn-discard').disabled = !dirty || state.saving;
        $('btn-reset').disabled = state.saving || !state.status;
    }

    // Wird nach jeder Statusabfrage aufgerufen.
    function syncForm(d) {
        applyLimits(d.limits);
        showDefaults(d.defaults);
        const server = d.settings;
        if (!server) return;

        // Gerade gespeichert: Bis der Spiel-Thread die neuen Werte übernommen
        // hat, zeigt /api/status noch die alten. Das Formular in dieser Zeit
        // nicht auf die alten Werte zurückspringen lassen.
        if (state.savedPending) {
            if (sameSettings(server, state.savedPending.settings) || Date.now() > state.savedPending.until) {
                state.savedPending = null;
            } else {
                updateDirtyUi();
                return;
            }
        }

        const dirty = isDirty();
        if (state.baseline === null || (!dirty && !isEditing() && !state.saving)) {
            if (!sameSettings(server, state.baseline)) {
                fillForm(server);
                state.baseline = { ...server };
            }
            $('stale-note').hidden = true;
        } else {
            // Der Benutzer bearbeitet das Formular; wurde inzwischen woanders
            // (z. B. in einem zweiten Browserfenster) etwas geändert, nur darauf hinweisen.
            $('stale-note').hidden = !dirty || sameSettings(server, state.baseline);
        }
        updateDirtyUi();
    }

    function showMessage(text, kind) {
        const box = $('settings-msg');
        box.textContent = text;
        box.className = `note ${kind === 'ok' ? 'note-ok' : 'note-error'}`;
        box.hidden = false;
    }

    // Prüft die Eingaben vor dem Senden (der Server prüft trotzdem noch einmal).
    function validateForm() {
        const maxBytes = Number($('f-serverName').dataset.maxBytes || 40);
        const n = nameBytes();
        if (n === 0) return fail('f-serverName', 'Bitte einen Servernamen eingeben.');
        if (n > maxBytes) return fail('f-serverName', `Der Servername ist zu lang (${n} von höchstens ${maxBytes} Byte; Umlaute zählen doppelt).`);
        const numbers = [
            ['f-enemyShips', 'Anzahl feindlicher Raumschiffe'],
            ['f-tradeStations', 'Handelsstationen'],
            ['f-shipyards', 'Werften'],
            ['f-maxPlayers', 'Maximale Spieler'],
        ];
        for (const [id, label] of numbers) {
            const input = $(id);
            if (input.disabled) continue;
            const v = Number(input.value);
            const min = Number(input.min), max = Number(input.max);
            if (input.value.trim() === '' || !Number.isInteger(v) || v < min || v > max) {
                return fail(id, `${label}: bitte eine ganze Zahl von ${min} bis ${max} eingeben.`);
            }
        }
        if (!readForm().moon) return fail(null, 'Bitte eine Mondoberfläche auswählen.');
        return true;

        function fail(id, text) {
            showMessage(text, 'error');
            if (id) $(id).focus();
            return false;
        }
    }

    // Eigener Bestätigungsdialog (<dialog>), Rückfall auf confirm() in alten Browsern.
    function confirmDialog(title, paragraphs, okLabel) {
        const dlg = $('confirm-dialog');
        if (!dlg || typeof dlg.showModal !== 'function') {
            return Promise.resolve(window.confirm(paragraphs.join('\n\n')));
        }
        setText('confirm-title', title);
        const box = $('confirm-text');
        box.replaceChildren(...paragraphs.map((p) => el('p', '', p)));
        setText('confirm-ok', okLabel);
        return new Promise((resolve) => {
            dlg.returnValue = '';
            dlg.addEventListener('close', () => resolve(dlg.returnValue === 'ok'), { once: true });
            dlg.showModal();
            $('confirm-cancel').focus();   // sicherer Standard: Abbrechen
        });
    }

    // Gemeinsamer Teil von "Speichern" und "Zurücksetzen".
    async function postSettings(url, body) {
        state.saving = true;
        updateDirtyUi();
        try {
            const res = await fetch(url, {
                method: 'POST',
                credentials: 'same-origin',
                cache: 'no-store',
                headers: {
                    'X-Requested-With': 'mondschuerfer',   // ohne diesen Header lehnt der Server ab (CSRF-Schutz)
                    'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8',
                    Accept: 'application/json',
                },
                body: body ? body.toString() : '',
            });
            let data = null;
            try { data = await res.json(); } catch (e) { data = null; }
            if (res.ok && data && data.ok) {
                fillForm(data.settings);
                state.baseline = { ...data.settings };
                state.savedPending = { settings: { ...data.settings }, until: Date.now() + SAVE_SETTLE_MS };
                $('stale-note').hidden = true;
                showMessage(data.message || 'Gespeichert.', 'ok');
            } else if (res.status === 401) {
                showMessage('Nicht angemeldet – bitte die Seite neu laden.', 'error');
            } else {
                showMessage((data && data.error) || `Speichern fehlgeschlagen (HTTP ${res.status}).`, 'error');
            }
        } catch (e) {
            showMessage('Speichern fehlgeschlagen: keine Verbindung zum Server.', 'error');
        } finally {
            state.saving = false;
            updateDirtyUi();
        }
    }

    async function onSave(ev) {
        ev.preventDefault();
        if (state.saving || !validateForm()) return;
        const s = readForm();
        const cur = state.status && state.status.settings;
        // Stationen oder Mondfarbe geändert -> der Server baut die Welt neu auf.
        const rebuild = !!cur && (s.tradeStations !== cur.tradeStations || s.shipyards !== cur.shipyards || s.moon !== cur.moon);
        if (rebuild && !(await confirmDialog('Welt neu aufbauen?', [REBUILD_TEXT], 'Speichern und neu aufbauen'))) return;

        const body = new URLSearchParams();
        body.set('serverName', s.serverName);
        body.set('difficulty', s.difficulty);
        body.set('enemyShips', String(s.difficulty === 'custom' ? s.enemyShips : PRESET_ENEMIES[s.difficulty]));
        body.set('tradeStations', String(s.tradeStations));
        body.set('shipyards', String(s.shipyards));
        body.set('moon', s.moon);
        body.set('maxPlayers', String(s.maxPlayers));
        await postSettings('/api/settings', body);
    }

    async function onReset() {
        if (state.saving || !state.status) return;
        const def = state.status.defaults || {};
        const cur = state.status.settings || {};
        const text = [
            'Alle Spielparameter auf die Standardwerte aus docker-compose.yml zurücksetzen?',
            `Standard: ${DIFFICULTY_NAMES[def.difficulty] || '–'} (${def.enemyShips} feindliche Schiffe), ` +
                `${def.tradeStations} Handelsstation(en), ${def.shipyards} Werft(en), Mond ${MOON_NAMES[def.moon] || '–'}, ` +
                `max. ${def.maxPlayers} Spieler.`,
        ];
        if (def.tradeStations !== cur.tradeStations || def.shipyards !== cur.shipyards || def.moon !== cur.moon) {
            text.push(REBUILD_TEXT);
        }
        if (!(await confirmDialog('Auf Standard zurücksetzen?', text, 'Zurücksetzen'))) return;
        await postSettings('/api/settings/reset', null);
    }

    function onDiscard() {
        if (!state.status || !state.status.settings) return;
        fillForm(state.status.settings);
        state.baseline = { ...state.status.settings };
        $('stale-note').hidden = true;
        $('settings-msg').hidden = true;
        updateDirtyUi();
    }

    // =========================================================================
    // Start: Ereignisse verbinden, erste Abfrage
    // =========================================================================
    function init() {
        $('f-serverName').dataset.maxBytes = '40';   // limits::MAX_SERVER_NAME

        $('player-search').addEventListener('input', () => renderPlayers(state.status, true));

        const f = form();
        f.addEventListener('submit', onSave);
        f.addEventListener('input', () => { updateNameCounter(); updateDirtyUi(); });
        f.addEventListener('change', (ev) => {
            if (ev.target && ev.target.id === 'f-difficulty') updateEnemyField();
            updateDirtyUi();
        });
        $('btn-discard').addEventListener('click', onDiscard);
        $('btn-reset').addEventListener('click', onReset);
        updateDirtyUi();

        // Tab wieder sichtbar: sofort aktualisieren statt bis zu 10 s zu warten
        document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });
        poll();
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();
