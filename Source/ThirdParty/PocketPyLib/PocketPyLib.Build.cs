// Copyright Joshua Eger. All Rights Reserved.
using System.IO;
using UnrealBuildTool;

// Vendored PocketPy v2.2.0 (MIT). See VERSION for the exact tag, commit and retrieval date, and
// for the source patches: the lab's own allocator-hook forward declaration (PocketPy's
// amalgamation otherwise compiles under plain cl.exe with only compiler defines -- see the
// ScriptLab sandbox's EVALUATION.md at D:\UEProjects\ScriptLabTools\python\EVALUATION.md), and a
// second, Mantle-specific one: a deterministic per-opcode instruction-budget interrupt in
// pocketpy.c's dispatch loop, added after the hosted build showed a runaway script's `while
// True:` loop freeze the whole game thread (py_sys_settrace's line-change-only trace hook never
// fired for it) -- see patches/README.txt.
//
// S6.1: this module exists only to compile PocketPy's amalgamation (pocketpy.c/.h) into a UE
// module. It is a PRIVATE dependency of MantleScript and of nothing else -- grep every other
// .Build.cs in the repo for "PocketPyLib" to confirm that invariant still holds before adding a
// new dependent.
//
// OS exclusions below are only part of the sandbox; the initialized Mantle binding also
// gates cached native imports and VM controls (patch 3, #123).
// THE SANDBOX BEGINS HERE. PK_ENABLE_OS=0 removes os.c and common/socket.c -- and open() --
// from the build entirely (a compile-time exclusion, not a runtime check): neither file exists
// in the vendored upstream/ tree's compiled output once this define is set, mirroring how
// LuauLib and QuickJSLib exclude filesystem/socket access. PK_ENABLE_THREADS is left off (0):
// MantleScript drives every VM from the game thread only, exactly like the Luau and QuickJS VMs.
public class PocketPyLib : ModuleRules
{
	public PocketPyLib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.CPlusPlus;

		// Third-party C code with its own conventions; UE's shared PCH is neither available
		// nor useful here.
		PCHUsage = PCHUsageMode.NoPCHs;

		bUseRTTI = false;
		bEnableExceptions = false;

		string UpstreamDir = Path.Combine(ModuleDirectory, "upstream");
		PublicIncludePaths.Add(UpstreamDir);
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

		// Route PocketPy's compile-time allocator macro seam (pocketpy.h's own extension
		// point -- there is no runtime allocator-callback parameter, unlike Luau's
		// lua_newstate(allocator, ud)) to the hooks in Private/MantlePocketPyAlloc.c. See
		// Public/MantlePocketPyHooks.h and VERSION for why this needs one small
		// forward-declaration patch in upstream/pocketpy.h.
		PublicDefinitions.Add("PK_MALLOC(size)=MantlePocketPy_Malloc(size)");
		PublicDefinitions.Add("PK_REALLOC(ptr,size)=MantlePocketPy_Realloc(ptr,size)");
		PublicDefinitions.Add("PK_FREE(ptr)=MantlePocketPy_Free(ptr)");

		// pocketpy.c uses C11 keywords (_Static_assert, _Alignas) unconditionally -- under
		// cl.exe this needs /std:c11 or the compile fails at link time with "unresolved
		// external symbol _Static_assert". CStandard = C11 is the UBT equivalent of that flag
		// and also covers the Linux/clang dedicated-server build (QuickJSLib uses the same
		// mechanism, at C17, for its own C11-keyword requirement).
		CStandard = CStandardVersion.C11;

