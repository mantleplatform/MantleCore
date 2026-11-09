// Force-included into every Luau translation unit by MantleGameLuauLib.Build.cs (ForceIncludeFiles). Nothing else
// includes it. It turns off the MSVC warnings that upstream Luau trips on its own style, each read in the source
// and found harmless. upstream/ stays untouched. UBT has no per-module raw compiler flag, which is why this is a
// header and not "/wd" options. Keep in step with the plugin's LuauLib.
//
// The reason for each (Luau 0.746):
//   4127  conditional expression is constant: `if` on a template parameter (lmem, ltable, lgc) or on build-time
//         macros (LUA_VECTOR_DOUBLE). The compiler removes the dead branch; that is the point.
//   4310  cast truncates constant value: lstrlib packint's `(char)MC` (255) is a deliberate byte fill.
//   4324  structure padded due to alignment specifier: Luau::Variant (alignas storage). Only padding.
//   4389  signed/unsigned mismatch in `==`/`!=`: tag fields (unsigned 4-bit key tag vs int value tag, always 0..15),
//         the opcode compare in an assert, and 0/1 flag compares in JUMPXEQK*. No value is out of range.
//   4457  declaration hides function parameter: an inner loop/if variable (attributes, expr, ty, func) that is the one
//         used inside its scope; the outer parameter is not meant there and is used again after the scope.
//   4505  unreferenced function removed: ltable.cpp hashvec(double*); only the float overload is used with 3/4-float
//         vectors. Upstream's LUAU_MAYBE_UNUSED is empty on MSVC.
//   4702  unreachable code: the `return 0;` after the endless loop in ltable.cpp rawfindnode, kept for other compilers.
#ifdef _MSC_VER
#pragma warning(disable: 4127 4310 4324 4389 4457 4505 4702)
#endif
