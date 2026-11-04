// The user store lives in IndexedDB, so what is written on the calculator
// survives a reload. It is read in before main() runs; the backend writes it
// back after every change (see sim_sdl3.c).
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
	FS.mkdir('/qdos-store');
	FS.mkdir('/qdos-inbox');
	FS.mount(IDBFS, {}, '/qdos-store');
	addRunDependency('qdos-store');
	FS.syncfs(true, function (err) {
		if (err) {
			console.warn('qdos: could not read the saved store', err);
		}
		removeRunDependency('qdos-store');
	});
});

// Writes are coalesced: one sync in flight at a time, and one more queued
// behind it if anything changed meanwhile.
(function () {
	var syncing = false;
	var again = false;

	Module.qdosPersist = function () {
		if (syncing) {
			again = true;
			return;
		}
		syncing = true;
		FS.syncfs(false, function (err) {
			syncing = false;
			if (err) {
				console.warn('qdos: could not save the store', err);
			}
			if (again) {
				again = false;
				Module.qdosPersist();
			}
		});
	};

	// The shell saves its session when it next looks, which is within a slice
	// of its wait; see sim_wait in sim_sdl3.c
	document.addEventListener('visibilitychange', function () {
		if (document.visibilityState === 'hidden') {
			Module.qdosHidden = true;
		}
	});
})();

// Keys the page presses for the reader: Module.qdosType(steps) queues them,
// and the shell takes them one at a time, paced so they can be watched going
// in. A step is {key: 'ENTER'} or {text: '7 sq'}, and a text step may carry
// its own {gap} in milliseconds between characters, for a long one. Named keys travel as the
// negative codes sim_sdl3.c's WEB_KEYS lists, characters as themselves. Letters
// are typed as on the keypad, with ALPHA put on before them and off after.
(function () {
	var KEYS = {
		ENTER: -1, ESC: -2,
		SOFT1: -3, SOFT2: -4, SOFT3: -5, SOFT4: -6, SOFT5: -7,
		MUL: -8, DIV: -9, ADD: -10, SUB: -11,
		CPLX: -12, PWR: -13, DEL: -14,
	};
	var ALPHA_ON = -15;
	var ALPHA_OFF = -16;
	var KEY_GAP_MS = 190;
	var CHAR_GAP_MS = 55;

	var queue = [];
	var dueAt = 0;
	var done = null;

	Module.qdosType = function (steps, onDone) {
		queue = [];
		var alpha = null; // not known until the first change
		function wantAlpha(on, gap) {
			if (alpha !== on) {
				queue.push({code: on ? ALPHA_ON : ALPHA_OFF, gap: alpha === null ? 0 : gap});
				alpha = on;
			}
		}
		steps.forEach(function (step) {
			if (step.key) {
				wantAlpha(false, KEY_GAP_MS);
				queue.push({code: KEYS[step.key], gap: KEY_GAP_MS});
			} else {
				var gap = step.gap || CHAR_GAP_MS;
				for (var i = 0; i < step.text.length; i++) {
					var ch = step.text.charAt(i);
					// The digits work under ALPHA, so a name like i64 stays in it
					if (ch >= 'a' && ch <= 'z') {
						wantAlpha(true, gap);
					} else if (!(ch >= '0' && ch <= '9')) {
						wantAlpha(false, gap);
					}
					queue.push({code: step.text.charCodeAt(i), gap: gap});
				}
			}
		});
		if (alpha) {
			wantAlpha(false, KEY_GAP_MS);
		}
		dueAt = 0;
		done = onDone || null;
		if (queue.length === 0 && done) {
			var finished = done;
			done = null;
			setTimeout(finished, 0);
		}
	};

	Module.qdosKeyDue = function () {
		return queue.length > 0 && performance.now() >= dueAt ? 1 : 0;
	};

	Module.qdosNextKey = function () {
		if (!Module.qdosKeyDue()) {
			return 0;
		}
		var next = queue.shift();
		dueAt = performance.now() + next.gap;
		if (queue.length === 0 && done) {
			var finished = done;
			done = null;
			setTimeout(finished, next.gap);
		}
		return next.code;
	};
})();
