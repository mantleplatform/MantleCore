// Copyright Joshua Eger. All Rights Reserved.
using System.IO;
using UnrealBuildTool;

// Vendored quickjs-ng v0.16.2 (MIT). See VERSION for the exact tag, commit and retrieval
// date, and for the full list of what was pruned from upstream and why. upstream/ is
// pristine except the creator-sandbox patches in patches/README.txt (#386), already
// applied and marked MANTLE in the source; "pristine upstream" below means everything else.
//
// T4.8: this module exists only to compile QuickJS's engine (quickjs.c, dtoa.c,
// libregexp.c, libunicode.c) into a UE module. It is a PRIVATE dependency of
// MantleScript and of nothing else -- grep every other .Build.cs in the repo for
// "QuickJSLib" to confirm that invariant still holds before adding a new dependent.
//
// THE SANDBOX BEGINS HERE.
//
//     quickjs-libc.c / quickjs-libc.h are NOT vendored and are NEVER compiled.
//
// That file is upstream's "quickjs-libc" standard-library layer: it is where `std`,
// file I/O (open/read/write/fstat), process spawning (exec/popen/kill) and OS module
// access live. Upstream's own CMakeLists.txt only adds it to the library when
// QJS_BUILD_LIBC is explicitly turned on; we never turn it on, and unlike a
// .Build.cs source-list omission, this file is not present anywhere under
// ThirdParty/QuickJSLib/upstream/ at all -- there is nothing for a future glob or a
// careless "just add the missing file" edit to pick up. See VERSION for the full
// list of what else was left out of upstream/ for the same reason (qjs.c/qjsc.c,
// the CLI tools that link quickjs-libc.c; gen/, generated CLI-only sources).
//
// The only vendored .c files are quickjs.c, dtoa.c, libregexp.c and libunicode.c --
// exactly upstream's CMakeLists.txt `qjs_sources` list with QJS_BUILD_LIBC off. None
// of the four touch the filesystem, the network or a process.
public class QuickJSLib : ModuleRules
{
	public QuickJSLib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.CPlusPlus;

		// Third-party C code with its own conventions; UE's shared PCH is neither
		// available nor useful here.
		PCHUsage = PCHUsageMode.NoPCHs;

		bUseRTTI = false;
		bEnableExceptions = false;

		// quickjs.h already carries the correct shared-library import/export contract.
		// Enable it only when UE is building modular DLLs/shared objects; monolithic
		// Game/Server targets need ordinary external linkage. BUILDING is private to this
		// module, while USING propagates to the sole dependent (MantleScript). When both
		// are visible here, quickjs.h deliberately gives BUILDING precedence.
		if (Target.LinkType != TargetLinkType.Monolithic)
		{
			PrivateDefinitions.Add("BUILDING_QJS_SHARED=1");
			PublicDefinitions.Add("USING_QJS_SHARED=1");
		}

		string UpstreamDir = Path.Combine(ModuleDirectory, "upstream");
		PublicIncludePaths.Add(UpstreamDir);

		// quickjs.c uses C11 keywords (_Alignas) unconditionally, so this module must be
		// compiled as C11 or later. quickjs-ng already carries its own MSVC compatibility
		// layer (cutils.h's __attribute__/inline/format shims, and a switch-based bytecode
		// dispatch fallback in quickjs.c guarded by "#if defined(EMSCRIPTEN) ||
		// defined(_MSC_VER)") -- none of that needed a patch here.
		CStandard = CStandardVersion.C17;

		// quickjs-ng expects the ordinary GNU/POSIX declarations on Linux. UE's
		// cross-toolchain otherwise compiles C17 in a strict feature profile that
		// hides clock_gettime, readlink, localtime_r, tm_gmtoff and alloca.
		if (Target.IsInPlatformGroup(UnrealPlatformGroup.Unix))
		{
			PrivateDefinitions.Add("_GNU_SOURCE=1");
		}

		// quickjs.c unconditionally does "#include quickjs-c-atomics.h" + "#define
		// CONFIG_ATOMICS" unless __STDC_NO_ATOMICS__ is truthy (quickjs.c line ~75), which
		// is the standard C11 way for a translation unit to say "no atomic support here".
		// Real support requires MSVC's C11 <stdatomic.h>, which needs a raw
		// "/experimental:c11atomics" compiler switch -- and UnrealBuildTool's ModuleRules
		// has no per-module mechanism to pass an arbitrary raw compiler argument (the only
		// such property, TargetRules.AdditionalCompilerArguments, is target-wide and would
		// leak the flag to every module in the build, which the ticket's "relax nothing
		// outside these two modules" rule rules out). Rather than patch quickjs.c itself
		// (pristine upstream) or widen the blast radius of a raw flag, this define takes the
		// portable escape hatch upstream already built for exactly this situation. The only
		// cost is the JS `Atomics` object / SharedArrayBuffer low-level atomic ops, which
		// nothing in Mantle's script capability surface (the registry, MantleApi.h) uses or
		// will bind against -- see T4.8's report for the full reasoning. Revisit if a later
		// ticket genuinely needs them.
		PrivateDefinitions.Add("__STDC_NO_ATOMICS__=1");

		// --- Warning relaxations, scoped to this module only ---
		// These mirror the /wd list quickjs-ng's own CMakeLists.txt applies for MSVC
		// (sign conversion, implicit fallthrough, unused parameters, narrowing casts,
		// shadowed locals/parameters) -- upstream already knows it trips these under MSVC
		// and disables them itself; UE's ModuleRules exposes the same set as named
		// WarningLevel properties instead of raw /wd numbers.
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off; // upstream /wd4456, /wd4457
		CppCompileWarningSettings.UnsafeTypeCastWarningLevel = WarningLevel.Off; // upstream /wd4242, /wd4244, /wd4267
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off; // feature-detection #if's on compiler-specific macros
		CppCompileWarningSettings.UndefinedBoolConversionWarningLevel = WarningLevel.Off; // upstream JS_ToBool returns int; JS_NewBool accepts bool

		// UE 5.8's default BuildSettingsVersion treats C4702 (unreachable code) as an error
		// project-wide (CppCompileWarnings.cs: UnreachableCodeWarningLevel defaults to Error
		// from BuildSettingsVersion V7 on). quickjs.c hits this nine times -- e.g. a switch
		// that ends every case in "return"/"abort" plus a trailing defensive return upstream
		// keeps for pre-C11-toolchain compatibility. Real MSVC-level-4 warning, not something
		// a pristine-upstream edit should chase; downgrade it for this module only.
		CppCompileWarningSettings.UnreachableCodeWarningLevel = WarningLevel.Off;

		// A unity build concatenates this module's four translation units into one file.
		// quickjs.c, dtoa.c, libregexp.c and libunicode.c are written as freestanding C
		// amalgamation-style files with plenty of same-named static helpers; keeping them
		// as separate translation units (the vendoring's normal state) avoids relying on
		// upstream never reusing a static identifier across files.
		bUseUnity = false;

		// MSVC's static analyzer is not a tool quickjs-ng is developed against, and it flags
		// a large number of intentional patterns (narrowing casts throughout the bytecode
		// interpreter, the switch-based opcode dispatch) that are not going to be "fixed" by
		// editing pristine upstream source. Analysis of first-party Mantle code is
		// unaffected -- this flag is local to the QuickJSLib module only.
		bDisableStaticAnalysis = true;
	}
}
