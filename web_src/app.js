    // ── Button-press feedback (vibration, falling back to a beep) ────────
    // Ported verbatim from Favero_OPP2/data/index.html -- iOS Safari has no
    // navigator.vibrate() (deliberate WebKit policy, not a lagging
    // implementation) and Firefox dropped it in 2024; the Web Audio tone
    // is generated in JS so there's nothing extra to ship.
    let audioCtx = null;
    function beep() {
      try {
        if (!audioCtx) {
          const AudioCtx = window.AudioContext || window.webkitAudioContext;
          if (!AudioCtx) return;
          audioCtx = new AudioCtx();
        }
        if (audioCtx.state === 'suspended') audioCtx.resume();
        const osc = audioCtx.createOscillator();
        const gain = audioCtx.createGain();
        osc.type = 'sine';
        osc.frequency.value = 880;
        gain.gain.value = 0.15;
        osc.connect(gain).connect(audioCtx.destination);
        osc.start();
        osc.stop(audioCtx.currentTime + 0.08);
      } catch (e) {
        // No audio feedback available either -- silently do nothing.
      }
    }
    function feedback(ms) {
      ms = ms || 100;
      if (navigator.vibrate && navigator.vibrate(ms)) return;
      beep();
    }

    // ── UI input actions ─────────────────────────────────────────────────
    // Route names match the UI_INPUT_* constants in EventDefinitions.h
    // 1:1 (lowercased) -- WebRemoteHandler.cpp translates each one straight
    // into UDPIOHandler::getInstance().InputChanged(EVENT_UI_INPUT |
    // UI_INPUT_*), the exact same call physical buttons and the existing
    // OPRCP UDP remote protocol already make. No IR, no bridge -- this
    // device owns FSM state directly.
    function ui(name, feedbackMs) {
      feedback(feedbackMs);
      fetch('/ui/' + name, { method: 'POST' });
    }

    // Generalized long-press gesture: short tap fires opts.tap, long-press
    // fires opts.long instead (opts.onTapOnly, if given, replaces the tap
    // action with a plain JS callback -- used by Reset, which only warns on
    // a tap and needs long-press to actually confirm). Ported from
    // Favero_OPP2's bindLongPress().
    function bindLongPress(el, opts) {
      const LONG_PRESS_MS = 500;
      let timer = null;
      let firedLong = false;

      function start() {
        if (el.disabled) return;
        firedLong = false;
        timer = setTimeout(function () {
          firedLong = true;
          if (opts.long) ui(opts.long, opts.longFeedbackMs);
        }, LONG_PRESS_MS);
      }
      function cancel() {
        if (timer) { clearTimeout(timer); timer = null; }
      }
      function end() {
        cancel();
        if (firedLong) return;
        if (opts.onTapOnly) {
          opts.onTapOnly();
        } else if (opts.tap) {
          ui(opts.tap, opts.tapFeedbackMs);
        }
      }

      el.addEventListener('pointerdown', start);
      el.addEventListener('pointerup', end);
      el.addEventListener('pointercancel', cancel);
      el.addEventListener('pointerleave', cancel);
      el.addEventListener('contextmenu', function (e) { e.preventDefault(); });
    }

    document.getElementById('btnStartStop').addEventListener('click', function () { ui('toggle_timer'); });
    document.getElementById('btnNextPeriod').addEventListener('click', function () { ui('next_period'); });
    bindLongPress(document.getElementById('btnReset'), {
      long: 'reset', longFeedbackMs: 200,
      onTapOnly: function () {
        const el = document.getElementById('statusLineMain');
        el.textContent = 'Long-press to reset';
        setTimeout(function () { el.textContent = ''; }, 1500);
      }
    });
    bindLongPress(document.getElementById('btnScoreLeft'),
      { tap: 'incr_score_left', long: 'decr_score_left', longFeedbackMs: 200 });
    bindLongPress(document.getElementById('btnScoreRight'),
      { tap: 'incr_score_right', long: 'decr_score_right', longFeedbackMs: 200 });
    // Mini +/- buttons: plain click, same events as the score's own
    // tap/long-press -- an added convenience, not a separate control.
    document.getElementById('btnPlusLeft').addEventListener('click', function () { ui('incr_score_left'); });
    document.getElementById('btnMinusLeft').addEventListener('click', function () { ui('decr_score_left', 200); });
    document.getElementById('btnPlusRight').addEventListener('click', function () { ui('incr_score_right'); });
    document.getElementById('btnMinusRight').addEventListener('click', function () { ui('decr_score_right', 200); });
    bindLongPress(document.getElementById('btnYellowLeft'),
      { tap: 'yellow_card_left', long: 'yellow_card_left_decr', longFeedbackMs: 200 });
    bindLongPress(document.getElementById('btnYellowRight'),
      { tap: 'yellow_card_right', long: 'yellow_card_right_decr', longFeedbackMs: 200 });
    bindLongPress(document.getElementById('btnRedLeft'),
      { tap: 'red_card_left', long: 'red_card_left_decr', longFeedbackMs: 200 });
    bindLongPress(document.getElementById('btnRedRight'),
      { tap: 'red_card_right', long: 'red_card_right_decr', longFeedbackMs: 200 });
    bindLongPress(document.getElementById('btnBlackLeft'),
      { tap: 'black_card_left', long: 'black_card_left_decr', longFeedbackMs: 200 });
    bindLongPress(document.getElementById('btnBlackRight'),
      { tap: 'black_card_right', long: 'black_card_right_decr', longFeedbackMs: 200 });
    bindLongPress(document.getElementById('btnUW2F'),
      { tap: 'p_card', long: 'p_card_undo', longFeedbackMs: 200 });
    document.getElementById('btnPrio').addEventListener('click', function () { ui('prio'); });
    document.getElementById('btnRestoreUw2f').addEventListener('click', function () { ui('restore_uw2f_timer'); });

    // ── Three-screen navigation (Penalties <-> Main <-> Match) ───────────
    // Main sits in the middle of the sequence, matching Favero_OPP2's own
    // linear-sequence placement of its "Home" position. Center icon always
    // jumps straight to Main from anywhere -- no "settings"-style second
    // destination the way Favero_OPP2's center icon has, since this page
    // has no equivalent screen to toggle to.
    const NAV_SEQUENCE = ['penalties', 'main', 'match'];
    let navIndex = NAV_SEQUENCE.indexOf('main');
    function showView(name) {
      document.getElementById('viewMain').classList.toggle('active', name === 'main');
      document.getElementById('viewPenalties').classList.toggle('active', name === 'penalties');
      document.getElementById('viewMatch').classList.toggle('active', name === 'match');
    }
    function applyNavPosition() {
      showView(NAV_SEQUENCE[navIndex]);
      document.getElementById('navLeft').disabled = navIndex <= 0;
      document.getElementById('navRight').disabled = navIndex >= NAV_SEQUENCE.length - 1;
    }
    function showNavPosition(name) {
      const idx = NAV_SEQUENCE.indexOf(name);
      if (idx === -1) return;
      navIndex = idx;
      applyNavPosition();
    }
    // No-op at either end of the sequence rather than wrapping -- matches
    // "can only be reached sequentially", not a carousel (same as
    // Favero_OPP2's compactNavStep()).
    function navStep(delta) {
      const next = navIndex + delta;
      if (next < 0 || next >= NAV_SEQUENCE.length) return;
      navIndex = next;
      applyNavPosition();
    }
    document.getElementById('navLeft').addEventListener('click', function () { navStep(-1); });
    document.getElementById('navRight').addEventListener('click', function () { navStep(1); });
    document.getElementById('navCenter').addEventListener('click', function () { showNavPosition('main'); });
    applyNavPosition();

    // Visual press feedback: toggles .pressed on the button's own
    // .icon-glyph wrapper (a translucent navy circle) -- ported from
    // Favero_OPP2's bindIconPress().
    function bindIconPress(btn) {
      const glyph = btn.querySelector('.icon-glyph');
      function press() { glyph.classList.add('pressed'); }
      function release() { glyph.classList.remove('pressed'); }
      btn.addEventListener('pointerdown', press);
      ['pointerup', 'pointercancel', 'pointerleave'].forEach(function (evt) {
        btn.addEventListener(evt, release);
      });
    }
    bindIconPress(document.getElementById('navLeft'));
    bindIconPress(document.getElementById('navRight'));
    bindIconPress(document.getElementById('navCenter'));

    // Match/lifecycle actions -- see the comment on #viewMatch in
    // index.html for why fencer entry isn't here.
    document.getElementById('btnCyranoPrev').addEventListener('click', function () { ui('cyrano_prev'); });
    document.getElementById('btnCyranoBegin').addEventListener('click', function () { ui('cyrano_begin'); });
    document.getElementById('btnCyranoNext').addEventListener('click', function () { ui('cyrano_next'); });
    document.getElementById('btnCyranoEnd').addEventListener('click', function () { ui('cyrano_end'); });
    document.getElementById('btnCycleWeapon').addEventListener('click', function () { ui('cycle_weapon'); });

    // Swipe between screens -- ported from Favero_OPP2's compact nav swipe
    // handling (same horizontal-lock-with-deadzone technique, same
    // reasons: listens on `document` not `document.body` so it still
    // works regardless of how body sizes itself; non-passive touchmove so
    // it can preventDefault() once a gesture is confirmed horizontal,
    // which stops both the page drifting vertically mid-swipe and (iOS)
    // the system edge-swipe-back gesture from stealing it.
    (function () {
      let startX = 0, startY = 0;
      let dragging = false, horizontalLock = false;
      document.addEventListener('touchstart', function (e) {
        startX = e.touches[0].clientX;
        startY = e.touches[0].clientY;
        dragging = true;
        horizontalLock = false;
      }, { passive: true });
      document.addEventListener('touchmove', function (e) {
        if (!dragging) return;
        const dx = e.touches[0].clientX - startX;
        const dy = e.touches[0].clientY - startY;
        if (!horizontalLock) {
          if (Math.abs(dx) > 10 && Math.abs(dx) > Math.abs(dy)) {
            horizontalLock = true;
          } else if (Math.abs(dy) > 10) {
            dragging = false;
            return;
          }
        }
        if (horizontalLock) e.preventDefault();
      }, { passive: false });
      document.addEventListener('touchend', function (e) {
        const wasDragging = dragging;
        dragging = false;
        if (!wasDragging) return;
        const dx = e.changedTouches[0].clientX - startX;
        const dy = e.changedTouches[0].clientY - startY;
        if (Math.abs(dx) < 60 || Math.abs(dx) < Math.abs(dy)) return;
        navStep(dx < 0 ? 1 : -1);
      }, { passive: true });
      document.addEventListener('touchcancel', function () { dragging = false; }, { passive: true });
    })();

    // ── Fullscreen ────────────────────────────────────────────────────────
    // Feature-detected, vendor-prefixed fallbacks included -- ported from
    // Favero_OPP2 (see its CLAUDE.md for the real-device findings behind
    // this: older Chromium/WebKit only expose prefixed methods, iOS Safari
    // gates it behind an off-by-default Feature Flag entirely). Trimmed to
    // just the toggle -- no auto-fullscreen-on-first-tap or persisted
    // preference here, this page has no Settings view to hold that
    // checkbox.
    (function () {
      const btn = document.getElementById('btnFullscreen');
      const docEl = document.documentElement;
      function requestFn(el) {
        return el.requestFullscreen || el.webkitRequestFullscreen ||
               el.mozRequestFullScreen || el.msRequestFullscreen;
      }
      function exitFn() {
        return document.exitFullscreen || document.webkitExitFullscreen ||
               document.mozCancelFullScreen || document.msExitFullscreen;
      }
      function currentFsElement() {
        return document.fullscreenElement || document.webkitFullscreenElement ||
               document.mozFullScreenElement || document.msFullscreenElement;
      }
      if (!requestFn(docEl)) { btn.style.display = 'none'; return; }
      btn.addEventListener('click', function () {
        if (currentFsElement()) {
          try { exitFn().call(document); } catch (e) {}
        } else {
          try { requestFn(docEl).call(docEl); } catch (e) {}
        }
      });
    })();

    // ── State polling ────────────────────────────────────────────────────
    const APPARATUS_STATE = ["Fencing", "Halt", "Pause", "Waiting", "Ending", "Unknown"];
    const WEAPON = ["Foil", "Epee", "Sabre", "Unknown"];
    // red is the lit/unlit boolean; redCount is optional and ONLY passed
    // by the row-1 "real cards" call (to show the count, from
    // OPP2::ScoreState.red_cards, opp2_types.h -- a 0-9 count, no separate
    // boolean field exists there). The P-card row passes red only (a
    // bucketed boolean from pCardBuckets() below) and omits redCount
    // entirely, so the square keeps its static "P" label -- passing
    // pLeft.red into the old single combined redCount parameter here was
    // the bug that made it show the literal text "true" instead of "P".
    function setCardStatusTrio(idPrefix, side, yellow, red, black, redCount) {
      const yellowEl = document.getElementById(idPrefix + side + 'Yellow');
      const redEl    = document.getElementById(idPrefix + side + 'Red');
      const blackEl  = document.getElementById(idPrefix + side + 'Black');
      yellowEl.classList.toggle('lit', !!yellow);
      redEl.classList.toggle('lit', !!red);
      if (redCount !== undefined) redEl.textContent = redCount > 0 ? redCount : '';
      blackEl.classList.toggle('lit', !!black);
    }
    // P-card ordinal (0-5): bucket thresholds 1=yellow,2=red,3=black,
    // cumulative (reaching red keeps yellow lit too) -- same scheme
    // Favero_OPP2 settled on (see its CLAUDE.md: an earlier 1-2/3-4/5
    // spacing made every other press invisible).
    function pCardBuckets(pCard) {
      return { yellow: pCard >= 1, red: pCard >= 2, black: pCard >= 3 };
    }

    async function poll() {
      try {
        const res = await fetch('/api/state', { cache: 'no-store' });
        const s = await res.json();

        document.getElementById('btnScoreLeft').textContent = s.left.score;
        document.getElementById('btnScoreRight').textContent = s.right.score;
        const mins = Math.floor(s.clock.time_ms / 60000);
        const secs = Math.floor((s.clock.time_ms % 60000) / 1000);
        document.getElementById('clockText').textContent = mins + ':' + String(secs).padStart(2, '0');
        document.getElementById('roundText').textContent = s.round;

        document.getElementById('matchState').textContent = APPARATUS_STATE[s.apparatus_state] || '?';
        document.getElementById('matchNum').textContent = s.match_num;
        document.getElementById('matchRound').textContent = s.round;
        document.getElementById('matchWeapon').textContent = WEAPON[s.weapon] || '?';

        setCardStatusTrio('cardStatus', 'Left', s.left.yellow_card, s.left.red_cards > 0, s.left.black_card, s.left.red_cards);
        setCardStatusTrio('cardStatus', 'Right', s.right.yellow_card, s.right.red_cards > 0, s.right.black_card, s.right.red_cards);
        const pLeft = pCardBuckets(s.left.p_card);
        const pRight = pCardBuckets(s.right.p_card);
        setCardStatusTrio('pcardStatus', 'Left', pLeft.yellow, pLeft.red, pLeft.black);
        setCardStatusTrio('pcardStatus', 'Right', pRight.yellow, pRight.red, pRight.black);

        // priority: 0=NONE, 1=RIGHT, 2=LEFT (OPP2::Priority enum order)
        document.getElementById('prioIndicatorLeft').classList.toggle('active', s.priority === 2);
        document.getElementById('prioIndicatorRight').classList.toggle('active', s.priority === 1);

        // FencingStateMachine::update(UDPIOHandler*) ignores every
        // UI_INPUT_* event except stop-timer while the clock runs --
        // disabling these client-side just makes a tap fail visibly
        // (button looks unavailable) instead of silently doing nothing.
        // Yellow additionally stays disabled once a side already has a red
        // card, same guard as the FSM itself (UI_INPUT_YELLOW_CARD_LEFT/
        // RIGHT no-ops when m_RedCardLeft/Right != 0).
        const running = !!s.clock.running;
        document.getElementById('btnYellowLeft').disabled = running || s.left.red_cards > 0;
        document.getElementById('btnYellowRight').disabled = running || s.right.red_cards > 0;
        ['btnScoreLeft', 'btnScoreRight', 'btnNextPeriod', 'btnReset',
         'btnPlusLeft', 'btnMinusLeft', 'btnPlusRight', 'btnMinusRight',
         'btnRedLeft', 'btnRedRight', 'btnBlackLeft', 'btnBlackRight',
         'btnUW2F', 'btnPrio', 'btnRestoreUw2f'
        ].forEach(function (id) {
          document.getElementById(id).disabled = running;
        });

        document.getElementById('statusLineMain').textContent = '';
      } catch (e) {
        document.getElementById('statusLineMain').textContent = 'Connection lost, retrying...';
      }
    }
    // Self-rescheduling rather than setInterval(poll, 500) -- setInterval
    // fires on a fixed clock regardless of whether the previous /api/state
    // fetch has resolved yet. On a slow/lossy WiFi link (or while the
    // device is already busy answering a button POST), that let requests
    // pile up as genuinely concurrent connections; the device's web server
    // isn't safe against that (see WebRemoteHandler.cpp's request-slot
    // guard, added for the same reason after this was traced to a crash
    // 2026-08-12). Waiting for each poll to finish before scheduling the
    // next means this client only ever has one /api/state request in
    // flight, no matter how slow the network gets.
    //
    // Elapsed-time-compensated (2026-08-13): DevTools Network tab showed
    // real fetch time swinging ~31ms-500ms+ even with the match timer
    // stopped (Core 0 -- WiFi/lwIP, MQTT client, and this web server's task
    // all share it -- contention, not anything clock-related). A plain
    // `setTimeout(scheduleNextPoll, 500)` after every fetch made that
    // jitter compound directly into the update cadence (fetch_time + 500ms
    // between updates, so ~530ms-1000ms+ instead of a flat 500ms) --
    // exactly the "bursty, sometimes stalls over a second" symptom this
    // was reported for. Subtracting elapsed fetch time keeps the target a
    // genuine ~500ms between poll *starts* while still never firing the
    // next request before the previous one resolved (Math.max floor at 0
    // preserves that -- a slow fetch just means the next one fires
    // immediately, never overlapping).
    function scheduleNextPoll() {
      const started = performance.now();
      poll().finally(function () {
        const elapsed = performance.now() - started;
        setTimeout(scheduleNextPoll, Math.max(0, 500 - elapsed));
      });
    }
    scheduleNextPoll();
