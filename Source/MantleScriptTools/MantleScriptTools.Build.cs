// Copyright Joshua Eger. All Rights Reserved.
using UnrealBuildTool;

/**
 * The authoring-side script checks, public source in the Fab plugin. Syntax checks and
 * TypeScript conversion over the open-source Luau, QuickJS and PocketPy, the script catalog's file
 * rules (which files are scripts and packages, and how require() resolves), and the RmlUi document
 * text rules. The editor uses this module, never MantleScript or MantleRml; the game's MantleScript
 * and MantleRml use it too, so a place passes the same checks in both.
 */
public class MantleScriptTools : ModuleRules
{
	public MantleScriptTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// The script catalog reads the place format's package manifests (MantlePlaceTypes.h).
			"MantleRuntime"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"LuauLib",
			"QuickJSLib",
			"PocketPyLib"
		});

		// Same imports as MantleScript: Luau and PocketPy's hooks are DLLs in modular builds.
		if (Target.LinkType != TargetLinkType.Monolithic &&
			Target.IsInPlatformGroup(UnrealPlatformGroup.Windows))
		{
			PrivateDefinitions.Add("LUA_API=__declspec(dllimport)");
			PrivateDefinitions.Add("LUACODE_API=__declspec(dllimport)");
			PrivateDefinitions.Add("POCKETPY_HOOKS_API=__declspec(dllimport)");
		}
	}
}
