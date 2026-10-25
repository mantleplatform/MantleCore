// Copyright Joshua Eger. All Rights Reserved.
using System.IO;
using UnrealBuildTool;

public class MantleEditor : ModuleRules
{
	public MantleEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"MantleRuntime",
			// The script catalog's file rules (MantlePlaceExporter.h exposes its types), the syntax checks
			// and the RmlUi text rules. Never MantleScript or MantleRml: those run only in the game.
			"MantleScriptTools"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"Slate",
			"SlateCore",
			// Mantle.Dev.ConvertPanelShot renders the Cosmetics panel's Convert section offscreen (FWidgetRenderer).
			"UMG",
			// Creator-declared clothing region default and preview color pickers.
			"AppFramework",
			"UnrealEd",
			"ToolMenus",
			"Json",
			// The exporter reads a static mesh's simple collision (#470): UBodySetupCore lives here.
			"PhysicsCore",
			"JsonUtilities",
			"HTTP",
			"DeveloperSettings",
			"Projects",
			// #563 stage 3: the Mantle Preview panel's Choose client... file picker (IDesktopPlatform::OpenFileDialog).
			"DesktopPlatform",
			// #563 stage 3: the Mantle Preview panel's Copy messages (FPlatformApplicationMisc::ClipboardCopy).
			"ApplicationCore",
			// S1.7's Scripts panel: puts the "Mantle Scripts" nomad tab under the editor's
			// Window menu (WorkspaceMenu::GetMenuStructure().GetToolsCategory()).
			"WorkspaceMenuStructure",
			// S1.19's graph editor: SGraphEditor/UEdGraphSchema pin widgets (GraphEditor -- note
			// SGraphEditor itself is declared in UnrealEd's GraphEditor.h, already a dependency
			// above), the node/My Blueprint/details panels (PropertyEditor), the asset type action
			// and Content Browser "Mantle" category (AssetTools), and locating graph assets under a
			// level's Scripts/ folder for the Scripts panel (AssetRegistry).
			"GraphEditor",
			"PropertyEditor",
			"AssetTools",
			"AssetRegistry",
			// An emote's retarget puts the Content Browser back where the creator had it (IK Rig's batch retarget moves it).
			"ContentBrowser",
			"ContentBrowserData",
			// S1.20: graph runtime errors are emitted by MantleScript as neutral data; the editor
			// projects those into clickable Message Log rows without creating a reverse dependency.
			"MessageLog",
			// A2.4 (#283) cosmetic upload panel. GLTFExporter bakes materials whose textures are not plain
			// parameters (the engine plugin MantleCore.uplugin names, decision 11; shared with the Fab track's
			// F5); ImageCore reads texture source pixels; RenderCore and RHI for the mesh render data the
			// export reads and the null-RHI check before rendering a thumbnail.
			"GLTFExporter",
			// The place-asset tests build static meshes from a MeshDescription (F5 #339);
			// MantleRuntime already links both.
			"MeshDescription",
			"StaticMeshDescription",
			// #610: the avatar converter's Repose and RigMap steps re-bind and re-weight a skeletal mesh's LODs in their
			// mesh descriptions (FSkeletalMeshAttributes; FBoneWeight from AnimationCore). Engine runtime modules, no plugin.
			"SkeletalMeshDescription",
			"AnimationCore",
			"ImageCore",
			// The Library panel reads a card image's dimensions from its header before decoding it.
			"ImageWrapper",
			// Optional native strand-to-card authoring dispatch. The generator implementation is never loaded/enabled here.
			"HairStrandsCore",
			"HairCardGeneratorFramework",
			// Owned native cards graph evaluation; editor-only, no optional generator implementation dependency.
			"DataflowCore",
			"DataflowEngine",
			"RenderCore",
			"RHI",
			// A3.8 (#290): LODs generated at upload with the editor's skeletal mesh reduction
			// (IMeshReduction via FLODUtilities::SimplifySkeletalMeshLOD, run for the running platform).
			// Engine Developer modules the editor already loads; no new plugin or third-party code.
			"MeshReductionInterface",
			// #804: FOverlappingCorners (OverlappingCorners.h), which the static mesh reducer takes beside the mesh
			// description. An engine runtime module, already loaded by the editor; no plugin or third-party code.
			"MeshUtilitiesCommon",
			"SkeletalMeshUtilitiesCommon",
			"TargetPlatform",
			// #610's Atlas fixer: rewrites a skeletal mesh's source geometry (FSkeletalMeshAttributes) and renders any
			// material into an atlas through the editor's material baking (IMaterialBakingModule, what GLTFExporter's
			// bake uses). Engine Developer/Runtime modules the editor already loads; no new plugin. SkeletalMeshDescription
			// is listed above for the Repose fixer.
			"MaterialBaking",
			// #466: publishing a landscape reads its heights and paint weights (ULandscapeInfo,
			// FLandscapeEditDataInterface). An engine runtime module every editor loads, not a plugin;
			// the shipped client never builds this module, so nothing is added to it.
			"Landscape",
			// GhzGarage/mantle#997: the Water plugin (Experimental, off by default) is deliberately NOT a dependency here or in
			// MantleCore.uplugin. MantleWaterBodyExport finds its classes by name through reflection, so a project without the plugin loads Mantle.
			// LandscapeEdit.h includes InstancedFoliageActor.h (engine runtime module).
			"Foliage",
			// #625 (owner, 2026-10-02: the MetaHuman Creator plugin is a normal dependency): Convert makes the avatar a
			// MetaHuman with Epic's own tools (MetaHumanCharacterEditorSubsystem: conform, landmarks, cloud textures and
			// auto-rig, build), its IK Retargeter (the A-pose, IKRigEditor's controllers and batch retarget) and Geometry
			// Script (the strip and the posed mesh, the calls the proven pipeline made). Editor plugins MantleCore.uplugin
			// lists for the Editor only; the shipped client never builds this module.
			"MetaHumanCharacterEditor",
			"MetaHumanCharacter",
			"MetaHumanCharacterPalette",
			"MetaHumanCoreTechLib",
			"MetaHumanSDKEditor",
			"MetaHumanSDKRuntime",
			// Dev-only pre-discard native body evidence reads the attached DNA geometry.
			"RigLogicModule",
			"IKRig",
			"IKRigEditor",
			"GeometryScriptingCore",
			// Step 03 makes the posed static mesh (CreateNewStaticMeshAssetFromMesh); the pose gate reads
			// the A-pose clip with the engine's AnimPose (both editor modules, no new plugin).
			"GeometryScriptingEditor",
			"AnimationBlueprintLibrary",
			"GeometryFramework",
			"GeometryCore",
			// #646: evaluated native Outfit source sizes are editor authoring input only.
			"ChaosClothAssetEngine",
			"ChaosClothAsset",
			"Chaos",
			"ChaosOutfitAssetEngine",
			// Exercise Epic's public runtime RBF API against the transported clothing weights.
			"MeshResizingCore",
			"ChaosClothAssetTools",
			"MetaHumanDefaultPipeline",
			"MetaHumanDefaultEditorPipeline",
			// Public native pipeline headers expose editor utility objects and Dataflow/InstancedStruct types.
			"Blutility"
		});

		// TypeScript is an editor authoring format. Stage the compiler and its notices only
		// with the Editor module; the Game target neither compiles this module nor carries
		// the 9 MiB compiler resource.
		// New Place must work in projects outside the Mantle repository.
		RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "Templates", "new-place.json"), StagedFileType.NonUFS);
		string TypeScriptDir = Path.Combine(PluginDirectory, "Source", "ThirdParty", "TypeScriptCompiler");
		RuntimeDependencies.Add(Path.Combine(TypeScriptDir, "typescript.js"), StagedFileType.NonUFS);
		RuntimeDependencies.Add(Path.Combine(TypeScriptDir, "LICENSE.txt"), StagedFileType.NonUFS);
		RuntimeDependencies.Add(Path.Combine(TypeScriptDir, "ThirdPartyNoticeText.txt"), StagedFileType.NonUFS);
		RuntimeDependencies.Add(Path.Combine(TypeScriptDir, "VERSION"), StagedFileType.NonUFS);
	}
}
