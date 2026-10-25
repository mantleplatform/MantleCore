No patches were needed to vendor Luau 0.738 for UE 5.8 / MSVC 14.44. Warnings
that upstream's own code trips under UE's default warning level are relaxed at
the module level in LuauLib.Build.cs instead of by editing upstream/ in place.

If a genuine source change ever becomes unavoidable, it belongs here as a
.patch file plus a note on what it does and why upstream cannot take it --
never as a hand-edit inside upstream/.
