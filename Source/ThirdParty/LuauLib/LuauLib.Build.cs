// Copyright Joshua Eger. All Rights Reserved.
using System.IO;
using UnrealBuildTool;

// Vendored Luau 0.738 (MIT). See VERSION for the exact tag, commit and retrieval date, and
// for the full list of what was pruned from upstream and why.
//
// T4.8: this module exists only to compile Luau's Ast, Bytecode (one file), Compiler and VM
// into a UE module. It is a PRIVATE dependency of MantleScript and of nothing else --
// grep every other .Build.cs in the repo for "LuauLib" to confirm that invariant still holds
// before adding a new dependent.
//
// THE SANDBOX BEGINS HERE. This module deliberately never compiles:
//   - Luau's CodeGen (the native JIT). Not vendored at all -- see VERSION. A JIT inside a
//     shipped client is attack surface (it maps executable memory at runtime) for a use case
//     (running short creator scripts) that does not need native code generation speed.
//   - Luau's CLI, REPL, luau-analyze and Require. Not vendored at all. Require in particular
//     is upstream's disk-based require() resolver -- exactly the filesystem access this
//     vendoring must not carry, and it is never linked into anything that ships.
//   - Luau's own test/bench/fuzz trees. Not vendored -- irrelevant to a shipped module.
// What remains (Ast, Bytecode/BytecodeBuilder.cpp only, Compiler, VM, Common) has no
// file or network access by construction: Luau's VM ships no "io" library at all, and its
// "os" library is deliberately restricted to os.time/os.clock/os.date/os.difftime with no
// os.execute, os.remove or os.getenv. There is no single OS-facing translation unit inside
// VM/src the way quickjs-libc.c is a single file inside QuickJS -- the restriction is
// upstream's own design, not something this Build.cs has to carve out.
public class LuauLib : ModuleRules
{
	public LuauLib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.CPlusPlus;

		// Third-party code with its own conventions; UE's shared PCH is neither available
		// nor useful here.
		PCHUsage = PCHUsageMode.NoPCHs;

		// This is a private implementation detail of MantleScript. Nothing here should be
		// visible to a module that only depends on LuauLib transitively.
		bUseRTTI = false;
		// Luau's parser and compiler use C++ exceptions internally to turn syntax and
		// compilation failures into ordinary error results. Disabling unwind semantics
		// compiles under MSVC with C4530 but makes those failure paths unsound.
		bEnableExceptions = true;

		// Modular UE targets put every module behind a shared-library boundary. Luau
		// defaults LUA_API and LUACODE_API to plain `extern`, so its public entry points
		// would compile into this module but remain hidden from MantleScript. Export them
		// for modular targets; monolithic Game/Server targets need no annotation. The
		// matching Windows imports live in MantleScript.Build.cs. This changes compiler
		// definitions only; upstream/ remains pristine.
		if (Target.LinkType != TargetLinkType.Monolithic)
		{
			if (Target.IsInPlatformGroup(UnrealPlatformGroup.Windows))
			{
				PrivateDefinitions.Add("LUA_API=__declspec(dllexport)");
				PrivateDefinitions.Add("LUACODE_API=__declspec(dllexport)");
			}
			else
			{
				PrivateDefinitions.Add("LUA_API=__attribute__((visibility(\"default\")))");
				PrivateDefinitions.Add("LUACODE_API=__attribute__((visibility(\"default\")))");
			}
		}

		string UpstreamDir = Path.Combine(ModuleDirectory, "upstream");

		// PublicIncludePaths (not Private) because LuauLib's own Compiler/src needs to reach
		// across into Bytecode/include and Common/include via "Luau/Whatever.h", and because
		// MantleScript (the only permitted dependent, see header comment) needs lua.h,
		// lualib.h and luacode.h to actually call into the VM and compiler it links.
		PublicIncludePaths.AddRange(new string[]
		{
			Path.Combine(UpstreamDir, "Common", "include"),
			Path.Combine(UpstreamDir, "Ast", "include"),
			Path.Combine(UpstreamDir, "Bytecode", "include"),
			Path.Combine(UpstreamDir, "Compiler", "include"),
			Path.Combine(UpstreamDir, "VM", "include"),
		});

		// --- Warning relaxations, scoped to this module only ---
		// Luau is developed against GCC/Clang/MSVC directly via CMake, not against UE's
		// warning set, and its own CMake build disables a comparable list of MSVC warnings
		// (see luau-lang/luau's CMakeLists.txt). None of these bear on memory safety; they
		// are style/portability warnings UE happens to elevate that upstream does not use.
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off; // Luau's parser and VM reuse names in nested scopes throughout.
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off; // Feature-detection #if's on macros only some toolchains define.
		CppCompileWarningSettings.UnsafeTypeCastWarningLevel = WarningLevel.Off; // Bytecode encode/decode is inherently full of narrowing int casts.
		CppCompileWarningSettings.UnreachableCodeWarningLevel = WarningLevel.Off; // Defensive switch exits retained by upstream trip Clang 20's extended unreachable-code warnings.

		// A unity build concatenates this module's translation units into one file. Luau's
		// interpreter/compiler sources use short, file-local static helper names (e.g.
		// "next", "fold") in more than one .cpp; those never collided as separate translation
		// units but easily collide once concatenated. Not worth chasing for a third-party
		// vendoring -- turn unity off for this module only.
		bUseUnity = false;

		// MSVC's static analyzer flags a large number of upstream Luau patterns (signed/
		// unsigned comparisons in the bytecode format, intentional fallthrough in the
		// interpreter's opcode dispatch) that are correct as written and are not going to be
		// fixed by editing pristine upstream source. Analysis of first-party Mantle code is
		// unaffected -- this flag is local to the LuauLib module only.
		bDisableStaticAnalysis = true;
	}
}
