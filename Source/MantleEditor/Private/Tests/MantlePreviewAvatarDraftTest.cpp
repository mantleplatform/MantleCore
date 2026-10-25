// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetStore.h"
#include "MantleCosmeticItem.h"
#include "MantleCosmeticPackage.h"
#include "MantlePreviewAvatarExport.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// Chunk 9, Test locally for avatars: a checked avatar that was never uploaded goes into the preview bundle as a local draft. The
// manifest flag and shape are the ones agreed with the client (MantleBuiltinBundle): "localDraft": true, version 0 in the manifest
// and in item.json, a hash-made itm_u id, and the assets laid out exactly as for a catalog avatar.

namespace MantlePreviewAvatarDraftTestPrivate
{
	TSharedPtr<FJsonObject> ReadJson(const FString& File)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *File))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		}
		return Object;
	}

	TSharedPtr<FJsonObject> ParseObject(const TCHAR* Text)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text)), Object);
		return Object;
	}

	TArray<uint8> Bytes(const char* Text)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	}

	FMantleCosmeticMaterialRef ClientMaterial(const TCHAR* Role, const TCHAR* Material)
	{
		FMantleCosmeticMaterialRef Ref;
		Ref.Role = Role;
		Ref.ClientMaterial = Material;
		return Ref;
	}

	/** A converted MetaHuman as Check leaves it (public, no rights ticked: a draft never needs them), and its cooked files. */
	FMantleCosmeticPackage ConvertedPackage(TArray<MantlePreviewAvatarExport::FCookedDraftAsset>& OutCooked)
	{
		FMantleCosmeticPackage Package;
		Package.Class = EMantleCosmeticClass::Avatar;
		Package.AvatarPublicationMode = TEXT("converted");
		Package.Slot = TEXT("avatar");
		Package.Name = TEXT("Vampire");
		Package.MeshKey = TEXT("mesh");
		Package.FaceMeshKey = TEXT("face");
		Package.ThumbnailKey = TEXT("thumbnail");
		Package.Materials.Add(ClientMaterial(TEXT("skin"), TEXT("skin_body_v1")));
		Package.FaceMaterials.Add(ClientMaterial(TEXT("skin"), TEXT("skin_face_v1")));
		Package.FaceMaterials.Add(ClientMaterial(TEXT("eyes"), TEXT("eye_left_v1")));
		Package.FaceMaterials.Add(ClientMaterial(TEXT("eyes"), TEXT("eye_right_v1")));

		OutCooked.Reset();
		MantlePreviewAvatarExport::FCookedDraftAsset& Body = OutCooked.AddDefaulted_GetRef();
		Body.Key = TEXT("mesh");
		Body.Bytes = Bytes("MSK1 cooked body");
		Body.Info = ParseObject(TEXT("{\"format\":\"MSK1\",\"skeleton\":\"metahuman\",\"garmentCompatible\":true,\"bones\":[\"root\",\"pelvis\"],")
			TEXT("\"max\":[40,20,150.2],\"metrics\":{\"standingHeightCm\":150.2,\"pelvisHeightCm\":95.5,\"armLengthCm\":60,\"legLengthCm\":88,")
			TEXT("\"shoulderWidthCm\":38,\"hipWidthCm\":33}}"));
		MantlePreviewAvatarExport::FCookedDraftAsset& Face = OutCooked.AddDefaulted_GetRef();
		Face.Key = TEXT("face");
		Face.Bytes = Bytes("MSK1 cooked face");
		Face.Info = ParseObject(TEXT("{\"format\":\"MSK1\",\"skeleton\":\"metahuman\",\"bones\":[\"head\",\"FACIAL_C_Jaw\"],\"max\":[9,12,171.96],\"eyeHeightCm\":160.4}"));
		return Package;
	}

	FString TempBundle()
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("PreviewAvatarDraft"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		return Dir;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewAvatarDraftManifestTest,
	"Mantle.Editor.PreviewAvatarDraft.ManifestIsALocalDraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewAvatarDraftManifestTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewAvatarDraftTestPrivate;
	TArray<MantlePreviewAvatarExport::FCookedDraftAsset> Cooked;
	const FMantleCosmeticPackage Package = ConvertedPackage(Cooked);
	const FString Bundle = TempBundle();

	FString Descriptor, Builtin, Error;
	const bool bWritten = MantlePreviewAvatarExport::WriteLocalDraft(Bundle, Package, Cooked, Descriptor, Builtin, Error);
	TestTrue(FString::Printf(TEXT("the draft is written (%s)"), *Error), bWritten);
	TestEqual(TEXT("the descriptor's place"), Descriptor, FString(TEXT("Avatar/descriptor.json")));
	TestEqual(TEXT("the builtin folder's place"), Builtin, FString(TEXT("Avatar/Builtin")));

	const FMantleContentHash BodyHash = FMantleContentHash::OfBytes(Cooked[0].Bytes);
	const FMantleContentHash FaceHash = FMantleContentHash::OfBytes(Cooked[1].Bytes);
	const FString Id = MantlePreviewAvatarExport::LocalDraftItemId(BodyHash);
	TestTrue(TEXT("the draft's id has an item id's form"), MantleCosmetic::IsItemId(Id));
	TestEqual(TEXT("it is made from the body's cooked hash"), Id, TEXT("itm_u") + BodyHash.ToHex().Left(32));

	// ---- manifest.json: the agreed flag, version 0, the assets as for a catalog avatar. ----
	const FString ItemDir = FPaths::Combine(Bundle, TEXT("Avatar"), TEXT("Builtin"), Id);
	const TSharedPtr<FJsonObject> Manifest = ReadJson(FPaths::Combine(ItemDir, TEXT("manifest.json")));
	if (!TestTrue(TEXT("manifest.json is there"), Manifest.IsValid()))
	{
		return false;
	}
	bool bLocalDraft = false;
	TestTrue(TEXT("the manifest says localDraft"), Manifest->TryGetBoolField(TEXT("localDraft"), bLocalDraft) && bLocalDraft);
	TestEqual(TEXT("version 0, never a made-up 1"), Manifest->GetIntegerField(TEXT("version")), 0);
	TestEqual(TEXT("schema 1"), Manifest->GetIntegerField(TEXT("schema")), 1);
	TestEqual(TEXT("the key is the folder"), Manifest->GetStringField(TEXT("key")), Id);
	TestEqual(TEXT("the item id"), Manifest->GetStringField(TEXT("itemId")), Id);
	const TArray<TSharedPtr<FJsonValue>>& Assets = Manifest->GetArrayField(TEXT("assets"));
	TestEqual(TEXT("the body and the face, no thumbnail"), Assets.Num(), 2);
	TMap<FString, TSharedPtr<FJsonObject>> ByField;
	for (const TSharedPtr<FJsonValue>& Value : Assets)
	{
		ByField.Add(Value->AsObject()->GetStringField(TEXT("field")), Value->AsObject());
	}
	if (TestTrue(TEXT("the body mesh is listed"), ByField.Contains(TEXT("mesh"))))
	{
		const TSharedPtr<FJsonObject> Body = ByField[TEXT("mesh")];
		TestEqual(TEXT("its hash"), Body->GetStringField(TEXT("cookedHash")), BodyHash.ToString());
		TestEqual(TEXT("its size"), Body->GetIntegerField(TEXT("cookedBytes")), Cooked[0].Bytes.Num());
		TestEqual(TEXT("its file"), Body->GetStringField(TEXT("file")), TEXT("assets/") + BodyHash.ToHex());
	}
	TestTrue(TEXT("the face mesh is listed"), ByField.Contains(TEXT("faceMesh")));
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the cooked body is the file in assets/"), FFileHelper::LoadFileToArray(OnDisk, *FPaths::Combine(ItemDir, TEXT("assets"), BodyHash.ToHex()))
		&& OnDisk == Cooked[0].Bytes);
	TestTrue(TEXT("the cooked face is the file in assets/"), FFileHelper::LoadFileToArray(OnDisk, *FPaths::Combine(ItemDir, TEXT("assets"), FaceHash.ToHex()))
		&& OnDisk == Cooked[1].Bytes);

	// ---- item.json: what the catalog would serve once published, with version 0. ----
	const TSharedPtr<FJsonObject> Item = ReadJson(FPaths::Combine(ItemDir, TEXT("item.json")));
	if (!TestTrue(TEXT("item.json is there"), Item.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("its id"), Item->GetStringField(TEXT("id")), Id);
	TestEqual(TEXT("its version matches the manifest's"), Item->GetIntegerField(TEXT("version")), 0);
	TestEqual(TEXT("an avatar"), Item->GetStringField(TEXT("kind")), FString(TEXT("avatar")));
	TestEqual(TEXT("converted"), Item->GetStringField(TEXT("avatarPublicationMode")), FString(TEXT("converted")));
	TestEqual(TEXT("the cooked body's skeleton"), Item->GetStringField(TEXT("skeleton")), FString(TEXT("metahuman")));
	TestTrue(TEXT("garment compatible, from the cook"), Item->GetBoolField(TEXT("garmentCompatible")));
	TestTrue(TEXT("a converted MetaHuman with a face can be customised"), Item->GetBoolField(TEXT("customizationEligible")));
	TestEqual(TEXT("the face's jaw by default"), Item->GetStringField(TEXT("jawBone")), FString(TEXT("FACIAL_C_Jaw")));
	const TSharedPtr<FJsonObject> Metrics = Item->GetObjectField(TEXT("bodyMetrics"));
	TestEqual(TEXT("as tall as the face's top, to 0.1 cm"), Metrics->GetNumberField(TEXT("standingHeightCm")), 172.0);
	TestEqual(TEXT("the face's eye height"), Metrics->GetNumberField(TEXT("eyeHeightCm")), 160.4);
	TestEqual(TEXT("the body's own measures kept"), Metrics->GetNumberField(TEXT("pelvisHeightCm")), 95.5);
	TestEqual(TEXT("the body as a reference"), Item->GetObjectField(TEXT("mesh"))->GetStringField(TEXT("cookedHash")), BodyHash.ToString());
	TestEqual(TEXT("the face as a reference"), Item->GetObjectField(TEXT("faceMesh"))->GetStringField(TEXT("cookedHash")), FaceHash.ToString());
	for (const TCHAR* Gone : { TEXT("meshAssetId"), TEXT("faceMeshAssetId"), TEXT("thumbnailAssetId"), TEXT("thumbnail"), TEXT("visibility"),
		TEXT("rightsDeclarationVersion"), TEXT("provenance") })
	{
		TestFalse(FString::Printf(TEXT("no %s"), Gone), Item->HasField(Gone));
	}
	TestEqual(TEXT("the client materials as the create request names them"),
		Item->GetArrayField(TEXT("faceMaterials"))[1]->AsObject()->GetStringField(TEXT("clientMaterial")), FString(TEXT("eye_left_v1")));

	// The client reads it with the same parser it reads the catalog with, once MantleBuiltinBundle::ReadEntry has given each
	// reference its mantle-builtin: url.
	for (const TCHAR* Field : { TEXT("mesh"), TEXT("faceMesh") })
	{
		const TSharedPtr<FJsonObject> Ref = Item->GetObjectField(Field);
		FMantleContentHash Hash;
		FMantleContentHash::Parse(Ref->GetStringField(TEXT("cookedHash")), Hash);
		Ref->SetStringField(TEXT("url"), FString::Printf(TEXT("mantle-builtin:%s/assets/%s"), *Id, *Hash.ToHex()));
	}
	FString Wrapped;
	FJsonSerializer::Serialize(Item.ToSharedRef(), TJsonWriterFactory<>::Create(&Wrapped));
	TArray<FMantleCosmeticItem> Parsed;
	TArray<FString> Missing;
	FString Why;
	if (TestTrue(FString::Printf(TEXT("the item parses (%s)"), *Why), MantleCosmetic::ParseItemBatch(TEXT("{\"items\":[") + Wrapped + TEXT("]}"), Parsed, Missing, Why))
		&& TestEqual(TEXT("one item"), Parsed.Num(), 1))
	{
		TestTrue(TEXT("as an avatar"), Parsed[0].Kind == EMantleCosmeticKind::Avatar);
		TestEqual(TEXT("at version 0"), Parsed[0].Version, 0);
		TestEqual(TEXT("every file verifiable"), Parsed[0].UnverifiedAssets.Num(), 0);
	}

	// ---- The descriptor wears it. ----
	const TSharedPtr<FJsonObject> DescriptorJson = ReadJson(FPaths::Combine(Bundle, Descriptor));
	TestTrue(TEXT("the descriptor wears the draft"), DescriptorJson.IsValid()
		&& DescriptorJson->GetObjectField(TEXT("body"))->GetStringField(TEXT("avatarItemId")) == Id);

	IFileManager::Get().DeleteDirectory(*Bundle, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewAvatarDraftMaterialsTest,
	"Mantle.Editor.PreviewAvatarDraft.TexturedMaterialsBecomeReferences",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewAvatarDraftMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewAvatarDraftTestPrivate;
	// An as-is body with its own textures: each texture key becomes {cookedHash, cookedBytes}; one file used twice is listed twice
	// (once per field, as the client matches them) and written once.
	FMantleCosmeticPackage Package;
	Package.Class = EMantleCosmeticClass::Avatar;
	Package.AvatarPublicationMode = TEXT("as_is");
	Package.Visibility = TEXT("private");
	Package.Slot = TEXT("avatar");
	Package.Name = TEXT("Ronin");
	Package.MeshKey = TEXT("mesh");
	FMantleCosmeticMaterialRef Skin;
	Skin.Role = TEXT("skin");
	Skin.AlbedoKey = TEXT("texture.skin.color");
	Skin.NormalKey = TEXT("texture.skin.normal");
	Skin.OrmKey = TEXT("texture.skin.color");
	Package.Materials.Add(Skin);

	TArray<MantlePreviewAvatarExport::FCookedDraftAsset> Cooked;
	MantlePreviewAvatarExport::FCookedDraftAsset& Body = Cooked.AddDefaulted_GetRef();
	Body.Key = TEXT("mesh");
	Body.Bytes = Bytes("MSK1 ronin");
	Body.Info = ParseObject(TEXT("{\"skeleton\":\"mantle\",\"garmentCompatible\":false,\"metrics\":{\"standingHeightCm\":180}}"));
	MantlePreviewAvatarExport::FCookedDraftAsset& Color = Cooked.AddDefaulted_GetRef();
	Color.Key = TEXT("texture.skin.color");
	Color.Bytes = Bytes("MTX1 color");
	MantlePreviewAvatarExport::FCookedDraftAsset& Normal = Cooked.AddDefaulted_GetRef();
	Normal.Key = TEXT("texture.skin.normal");
	Normal.Bytes = Bytes("MTX1 normal");

	FString Error;
	const TSharedPtr<FJsonObject> Item = MantlePreviewAvatarExport::MakeLocalDraftItem(Package, Cooked, Error);
	if (!TestTrue(FString::Printf(TEXT("the item is made (%s)"), *Error), Item.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Material = Item->GetArrayField(TEXT("materials"))[0]->AsObject();
	TestEqual(TEXT("albedo"), Material->GetObjectField(TEXT("albedo"))->GetStringField(TEXT("cookedHash")), FMantleContentHash::OfBytes(Color.Bytes).ToString());
	TestEqual(TEXT("normal"), Material->GetObjectField(TEXT("normal"))->GetStringField(TEXT("cookedHash")), FMantleContentHash::OfBytes(Normal.Bytes).ToString());
	TestEqual(TEXT("orm"), Material->GetObjectField(TEXT("orm"))->GetIntegerField(TEXT("cookedBytes")), Color.Bytes.Num());
	TestFalse(TEXT("no asset ids left"), Material->HasField(TEXT("albedoAssetId")) || Material->HasField(TEXT("normalAssetId")) || Material->HasField(TEXT("ormAssetId")));
	TestEqual(TEXT("its role"), Material->GetStringField(TEXT("role")), FString(TEXT("skin")));
	TestFalse(TEXT("an as-is body cannot be customised"), Item->GetBoolField(TEXT("customizationEligible")));
	TestFalse(TEXT("no face, no jaw"), Item->HasField(TEXT("jawBone")));

	const FString Bundle = TempBundle();
	FString Descriptor, Builtin;
	if (TestTrue(FString::Printf(TEXT("the draft is written (%s)"), *Error), MantlePreviewAvatarExport::WriteLocalDraft(Bundle, Package, Cooked, Descriptor, Builtin, Error)))
	{
		const FString ItemDir = FPaths::Combine(Bundle, TEXT("Avatar"), TEXT("Builtin"), MantlePreviewAvatarExport::LocalDraftItemId(FMantleContentHash::OfBytes(Body.Bytes)));
		const TSharedPtr<FJsonObject> Manifest = ReadJson(FPaths::Combine(ItemDir, TEXT("manifest.json")));
		TestEqual(TEXT("four fields: mesh, albedo, normal, orm"), Manifest.IsValid() ? Manifest->GetArrayField(TEXT("assets")).Num() : 0, 4);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(ItemDir, TEXT("assets"), TEXT("*")), /*Files*/ true, /*Directories*/ false);
		TestEqual(TEXT("three distinct files"), Files.Num(), 3);
	}
	IFileManager::Get().DeleteDirectory(*Bundle, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewAvatarDraftRefusalsTest,
	"Mantle.Editor.PreviewAvatarDraft.RefusesAndLeavesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewAvatarDraftRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewAvatarDraftTestPrivate;
	TArray<MantlePreviewAvatarExport::FCookedDraftAsset> Cooked;
	const FMantleCosmeticPackage Package = ConvertedPackage(Cooked);
	const FString Bundle = TempBundle();
	const FString AvatarDir = FPaths::Combine(Bundle, TEXT("Avatar"));

	// The face was never cooked: refused, and no half-written avatar is left for the preview to find.
	TArray<MantlePreviewAvatarExport::FCookedDraftAsset> NoFace = Cooked;
	NoFace.RemoveAt(1);
	FString Descriptor, Builtin, Error;
	TestFalse(TEXT("a missing cooked file is refused"), MantlePreviewAvatarExport::WriteLocalDraft(Bundle, Package, NoFace, Descriptor, Builtin, Error));
	TestTrue(FString::Printf(TEXT("it says which (%s)"), *Error), Error.Contains(TEXT("face")));
	TestFalse(TEXT("no Avatar folder is left"), IFileManager::Get().DirectoryExists(*AvatarDir));

	// The cook said nothing about the skeleton: refused rather than guessed.
	TArray<MantlePreviewAvatarExport::FCookedDraftAsset> NoInfo = Cooked;
	NoInfo[0].Info.Reset();
	TestFalse(TEXT("a body the cook did not describe is refused"), MantlePreviewAvatarExport::WriteLocalDraft(Bundle, Package, NoInfo, Descriptor, Builtin, Error));
	TestFalse(TEXT("still no Avatar folder"), IFileManager::Get().DirectoryExists(*AvatarDir));

	// Only avatars.
	FMantleCosmeticPackage Garment = Package;
	Garment.Class = EMantleCosmeticClass::Garment;
	TestFalse(TEXT("a garment is not an avatar"), MantlePreviewAvatarExport::MakeLocalDraftItem(Garment, Cooked, Error).IsValid());

	IFileManager::Get().DeleteDirectory(*Bundle, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

#endif
