# Mantle Kit for Python: small conveniences written in Python on top of the host API.
# Runs once per VM before any script, in its own module. It only calls the registry-generated
# API (Signal.connect/once, State.get, Entity.get_attribute/attribute_changed) plus the two
# builtins __mantle_extend and __mantle_signal_wait_token, so it can't widen what scripts are
# allowed to do -- exactly like Resources/Kit/luau/prelude.luau and Kit/js/prelude.js. Ported
# from ScriptLab's Resources/Kit/python/prelude.py (Lab* -> Mantle* throughout).
#
# Signal.wait() has no coroutine trick to write here (unlike the Luau Kit's
# coroutine.yield()-based Wait): PocketPy generators are driven by the host, not by the script,
# so `signal.wait()` just returns a token object; the *caller* suspends with `yield token`, and
# the host (MantlePocketPy.cpp's FVm::ResumeTask) fills in `token.result` with the signal's
# arguments before resuming. See MantlePocketPy.cpp's header comment for why `x = yield expr`
# (PocketPy's grammar only allows `yield expr` as a bare statement, not an expression) isn't the
# idiom here.


def _signal_wait(self):
    return __mantle_signal_wait_token(self)


__mantle_extend("Signal", "wait", _signal_wait)


def _state_observe(self, key, fn):
    if not isinstance(key, str):
        raise TypeError("State.observe: argument 1 (key) must be a string")
    if not callable(fn):
        raise TypeError("State.observe: argument 2 (handler) must be a function")
    task.spawn(fn, self.get(key))

    def _relay(changed_key, value):
        if changed_key == key:
            fn(value)

    return self.changed.connect(_relay)


__mantle_extend("State", "observe", _state_observe)


def _entity_observe_attribute(self, name, fn):
    if not isinstance(name, str):
        raise TypeError("Entity.observe_attribute: argument 1 (name) must be a string")
    if not callable(fn):
        raise TypeError("Entity.observe_attribute: argument 2 (handler) must be a function")
    task.spawn(fn, self.get_attribute(name))

    def _relay(changed_name, value):
        if changed_name == name:
            fn(value)

    return self.attribute_changed.connect(_relay)


__mantle_extend("Entity", "observe_attribute", _entity_observe_attribute)


def _export_function(package, name):
    def _call(*args):
        return Exports.call(package, name, *args)

    return _call


def _export_table(package):
    # The package lives in the closure, not on the object, so no export name can clash with it. An export whose name Python
    # resolves itself (a dunder) is reached as table["name"].
    class _ExportTable:
        def __getattr__(self, name):
            return _export_function(package, name)

        def __getitem__(self, name):
            return _export_function(package, name)

        def __setattr__(self, name, value):
            raise TypeError("an export table is read-only; the package that owns it sets exports with Exports.set")

    return _ExportTable()


def _exports_get(self, package):
    if not isinstance(package, str):
        raise TypeError("Exports.get: argument 1 (package) must be a string")
    return _export_table(package)


__mantle_extend("Exports", "get", _exports_get)
__mantle_extend("Exports", "get_place", lambda self: _export_table(""))
