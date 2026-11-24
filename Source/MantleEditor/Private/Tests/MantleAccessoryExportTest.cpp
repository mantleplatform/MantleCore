// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleMeshReduce.h"
#include "MantlePng.h"
#include "MantleThumbnailChoice.h"

#include "MantleEditorActions.h"
#include "MantleToolCall.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// A rigid accessory over the class's triangle limit uploads a reduced copy of itself instead of being refused, and
// one under the limit uploads as it is. Small generated meshes: a wavy disc of quads, 20 cm across, pivot at its center.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAccessoryExportTestPrivate
{
	/** A transient one-material static mesh of Rings x Segments quads over a 10 cm radius disc, gently waved. */
	UStaticMesh* MakeDisc(const int32 Rings, const int32 Segments, UMaterialInterface* Material, const FVector3f Offset = FVector3f::ZeroVector, const float RadiusCm = 10.0f)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(1);
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = FName(TEXT("Surface"));

		TArray<FVertexID> Vertices;
		for (int32 Ring = 0; Ring <= Rings; ++Ring)
		{
			for (int32 Segment = 0; Segment <= Segments; ++Segment)
			{
				const float Radius = RadiusCm * Ring / Rings;
				const float Angle = 2.0f * PI * Segment / Segments;
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = Offset + FVector3f(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), 0.5f * FMath::Sin(Radius) * FMath::Cos(3.0f * Angle));
				Vertices.Add(Vertex);
			}
		}
		auto AddTriangle = [&](int32 A, int32 B, int32 C)
		{
			TArray<FVertexInstanceID> Corners;
			for (const int32 Index : { A, B, C })
			{
				const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertices[Index]);
				const FVector3f Position = Positions[Vertices[Index]];
				Normals[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
				Tangents[Instance] = FVector3f(1.0f, 0.0f, 0.0f);
				BinormalSigns[Instance] = 1.0f;
				Colors[Instance] = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
				UVs.Set(Instance, 0, FVector2f(0.5f + (Position.X - Offset.X) / (2.0f * RadiusCm), 0.5f + (Position.Y - Offset.Y) / (2.0f * RadiusCm)));
				Corners.Add(Instance);
			}
			Description.CreateTriangle(Group, Corners);
		};
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				const int32 V00 = Ring * (Segments + 1) + Segment;
				const int32 V01 = V00 + 1;
				const int32 V10 = V00 + Segments + 1;
				const int32 V11 = V10 + 1;
				AddTriangle(V00, V10, V11);
				AddTriangle(V00, V11, V01);
			}
		}

		UObject* Outer = GetTransientPackage();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MantleAccessoryTestMesh"))), RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, FName(TEXT("Surface")), FName(TEXT("Surface"))));
		Mesh->bAllowCPUAccess = true;
		Mesh->SetNumSourceModels(1);
		FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
		Settings.bRecomputeNormals = false;
		Settings.bRecomputeTangents = false;
		Settings.bGenerateLightmapUVs = false;
		Settings.bBuildReversedIndexBuffer = false;
		Settings.DistanceFieldResolutionScale = 0.0f;
		Settings.MaxLumenMeshCards = 0;
		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bMarkPackageDirty = false;
		Params.bAllowCpuAccess = true;
		Mesh->BuildFromMeshDescriptions({ &Description }, Params);
		return Mesh;
	}

	int32 Triangles(const UStaticMesh& Mesh)
	{
		const FStaticMeshRenderData* Render = Mesh.GetRenderData();
		return Render && Render->LODResources.Num() > 0 ? Render->LODResources[0].GetNumTriangles() : 0;
	}

	bool HasNote(const FMantleCosmeticPackage& Package, const TCHAR* Text)
	{
		return Package.Notes.ContainsByPredicate([Text](const FString& Note) { return Note.Contains(Text); });
	}

	/** A transient square texture of one flat color (a normal map when bNormal). */
	UTexture2D* MakeTexture(const int32 Size, const FColor Color, const bool bNormal)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Color, Size * Size);
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Texture->SRGB = !bNormal;
		if (bNormal)
		{
			Texture->CompressionSettings = TC_Normalmap;
		}
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
		return Texture;
	}

	/** A material with a BaseColor texture parameter and, when NormalSize > 0, a Normal one of that size. */
	UMaterial* MakeMaterial(const int32 NormalSize)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		auto AddSample = [Material](const TCHAR* Name, UTexture2D* Texture, const EMaterialSamplerType Type) -> UMaterialExpressionTextureSampleParameter2D*
		{
			UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
			Sample->Material = Material;
			Sample->ParameterName = Name;
			Sample->Texture = Texture;
			Sample->SamplerType = Type;
			Sample->UpdateParameterGuid(true, false);
			Sample->UpdateMaterialExpressionGuid(true, false);
			Material->GetExpressionCollection().AddExpression(Sample);
			return Sample;
		};
		UMaterialEditorOnlyData* Editor = Material->GetEditorOnlyData();
		Editor->BaseColor.Connect(0, AddSample(TEXT("BaseColor"), MakeTexture(256, FColor(200, 160, 60, 255), false), SAMPLERTYPE_Color));
		if (NormalSize > 0)
		{
			Editor->Normal.Connect(0, AddSample(TEXT("Normal"), MakeTexture(NormalSize, FColor(128, 128, 255, 255), true), SAMPLERTYPE_Normal));
		}
		Material->UpdateCachedExpressionData();
		return Material;
	}

	/** The width of an uploaded texture, or 0 when the package has no such texture. */
	int32 UploadedWidth(const FMantleCosmeticPackage& Package, const FString& Key)
	{
		const FMantleCosmeticUploadAsset* Asset = Package.FindAsset(Key);
		int32 Width = 0, Height = 0;
		bool bAlpha = false;
		return Asset && FMantlePng::ReadHeader(Asset->Bytes, Width, Height, bAlpha) ? Width : 0;
	}

	FMantleCosmeticDraft NeckDraft(UStaticMesh* Mesh)
	{
		FMantleCosmeticDraft Draft;
		Draft.Class = EMantleCosmeticClass::Accessory;
		Draft.Slot = TEXT("acc_neck");
		Draft.Name = TEXT("Test chain");
		Draft.bPublic = false;
		Draft.bRenderThumbnail = false;
		Draft.Source = Mesh;
		return Draft;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryReduceTest, "Mantle.Cosmetics.Accessory.ReduceOverLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessoryReduceTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		AddError(ContractError);
		return false;
	}
	const int32 Limit = Contract->RigidAccessory.MaxTriangles;
	if (!TestTrue(TEXT("rigid accessories have a triangle limit"), Limit > 0))
	{
		return false;
	}
	FMantleMeshReduce::ClearCache();
	ON_SCOPE_EXIT { FMantleMeshReduce::ClearCache(); };
	TStrongObjectPtr<UMaterialInterface> Material(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));

	// Over the limit: 60 x 50 quads, 6,000 triangles against 4,000.
	TStrongObjectPtr<UStaticMesh> Dense(MakeDisc(60, 50, Material.Get()));
	const int32 DenseBefore = Triangles(*Dense);
	TestTrue(TEXT("the dense disc is over the limit"), DenseBefore > Limit);
	const FMantleCosmeticPackage Reduced = FMantleCosmeticExport::BuildCore(NeckDraft(Dense.Get()), *Contract);
	for (const FMantleCosmeticIssue& Issue : Reduced.Issues)
	{
		if (Issue.Severity == EMantleCosmeticSeverity::Error)
		{
			AddError(FString::Printf(TEXT("over-limit accessory: %s"), *Issue.ToString()));
		}
	}
	TestFalse(TEXT("an over-limit accessory exports without errors"), Reduced.HasErrors());
	TestTrue(TEXT("its notes say it was reduced"), HasNote(Reduced, TEXT("reduced from")));
	const FMantleCosmeticUploadAsset* ReducedMesh = Reduced.FindAsset(TEXT("mesh"));
	TestTrue(TEXT("it uploads a mesh"), ReducedMesh && ReducedMesh->Bytes.Num() > 0);
	TestEqual(TEXT("the creator's mesh is untouched"), Triangles(*Dense), DenseBefore);

	// Under the limit: 30 x 30 quads, 1,800 triangles, exported as it is.
	TStrongObjectPtr<UStaticMesh> Light(MakeDisc(30, 30, Material.Get()));
	const FMantleCosmeticPackage Kept = FMantleCosmeticExport::BuildCore(NeckDraft(Light.Get()), *Contract);
	TestFalse(TEXT("an under-limit accessory exports without errors"), Kept.HasErrors());
	TestFalse(TEXT("an under-limit accessory is not reduced"), HasNote(Kept, TEXT("reduced from")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryUnderLimitTest, "Mantle.Cosmetics.Accessory.UnderLimitUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessoryUnderLimitTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		AddError(ContractError);
		return false;
	}
	FMantleMeshReduce::ClearCache();
	ON_SCOPE_EXIT { FMantleMeshReduce::ClearCache(); };
	TStrongObjectPtr<UMaterialInterface> Material(MakeMaterial(0));
	TStrongObjectPtr<UStaticMesh> Light(MakeDisc(30, 30, Material.Get()));
	const int32 Before = Triangles(*Light);
	const FBox BoundsBefore = Light->GetBoundingBox();
	TestTrue(TEXT("the disc is under the limit"), Before > 0 && Before <= Contract->RigidAccessory.MaxTriangles);
	const FMantleCosmeticPackage First = FMantleCosmeticExport::BuildCore(NeckDraft(Light.Get()), *Contract);
	const FMantleCosmeticPackage Second = FMantleCosmeticExport::BuildCore(NeckDraft(Light.Get()), *Contract);
	TestFalse(TEXT("it exports without errors"), First.HasErrors());
	TestFalse(TEXT("it is not reduced"), HasNote(First, TEXT("reduced from")));
	TestFalse(TEXT("its pivot is not moved"), HasNote(First, TEXT("centered on the socket")));
	TestTrue(TEXT("the preview shows the source itself"), FMantleAccessorySettings::UploadMesh(Light.Get(), Contract->RigidAccessory) == Light.Get());
	const FMantleCosmeticUploadAsset* A = First.FindAsset(TEXT("mesh"));
	const FMantleCosmeticUploadAsset* B = Second.FindAsset(TEXT("mesh"));
	if (TestTrue(TEXT("it uploads a mesh"), A && B))
	{
		TestTrue(TEXT("the same mesh uploads each time"), A->Bytes == B->Bytes);
	}
	TestEqual(TEXT("the source's triangles are untouched"), Triangles(*Light), Before);
	TestTrue(TEXT("the source's bounds are untouched"), Light->GetBoundingBox().Equals(BoundsBefore, 1e-4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryFeetPivotTest, "Mantle.Cosmetics.Accessory.PivotAtFeetExports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessoryFeetPivotTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		AddError(ContractError);
		return false;
	}
	FMantleMeshReduce::ClearCache();
	ON_SCOPE_EXIT { FMantleMeshReduce::ClearCache(); };
	TStrongObjectPtr<UMaterialInterface> Material(MakeMaterial(0));

	// A 20 cm item modeled 150 cm up from a pivot at the feet: out of reach of the neck, small enough for it.
	TStrongObjectPtr<UStaticMesh> Chain(MakeDisc(30, 30, Material.Get(), FVector3f(0.0f, 0.0f, 150.0f)));
	const FBox BoundsBefore = Chain->GetBoundingBox();
	const FMantleCosmeticPackage Package = FMantleCosmeticExport::BuildCore(NeckDraft(Chain.Get()), *Contract);
	for (const FMantleCosmeticIssue& Issue : Package.Issues)
	{
		if (Issue.Severity == EMantleCosmeticSeverity::Error)
		{
			AddError(FString::Printf(TEXT("pivot at the feet: %s"), *Issue.ToString()));
		}
	}
	TestFalse(TEXT("an accessory with its pivot at the feet exports for acc_neck"), Package.HasErrors());
	TestTrue(TEXT("its notes say it was centered"), HasNote(Package, TEXT("centered on the socket")));
	TestTrue(TEXT("the source's bounds are untouched"), Chain->GetBoundingBox().Equals(BoundsBefore, 1e-4));

	// Too big for the slot either way: refused and not moved. The rule itself, on boxes: 40 cm centers (its farthest
	// corner then reaches 34.6 of 35 cm), a 45 cm cube does not (39), nor does a flat 60 cm one (over the size limit).
	TStrongObjectPtr<UStaticMesh> Huge(MakeDisc(30, 30, Material.Get(), FVector3f(0.0f, 0.0f, 150.0f), 40.0f));
	const FMantleCosmeticPackage Refused = FMantleCosmeticExport::BuildCore(NeckDraft(Huge.Get()), *Contract);
	TestTrue(TEXT("an accessory too big for the slot is still refused"), Refused.HasErrors());
	TestFalse(TEXT("and not moved"), HasNote(Refused, TEXT("centered on the socket")));
	const FMantleContractAccessoryBounds* Neck = Contract->AccessoryBounds.Find(TEXT("acc_neck"));
	if (TestNotNull(TEXT("acc_neck has bounds"), Neck))
	{
		auto Box = [](const float Side, const float Height) { return FBox3f(FVector3f(-Side / 2, -Side / 2, 150.0f), FVector3f(Side / 2, Side / 2, 150.0f + Height)); };
		TestFalse(TEXT("a 40 cm cube far from its pivot is centered"), FMantleAccessorySettings::RecenterOffset(Box(40.0f, 40.0f), *Neck).IsZero());
		TestTrue(TEXT("a 45 cm cube would still reach too far when centered, so it is not moved"), FMantleAccessorySettings::RecenterOffset(Box(45.0f, 45.0f), *Neck).IsZero());
		TestTrue(TEXT("a 60 cm item is too big, so it is not moved"), FMantleAccessorySettings::RecenterOffset(Box(60.0f, 2.0f), *Neck).IsZero());
		TestTrue(TEXT("an item whose pivot already reaches is not moved"), FMantleAccessorySettings::RecenterOffset(Box(10.0f, 10.0f).ShiftBy(FVector3f(0.0f, 0.0f, -150.0f)), *Neck).IsZero());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryNormalMapTest, "Mantle.Cosmetics.Accessory.NormalMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessoryNormalMapTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		AddError(ContractError);
		return false;
	}
	if (!TestEqual(TEXT("rigid accessories take a 1024 normal map"), Contract->RigidAccessory.NormalMax, 1024))
	{
		return false;
	}
	FMantleMeshReduce::ClearCache();
	ON_SCOPE_EXIT { FMantleMeshReduce::ClearCache(); };

	auto Export = [&](const int32 NormalSize, FMantleCosmeticPackage& Out) -> bool
	{
		TStrongObjectPtr<UMaterialInterface> Material(MakeMaterial(NormalSize));
		TStrongObjectPtr<UStaticMesh> Mesh(MakeDisc(30, 30, Material.Get()));
		Out = FMantleCosmeticExport::BuildCore(NeckDraft(Mesh.Get()), *Contract);
		return !Out.HasErrors() && Out.Materials.Num() == 1;
	};

	FMantleCosmeticPackage With, Without, Over;
	if (TestTrue(TEXT("an accessory with a 1024 normal map exports"), Export(1024, With)))
	{
		TestEqual(TEXT("the normal uploads at 1024"), UploadedWidth(With, With.Materials[0].NormalKey), 1024);
	}
	if (TestTrue(TEXT("an accessory with no normal map exports"), Export(0, Without)))
	{
		TestTrue(TEXT("and has no normal"), Without.Materials[0].NormalKey.IsEmpty());
		TestTrue(TEXT("its color uploads as before"), UploadedWidth(Without, Without.Materials[0].AlbedoKey) > 0);
		if (With.Materials.Num() == 1)
		{
			const FMantleCosmeticUploadAsset* AlbedoWith = With.FindAsset(With.Materials[0].AlbedoKey);
			const FMantleCosmeticUploadAsset* AlbedoWithout = Without.FindAsset(Without.Materials[0].AlbedoKey);
			if (TestTrue(TEXT("both upload a color texture"), AlbedoWith && AlbedoWithout))
			{
				TestTrue(TEXT("its color texture is the same with or without a normal"), AlbedoWith->Bytes == AlbedoWithout->Bytes);
			}
		}
	}
	if (TestTrue(TEXT("an accessory with a 2048 normal map exports, not refused"), Export(2048, Over)))
	{
		TestEqual(TEXT("the normal is resized to 1024"), UploadedWidth(Over, Over.Materials[0].NormalKey), 1024);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryCustomThumbnailTest, "Mantle.Cosmetics.Accessory.CustomThumbnail",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// #124: a custom thumbnail image goes out as the item's 512 x 512 thumbnail, and one that cannot be used refuses the export with a reason.
bool FMantleAccessoryCustomThumbnailTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		AddError(ContractError);
		return false;
	}
	TStrongObjectPtr<UMaterialInterface> Material(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
	TStrongObjectPtr<UStaticMesh> Light(MakeDisc(30, 30, Material.Get()));

	// A 600 x 300 picture: cut to the middle 300 x 300 and enlarged to 512 x 512.
	TArray<FColor> Pixels;
	Pixels.Init(FColor(40, 160, 220, 255), 600 * 300);
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleCustomThumbnail.png"));
	FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(600, 300, Pixels), *File);

	FMantleCosmeticDraft Draft = NeckDraft(Light.Get());
	Draft.Thumbnail.Mode = EMantleThumbnailMode::Custom;
	Draft.Thumbnail.ImageFile.FilePath = File;
	const FMantleCosmeticPackage Package = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	TestFalse(TEXT("a custom thumbnail exports without errors"), Package.HasErrors());
	TestEqual(TEXT("the thumbnail is 512 wide"), UploadedWidth(Package, TEXT("thumbnail")), 512);
	TestTrue(TEXT("the notes say it is the custom image"), HasNote(Package, TEXT("custom image")));

	// A picture that is not there refuses the export and says why.
	Draft.Thumbnail.ImageFile.FilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MantleNoSuchThumbnail.png"));
	const FMantleCosmeticPackage Refused = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	TestTrue(TEXT("a missing custom picture is an error"), Refused.HasErrors());
	return true;
}

// The agent tools take a plain static mesh in an accessory slot (a necklace) through the panel's own Accessory export, and
// still refuse a static mesh in a clothing slot as hair cards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryAgentTest, "Mantle.Cosmetics.Accessory.AgentTakesStaticMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessoryAgentTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	TStrongObjectPtr<UMaterialInterface> Material(MakeMaterial(0));
	TStrongObjectPtr<UStaticMesh> Light(MakeDisc(30, 30, Material.Get()));
	auto Check = [this, &Light](const FString& Slot) -> TSharedPtr<FJsonObject>
	{
		const FString Json = MantleToolCall::CallTool(TEXT("CheckCosmetic"), { Light->GetPathName(), Slot, FString(), FString(), FString() });
		TSharedPtr<FJsonObject> Out;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Out);
		TestTrue(FString::Printf(TEXT("%s: the agent answered (%s)"), *Slot, *Json.Left(300)), Out.IsValid());
		return Out;
	};
	if (const TSharedPtr<FJsonObject> Neck = Check(TEXT("acc_neck")))
	{
		TestTrue(FString::Printf(TEXT("a static mesh in acc_neck is checked, not refused (%s)"), *Neck->GetStringField(TEXT("refused"))), Neck->GetBoolField(TEXT("ok")));
	}
	// The placement the panel opens with: the mesh's axes aligned to the avatar's, so a necklace is not uploaded sideways.
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		const FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Accessory, Light.Get(), TEXT("acc_neck"), FString(), *Contract, EMantleFitSex::Auto);
		TestTrue(TEXT("a static mesh accessory starts aligned to the avatar's axes"), Draft.bAttach);
		TestTrue(TEXT("a neck accessory starts lowered onto the shoulders, not centered on the neck socket"), !Draft.AttachLocation.IsNearlyZero(0.5f));
		const FMantleCosmeticDraft Wrist = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Accessory, Light.Get(), TEXT("acc_face"), FString(), *Contract, EMantleFitSex::Auto);
		TestTrue(TEXT("another slot is only aligned, not lowered"), Wrist.AttachLocation.IsNearlyZero(0.01f));
	}
	if (const TSharedPtr<FJsonObject> Top = Check(TEXT("top")))
	{
		TestFalse(TEXT("a static mesh in a clothing slot is refused"), Top->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("as cards_not_hair"), Top->GetStringField(TEXT("refused")), FString(TEXT("cards_not_hair")));
	}
	return true;
}

