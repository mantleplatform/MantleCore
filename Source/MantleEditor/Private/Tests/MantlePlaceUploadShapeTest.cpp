// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantleMeshLumenData.h"
#include "MantleNaniteSideFile.h"
#include "MantleNaniteTestSupport.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceMaterial.h"
#include "MantlePlacePublishSession.h"
#include "MantlePlaceUploadShape.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceUploadShapeTestPrivate
{
	FMantleAssetRef Entry(const FString& Id, const EMantleAssetKind Kind)
	{
		FMantleAssetRef Ref;
		Ref.Id = Id;
		Ref.Kind = Kind;
		Ref.Hash = TEXT("md5:") + FString::ChrN(32, TEXT('0'));
		Ref.Bytes = 1234;
		return Ref;
	}

	int32 CountKind(const TArray<FMantleAssetRef>& Assets, const EMantleAssetKind Kind)
	{
		return Assets.FilterByPredicate([Kind](const FMantleAssetRef& Ref) { return Ref.Kind == Kind; }).Num();
	}

	/** A closed 1 m box on one slot drawn with Material: solid, so it gets a distance field, and with it a Lumen file. */
	UStaticMesh* MakeBox(const TCHAR* Name, UMaterialInterface* Material)
	{
		UPackage* Transient = GetTransientPackage();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Transient, MakeUniqueObjectName(Transient, UStaticMesh::StaticClass(), Name), RF_Transient);
		Mesh->SetNumSourceModels(1);
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		Attributes.GetVertexInstanceUVs().SetNumChannels(1);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, TEXT("Slot0"), TEXT("Slot0")));
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Slot0");
		// Each face its own four corners, so each keeps its own normal.
		const FVector3f Axes[3] = { FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1) };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (const float Sign : { -1.f, 1.f })
			{
				const FVector3f Normal = Axes[Axis] * Sign;
				const FVector3f U = Axes[(Axis + 1) % 3];
				const FVector3f V = FVector3f::CrossProduct(Normal, U);
				const FVector3f Offsets[4] = { -U - V, U - V, U + V, -U + V };
				const FVector2f UVs[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
				FVertexInstanceID Corners[4];
				for (int32 C = 0; C < 4; ++C)
				{
					const FVertexID Vertex = Description.CreateVertex();
					Attributes.GetVertexPositions()[Vertex] = (Normal + Offsets[C]) * 50.f;
					Corners[C] = Description.CreateVertexInstance(Vertex);
					Attributes.GetVertexInstanceNormals()[Corners[C]] = Normal;
					Attributes.GetVertexInstanceTangents()[Corners[C]] = U;
					Attributes.GetVertexInstanceBinormalSigns()[Corners[C]] = 1.f;
					Attributes.GetVertexInstanceUVs().Set(Corners[C], 0, UVs[C]);
				}
				Description.CreateTriangle(Group, { Corners[0], Corners[1], Corners[2] });
				Description.CreateTriangle(Group, { Corners[0], Corners[2], Corners[3] });
			}
		}
		Mesh->CreateMeshDescription(0, MoveTemp(Description));
		Mesh->CommitMeshDescription(0);
		Mesh->GetSectionInfoMap().Set(0, 0, FMeshSectionInfo(0));
		Mesh->GetOriginalSectionInfoMap().CopyFrom(Mesh->GetSectionInfoMap());
		Mesh->Build(/*bInSilent*/ true);
		FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });
		return Mesh;
	}

	UMaterial* MakeMaterial(const TCHAR* Name, const EBlendMode Blend)
	{
		UPackage* Transient = GetTransientPackage();
		UMaterial* Material = NewObject<UMaterial>(Transient, MakeUniqueObjectName(Transient, UMaterial::StaticClass(), Name), RF_Transient);
		Material->BlendMode = Blend;
		return Material;
	}

	/** A 4 x 4 PNG of one color. */
	TArray<uint8> Png(const FColor Color)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TArray<uint8> Pixels;
		for (int32 Pixel = 0; Pixel < 16; ++Pixel)
		{
			Pixels.Append({ Color.R, Color.G, Color.B, 255 });
		}
		const TSharedPtr<IImageWrapper> Writer = Module.CreateImageWrapper(EImageFormat::PNG);
		Writer->SetRaw(Pixels.GetData(), Pixels.Num(), 4, 4, ERGBFormat::RGBA, 8);
		const TArray64<uint8> Encoded = Writer->GetCompressed();
		return TArray<uint8>(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
	}

	/**
	 * The material bake, scripted: every material bakes to a base color, a metallic-roughness, a normal and an emissive image, colored
	 * by the material's name so no two materials share one. Four textures per material, as many as a document has slots.
	 */
	class FFourImageFlattener : public IMantleMaterialFlattener
	{
	public:
		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			const uint32 Seed = GetTypeHash(Material ? Material->GetName() : FString());
			OutDocument = FMantleGlbDocument();
			OutDocument.Json = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Images;
			TArray<TSharedPtr<FJsonValue>> Textures;
			TArray<TSharedPtr<FJsonValue>> Views;
			for (int32 Image = 0; Image < 4; ++Image)
			{
				const TArray<uint8> Bytes = Png(FColor(static_cast<uint8>(Seed >> (Image * 8)), static_cast<uint8>(40 + Image * 50), static_cast<uint8>(Seed), 255));
				const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
				View->SetNumberField(TEXT("buffer"), 0);
				View->SetNumberField(TEXT("byteOffset"), OutDocument.Bin.Num());
				View->SetNumberField(TEXT("byteLength"), Bytes.Num());
				OutDocument.Bin.Append(Bytes);
				Views.Add(MakeShared<FJsonValueObject>(View));
				const TSharedRef<FJsonObject> ImageObject = MakeShared<FJsonObject>();
				ImageObject->SetNumberField(TEXT("bufferView"), Image);
				ImageObject->SetStringField(TEXT("mimeType"), TEXT("image/png"));
				ImageObject->SetStringField(TEXT("name"), FString::Printf(TEXT("Baked%d"), Image));
				Images.Add(MakeShared<FJsonValueObject>(ImageObject));
				const TSharedRef<FJsonObject> Texture = MakeShared<FJsonObject>();
				Texture->SetNumberField(TEXT("source"), Image);
				Textures.Add(MakeShared<FJsonValueObject>(Texture));
			}
			auto Info = [](const int32 Index)
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetNumberField(TEXT("index"), Index);
				return Object;
			};
			const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
			Pbr->SetObjectField(TEXT("baseColorTexture"), Info(0));
			Pbr->SetObjectField(TEXT("metallicRoughnessTexture"), Info(1));
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("alphaMode"), Material && IsTranslucentBlendMode(Material->GetBlendMode()) ? TEXT("BLEND") : TEXT("OPAQUE"));
			Entry->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
			Entry->SetObjectField(TEXT("normalTexture"), Info(2));
			Entry->SetObjectField(TEXT("emissiveTexture"), Info(3));
			TArray<TSharedPtr<FJsonValue>> Emissive;
			for (const double Channel : { 1.0, 1.0, 1.0 })
			{
				Emissive.Add(MakeShared<FJsonValueNumber>(Channel));
			}
			Entry->SetArrayField(TEXT("emissiveFactor"), Emissive);
			TArray<TSharedPtr<FJsonValue>> Materials;
			Materials.Add(MakeShared<FJsonValueObject>(Entry));
			OutDocument.Json->SetArrayField(TEXT("materials"), Materials);
			OutDocument.Json->SetArrayField(TEXT("images"), Images);
			OutDocument.Json->SetArrayField(TEXT("textures"), Textures);
			OutDocument.Json->SetArrayField(TEXT("bufferViews"), Views);
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted four-image bake"); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceUploadShapeProjectTest,
	"Mantle.Editor.PlaceSize.UploadShapeListsWhatTheUploadAdds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceUploadShapeProjectTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceUploadShapeTestPrivate;

	const FString DrawnKey = TEXT("mesh:/Game/Drawn.Drawn");
	const FString CollisionKey = TEXT("mesh:/Game/Wall.Wall");
	const FString GlassKey = TEXT("mesh:/Game/Glass.Glass");
	const FString MaterialKey = TEXT("material:/Game/M_Brick.M_Brick");
	const FString UploadedId = TEXT("ast_u0123456789abcdef0123456789abcdef");
	TArray<FMantleAssetRef> Assets;
	Assets.Add(Entry(TEXT("lib_stone"), EMantleAssetKind::Material));
	Assets.Add(Entry(UploadedId, EMantleAssetKind::StaticMesh));
	TMap<FString, FString> Keys;
	for (const FString& Key : { DrawnKey, CollisionKey, GlassKey })
	{
		Assets.Add(Entry(FMantlePlaceAssets::ProvisionalId(Key), EMantleAssetKind::StaticMesh));
		Keys.Add(Assets.Last().Id, Key);
	}
	Assets.Add(Entry(FMantlePlaceAssets::ProvisionalId(MaterialKey), EMantleAssetKind::Material));
	Keys.Add(Assets.Last().Id, MaterialKey);

	FMantlePlaceAssetLimits Limits;
	int32 Asked = 0;
	const FMantlePlaceUploadShape Shape = FMantlePlaceUploadShape::Project(Assets, Keys, nullptr, Limits, EMantleUploadShapeBound::Worst,
		[&](const FMantleAssetRef& Mesh, const FString& Key)
		{
			++Asked;
			FMantleExpectedSideFiles Expected;
			Expected.bLumen = Key != CollisionKey;
			Expected.bNanite = Key == DrawnKey;
			return Expected;
		});
	TestEqual(TEXT("asked once for each mesh not uploaded"), Asked, 3);
	TestEqual(TEXT("two Lumen files: the drawn mesh's and the glass's"), Shape.AddedLumen, 2);
	TestEqual(TEXT("one Nanite file: the drawn mesh's"), Shape.AddedNanite, 1);
	TestEqual(TEXT("four textures for the material"), Shape.AddedTextures, MantlePlaceTextureSlotCount);
	TestEqual(TEXT("every entry kept, the added ones beside them"), Shape.Assets.Num(), Assets.Num() + Shape.NumAdded());
	TestEqual(TEXT("Lumen entries"), CountKind(Shape.Assets, EMantleAssetKind::Lumen), 2);
	TestEqual(TEXT("Nanite entries"), CountKind(Shape.Assets, EMantleAssetKind::Nanite), 1);
	TestEqual(TEXT("texture entries"), CountKind(Shape.Assets, EMantleAssetKind::Texture), MantlePlaceTextureSlotCount);

	const FMantleAssetRef* Library = Shape.Assets.FindByPredicate([](const FMantleAssetRef& Ref) { return Ref.Id == TEXT("lib_stone"); });
	if (TestNotNull(TEXT("the library entry is listed"), Library))
	{
		TestEqual(TEXT("as it was"), Library->Hash, Assets[0].Hash);
		TestEqual(TEXT("its size too"), Library->Bytes, Assets[0].Bytes);
	}
	const FMantleAssetRef* Uploaded = Shape.Assets.FindByPredicate([&UploadedId](const FMantleAssetRef& Ref) { return Ref.Id == UploadedId; });
	if (TestNotNull(TEXT("an entry already uploaded is listed"), Uploaded))
	{
		TestEqual(TEXT("as it was"), Uploaded->Hash, Assets[1].Hash);
		TestTrue(TEXT("with no side files made up for it"), Uploaded->Lumen.IsEmpty() && Uploaded->Nanite.IsEmpty());
	}
	const FString DrawnId = FMantlePlaceAssets::ProvisionalId(DrawnKey);
	const FMantleAssetRef* Drawn = Shape.Assets.FindByPredicate([&DrawnId](const FMantleAssetRef& Ref) { return Ref.Id == DrawnId; });
	if (TestNotNull(TEXT("the drawn mesh is listed"), Drawn))
	{
		TestEqual(TEXT("with an upload's hash"), Drawn->Hash, FMantlePlaceUploadShape::UploadedHashShape());
		TestEqual(TEXT("that is sha256 and 64 hex digits"), Drawn->Hash.Len(), 7 + 64);
		TestEqual(TEXT("and the largest size an upload may be"), Drawn->Bytes, Limits.MaxUploadBytes);
		TestEqual(TEXT("an id as long as an uploaded one"), Drawn->Id.Len(), 5 + 32);
		const FMantleAssetRef* Lumen = Shape.Assets.FindByPredicate([Drawn](const FMantleAssetRef& Ref) { return Ref.Id == Drawn->Lumen; });
		const FMantleAssetRef* Nanite = Shape.Assets.FindByPredicate([Drawn](const FMantleAssetRef& Ref) { return Ref.Id == Drawn->Nanite; });
		if (TestNotNull(TEXT("naming its Lumen entry"), Lumen) && TestNotNull(TEXT("and its Nanite entry"), Nanite))
		{
			TestTrue(TEXT("a Lumen entry"), Lumen->Kind == EMantleAssetKind::Lumen);
			TestEqual(TEXT("with the engine a Lumen file is baked by"), Lumen->Engine, FMantleMeshLumenFile::EngineTag());
			TestEqual(TEXT("and a Lumen file's largest size"), Lumen->Bytes, FMantleMeshLumenFile::MaxBytes);
			TestTrue(TEXT("a Nanite entry"), Nanite->Kind == EMantleAssetKind::Nanite);
			TestEqual(TEXT("with the engine a Nanite file is baked by"), Nanite->Engine, FMantleNaniteSideFile::EngineTag());
			TestEqual(TEXT("and a Nanite file's largest size"), Nanite->Bytes, Limits.MaxNaniteBytes);
			TestEqual(TEXT("ids as long as uploaded ones"), Lumen->Id.Len() + Nanite->Id.Len(), 2 * (5 + 32));
		}
	}
	const FString CollisionId = FMantlePlaceAssets::ProvisionalId(CollisionKey);
	const FMantleAssetRef* Collision = Shape.Assets.FindByPredicate([&CollisionId](const FMantleAssetRef& Ref) { return Ref.Id == CollisionId; });
	if (TestNotNull(TEXT("the collision mesh is listed"), Collision))
	{
		TestTrue(TEXT("with no side files"), Collision->Lumen.IsEmpty() && Collision->Nanite.IsEmpty());
	}

	// Converted already: its own entries are in the export's list, so none are made up for it.
	FMantlePlaceAssetSet Converted;
	Converted.Meshes.AddDefaulted_GetRef().Key = DrawnKey;
	Converted.Materials.AddDefaulted_GetRef().Key = MaterialKey;
	const FMantlePlaceUploadShape After = FMantlePlaceUploadShape::Project(Assets, Keys, &Converted, Limits, EMantleUploadShapeBound::Worst,
		[](const FMantleAssetRef&, const FString&) { return FMantleExpectedSideFiles(); });
	TestEqual(TEXT("with the drawn mesh and the material converted, only the other two meshes get side files"), After.AddedLumen + After.AddedNanite, 4);
	TestEqual(TEXT("and no textures are made up"), After.AddedTextures, 0);

	// The lower bound: the same hashes and side files, sizes written as "0", and no textures for a material not converted yet.
	const FMantlePlaceUploadShape Lowest = FMantlePlaceUploadShape::Project(Assets, Keys, nullptr, Limits, EMantleUploadShapeBound::Lowest,
		[&](const FMantleAssetRef& Mesh, const FString& Key)
		{
			FMantleExpectedSideFiles Expected;
			Expected.bLumen = Key != CollisionKey;
			Expected.bNanite = Key == DrawnKey;
			return Expected;
		});
	TestEqual(TEXT("the lower bound counts no textures"), Lowest.AddedTextures, 0);
	TestEqual(TEXT("but the same side files"), Lowest.AddedLumen + Lowest.AddedNanite, Shape.AddedLumen + Shape.AddedNanite);
	const FMantleAssetRef* LowDrawn = Lowest.Assets.FindByPredicate([&DrawnId](const FMantleAssetRef& Ref) { return Ref.Id == DrawnId; });
	if (TestNotNull(TEXT("the drawn mesh is in the lower bound"), LowDrawn))
	{
		TestEqual(TEXT("with an upload's hash"), LowDrawn->Hash, FMantlePlaceUploadShape::UploadedHashShape());
		TestEqual(TEXT("and the shortest size there is"), LowDrawn->Bytes, static_cast<int64>(0));
	}
	TestTrue(TEXT("every lower-bound size is 0 or as it was"), !Lowest.Assets.ContainsByPredicate([](const FMantleAssetRef& Ref) { return Ref.Bytes != 0 && Ref.Bytes != 1234; }));

	// A converted file keeps the hash and size it has: they are the upload's.
	TArray<FMantleAssetRef> ConvertedAssets = Assets;
	FMantleAssetRef* ConvertedMesh = ConvertedAssets.FindByPredicate([&DrawnId](const FMantleAssetRef& Ref) { return Ref.Id == DrawnId; });
	ConvertedMesh->Hash = TEXT("sha256:") + FString::ChrN(64, TEXT('1'));
	ConvertedMesh->Bytes = 4321;
	for (const EMantleUploadShapeBound Bound : { EMantleUploadShapeBound::Lowest, EMantleUploadShapeBound::Worst })
	{
		const FMantlePlaceUploadShape Kept = FMantlePlaceUploadShape::Project(ConvertedAssets, Keys, nullptr, Limits, Bound,
			[](const FMantleAssetRef&, const FString&) { return FMantleExpectedSideFiles(); });
		const FMantleAssetRef* KeptMesh = Kept.Assets.FindByPredicate([&DrawnId](const FMantleAssetRef& Ref) { return Ref.Id == DrawnId; });
		if (TestNotNull(TEXT("the converted mesh is listed"), KeptMesh))
		{
			TestEqual(TEXT("with its own hash"), KeptMesh->Hash, ConvertedMesh->Hash);
			TestEqual(TEXT("and its own size"), KeptMesh->Bytes, static_cast<int64>(4321));
		}
	}

	// Nothing provisional: the list is as it is.
	const FMantlePlaceUploadShape Same = FMantlePlaceUploadShape::Project(Assets, TMap<FString, FString>(), nullptr, Limits, EMantleUploadShapeBound::Worst,
		[](const FMantleAssetRef&, const FString&) { return FMantleExpectedSideFiles(); });
	TestEqual(TEXT("an uploaded place's list stays as it is"), Same.Assets.Num(), Assets.Num());
	TestEqual(TEXT("nothing added"), Same.NumAdded(), 0);

	// The converter's rules for side files.
	const FMantleExpectedSideFiles None = FMantlePlaceUploadShape::ExpectedSideFiles(nullptr, /*bCollisionOnly*/ true);
	TestTrue(TEXT("a mesh only used as collision gets none"), !None.bLumen && !None.bNanite);
	const FMantleExpectedSideFiles Unknown = FMantlePlaceUploadShape::ExpectedSideFiles(nullptr, /*bCollisionOnly*/ false);
	TestTrue(TEXT("a mesh that is not found counts both (the worst case)"), Unknown.bLumen && Unknown.bNanite);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceUploadShapeSideFilesTest,
	"Mantle.Editor.PlaceSize.SideFilesFollowTheConverter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceUploadShapeSideFilesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceUploadShapeTestPrivate;

	UMaterial* Opaque = MakeMaterial(TEXT("M_ShapeOpaque"), BLEND_Opaque);
	UMaterial* Glass = MakeMaterial(TEXT("M_ShapeGlass"), BLEND_Translucent);
	UStaticMesh* Solid = MakeBox(TEXT("ShapeSolid"), Opaque);
	UStaticMesh* Clear = MakeBox(TEXT("ShapeClear"), Glass);
	UStaticMesh* Mixed = MakeBox(TEXT("ShapeMixed"), Opaque);
	// Two slots, one see-through: the second section draws the glass.
	Mixed->GetStaticMaterials().Add(FStaticMaterial(Glass, TEXT("Slot1"), TEXT("Slot1")));
	if (FStaticMeshRenderData* RenderData = Mixed->GetRenderData(); RenderData && RenderData->LODResources.Num() > 0)
	{
		FStaticMeshSection Extra = RenderData->LODResources[0].Sections[0];
		Extra.MaterialIndex = 1;
		RenderData->LODResources[0].Sections.Add(Extra);
	}

	const FMantleExpectedSideFiles SolidFiles = FMantlePlaceUploadShape::ExpectedSideFiles(Solid, false);
	TestTrue(TEXT("an opaque mesh gets a Lumen file"), SolidFiles.bLumen);
	TestTrue(TEXT("and a Nanite file (the converter builds one for a mesh made without Nanite)"), SolidFiles.bNanite);
	const FMantleExpectedSideFiles ClearFiles = FMantlePlaceUploadShape::ExpectedSideFiles(Clear, false);
	TestFalse(TEXT("a mesh drawn only see-through gets no Lumen file"), ClearFiles.bLumen);
	TestFalse(TEXT("and no Nanite file"), ClearFiles.bNanite);
	const FMantleExpectedSideFiles MixedFiles = FMantlePlaceUploadShape::ExpectedSideFiles(Mixed, false);
	TestTrue(TEXT("a mesh with one see-through section of two gets a Lumen file"), MixedFiles.bLumen);
	TestFalse(TEXT("but no Nanite file"), MixedFiles.bNanite);
	const FMantleExpectedSideFiles WallFiles = FMantlePlaceUploadShape::ExpectedSideFiles(Solid, true);
	TestTrue(TEXT("a mesh only collision gets neither"), !WallFiles.bLumen && !WallFiles.bNanite);

	for (UObject* Object : TArray<UObject*>{ Solid, Clear, Mixed, Opaque, Glass })
	{
		Object->MarkAsGarbage();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceUploadShapeEstimateTest,
	"Mantle.Editor.PlaceSize.EstimateCoversTheUploadedPlaceFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceUploadShapeEstimateTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceUploadShapeTestPrivate;

	if (!MantleNaniteTestEditorBuildsNanite())
	{
		AddInfo(TEXT("skipped: this editor builds no Nanite data (started without a graphics device)"));
		return true;
	}

	// A small place: an opaque box (a Lumen and a Nanite file), a glass box (neither), and an invisible wall (collision only, neither).
	UMaterial* Opaque = MakeMaterial(TEXT("M_EstimateOpaque"), BLEND_Opaque);
	UMaterial* Glass = MakeMaterial(TEXT("M_EstimateGlass"), BLEND_Translucent);
	UStaticMesh* Solid = MakeBox(TEXT("EstimateSolid"), Opaque);
	UStaticMesh* Clear = MakeBox(TEXT("EstimateClear"), Glass);
	UStaticMesh* Wall = MakeBox(TEXT("EstimateWall"), Opaque);

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	auto Place = [World](UStaticMesh* Mesh, const FVector& Where, const bool bHidden)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Where, FRotator::ZeroRotator);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->SetMobility(EComponentMobility::Static);
		Component->SetStaticMesh(Mesh);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		if (bHidden)
		{
			Actor->SetActorHiddenInGame(true);
			Component->SetHiddenInGame(true);
		}
	};
	Place(Solid, FVector(0, 0, 0), false);
	Place(Clear, FVector(300, 0, 0), false);
	Place(Wall, FVector(600, 0, 0), true);

	// Before anything is converted: what the publish's first check and Mantle.Check measure.
	FMantleExportOptions Options;
	Options.Name = TEXT("Upload shape");
	const FMantleExportResult Before = FMantlePlaceExporter::Export(World, Options);
	TestTrue(TEXT("the place exports"), Before.bSuccess);

	// Converted and uploaded, every file: the uploads recorded with ids, sha256 hashes and sizes as Mantle gives them.
	const FString CacheDir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantleUploadShape"));
	IFileManager::Get().DeleteDirectory(*CacheDir, /*RequireExists*/ false, /*Tree*/ true);
	FMantlePlaceAssetConverter Converter(MakeShared<FFourImageFlattener>(), CacheDir);
	FMantlePlaceAssetSet Set;
	TestTrue(TEXT("the place converts"), Converter.Convert(FMantlePlaceDependencies::Gather(World), Set));
	// Converted, nothing uploaded yet: what the publish measures again before its first upload.
	Options.Assets = &Set;
	const FMantleExportResult Prepared = FMantlePlaceExporter::Export(World, Options);
	TestTrue(TEXT("the prepared place exports"), Prepared.bSuccess);

	// MantleCore#189 review: a limit the worst case is over and the lower bound is under. The first check lets it through with a
	// warning; the check after preparing, with the textures known, refuses it.
	FMantleExportOptions Limited;
	Limited.Name = Options.Name;
	Limited.MaxPlaceFileBytes = (Before.PlaceBytes + Prepared.PlaceBytes) / 2;
	if (TestTrue(TEXT("the limit falls between the first check's lower bound and the prepared file"), Limited.MaxPlaceFileBytes > Before.PlaceBytes
		&& Limited.MaxPlaceFileBytes < Prepared.PlaceBytes && Limited.MaxPlaceFileBytes < Before.PlaceBytesWorst))
	{
		const FMantleExportResult First = FMantlePlaceExporter::Export(World, Limited);
		TestFalse(TEXT("the first check does not refuse it"), First.bOverBudget);
		TestTrue(TEXT("it says it may be too big, with both sizes"), First.BudgetWarning.Contains(TEXT("at least")) && First.BudgetWarning.Contains(TEXT("at most")));
		Limited.Assets = &Set;
		const FMantleExportResult Second = FMantlePlaceExporter::Export(World, Limited);
		TestTrue(TEXT("the check after preparing refuses it"), Second.bOverBudget);
		TestTrue(TEXT("saying it is too big"), Second.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path == TEXT("/entities"); }));
	}

	int32 Recorded = 0;
	for (const EMantlePlaceUploadStage Stage : { EMantlePlaceUploadStage::Texture, EMantlePlaceUploadStage::Material, EMantlePlaceUploadStage::Mesh })
	{
		TArray<FMantlePlaceUpload> Uploads;
		FString Error;
		TestTrue(FString::Printf(TEXT("stage %d is taken"), static_cast<int32>(Stage)), Set.TakeUploads(Stage, Uploads, Error));
		for (const FMantlePlaceUpload& Upload : Uploads)
		{
			++Recorded;
			Set.RecordUpload(Upload.Key, FString::Printf(TEXT("ast_u%032x"), Recorded), Upload.Hash, Upload.FileSize > 0 ? Upload.FileSize : Upload.Bytes.Num());
		}
	}
	const FMantleExportResult After = FMantlePlaceExporter::Export(World, Options);
	TestTrue(TEXT("the uploaded place exports"), After.bSuccess);
	TestEqual(TEXT("once uploaded nothing is left to estimate: the file is as written"), After.PlaceBytes, After.PlaceBytesAsWritten);
	TestEqual(TEXT("its worst case too"), After.PlaceBytesWorst, After.PlaceBytesAsWritten);
	TestEqual(TEXT("and lists what it will list"), After.UploadedAssets, After.Place.Assets.Num());

	// The real upload: textures for each material, one Lumen and one Nanite file (the opaque box's), and every hash sha256.
	const int32 RealTextures = CountKind(After.Place.Assets, EMantleAssetKind::Texture);
	const int32 RealLumen = CountKind(After.Place.Assets, EMantleAssetKind::Lumen);
	const int32 RealNanite = CountKind(After.Place.Assets, EMantleAssetKind::Nanite);
	TestEqual(TEXT("the opaque box's Lumen file is listed"), RealLumen, 1);
	TestEqual(TEXT("and its Nanite file"), RealNanite, 1);
	TestTrue(TEXT("textures are listed"), RealTextures > 0);
	TestTrue(TEXT("the upload lists more than the first check's export did"), After.Place.Assets.Num() > Before.Place.Assets.Num());

	// MantleCore#189: the file as written before conversion was the measure, and it was short of the uploaded file.
	TestTrue(TEXT("the file as written before upload is smaller than the uploaded one (the old measure)"), Before.PlaceBytesAsWritten < After.PlaceBytes);
	// Before conversion the two bounds hold it between them: the lower one never over, the worst case never short.
	TestTrue(TEXT("the lower bound is never over the uploaded file"), Before.PlaceBytes <= After.PlaceBytes);
	TestTrue(TEXT("but already more than the file as written"), Before.PlaceBytes > Before.PlaceBytesAsWritten);
	TestTrue(TEXT("the worst case is never short of it"), Before.PlaceBytesWorst >= After.PlaceBytes);
	TestTrue(TEXT("it counts at least every entry the upload lists"), Before.UploadedAssets >= After.Place.Assets.Num());
	const int64 Slack = static_cast<int64>(Before.UploadedAssets - After.Place.Assets.Num()) * 200 + static_cast<int64>(Before.UploadedAssets) * 8;
	TestTrue(FString::Printf(TEXT("and is close: %lld bytes over, at most %lld"), Before.PlaceBytesWorst - After.PlaceBytes, Slack), Before.PlaceBytesWorst - After.PlaceBytes <= Slack);
	// Converted, before any upload: the textures are known, so the lower bound is the uploaded size but for the materials' sizes.
	TestTrue(TEXT("once prepared, the lower bound is still never over"), Prepared.PlaceBytes <= After.PlaceBytes);
	TestTrue(FString::Printf(TEXT("and is within the materials' size fields of it: %lld bytes short"), After.PlaceBytes - Prepared.PlaceBytes),
		After.PlaceBytes - Prepared.PlaceBytes <= 16 * CountKind(After.Place.Assets, EMantleAssetKind::Material));
	TestTrue(TEXT("its worst case is never short either"), Prepared.PlaceBytesWorst >= After.PlaceBytes);
	AddInfo(FString::Printf(TEXT("before preparing: %lld bytes as written, at least %lld, at most %lld (%d assets); prepared: at least %lld, at most %lld; uploaded: %lld bytes (%d assets: %d textures, %d Lumen, %d Nanite)"),
		Before.PlaceBytesAsWritten, Before.PlaceBytes, Before.PlaceBytesWorst, Before.UploadedAssets, Prepared.PlaceBytes, Prepared.PlaceBytesWorst,
		After.PlaceBytes, After.Place.Assets.Num(), RealTextures, RealLumen, RealNanite));

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	for (UObject* Object : TArray<UObject*>{ Solid, Clear, Wall, Opaque, Glass })
	{
		Object->MarkAsGarbage();
	}
	return true;
}

