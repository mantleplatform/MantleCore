// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntimeTextureBuilder.h"

#include "Engine/Texture2D.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "TextureResource.h"

#if WITH_DEV_AUTOMATION_TESTS

// #189: a texture the builder makes is held once, on the GPU. Its mips are marked single use, so the render resource takes the bytes
// instead of copying them and frees them after the upload; before, every mip also stayed in RAM for the texture's life (City Sample
// held about twice its textures' size). The editor keeps its copy, since a property edit or a viewer there recreates the resource.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleRuntimeTextureBuilderTestPrivate
{
	/** A BGRA8 chain from Size x Size down to 1 x 1. Data: no Textures setting drops a mip (ComputeMipsToDrop). */
	FMantleDecodedTexture MakeChain(const int32 Size)
	{
		FMantleDecodedTexture Decoded;
		Decoded.Format = PF_B8G8R8A8;
		Decoded.Role = EMantleTextureRole::Data;
		Decoded.bSRGB = false;
		Decoded.SourceWidth = Size;
		Decoded.SourceHeight = Size;
		for (int32 Edge = Size; Edge >= 1; Edge /= 2)
		{
			FMantleTextureMip& Mip = Decoded.Mips.AddDefaulted_GetRef();
			Mip.Width = Edge;
			Mip.Height = Edge;
			Mip.Bytes.Init(128, FMantleRuntimeTextureBuilder::ComputeMipBytes(PF_B8G8R8A8, Edge, Edge));
		}
		return Decoded;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTextureBuilderFreesCpuMipsTest,
	"Mantle.Runtime.TextureBuilder.FreesCpuMipsAfterUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTextureBuilderFreesCpuMipsTest::RunTest(const FString& Parameters)
{
	using namespace MantleRuntimeTextureBuilderTestPrivate;
	if (IsRunningDedicatedServer())
	{
		AddInfo(TEXT("A dedicated server builds no textures."));
		return true;
	}

	TestEqual(TEXT("the editor keeps its copy by default, the game does not"), FMantleTextureBuildParams().bKeepCpuCopy, GIsEditor);

	FMantleTextureBuildParams Game;
	Game.bKeepCpuCopy = false;
	FMantleTextureBuildParams Editor;
	Editor.bKeepCpuCopy = true;
	FString Error;
	UTexture2D* Freed = FMantleRuntimeTextureBuilder::Build(MakeChain(64), Game, &Error);
	if (!TestNotNull(*FString::Printf(TEXT("the game's texture builds (%s)"), *Error), Freed))
	{
		return false;
	}
	UTexture2D* Kept = FMantleRuntimeTextureBuilder::Build(MakeChain(64), Editor, &Error);
	if (!TestNotNull(*FString::Printf(TEXT("the editor's texture builds (%s)"), *Error), Kept))
	{
		return false;
	}

	const int32 NumMips = FMantleRuntimeTextureBuilder::ComputeFullMipCount(64, 64);
	TestEqual(TEXT("the game's texture has every mip"), Freed->GetPlatformData()->Mips.Num(), NumMips);
	TestEqual(TEXT("the editor's texture has every mip"), Kept->GetPlatformData()->Mips.Num(), NumMips);
	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		TestTrue(*FString::Printf(TEXT("the game's mip %d is single use"), Mip), Freed->GetPlatformData()->Mips[Mip].BulkData.IsSingleUse());
		TestFalse(*FString::Printf(TEXT("the editor's mip %d is not"), Mip), Kept->GetPlatformData()->Mips[Mip].BulkData.IsSingleUse());
	}

	if (!FApp::CanEverRender())
	{
		// UTexture::UpdateResource creates no resource without a renderer, so nothing takes the bytes; the marking is all there is to see.
		AddInfo(TEXT("No renderer (-nullrhi): only the single-use marking is checked."));
		return true;
	}

	// The resource is made in Build (UpdateResource -> CreateResource, on the game thread) and takes the single-use mips there; the
	// upload itself runs on the render thread.
	FlushRenderingCommands();
	TestNotNull(TEXT("the game's texture has a render resource"), Freed->GetResource());
	TestTrue(TEXT("the game's texture is on the GPU"), Freed->GetResource() && Freed->GetResource()->TextureRHI.IsValid());
	TestTrue(TEXT("the editor's texture is on the GPU"), Kept->GetResource() && Kept->GetResource()->TextureRHI.IsValid());
	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		TestFalse(*FString::Printf(TEXT("the game's mip %d is no longer in RAM"), Mip), Freed->GetPlatformData()->Mips[Mip].BulkData.IsBulkDataLoaded());
		TestTrue(*FString::Printf(TEXT("the editor's mip %d still is"), Mip), Kept->GetPlatformData()->Mips[Mip].BulkData.IsBulkDataLoaded());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
