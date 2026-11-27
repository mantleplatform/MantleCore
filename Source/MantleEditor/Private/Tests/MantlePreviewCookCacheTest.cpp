// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewExport.h"
#include "MantleAssetStore.h"
#include "MantlePlaceTypes.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// Test locally's reuse: a cook an earlier export did is read back, keyed on everything the cook tool is given, so a changed asset cooks
// again; and a vehicle the cook refuses is left out with its spawns instead of stopping the export. Named, not anonymous: unity build.
namespace MantlePreviewCookCacheTestPrivate
{
	TArray<uint8> Bytes(const char* Text)
	{
		return TArray<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	}

	/** The result JSON the cook tool writes for Cooked, as the cache keeps it. */
	FString ResultFor(const TArray<uint8>& Cooked)
	{
		return FString::Printf(TEXT("{\"ok\":true,\"cookedHash\":\"%s\",\"cookedBytes\":%d,\"info\":{\"format\":\"MRM2\"}}"),
			*FMantleContentHash::OfBytes(Cooked).ToString(), Cooked.Num());
	}

	/** A fresh folder for one test. */
	FString Folder(const TCHAR* Name)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantlePreviewCookCache"), Name));
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}

	FMantleEntity Spawn(const TCHAR* Id, const TCHAR* Vehicle)
	{
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Name = Id;
		Entity.Type = EMantleEntityType::VehicleSpawn;
		FMantleVehicleSpawnProps Props;
		Props.Vehicle = Vehicle;
		Entity.Props.Set<FMantleVehicleSpawnProps>(MoveTemp(Props));
		return Entity;
	}

	FMantleAssetRef Ref(const TCHAR* Id, const EMantleAssetKind Kind)
	{
		FMantleAssetRef Out;
		Out.Id = Id;
		Out.Kind = Kind;
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewCookCacheKeyTest,
	"Mantle.Preview.CookCache.Key",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewCookCacheKeyTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewCookCacheTestPrivate;
	const TArray<uint8> Input = Bytes("a texture's pixels");
	const FString Key = FMantlePreviewExport::CookCacheKey(TEXT("tool|1|2"), TEXT("texture"), TEXT("{\"role\":\"color\"}"), TEXT("vehicleSeatKind"), Input);
	TestEqual(TEXT("64 hex digits"), Key.Len(), 64);
	TestEqual(TEXT("the same cook, the same key"), FMantlePreviewExport::CookCacheKey(TEXT("tool|1|2"), TEXT("texture"), TEXT("{\"role\":\"color\"}"), TEXT("vehicleSeatKind"), Input), Key);
	// Everything the cook tool is given changes the key: a changed asset, option, client feature set or tool cooks again.
	TArray<uint8> Changed = Input;
	Changed.Last() ^= 1;
	TestNotEqual(TEXT("one byte of the asset changed"), FMantlePreviewExport::CookCacheKey(TEXT("tool|1|2"), TEXT("texture"), TEXT("{\"role\":\"color\"}"), TEXT("vehicleSeatKind"), Changed), Key);
	TestNotEqual(TEXT("another kind"), FMantlePreviewExport::CookCacheKey(TEXT("tool|1|2"), TEXT("mesh"), TEXT("{\"role\":\"color\"}"), TEXT("vehicleSeatKind"), Input), Key);
	TestNotEqual(TEXT("other options"), FMantlePreviewExport::CookCacheKey(TEXT("tool|1|2"), TEXT("texture"), TEXT("{\"role\":\"normal\"}"), TEXT("vehicleSeatKind"), Input), Key);
	TestNotEqual(TEXT("other client features"), FMantlePreviewExport::CookCacheKey(TEXT("tool|1|2"), TEXT("texture"), TEXT("{\"role\":\"color\"}"), TEXT(""), Input), Key);
	TestNotEqual(TEXT("a rebuilt cook tool"), FMantlePreviewExport::CookCacheKey(TEXT("tool|1|3"), TEXT("texture"), TEXT("{\"role\":\"color\"}"), TEXT("vehicleSeatKind"), Input), Key);
	// A field's text can't spill into the next one's.
	TestNotEqual(TEXT("fields stay apart"),
		FMantlePreviewExport::CookCacheKey(TEXT("tool"), TEXT("texture\nkind=mesh"), TEXT(""), TEXT(""), Input),
		FMantlePreviewExport::CookCacheKey(TEXT("tool"), TEXT("texture"), TEXT("kind=mesh"), TEXT(""), Input));

	// The tool's identity: a file that is there, and changes when the file does.
	const FString Dir = Folder(TEXT("Tool"));
	const FString Tool = Dir / TEXT("mantle-cook.exe");
	FFileHelper::SaveStringToFile(TEXT("one"), *Tool);
	const FString Identity = FMantlePreviewExport::CookToolIdentity(Tool);
	TestFalse(TEXT("a tool that is there has an identity"), Identity.IsEmpty());
	FFileHelper::SaveStringToFile(TEXT("a rebuilt tool"), *Tool);
	TestNotEqual(TEXT("a rebuilt tool has another"), FMantlePreviewExport::CookToolIdentity(Tool), Identity);
	TestTrue(TEXT("no tool: nothing is cached"), FMantlePreviewExport::CookToolIdentity(Dir / TEXT("missing.exe")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewCookCacheStoreTest,
	"Mantle.Preview.CookCache.StoreAndLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewCookCacheStoreTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewCookCacheTestPrivate;
	const FString Dir = Folder(TEXT("Store"));
	const TArray<uint8> Cooked = Bytes("MRM2 cooked mesh bytes");
	const FString Key = FMantlePreviewExport::CookCacheKey(TEXT("tool"), TEXT("mesh"), TEXT(""), TEXT(""), Bytes("source"));
	TArray<uint8> Read;
	TSharedPtr<FJsonObject> Result;
	TestFalse(TEXT("nothing kept yet: a miss"), FMantlePreviewExport::LoadCookCache(Dir, Key, Read, Result));
	TestTrue(TEXT("kept"), FMantlePreviewExport::StoreCookCache(Dir, Key, Cooked, ResultFor(Cooked)));
	if (TestTrue(TEXT("read back"), FMantlePreviewExport::LoadCookCache(Dir, Key, Read, Result)))
	{
		TestEqual(TEXT("the same output"), Read, Cooked);
		const TSharedPtr<FJsonObject>* Info = nullptr;
		FString Format;
		TestTrue(TEXT("with the tool's info"), Result->TryGetObjectField(TEXT("info"), Info) && Info && (*Info)->TryGetStringField(TEXT("format"), Format) && Format == TEXT("MRM2"));
	}
	// A damaged entry cooks again: an output that doesn't match its result is a miss.
	FFileHelper::SaveArrayToFile(Bytes("MRM2 cooked mesh bytez"), *(Dir / Key + TEXT(".bin")));
	TestFalse(TEXT("an output that doesn't match its result: a miss"), FMantlePreviewExport::LoadCookCache(Dir, Key, Read, Result));
	TestEqual(TEXT("and nothing read"), Read.Num(), 0);
	// A refusal is never kept: a result that isn't a success is a miss even with its output there.
	FMantlePreviewExport::StoreCookCache(Dir, Key, Cooked, TEXT("{\"ok\":false,\"error\":\"refused\"}"));
	TestFalse(TEXT("a result that isn't a success: a miss"), FMantlePreviewExport::LoadCookCache(Dir, Key, Read, Result));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewCookCachePruneTest,
	"Mantle.Preview.CookCache.Prune",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewCookCachePruneTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewCookCacheTestPrivate;
	const FString Dir = Folder(TEXT("Prune"));
	const TArray<uint8> Cooked = Bytes("0123456789012345678901234567890123456789");
	const FString Old = FMantlePreviewExport::CookCacheKey(TEXT("tool"), TEXT("mesh"), TEXT(""), TEXT(""), Bytes("old"));
	const FString Mid = FMantlePreviewExport::CookCacheKey(TEXT("tool"), TEXT("mesh"), TEXT(""), TEXT(""), Bytes("mid"));
	const FString New = FMantlePreviewExport::CookCacheKey(TEXT("tool"), TEXT("mesh"), TEXT(""), TEXT(""), Bytes("new"));
	const FDateTime Now = FDateTime::UtcNow();
	int32 Age = 3;
	for (const FString& Key : { Old, Mid, New })
	{
		FMantlePreviewExport::StoreCookCache(Dir, Key, Cooked, ResultFor(Cooked));
		IFileManager::Get().SetTimeStamp(*(Dir / Key + TEXT(".json")), Now - FTimespan::FromHours(Age--));
	}
	const int64 Entry = IFileManager::Get().FileSize(*(Dir / Old + TEXT(".json"))) + Cooked.Num();
	FMantlePreviewExport::PruneCookCache(Dir, Entry * 3);
	TArray<uint8> Read;
	TSharedPtr<FJsonObject> Result;
	TestTrue(TEXT("under the cap: all kept"), FMantlePreviewExport::LoadCookCache(Dir, Old, Read, Result));
	// That read made the oldest the newest; the prune now drops what was used longest ago.
	FMantlePreviewExport::PruneCookCache(Dir, Entry * 2);
	TestFalse(TEXT("over the cap: the least recently used goes"), FMantlePreviewExport::LoadCookCache(Dir, Mid, Read, Result));
	TestTrue(TEXT("the one just used stays"), FMantlePreviewExport::LoadCookCache(Dir, Old, Read, Result));
	TestTrue(TEXT("and the newest"), FMantlePreviewExport::LoadCookCache(Dir, New, Read, Result));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewDropVehiclesTest,
	"Mantle.Preview.DropRefusedVehicles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewDropVehiclesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePreviewCookCacheTestPrivate;
	const TCHAR* Refused = TEXT("ast_u00000000000000000000000000000001");
	const TCHAR* Fine = TEXT("ast_u00000000000000000000000000000002");
	FMantlePlace Place;
	Place.Entities.Add(Spawn(TEXT("ent_f150_a"), Refused));
	Place.Entities.Add(Spawn(TEXT("ent_f150_b"), Refused));
	Place.Entities.Add(Spawn(TEXT("ent_civic"), Fine));
	FMantleEntity Track;
	Track.Id = TEXT("ent_track");
	Track.Name = TEXT("Track");
	Place.Entities.Add(Track);
	Place.Assets.Add(Ref(Refused, EMantleAssetKind::Vehicle));
	Place.Assets.Add(Ref(Fine, EMantleAssetKind::Vehicle));
	Place.Assets.Add(Ref(TEXT("ast_u00000000000000000000000000000003"), EMantleAssetKind::StaticMesh));

	FMantlePlace Attached = Place;
	FMantleEntity Flag;
	Flag.Id = TEXT("ent_flag");
	Flag.Name = TEXT("Flag");
	Flag.Parent = TEXT("ent_f150_a");
	Attached.Entities.Add(Flag);

	FString Error;
	TestEqual(TEXT("both spawns of the refused vehicle go"), FMantlePreviewExport::DropVehicles(Place, TSet<FString>{ FString(Refused) }, Error), 2);
	TestEqual(TEXT("the other spawn and the track stay"), Place.Entities.Num(), 2);
	TestTrue(TEXT("the civic's spawn is one of them"), Place.Entities.ContainsByPredicate([](const FMantleEntity& E) { return E.Id == TEXT("ent_civic"); }));
	TestFalse(TEXT("the refused vehicle's asset entry goes"), Place.Assets.ContainsByPredicate([Refused](const FMantleAssetRef& R) { return R.Id == Refused; }));
	TestEqual(TEXT("the others stay"), Place.Assets.Num(), 2);
	TestEqual(TEXT("nothing to drop: nothing goes"), FMantlePreviewExport::DropVehicles(Place, TSet<FString>{ FString(TEXT("ast_u00000000000000000000000000000009")) }, Error), 0);

	TestEqual(TEXT("something attached to a dropped spawn can't stay without it"), FMantlePreviewExport::DropVehicles(Attached, TSet<FString>{ FString(Refused) }, Error), -1);
	TestTrue(TEXT("and the creator is told what"), Error.Contains(TEXT("Flag")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
