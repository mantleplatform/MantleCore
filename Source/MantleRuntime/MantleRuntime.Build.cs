// Copyright Joshua Eger. All Rights Reserved.
using UnrealBuildTool;

public class MantleRuntime : ModuleRules
{
	public MantleRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		// The game builds its own copy of this module (MantleGameRuntime), which sets this to 1.
		PrivateDefinitions.Add("MANTLE_RUNTIME_IN_GAME=0");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json",
			"JsonUtilities",

			// The vehicle chassis reads its presets from the plugin's Resources directory (IPluginManager).
			"Projects",

			// AMantlePhysicsProp's replicated entity id: DOREPLIFETIME's condition types.
			"NetCore",

			// The physics presets' UPhysicalMaterial, which the Engine module itself uses. An
			// engine runtime module.
			"PhysicsCore",

			// UMantleSoundWave derives from USoundWaveProcedural, whose audio proxy factory
			// (IAudioProxyDataFactory) is defined in this engine runtime module.
			"AudioExtensions",

			// The API client lives here rather than in the editor module: the shipped
			// client has to fetch places too, and two implementations of how we talk to
			// our own backend would drift apart.
			"HTTP",

			// The runtime skeletal mesh builder fills render data (vertex, skin weight and
			// index buffers) directly, whose types live in these two engine modules.
			"RenderCore",
			"RHI",

			// The runtime texture builder: PNG/JPEG
			// decoding and FImage format conversion. Engine runtime modules the Engine module
			// itself uses; no third-party code is added.
			"ImageWrapper",
			"ImageCore",

			// The runtime static mesh builder: the mesh description that
			// UStaticMesh::BuildFromMeshDescriptions takes. Engine runtime modules.
			"MeshDescription",
			"StaticMeshDescription",

			// Eigen (the engine's header-only third-party module) supplies the SVD behind the
			// garment fitter's RBF warp, as the engine's MeshResizing code did; that code is
			// vendored in MantleRBFInterpolation.cpp.
			"Eigen"
		});
	}
}
