// Copyright Joshua Eger. All Rights Reserved.
using UnrealBuildTool;

// #434: the Mantle toolset for AI agents, served by Unreal's own MCP server through the engine's
// experimental Toolset Registry plugin. A module of its own because a toolset must derive from
// UToolsetDefinition, which links ToolsetRegistry: MantleEditor never does, so MantleCore loads
// exactly as before in a project without that plugin. MantleCore.uplugin lists ToolsetRegistry
// as an optional, disabled dependency (enabling it stays the project's choice) and this module
// with LoadingPhase None; MantleEditor loads it after engine init only when the plugin is enabled.
public class MantleAgentTools : ModuleRules
{
	public MantleAgentTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Json",
			"MantleRuntime",
			"MantleEditor",
			"MantleScriptTools",
			"ToolsetRegistry"
		});
	}
}