// A necklace ring around a neck-like cylinder falls until it meets the shoulders and stops at the surface (with clearance),
// even when the skin is a few big triangles whose corners are far from any link.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleShoulderRestTest, "Mantle.Cosmetics.Accessory.RestOnShoulders",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleShoulderRestTest::RunTest(const FString& Parameters)
{
	// Shoulders: a flat annulus at z = 0 between radius 5 (the neck) and 40, as 24 wide triangles pairs (corners 35 cm apart).
	TArray<FVector> Skin;
	constexpr int32 Segments = 24;
	for (int32 Index = 0; Index < Segments; ++Index)
	{
		const double A0 = 2.0 * PI * Index / Segments, A1 = 2.0 * PI * (Index + 1) / Segments;
		const FVector In0(5.0 * FMath::Cos(A0), 5.0 * FMath::Sin(A0), 0.0), In1(5.0 * FMath::Cos(A1), 5.0 * FMath::Sin(A1), 0.0);
		const FVector Out0(40.0 * FMath::Cos(A0), 40.0 * FMath::Sin(A0), 0.0), Out1(40.0 * FMath::Cos(A1), 40.0 * FMath::Sin(A1), 0.0);
		Skin.Append({ In0, Out0, Out1, In0, Out1, In1 });
		// The neck's wall: vertical triangles a ring outside it never meets.
		const FVector Up0(In0.X, In0.Y, 20.0), Up1(In1.X, In1.Y, 20.0);
		Skin.Append({ In0, In1, Up1, In0, Up1, Up0 });
	}
	// A ring of 7 cm radius, 10 cm above the shoulders, around the neck.
	TArray<FVector> Ring;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		const double Angle = 2.0 * PI * Index / 200;
		Ring.Add(FVector(7.0 * FMath::Cos(Angle), 7.0 * FMath::Sin(Angle), 10.0));
	}
	const double Drop = FMantleAccessorySettings::DropOntoSurface(Ring, Skin, 0.15, 30.0);
	TestTrue(TEXT("the ring finds the shoulders"), Drop >= 0.0);
	TestEqual(TEXT("it stops 0.15 cm above the surface, neither floating nor inside"), Drop, 10.0 - 0.15, 0.001);
	// With nothing beneath it, it does not move.
	TestTrue(TEXT("no surface below: no answer"), FMantleAccessorySettings::DropOntoSurface(Ring, TArray<FVector>(), 0.15, 30.0) < 0.0);
	// Too far above the surface for the limit: no answer.
	TestTrue(TEXT("beyond the limit: no answer"), FMantleAccessorySettings::DropOntoSurface(Ring, Skin, 0.15, 5.0) < 0.0);
	return true;
}

