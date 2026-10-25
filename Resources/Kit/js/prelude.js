// Mantle Kit for JavaScript/TypeScript: small conveniences written in JS on top of the host API.
// Runs once per script context, right after the generated API classes and services are set up,
// and before the entry script itself. It can only call the public API (World, Players, State,
// Net, UI, Entity, Signal, ...), so it can't widen what scripts are allowed to do.
// __mantleKitExtend is deleted from the global object right after this file finishes running.
//
// Reference copy for tooling (type generation, docs) -- the actual runtime never reads this
// file. MantleQuickJS.cpp embeds an identical copy directly (GPreludeSource), the same reason
// MantleLuau.cpp embeds its own Luau prelude: no filesystem read needed to execute it, Shipping
// or otherwise. Keep the two in sync by hand.

const extend = __mantleKitExtend;

// Signal.prototype.wait() -> Promise
// Resolves the next time the signal fires. Resolves with a single value when the signal fired
// with exactly one argument, otherwise with an array of every argument.
extend("Signal", "wait", function () {
	const self = this;
	return new Promise(function (resolve) {
		self.once(function (...args) {
			resolve(args.length <= 1 ? args[0] : args);
		});
	});
});

// State.observe(key, fn) -> Connection
// Calls fn(value) once immediately with the current value, then again every time it changes.
extend("State", "observe", function (key, fn) {
	if (typeof key !== "string") {
		throw new TypeError("State.observe: argument 1 (key) must be a string");
	}
	if (typeof fn !== "function") {
		throw new TypeError("State.observe: argument 2 (handler) must be a function");
	}
	const self = this;
	task.spawn(fn, self.get(key));
	return self.changed.connect(function (changedKey, value) {
		if (changedKey === key) {
			fn(value);
		}
	});
});

// Entity.prototype.observeAttribute(name, fn) -> Connection
// Calls fn(value) now and every time the named attribute changes.
extend("Entity", "observeAttribute", function (name, fn) {
	const self = this;
	task.spawn(fn, self.getAttribute(name));
	return self.attributeChanged.connect(function (changed, value) {
		if (changed === name) {
			fn(value);
		}
	});
});