		PublicDefinitions.Add("PK_ENABLE_OS=0");
		PublicDefinitions.Add("PK_ENABLE_WATCHDOG=1");
		PublicDefinitions.Add("PK_ENABLE_THREADS=0");
		// Explicit in both the upstream C and consuming C++ translation units: collector
		// diagnostics have no declarations, storage, callbacks, or branches in Shipping.
		PublicDefinitions.Add("MANTLE_POCKETPY_GC_DIAGNOSTICS=" +
			(Target.Configuration == UnrealTargetConfiguration.Shipping ? "0" : "1"));
		// Root-retirement tests count allocator ENTRIES (including free), not net bytes.
		// Explicit C/C++ definition: no counter storage or branches in Shipping.
		PublicDefinitions.Add("MANTLE_POCKETPY_ROOT_DIAGNOSTICS=" +
			(Target.Configuration == UnrealTargetConfiguration.Shipping ? "0" : "1"));

		// The production allocator denies only when this is 1. Enabled once
		// every creator-reachable caller propagated allocation failure and the
		// legacy NameDict path was made NULL-safe, so a denied allocation retires
		// the owning script rather than faulting. Admission is scoped to blocks
		// owned by a live quota cell; host and bootstrap work keeps its legacy
		// attribution. Set to 0 to compare against the pre-enforcement behaviour.
		PublicDefinitions.Add("MANTLE_POCKETPY_ENFORCE_QUOTA=1");

		// pocketpy.c expects the ordinary GNU/POSIX declarations on Linux. UE's cross-toolchain
		// otherwise compiles C11 in a strict feature profile that hides several declarations the
		// amalgamation relies on -- the exact reason QuickJSLib.Build.cs sets this too.
		if (Target.IsInPlatformGroup(UnrealPlatformGroup.Unix))
		{
			PrivateDefinitions.Add("_GNU_SOURCE=1");
		}

		// MantlePocketPyHooks.h's POCKETPY_HOOKS_API: export these symbols across the shared-
		// library boundary for modular targets (MantlePocketPy.cpp, in MantleScript, reads
		// them) -- same shape as LuauLib's LUA_API/LUACODE_API (LuauLib.Build.cs). Monolithic
		// Game/Server targets need no annotation. Missing the Unix branch here (this module
		// only had the Windows dllexport half) left every symbol on the header's plain-`extern`
		// fallback under Linux's default hidden visibility, so the modular Linux Editor/cooker
		// build -- which MantleScript's dedicated-server BuildCookRun runs on the dedicated
		// Ubuntu builder -- failed to link with "undefined symbol: MantlePocketPy_*" (#163).
		if (Target.LinkType != TargetLinkType.Monolithic)
		{
			if (Target.IsInPlatformGroup(UnrealPlatformGroup.Windows))
			{
				PrivateDefinitions.Add("POCKETPY_HOOKS_API=__declspec(dllexport)");
			}
			else
			{
				PrivateDefinitions.Add("POCKETPY_HOOKS_API=__attribute__((visibility(\"default\")))");
			}
		}

		// --- Warning relaxations, scoped to this module only ---
		// PocketPy is developed against GCC/Clang/MSVC directly via CMake, not against UE's
		// warning set. None of these bear on memory safety; they are the same category of
		// narrowing-cast/shadow/sign-compare warnings LuauLib and QuickJSLib already relax
		// for their own vendored C/C++.
		CppCompileWarningSettings.ShadowVariableWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.UnsafeTypeCastWarningLevel = WarningLevel.Off; // py_i64 -> int etc, cosmetic (C4244/C4267).
		CppCompileWarningSettings.UndefinedIdentifierWarningLevel = WarningLevel.Off;
		CppCompileWarningSettings.UnreachableCodeWarningLevel = WarningLevel.Off; // C4702 is an error project-wide; see QuickJSLib.Build.cs's comment for the same fix.

		// A unity build would concatenate this module's translation unit with others; keep it
		// isolated since it is a single, already-amalgamated ~1.3MB translation unit with its
		// own internal static helpers.
		bUseUnity = false;

		// MSVC's static analyzer is not a tool PocketPy is developed against and flags a large
		// number of intentional patterns in the amalgamated interpreter/compiler; analysis of
		// first-party Mantle code is unaffected -- this flag is local to this module only.
		bDisableStaticAnalysis = true;
	}
}