namespace MantlePlaceUploadShapeTestPrivate
{
	class FNoCallsTransport : public IMantleUploadTransport
	{
	public:
		int32 Calls = 0;
		virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) override { ++Calls; }
	};

	/** A level that fits at the first check and is found too big once its one step is converted. */
	class FGrowsWhenPreparedSource : public IMantlePlacePublishSource
	{
	public:
		int32 FirstChecks = 0;
		int32 PreparedChecks = 0;
		int32 ConvertCalls = 0;
		int32 ExportCalls = 0;

		virtual FString GetLevelName() const override { return TEXT("GrowingLevel"); }
		virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) override { return true; }
		virtual bool CheckPlaceSize(FMantlePlaceSizeCheck& OutCheck) override
		{
			++FirstChecks;
			OutCheck.Bytes = 6 * 1024 * 1024;
			OutCheck.WorstBytes = 9 * 1024 * 1024;
			OutCheck.Issues.Add({ EMantleIssueSeverity::Warning, FString(), TEXT("The place file may be too big for Mantle once its files are prepared: at least 6.0 MiB and at most 9.0 MiB."), FString() });
			return true;
		}
		virtual bool CheckConvertedPlaceSize(const FMantlePlaceAssetSet& Set, FMantlePlaceSizeCheck& OutCheck) override
		{
			++PreparedChecks;
			OutCheck.Bytes = 8 * 1024 * 1024 + 512 * 1024;
			OutCheck.bOver = true;
			OutCheck.Issues.Add({ EMantleIssueSeverity::Error, FString(), TEXT("The place file would be 8.5 MiB; Mantle accepts at most 8.0 MiB."), FString() });
			return false;
		}
		virtual int32 NumConversionSteps() const override { return 1; }
		virtual FString ConversionStepName(int32 Step) const override { return TEXT("Step"); }
		virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) override { ++ConvertCalls; }
		virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) override { return true; }
		virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
			const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) override
		{
			++ExportCalls;
			OutPlaceJson = TEXT("{}");
			return EMantlePlaceExportOutcome::Exported;
		}
		virtual FString FindPlaceId() override { return TEXT("plc_00000000000000000000000000000001"); }
		virtual void RememberPlaceId(const FString& PlaceId) override {}
		virtual void ForgetPlaceId() override {}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceUploadShapePublishTest,
	"Mantle.Editor.PlaceSize.PublishStopsAfterPreparingWhenTheFilesMakeItTooBig",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceUploadShapePublishTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceUploadShapeTestPrivate;

	double Now = 1000.0;
	const TSharedRef<FGrowsWhenPreparedSource> Source = MakeShared<FGrowsWhenPreparedSource>();
	const TSharedRef<FNoCallsTransport> Transport = MakeShared<FNoCallsTransport>();
	const TSharedRef<FMantlePlacePublishSession> Session = MakeShared<FMantlePlacePublishSession>(
		Source, Transport, /*bGoLive*/ true, FString(), [&Now] { return Now; });
	Session->Start();
	for (int32 Tick = 0; Tick < 20 && Session->GetPhase() != EMantlePublishPhase::Failed && Session->GetPhase() != EMantlePublishPhase::Done; ++Tick)
	{
		Now += 1.0;
		Session->Tick();
	}

	TestEqual(TEXT("the first check let it through"), Source->FirstChecks, 1);
	TestEqual(TEXT("its files were prepared"), Source->ConvertCalls, 1);
	TestEqual(TEXT("then it was measured again"), Source->PreparedChecks, 1);
	TestEqual(TEXT("and the publish stopped there"), static_cast<int32>(Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Failed));
	TestEqual(TEXT("nothing was exported"), Source->ExportCalls, 0);
	TestEqual(TEXT("and not one call went to Mantle"), Transport->Calls, 0);
	const FMantlePublishResult& Result = Session->GetResult();
	TestTrue(TEXT("the message says it is too big and that nothing was uploaded"), Result.Message.Contains(TEXT("too big")) && Result.Message.Contains(TEXT("Nothing was uploaded")));
	TestTrue(TEXT("with the size"), Result.Message.Contains(TEXT("8.5 MiB")));
	TestTrue(TEXT("the reason is listed as an error"), Result.Validation.HasErrors());
	TestTrue(TEXT("and the first check's warning is listed"), Result.Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("may be too big"));
		}));
	return true;
}

#endif
