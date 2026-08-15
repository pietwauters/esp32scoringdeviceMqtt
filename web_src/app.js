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
    // Favero_OPP2's bindLongPress(). opts.onLongFire is optional (added
    // 2026-08-14 for the WiFi button, which needs a persistent status
    // message on top of the normal fire-and-forget ui() POST) -- every
    // existing caller omits it, so this is purely additive.
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
          if (opts.onLongFire) opts.onLongFire();
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

    // ── Four-screen navigation (Penalties <-> Main <-> Match <-> Menu) ───
    // Main sits in the middle of the original three, matching Favero_OPP2's
    // own linear-sequence placement of its "Home" position. Menu was
    // appended at the end 2026-08-13 -- reachable by stepping/swiping past
    // Penalties, or via the center icon's Home<->Settings toggle while on
    // Main, matching Favero_OPP2's own 5-position center-icon behavior
    // (this page just has one settings-style screen, not several). Match
    // (OPP2 lifecycle: Prev/Begin/Next/End) and Penalties swapped sides
    // 2026-08-15 per explicit request -- Match/OPP2 left of Main, Penalties
    // right of Main.
    const NAV_SEQUENCE = ['match', 'main', 'penalties', 'menu'];
    let navIndex = NAV_SEQUENCE.indexOf('main');
    function showView(name) {
      document.getElementById('viewMain').classList.toggle('active', name === 'main');
      document.getElementById('viewPenalties').classList.toggle('active', name === 'penalties');
      document.getElementById('viewMatch').classList.toggle('active', name === 'match');
      document.getElementById('viewMenu').classList.toggle('active', name === 'menu');
    }
    // Center icon shows the cog and jumps to Menu only while on Main;
    // everywhere else (including Menu itself) it shows home and jumps to
    // Main -- so Menu is always one tap from Main, and Main is always one
    // tap from anywhere else, same "always a way back to Main" guarantee
    // the plain always-home version had before Menu existed.
    function updateCenterButton() {
      const onMain = NAV_SEQUENCE[navIndex] === 'main';
      const use = document.querySelector('#navCenter .icon use');
      const iconId = onMain ? '#icon-cog' : '#icon-home';
      use.setAttribute('href', iconId);
      use.setAttribute('xlink:href', iconId);
      document.getElementById('navCenter').setAttribute('aria-label', onMain ? 'Menu' : 'Main');
    }
    function applyNavPosition() {
      showView(NAV_SEQUENCE[navIndex]);
      document.getElementById('navLeft').disabled = navIndex <= 0;
      document.getElementById('navRight').disabled = navIndex >= NAV_SEQUENCE.length - 1;
      updateCenterButton();
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
    document.getElementById('navCenter').addEventListener('click', function () {
      showNavPosition(NAV_SEQUENCE[navIndex] === 'main' ? 'menu' : 'main');
    });
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
    document.getElementById('btnCycleRound').addEventListener('click', function () { ui('cycle_round'); });

    // WiFi reboots the device into a separate, dedicated setup mode
    // (WifiSetupMode.h) rather than serving a scan/connect page from
    // this same running app -- an earlier version tried the latter and
    // real-world testing found it fundamentally unreliable (WiFi
    // scanning conflicts with this AsyncWebServer being alive at all).
    // /wifi won't exist on *this* server afterward, so there's nothing
    // to navigate to here -- just tell the user where to go once they've
    // rejoined the device's own network. Same long-press-to-confirm
    // pattern as Full Reset below, since this is now also an
    // unconditional reboot.
    bindLongPress(document.getElementById('btnMenuWifi'), {
      long: 'start_wifi_portal', longFeedbackMs: 200,
      onTapOnly: function () {
        const el = document.getElementById('statusLineMenu');
        el.textContent = 'Long-press to reconfigure WiFi (reboots the device)';
        setTimeout(function () { el.textContent = ''; }, 1500);
      },
      onLongFire: function () {
        document.getElementById('statusLineMenu').textContent =
          'Rebooting into WiFi setup mode -- reconnect to this device\'s own WiFi network, then go to 192.168.4.1/wifi';
      }
    });
    document.getElementById('btnMenuSettings').addEventListener('click', function () {
      window.location.href = '/settings';
    });
    document.getElementById('btnMenuOta').addEventListener('click', function () {
      fetch('/ui/start_ota_portal', { method: 'POST' }).then(function () {
        window.location.href = '/update';
      });
    });
    // Same long-press-to-confirm pattern as btnReset -- a plain tap only
    // warns, since this reboots the device (no NVS wipe, see
    // WebRemoteHandler.cpp's comment on UI_FULL_RESET vs DoFactoryReset()).
    bindLongPress(document.getElementById('btnMenuFullReset'), {
      long: 'full_reset', longFeedbackMs: 200,
      onTapOnly: function () {
        const el = document.getElementById('statusLineMenu');
        el.textContent = 'Long-press to restart device';
        setTimeout(function () { el.textContent = ''; }, 1500);
      }
    });

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
    // gates it behind an off-by-default Feature Flag entirely).
    //
    // No floating per-page toggle button anymore -- "we almost always just
    // want it fullscreen" made a manual button on every screen feel wrong.
    // Instead: auto-request fullscreen on the very first tap/click anywhere
    // on the page (browsers require a real user gesture, so this can't
    // happen on load -- first interaction is the earliest legal moment).
    // btnMenuFullscreen (Menu view) is a plain session-only toggle, nothing
    // more -- an earlier version persisted "user exited" to localStorage
    // so it wouldn't auto-retry next time, but that meant exiting
    // fullscreen for *any* reason (even just to check something in the
    // browser chrome) silently disabled auto-fullscreen forever after,
    // which is the opposite of "we almost always want it." No persisted
    // state at all now -- every fresh page load gets one auto-attempt on
    // first tap, unconditionally.
    (function () {
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
      const menuBtn = document.getElementById('btnMenuFullscreen');
      if (!requestFn(docEl)) {
        if (menuBtn) menuBtn.style.display = 'none';
        return;
      }

      function enter() {
        try {
          const p = requestFn(docEl).call(docEl);
          // Fullscreen APIs return a Promise (or nothing, on older
          // prefixed implementations) -- catch the rejection instead of
          // letting it surface only as an unhandled-rejection console
          // warning, since there's no user-facing error path here.
          if (p && p.catch) p.catch(function () {});
        } catch (e) {}
      }
      function exit() { try { exitFn().call(document); } catch (e) {} }

      // Deliberately 'click', not 'pointerdown'/'mousedown' -- Chrome (and
      // others) only honor Fullscreen requests as a direct result of a
      // "real" click/touchend-derived gesture, not the earlier
      // pointerdown. First attempt used pointerdown and silently never
      // entered fullscreen anywhere except the Menu button (which already
      // used 'click') -- found 2026-08-14 after real-device testing showed
      // the auto-trigger never fired.
      document.addEventListener('click', function firstTap() {
        document.removeEventListener('click', firstTap);
        if (!currentFsElement()) enter();
      }, { once: true });

      if (menuBtn) {
        function refreshLabel() {
          menuBtn.textContent = currentFsElement() ? 'Exit Fullscreen' : 'Enter Fullscreen';
        }
        refreshLabel();
        document.addEventListener('fullscreenchange', refreshLabel);
        document.addEventListener('webkitfullscreenchange', refreshLabel);
        menuBtn.addEventListener('click', function () {
          if (currentFsElement()) exit(); else enter();
          setTimeout(refreshLabel, 200);
        });
      }
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
    // Mirrors Opp2Handler.cpp's EVENT_ROUND derivation (phase_type/match_type
    // from m_nrOfRounds) rather than re-deriving from a round count of our
    // own -- reads the same three end states the cycle button actually
    // produces (Pool, DE, DE+Team), OPP2::PhaseType/MatchType enum order.
    function matchFormatLabel(phaseType, matchType) {
      if (matchType === 1) return 'Team'; // MatchType::TEAM
      if (phaseType === 0) return 'Pool'; // PhaseType::POOL
      if (phaseType === 1) return 'DE';   // PhaseType::DE
      return '?';
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
        document.getElementById('matchFormat').textContent = matchFormatLabel(s.phase_type, s.match_type);

        // Both sides, not just one -- "who fences who" needs a pairing, a
        // lone name without its opponent isn't that.
        const fencersKnown = s.left.fencer_present && s.right.fencer_present;
        document.getElementById('fencersLine').style.display = fencersKnown ? '' : 'none';
        if (fencersKnown) {
          document.getElementById('fencerLeftName').textContent = s.left.fencer_name;
          document.getElementById('fencerLeftNoc').textContent = s.left.fencer_noc;
          document.getElementById('fencerRightName').textContent = s.right.fencer_name;
          document.getElementById('fencerRightNoc').textContent = s.right.fencer_noc;
        }

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
