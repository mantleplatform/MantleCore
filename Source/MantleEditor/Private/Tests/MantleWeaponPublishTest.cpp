// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLibraryPublish.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlacePublishSession.h"
#include "MantleSha256.h"
#include "MantleWeaponPublish.h"
#include "MantleWeaponSpawnPoint.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The weapon builder's Publish. A weapon is published on its own, with no place: the place publish session in its
// library-only mode uploads the weapon's model and materials, then its settings file naming their ids, then makes a library item of
// kind weapon, and sends nothing about a place. Every weapon has its 3D model (a fist weapon's too). A republish with nothing
// changed adds no item and no version. Pure: a scripted Mantle and a fake clock, no network; the refusals read a settings file
// written to Content/MantleWeapons for the test and deleted after it.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponPublishTestPrivate
{
	const TCHAR* const WeaponName = TEXT("TestPublishSword");

	TArray<uint8> Bytes(int32 Seed, int32 Size)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Out[Index] = static_cast<uint8>((Seed * 31 + Index * 13) & 0xFF);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	FMantleApiResult Ok(const FString& Body)
	{
		FMantleApiResult Result;
		Result.bSuccess = true;
		Result.StatusCode = 200;
		Result.Body = Body;
		return Result;
	}

	FString AssetJson(const FString& Id, const FString& Hash, const FString& Kind, bool bCreated)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"hash\":\"%s\",\"kind\":\"%s\",\"status\":\"approved\",\"cookState\":\"passed\",")
			TEXT("\"moderationState\":\"passed\",\"created\":%s}"), *Id, *Hash, *Kind, bCreated ? TEXT("true") : TEXT("false"));
	}

	/** A scripted Mantle that keeps uploads by hash and library items with their versions, and records every call. */
	class FFakeMantle : public IMantleUploadTransport
	{
	public:
		struct FCall
		{
			EMantleUploadCall Call = EMantleUploadCall::DiffAssets;
			FString Kind;
			FString Json;
			FString Body;
			FString Id;
			/** UploadAsset: the id Mantle gave the file. */
			FString AssetId;
		};
		TArray<FCall> Calls;
		TMap<FString, FString> IdByHash;
		TMap<FString, FString> KindByHash;
		/** Item id to the primary asset of each version, oldest first. */
		TMap<FString, TArray<FString>> Items;
		TMap<FString, FString> VisibilityByItem;
		int32 NextId = 1;

		virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) override
		{
			FCall& Call = Calls.AddDefaulted_GetRef();
			Call.Call = Request.Call;
			Call.Kind = Request.Options.Kind;
			Call.Json = Request.Json;
			Call.Id = Request.Id;
			if (Request.Bytes)
			{
				FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Request.Bytes->GetData()), Request.Bytes->Num());
				Call.Body = FString(Text.Length(), Text.Get());
			}
			const FMantleApiResult Result = Answer(Request);
			if (Request.Call == EMantleUploadCall::UploadAsset && Request.Bytes)
			{
				Call.AssetId = IdByHash.FindRef(FMantleSha256::AssetHash(*Request.Bytes));
			}
			Pending.Emplace(Result, OnDone);
		}

		void Flush()
		{
			while (Pending.Num() > 0)
			{
				TArray<TPair<FMantleApiResult, FMantleApiCallback>> Batch = MoveTemp(Pending);
				Pending.Reset();
				for (TPair<FMantleApiResult, FMantleApiCallback>& Item : Batch)
				{
					Item.Value.ExecuteIfBound(Item.Key);
				}
			}
		}

		int32 Count(EMantleUploadCall Kind) const
		{
			return Calls.FilterByPredicate([Kind](const FCall& Call) { return Call.Call == Kind; }).Num();
		}

		const FCall* LastUploadOfKind(const TCHAR* Kind) const
		{
			for (int32 Index = Calls.Num() - 1; Index >= 0; --Index)
			{
				if (Calls[Index].Call == EMantleUploadCall::UploadAsset && Calls[Index].Kind == Kind)
				{
					return &Calls[Index];
				}
			}
			return nullptr;
		}

	private:
		TArray<TPair<FMantleApiResult, FMantleApiCallback>> Pending;

		FMantleApiResult Answer(const FMantleUploadCallRequest& Request)
		{
			switch (Request.Call)
			{
			case EMantleUploadCall::DiffAssets:
				{
					FString Known;
					FString Unknown;
					for (const FString& Hash : Request.Hashes)
					{
						if (const FString* Id = IdByHash.Find(Hash))
						{
							Known += (Known.IsEmpty() ? TEXT("") : TEXT(",")) + AssetJson(*Id, Hash, KindByHash.FindRef(Hash), false);
						}
						else
						{
							Unknown += FString::Printf(TEXT("%s\"%s\""), Unknown.IsEmpty() ? TEXT("") : TEXT(","), *Hash);
						}
					}
					return Ok(FString::Printf(TEXT("{\"known\":[%s],\"unknown\":[%s]}"), *Known, *Unknown));
				}
			case EMantleUploadCall::UploadAsset:
				{
					const FString Hash = Request.Bytes ? FMantleSha256::AssetHash(*Request.Bytes) : FString();
					const FString Id = IdByHash.Contains(Hash) ? IdByHash[Hash] : FString::Printf(TEXT("ast_u%032x"), NextId++);
					IdByHash.Add(Hash, Id);
					KindByHash.Add(Hash, Request.Options.Kind);
					return Ok(AssetJson(Id, Hash, Request.Options.Kind, true));
				}
			case EMantleUploadCall::CreateLibraryItem:
				{
					const TSharedPtr<FJsonObject> Body = ReadObject(Request.Json);
					const FString Item = FString::Printf(TEXT("itm_u%032x"), 0xA000 + Items.Num());
					Items.Add(Item, { Body.IsValid() ? Body->GetStringField(TEXT("primaryAssetId")) : FString() });
					FString Visibility = TEXT("public");
					if (Body.IsValid())
					{
						Body->TryGetStringField(TEXT("visibility"), Visibility);
					}
					VisibilityByItem.Add(Item, Visibility);
					return Ok(FString::Printf(TEXT("{\"itemId\":\"%s\",\"version\":1}"), *Item));
				}
			case EMantleUploadCall::CreateLibraryVersion:
				{
					TArray<FString>* Versions = Items.Find(Request.Id);
					const TSharedPtr<FJsonObject> Body = ReadObject(Request.Json);
					if (!Versions || !Body.IsValid())
					{
						FMantleApiResult Missing;
						Missing.StatusCode = 404;
						return Missing;
					}
					Versions->Add(Body->GetStringField(TEXT("primaryAssetId")));
					return Ok(FString::Printf(TEXT("{\"itemId\":\"%s\",\"version\":%d}"), *Request.Id, Versions->Num()));
				}
			case EMantleUploadCall::GetLibraryItem:
				{
					const TArray<FString>* Versions = Items.Find(Request.Id);
					if (!Versions)
					{
						FMantleApiResult Missing;
						Missing.StatusCode = 404;
						return Missing;
					}
					FString List;
					for (int32 Index = 0; Index < Versions->Num(); ++Index)
					{
						List += FString::Printf(TEXT("%s{\"version\":%d,\"primaryAssetId\":\"%s\"}"), List.IsEmpty() ? TEXT("") : TEXT(","), Index + 1, *(*Versions)[Index]);
					}
					return Ok(FString::Printf(TEXT("{\"id\":\"%s\",\"kind\":\"weapon\",\"visibility\":\"%s\",\"status\":\"active\",\"versions\":[%s]}"),
						*Request.Id, *VisibilityByItem.FindRef(Request.Id), *List));
				}
			default:
				return Ok(TEXT("{}"));
			}
		}
	};

	/**
	 * A library-only source with a ready-made conversion: a sword with a 3D model and one material. Its remembered items live in a
	 * map, as UMantleSettings would keep them.
	 */
	class FFakeWeaponSource : public IMantlePlacePublishSource
	{
	public:
		TMap<FString, FString> Remembered;

		virtual FString GetLevelName() const override { return WeaponName; }
		virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) override { return true; }
		virtual int32 NumConversionSteps() const override { return 1; }
		virtual FString ConversionStepName(int32 Step) const override { return WeaponName; }

		virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) override
		{
			FMantleConvertedWeapon Weapon;
			Weapon.Key = FMantlePlaceAssets::WeaponKey(WeaponName);
			Weapon.Name = WeaponName;
			Weapon.Path = FString(TEXT("/Game/MantleWeapons/")) + WeaponName;
			FMantleConvertedSkeletalMesh Mesh;
			Mesh.Key = TEXT("skeletalmesh:/Game/MantleWeapons/TestPublishSword/SKM_TestPublishSword.SKM_TestPublishSword");
			Mesh.Name = TEXT("SKM_TestPublishSword");
			Mesh.Bytes = Bytes(3, 900);
			Mesh.Hash = FMantleSha256::AssetHash(Mesh.Bytes);
			Mesh.Slots = { 0 };
			FMantleConvertedMaterial Material;
			Material.Key = TEXT("material:/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");
			Material.Name = TEXT("WorldGridMaterial");
			Weapon.MeshKey = Mesh.Key;
			Weapon.Slots.AddDefaulted_GetRef().MaterialKey = Material.Key;
			// The builder writes its own local names (and, for a mesh slot with no material, an empty one).
			Weapon.DocumentText = TEXT("{\"schemaVersion\":2,\"name\":\"TestPublishSword\",\"hold\":\"melee_1h\",\"delivery\":\"sweep\",\"mesh\":\"SKM_TestPublishSword\",\"materials\":[\"\"]}");
			Set.SkeletalMeshes.Add(MoveTemp(Mesh));
			Set.Materials.Add(MoveTemp(Material));
			Set.Weapons.Add(MoveTemp(Weapon));
		}

		virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) override { return true; }
		virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
			const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) override
		{
			++ExportCalls;
			return EMantlePlaceExportOutcome::Failed;
		}
		virtual FString FindPlaceId() override { return FString(); }
		virtual void RememberPlaceId(const FString& PlaceId) override {}
		virtual void ForgetPlaceId() override {}
		virtual FString FindLibraryItem(const FString& SourcePath) override { return Remembered.FindRef(SourcePath); }
		virtual void RememberLibraryItem(const FString& SourcePath, const FString& ItemId) override { Remembered.Add(SourcePath, ItemId); }
		virtual void ForgetLibraryItem(const FString& SourcePath) override { Remembered.Remove(SourcePath); }
		virtual bool IsLibraryOnly() const override { return true; }

		int32 ExportCalls = 0;
	};

	/** Runs one publish of Source over Mantle on a fake clock, to its end. */
	TSharedRef<FMantlePlacePublishSession> RunPublish(const TSharedRef<FFakeWeaponSource>& Source, const TSharedRef<FFakeMantle>& Mantle, bool bPublic)
	{
		double Now = 1000.0;
		const TSharedRef<FMantlePlacePublishSession> Session = MakeShared<FMantlePlacePublishSession>(Source, Mantle, /*bGoLive*/ true, FString(),
			[&Now] { return Now; });
		Session->Timing.ConvertBudgetSeconds = 1.0e6;
		Session->VehicleChoice.bPublic = bPublic;
		Session->VehicleChoice.bRightsDeclared = bPublic;
		Session->Start();
		for (int32 Step = 0; Step < 2000 && !Session->IsFinished(); ++Step)
		{
			Mantle->Flush();
			Session->Tick();
			Now += 0.5;
		}
		return Session;
	}

	bool AnyPlaceCall(const FFakeMantle& Mantle)
	{
		return Mantle.Count(EMantleUploadCall::CreatePlace) + Mantle.Count(EMantleUploadCall::SaveDraft) + Mantle.Count(EMantleUploadCall::PublishPlace)
			+ Mantle.Count(EMantleUploadCall::UploadScript) > 0;
	}

	/** A settings file in Content/MantleWeapons/<Name>, deleted with this. */
	struct FBuiltWeaponFolder
	{
		FString Name;
		FString Folder;

		FBuiltWeaponFolder(const FString& Hold)
		{
			Name = FString::Printf(TEXT("ZzPublishTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
			Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / Name);
			FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"schemaVersion\":2,\"name\":\"%s\",\"hold\":\"%s\",\"delivery\":\"sweep\"}"), *Name, *Hold),
				*AMantleWeaponSpawnPoint::BuiltDocumentFile(Name));
		}

		~FBuiltWeaponFolder()
		{
			IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponPublishModelledTest,
	"Mantle.WeaponPublish.LibraryOnly.UploadsTheModelThenMakesAWeaponItem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponPublishModelledTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponPublishTestPrivate;
	const TSharedRef<FFakeMantle> Mantle = MakeShared<FFakeMantle>();
	const TSharedRef<FFakeWeaponSource> Source = MakeShared<FFakeWeaponSource>();
	const TSharedRef<FMantlePlacePublishSession> Session = RunPublish(Source, Mantle, /*bPublic*/ true);
	if (!TestTrue(TEXT("the publish succeeds"), Session->Succeeded()))
	{
		AddError(Session->GetResult().Message);
		return false;
	}
	TestFalse(TEXT("nothing about a place is sent"), AnyPlaceCall(*Mantle));
	TestEqual(TEXT("no place file is written"), Source->ExportCalls, 0);

	const FFakeMantle::FCall* Model = Mantle->LastUploadOfKind(TEXT("skeletalmesh"));
	const FFakeMantle::FCall* Material = Mantle->LastUploadOfKind(TEXT("material"));
	const FFakeMantle::FCall* Weapon = Mantle->LastUploadOfKind(TEXT("weapon"));
	if (!TestNotNull(TEXT("the model is uploaded"), Model) || !TestNotNull(TEXT("its material is uploaded"), Material)
		|| !TestNotNull(TEXT("the weapon is uploaded"), Weapon))
	{
		return false;
	}
	TestTrue(TEXT("the weapon after its model and material"), Weapon > Model && Weapon > Material);
	const TSharedPtr<FJsonObject> Document = ReadObject(Weapon->Body);
	if (!TestTrue(TEXT("the weapon upload is its settings file"), Document.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("it names the uploaded model"), Document->GetStringField(TEXT("mesh")), Model->AssetId);
	const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
	if (TestTrue(TEXT("it names its slot's material"), Document->TryGetArrayField(TEXT("materials"), Materials)) && TestEqual(TEXT("one slot"), Materials->Num(), 1))
	{
		TestEqual(TEXT("by its uploaded id, never the empty local name"), (*Materials)[0]->AsString(), Material->AssetId);
	}

	TestEqual(TEXT("one library item is made"), Mantle->Count(EMantleUploadCall::CreateLibraryItem), 1);
	const FFakeMantle::FCall* Create = Mantle->Calls.FindByPredicate([](const FFakeMantle::FCall& Call) { return Call.Call == EMantleUploadCall::CreateLibraryItem; });
	const TSharedPtr<FJsonObject> Body = Create ? ReadObject(Create->Json) : nullptr;
	if (TestTrue(TEXT("the item body is JSON"), Body.IsValid()))
	{
		TestEqual(TEXT("of kind weapon"), Body->GetStringField(TEXT("kind")), FString(TEXT("weapon")));
		TestEqual(TEXT("named after the weapon"), Body->GetStringField(TEXT("name")), FString(WeaponName));
		TestEqual(TEXT("its primary asset is the weapon upload"), Body->GetStringField(TEXT("primaryAssetId")), Weapon->AssetId);
		FString Visibility;
		TestFalse(TEXT("public, the default: no private visibility is sent"), Body->TryGetStringField(TEXT("visibility"), Visibility) && Visibility == TEXT("private"));
	}
	const TArray<FMantleLibraryVehicleRef>& Refs = Session->GetLibraryRefs();
	if (TestEqual(TEXT("the session reports the item"), Refs.Num(), 1))
	{
		TestEqual(TEXT("version 1"), Refs[0].Version, 1);
		TestEqual(TEXT("remembered under the weapon's folder, as a place go-live remembers it"), Source->Remembered.FindRef(TEXT("/Game/MantleWeapons/TestPublishSword")), Refs[0].ItemId);
	}
	TestTrue(TEXT("the result says it is in the library"), Session->GetResult().Message.Contains(TEXT("is in your library")));

	// Published again with nothing changed: nothing is uploaded and no item or version is added.
	const int32 UploadsBefore = Mantle->Count(EMantleUploadCall::UploadAsset);
	const TSharedRef<FMantlePlacePublishSession> Again = RunPublish(Source, Mantle, /*bPublic*/ true);
	TestTrue(TEXT("the republish succeeds"), Again->Succeeded());
	TestEqual(TEXT("it uploads nothing"), Mantle->Count(EMantleUploadCall::UploadAsset), UploadsBefore);
	TestEqual(TEXT("it makes no second item"), Mantle->Count(EMantleUploadCall::CreateLibraryItem), 1);
	TestEqual(TEXT("and no new version"), Mantle->Count(EMantleUploadCall::CreateLibraryVersion), 0);
	if (TestEqual(TEXT("it reports the same item"), Again->GetLibraryRefs().Num(), 1) && Refs.Num() == 1)
	{
		TestEqual(TEXT("the same item"), Again->GetLibraryRefs()[0].ItemId, Refs[0].ItemId);
		TestEqual(TEXT("at the same version"), Again->GetLibraryRefs()[0].Version, 1);
	}
	TestFalse(TEXT("still nothing about a place"), AnyPlaceCall(*Mantle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponPublishSourceTest,
	"Mantle.WeaponPublish.Source.RefusesAWeaponWithNoModel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponPublishSourceTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponPublishTestPrivate;
	TestFalse(TEXT("a name outside the builder's folder is refused"), FMantleWeaponPublishSource::ProblemWith(TEXT("..\\Elsewhere")).IsEmpty());
	TestTrue(TEXT("a weapon with no settings file is refused in plain words"),
		FMantleWeaponPublishSource::ProblemWith(TEXT("ZzNoSuchWeapon")).Contains(TEXT("Press Create weapon")));

	{
		FBuiltWeaponFolder Sword(TEXT("melee_1h"));
		TestTrue(TEXT("a sword with no 3D model is refused in plain words"), FMantleWeaponPublishSource::ProblemWith(Sword.Name).Contains(TEXT("no 3D model")));
	}

	// A fist weapon is worn on the hand, so it needs its model like any; empty hands are built in, never published.
	FBuiltWeaponFolder Fists(TEXT("unarmed"));
	TestTrue(TEXT("a fist weapon with no 3D model is refused in plain words"), FMantleWeaponPublishSource::ProblemWith(Fists.Name).Contains(TEXT("no 3D model")));
	FMantlePlaceDependencies Dependencies;
	TestFalse(TEXT("and is not gathered"), Dependencies.AddBuiltWeapon(Fists.Name, Fists.Name));
	TestEqual(TEXT("leaving nothing gathered"), Dependencies.Weapons.Num(), 0);
	return true;
}

// The hotbar icon: the weapon's settings file names its library picture by uploaded id, and names none when it has no picture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponPublishIconTest,
	"Mantle.WeaponPublish.IconIsRewrittenToTheUploadedPicture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponPublishIconTest::RunTest(const FString& Parameters)
{
	FMantleConvertedWeapon Weapon;
	Weapon.Key = TEXT("weapon:icon");
	Weapon.Name = TEXT("IconSword");
	Weapon.MeshKey = TEXT("mesh:icon");
	Weapon.DocumentText = TEXT("{\"schemaVersion\":2,\"name\":\"IconSword\",\"mesh\":\"SKM_IconSword\",\"icon\":\"IconSword_Icon_0\"}");

	auto IconOf = [this](const FMantlePlaceAssetSet& Set, const FMantleConvertedWeapon& Converted, FString& OutIcon) -> bool
	{
		TArray<uint8> Bytes;
		FString Error;
		if (!Set.WeaponBytes(Converted, Bytes, Error))
		{
			AddError(Error);
			return false;
		}
		const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Utf8.Length(), Utf8.Get())), Root) || !Root.IsValid())
		{
			AddError(TEXT("the upload is not JSON"));
			return false;
		}
		OutIcon = Root->HasField(TEXT("icon")) ? Root->GetStringField(TEXT("icon")) : FString();
		return true;
	};

	FMantlePlaceAssetSet Set;
	Set.Uploaded.Add(Weapon.MeshKey).Id = TEXT("ast_u00000000000000000000000000000001");
	FString Icon;

	// No picture: no icon, and the local name does not leak.
	if (IconOf(Set, Weapon, Icon))
	{
		TestTrue(TEXT("a weapon without a picture names no icon"), Icon.IsEmpty());
	}

	// A picture that never got an id is the same.
	Weapon.ThumbnailKey = TEXT("weaponpicture:abc");
	if (IconOf(Set, Weapon, Icon))
	{
		TestTrue(TEXT("a picture that was not uploaded names no icon"), Icon.IsEmpty());
	}

	// An uploaded picture: the icon is its id.
	Set.Uploaded.Add(Weapon.ThumbnailKey).Id = TEXT("ast_u00000000000000000000000000000002");
	if (IconOf(Set, Weapon, Icon))
	{
		TestEqual(TEXT("the icon is the uploaded picture's id"), Icon, FString(TEXT("ast_u00000000000000000000000000000002")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