// The small slots each export a mesh sized for them, and each reduces to its own triangle cap (a nose stud's is not a hat's).
namespace MantleAccessoryExportTestPrivate
{
	struct FSmallSlot { const TCHAR* Slot; float RadiusCm; int32 Cap; };

	/** Each small slot with a disc that fits its box (limits.json boundsPerSlot) and the slot's triangle cap. */
	const FSmallSlot SmallSlots[] = { { TEXT("acc_eyes"), 10.0f, 3000 }, { TEXT("acc_ear_l"), 5.0f, 1500 }, { TEXT("acc_ear_r"), 5.0f, 1500 },
		{ TEXT("acc_nose"), 2.0f, 800 }, { TEXT("acc_wrist_l"), 6.0f, 2000 }, { TEXT("acc_wrist_r"), 6.0f, 2000 }, { TEXT("acc_ring"), 2.5f, 1000 } };

	FMantleCosmeticDraft SlotDraft(UStaticMesh* Mesh, const TCHAR* Slot)
	{
		FMantleCosmeticDraft Draft = NeckDraft(Mesh);
		Draft.Slot = Slot;
		return Draft;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessorySmallSlotsExportTest, "Mantle.Cosmetics.Accessory.SmallSlotsExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessorySmallSlotsExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract))
	{
		AddError(ContractError);
		return false;
	}
	FMantleMeshReduce::ClearCache();
	ON_SCOPE_EXIT { FMantleMeshReduce::ClearCache(); };
	TStrongObjectPtr<UMaterialInterface> Material(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
	for (const FSmallSlot& Entry : SmallSlots)
	{
		const FString Slot(Entry.Slot);
		TestEqual(FString::Printf(TEXT("%s: the contract's triangle cap"), *Slot), Contract->AccessoryMaxTriangles(Slot), Entry.Cap);
		TestEqual(FString::Printf(TEXT("%s: its class carries the cap"), *Slot), Contract->AccessoryClassFor(Slot).MaxTriangles, Entry.Cap);
		// Under the cap, sized for the slot: 10 x 10 quads, 200 triangles. Exported as it is.
		TStrongObjectPtr<UStaticMesh> Fits(MakeDisc(10, 10, Material.Get(), FVector3f::ZeroVector, Entry.RadiusCm));
		const FMantleCosmeticPackage Kept = FMantleCosmeticExport::BuildCore(SlotDraft(Fits.Get(), Entry.Slot), *Contract);
		for (const FMantleCosmeticIssue& Issue : Kept.Issues)
		{
			if (Issue.Severity == EMantleCosmeticSeverity::Error) AddError(FString::Printf(TEXT("%s: %s"), *Slot, *Issue.ToString()));
		}
		TestFalse(FString::Printf(TEXT("%s: a mesh sized for the slot exports"), *Slot), Kept.HasErrors());
		TestFalse(FString::Printf(TEXT("%s: under the cap it is not reduced"), *Slot), HasNote(Kept, TEXT("reduced from")));
		// Over the cap: a 40 x 40 grid of quads is 3,200 triangles, over every small slot's cap. It is reduced and the note names the real cap.
		TStrongObjectPtr<UStaticMesh> Dense(MakeDisc(40, 40, Material.Get(), FVector3f::ZeroVector, Entry.RadiusCm));
		TestTrue(FString::Printf(TEXT("%s: the dense mesh is over the cap"), *Slot), Triangles(*Dense) > Entry.Cap);
		const FMantleCosmeticPackage Reduced = FMantleCosmeticExport::BuildCore(SlotDraft(Dense.Get(), Entry.Slot), *Contract);
		TestFalse(FString::Printf(TEXT("%s: an over-cap mesh exports"), *Slot), Reduced.HasErrors());
		TestTrue(FString::Printf(TEXT("%s: the note names the %d limit"), *Slot, Entry.Cap), HasNote(Reduced, *FString::Printf(TEXT("to fit the %d limit"), Entry.Cap)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryPerSlotCapTest, "Mantle.Cosmetics.Accessory.PerSlotTriangleCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessoryPerSlotCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!TestNotNull(TEXT("the avatar contract loads"), Contract)) return false;
	TestEqual(TEXT("a slot without its own cap keeps the class limit"), Contract->AccessoryMaxTriangles(TEXT("acc_hat")), 4000);
	FMantleMeshReduce::ClearCache();
	ON_SCOPE_EXIT { FMantleMeshReduce::ClearCache(); };
	TStrongObjectPtr<UMaterialInterface> Material(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
	// One source mesh, 6,000 triangles, uploaded as a nose stud and as a hat: the reduced copy made for one is never reused for the other.
	TStrongObjectPtr<UStaticMesh> Dense(MakeDisc(60, 50, Material.Get()));
	const int32 Before = Triangles(*Dense);
	const UStaticMesh* AsNose = FMantleAccessorySettings::UploadMesh(Dense.Get(), Contract->AccessoryClassFor(TEXT("acc_nose")));
	const UStaticMesh* AsHat = FMantleAccessorySettings::UploadMesh(Dense.Get(), Contract->AccessoryClassFor(TEXT("acc_hat")));
	TestTrue(TEXT("a nose stud is reduced to 800 or fewer"), AsNose && Triangles(*AsNose) > 0 && Triangles(*AsNose) <= 800);
	TestTrue(TEXT("a hat keeps up to 4000, not the nose's 800"), AsHat && Triangles(*AsHat) > 2000 && Triangles(*AsHat) <= 4000);
	TestTrue(TEXT("they are different copies"), AsNose != AsHat);
	TestEqual(TEXT("the creator's mesh is untouched"), Triangles(*Dense), Before);
	// About 1,200 triangles (the disc's center ring folds to a point and its 20 empty triangles are dropped): over the nose's cap, under the hat's.
	TStrongObjectPtr<UStaticMesh> Middle(MakeDisc(30, 20, Material.Get(), FVector3f::ZeroVector, 2.0f));
	const int32 MiddleTriangles = Triangles(*Middle);
	TestTrue(FString::Printf(TEXT("the middle mesh (%d triangles) is over the nose's 800 and under the hat's 4000"), MiddleTriangles), MiddleTriangles > 800 && MiddleTriangles <= 4000);
	const UStaticMesh* MiddleNose = FMantleAccessorySettings::UploadMesh(Middle.Get(), Contract->AccessoryClassFor(TEXT("acc_nose")));
	TestTrue(TEXT("over the nose cap it is reduced to 800 or fewer"), MiddleNose != Middle.Get() && Triangles(*MiddleNose) <= 800);
	TestTrue(TEXT("under the hat cap it uploads as it is"), FMantleAccessorySettings::UploadMesh(Middle.Get(), Contract->AccessoryClassFor(TEXT("acc_hat"))) == Middle.Get());
	return true;
}

// Where a small accessory is seated by default: its bounds' center on the socket (ring, wrist, nose, glasses), an earring's hook on it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleAccessoryAnchorTest, "Mantle.Cosmetics.Accessory.SmallSlotPlacementAnchor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMantleAccessoryAnchorTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryExportTestPrivate;
	for (const TCHAR* Slot : { TEXT("acc_ring"), TEXT("acc_wrist_l"), TEXT("acc_wrist_r"), TEXT("acc_nose"), TEXT("acc_eyes") })
	{
		TestTrue(FString::Printf(TEXT("%s is seated by its center"), Slot), FMantleAccessorySettings::DefaultAnchor(Slot) == EMantleAccessoryAnchor::Center);
	}
	for (const TCHAR* Slot : { TEXT("acc_ear_l"), TEXT("acc_ear_r") })
	{
		TestTrue(FString::Printf(TEXT("%s is seated by its hook (the top)"), Slot), FMantleAccessorySettings::DefaultAnchor(Slot) == EMantleAccessoryAnchor::TopCenter);
	}
	for (const TCHAR* Slot : { TEXT("acc_hat"), TEXT("acc_face"), TEXT("acc_neck"), TEXT("acc_shoulder_l"), TEXT("acc_front"), TEXT("acc_back"), TEXT("acc_waist") })
	{
		TestTrue(FString::Printf(TEXT("%s keeps its pivot on the socket"), Slot), FMantleAccessorySettings::DefaultAnchor(Slot) == EMantleAccessoryAnchor::Pivot);
	}
	const FBox3f Box(FVector3f(0.0f, 0.0f, 0.0f), FVector3f(2.0f, 4.0f, 6.0f));
	TestTrue(TEXT("the center anchor is the box's center"), FMantleAccessorySettings::AnchorPoint(EMantleAccessoryAnchor::Center, Box).Equals(FVector3f(1.0f, 2.0f, 3.0f), 1e-4f));
	TestTrue(TEXT("the top anchor is the top face's center"), FMantleAccessorySettings::AnchorPoint(EMantleAccessoryAnchor::TopCenter, Box).Equals(FVector3f(1.0f, 2.0f, 6.0f), 1e-4f));

	// Seating a mesh made 6 cm above its pivot: the attach location brings the anchor back onto the socket (rotation zero, scale 1).
	TStrongObjectPtr<UMaterialInterface> Material(LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")));
	for (const TCHAR* Slot : { TEXT("acc_eyes"), TEXT("acc_ear_l") })
	{
		TStrongObjectPtr<UStaticMesh> Raised(MakeDisc(10, 10, Material.Get(), FVector3f(0.0f, 0.0f, 6.0f), 3.0f));
		FMantleCosmeticDraft Draft = SlotDraft(Raised.Get(), Slot);
		Draft.AttachRotation = FVector3f::ZeroVector;
		Draft.AttachScale = FVector3f::OneVector;
		FString Error;
		const EMantleAccessoryAnchor Anchor = FMantleAccessorySettings::DefaultAnchor(Slot);
		TestTrue(FString::Printf(TEXT("%s: it is seated (%s)"), Slot, *Error), FMantleAccessorySettings::SeatOnSocket(Draft, Anchor, Error));
		const FVector3f Expected = -FMantleAccessorySettings::AnchorPoint(Anchor, FBox3f(Raised->GetBoundingBox()));
		TestTrue(FString::Printf(TEXT("%s: the anchor lands on the socket (offset %s, wanted %s)"), Slot, *Draft.AttachLocation.ToString(), *Expected.ToString()),
			Draft.AttachLocation.Equals(Expected, 0.01f));
		TestTrue(FString::Printf(TEXT("%s: and is within the panel's range"), Slot), Draft.AttachLocation.GetAbsMax() <= 50.0f);
		TestTrue(FString::Printf(TEXT("%s: the placement is on"), Slot), Draft.bAttach);
	}
	return true;
}

#endif
