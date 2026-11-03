// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceStorage.h"
#include "MantlePlaceTypes.h"
#include "MantleWheelAsset.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Wheels, the client's asset-preparation half: the strict MWL1 document reader, and the place's storage keeping wheel
// entries in storage order. No network, no world.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWheelAssetTestPrivate
{
	const TCHAR* const AssetMesh = TEXT("ast_u0123456789abcdef0123456789abcdef");
	const TCHAR* const AssetMatA = TEXT("ast_u11111111111111111111111111111111");
	const TCHAR* const AssetMatB = TEXT("ast_u22222222222222222222222222222222");
	const TCHAR* const AssetWheelA = TEXT("ast_u33333333333333333333333333333333");
	const TCHAR* const AssetWheelB = TEXT("ast_u44444444444444444444444444444444");
	const TCHAR* const AssetTruck = TEXT("ast_u55555555555555555555555555555555");
	const TCHAR* const ItemWheelA = TEXT("itm_u0000000000000000000000000000000a");
	const TCHAR* const ItemWheelB = TEXT("itm_u0000000000000000000000000000000b");
	const TCHAR* const ItemTruck = TEXT("itm_u0000000000000000000000000000000c");

	/** A full valid document: every field, two materials, both slots painted. */
	FString FullDocument()
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"name\":\"Deep dish\",\"mesh\":\"%s\",\"materials\":[\"%s\",\"%s\"],")
				TEXT("\"referenceRadius\":34.5,\"referenceWidth\":22,\"wheelSlots\":[0,1]}"),
			AssetMesh, AssetMatA, AssetMatB);
	}

	/** True when some problem is located at Pointer. */
	bool HasErrorAt(const TArray<FMantleWheelDocError>& Errors, const TCHAR* Pointer)
	{
		return Errors.ContainsByPredicate([Pointer](const FMantleWheelDocError& Error)
		{
			return Error.Pointer.Equals(Pointer, ESearchCase::CaseSensitive);
		});
	}

	FMantleStorageEntry Declared(const TCHAR* Name, const TCHAR* Item, const int32 Version)
	{
		FMantleStorageEntry Entry;
		Entry.Name = Name;
		Entry.Item = Item;
		Entry.Version = Version;
		return Entry;
	}

	FMantleResolvedStorageEntry Resolved(const TCHAR* Item, const int32 Version, const TCHAR* Kind, const TCHAR* Primary)
	{
		FMantleResolvedStorageEntry Entry;
		Entry.Item = Item;
		Entry.Version = Version;
		Entry.Kind = Kind;
		Entry.Status = TEXT("active");
		Entry.PrimaryAssetId = Primary;
		return Entry;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWheelDocumentAcceptsTest,
	"Mantle.Wheel.Document.AcceptsFullDocument",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWheelDocumentAcceptsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWheelAssetTestPrivate;
	FMantleWheelDesc Desc;
	TArray<FMantleWheelDocError> Errors;
	if (!TestTrue(TEXT("a full valid document parses"), FMantleWheelDocument::Parse(FullDocument(), Desc, Errors)))
	{
		for (const FMantleWheelDocError& Error : Errors)
		{
			AddError(Error.ToString());
		}
		return false;
	}
	TestEqual(TEXT("name"), Desc.Name, FString(TEXT("Deep dish")));
	TestEqual(TEXT("mesh"), Desc.MeshId, FString(AssetMesh));
	if (TestEqual(TEXT("two materials"), Desc.MaterialIds.Num(), 2))
	{
		TestEqual(TEXT("first material"), Desc.MaterialIds[0], FString(AssetMatA));
		TestEqual(TEXT("second material"), Desc.MaterialIds[1], FString(AssetMatB));
	}
	TestEqual(TEXT("reference radius"), Desc.ReferenceRadius, 34.5);
	TestEqual(TEXT("reference width"), Desc.ReferenceWidth, 22.0);
	TestTrue(TEXT("wheel slots"), Desc.WheelSlots == TArray<int32>({ 0, 1 }));

	// materials and wheelSlots are optional.
	const FString Minimal = FString::Printf(
		TEXT("{\"schemaVersion\":1,\"name\":\"Plain\",\"mesh\":\"%s\",\"referenceRadius\":5,\"referenceWidth\":100}"), AssetMesh);
	FMantleWheelDesc Plain;
	TestTrue(TEXT("a document without materials and wheelSlots parses"), FMantleWheelDocument::Parse(Minimal, Plain, Errors));
	TestEqual(TEXT("no materials"), Plain.MaterialIds.Num(), 0);
	TestEqual(TEXT("no wheel slots"), Plain.WheelSlots.Num(), 0);

	// The cooked bytes read the same way, and name their mesh and materials.
	const FTCHARToUTF8 Utf8(*FullDocument());
	FMantleWheelAsset Asset;
	FString Error;
	if (TestTrue(TEXT("the cooked bytes read"), FMantleWheelAsset::Read(
		TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Asset, Error)))
	{
		TestEqual(TEXT("the asset names the mesh and both materials"), Asset.NamedIds().Num(), 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWheelDocumentRefusesTest,
	"Mantle.Wheel.Document.RefusesBadDocuments",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWheelDocumentRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantleWheelAssetTestPrivate;
	struct FCase
	{
		const TCHAR* Label;
		FString Json;
		const TCHAR* Pointer;
	};
	const FString MeshField = FString::Printf(TEXT("\"mesh\":\"%s\""), AssetMesh);
	const auto Body = [](const FString& Mesh, const FString& Radius, const FString& Extra)
	{
		return FString::Printf(TEXT("{\"schemaVersion\":1,\"name\":\"W\",%s,\"referenceRadius\":%s,\"referenceWidth\":20%s}"),
			*Mesh, *Radius, *Extra);
	};
	const TArray<FCase> Cases = {
		{ TEXT("an unknown key"), Body(MeshField, TEXT("30"), TEXT(",\"spokes\":5")), TEXT("/spokes") },
		{ TEXT("a mesh that is not an uploaded id"), Body(TEXT("\"mesh\":\"ast_cube\""), TEXT("30"), FString()), TEXT("/mesh") },
		{ TEXT("a material that is not an uploaded id"), Body(MeshField, TEXT("30"), TEXT(",\"materials\":[\"ast_mat_basic\"]")), TEXT("/materials/0") },
		{ TEXT("a radius below the range"), Body(MeshField, TEXT("4.9"), FString()), TEXT("/referenceRadius") },
		{ TEXT("a radius above the range"), Body(MeshField, TEXT("150.5"), FString()), TEXT("/referenceRadius") },
		{ TEXT("a width above the range"),
			FString::Printf(TEXT("{\"schemaVersion\":1,\"name\":\"W\",%s,\"referenceRadius\":30,\"referenceWidth\":101}"), *MeshField),
			TEXT("/referenceWidth") },
		{ TEXT("a duplicate wheel slot"), Body(MeshField, TEXT("30"), TEXT(",\"wheelSlots\":[1,1]")), TEXT("/wheelSlots/1") },
		{ TEXT("a fractional wheel slot"), Body(MeshField, TEXT("30"), TEXT(",\"wheelSlots\":[0.5]")), TEXT("/wheelSlots/0") },
		{ TEXT("a negative wheel slot"), Body(MeshField, TEXT("30"), TEXT(",\"wheelSlots\":[-1]")), TEXT("/wheelSlots/0") },
		{ TEXT("a wheel slot past the listed materials"),
			FString::Printf(TEXT("{\"schemaVersion\":1,\"name\":\"W\",\"mesh\":\"%s\",\"materials\":[\"%s\"],\"referenceRadius\":30,\"referenceWidth\":20,\"wheelSlots\":[1]}"),
				AssetMesh, AssetMatA),
			TEXT("/wheelSlots/0") },
		{ TEXT("a wrong schema version"), FString::Printf(TEXT("{\"schemaVersion\":2,\"name\":\"W\",%s,\"referenceRadius\":30,\"referenceWidth\":20}"), *MeshField),
			TEXT("/schemaVersion") },
		{ TEXT("an empty name"), FString::Printf(TEXT("{\"schemaVersion\":1,\"name\":\"\",%s,\"referenceRadius\":30,\"referenceWidth\":20}"), *MeshField),
			TEXT("/name") },
		{ TEXT("no radius"), FString::Printf(TEXT("{\"schemaVersion\":1,\"name\":\"W\",%s,\"referenceWidth\":20}"), *MeshField),
			TEXT("/referenceRadius") },
		{ TEXT("text that is not JSON"), TEXT("not json"), TEXT("") },
	};
	for (const FCase& Case : Cases)
	{
		FMantleWheelDesc Desc;
		TArray<FMantleWheelDocError> Errors;
		const bool bParsed = FMantleWheelDocument::Parse(Case.Json, Desc, Errors);
		TestFalse(FString::Printf(TEXT("%s is refused"), Case.Label), bParsed);
		TestTrue(FString::Printf(TEXT("%s is reported at '%s'"), Case.Label, Case.Pointer), HasErrorAt(Errors, Case.Pointer));
	}

	// Bytes that are not a document are refused by the asset reader too.
	FMantleWheelAsset Asset;
	FString Error;
	TestFalse(TEXT("no bytes are refused"), FMantleWheelAsset::Read(TConstArrayView<uint8>(), Asset, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWheelStorageTest,
	"Mantle.Wheel.Storage.KeepsWheelsInStorageOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWheelStorageTest::RunTest(const FString& Parameters)
{
	using namespace MantleWheelAssetTestPrivate;
	FMantlePlace Place;
	Place.PlaceId = TEXT("plc_wheel_storage_test");
	// Declared: Rim B, a truck, Rim A. The answer lists them in another order, as the backend may.
	Place.Storage.Add(Declared(TEXT("RimB"), ItemWheelB, 1));
	Place.Storage.Add(Declared(TEXT("Truck"), ItemTruck, 1));
	Place.Storage.Add(Declared(TEXT("RimA"), ItemWheelA, 3));

	FMantleStorageExpansion Expansion;
	Expansion.Entries.Add(Resolved(ItemWheelA, 3, TEXT("wheel"), AssetWheelA));
	Expansion.Entries.Add(Resolved(ItemTruck, 1, TEXT("vehicle"), AssetTruck));
	Expansion.Entries.Add(Resolved(ItemWheelB, 1, TEXT("wheel"), AssetWheelB));
	FMantleStorage::Apply(Place, Expansion);

	TestEqual(TEXT("the wheels and the truck are kept"), Place.StorageResolved.Num(), 3);
	const TArray<FMantleStoredWheel> Wheels = FMantleStorage::StoredWheels(Place);
	if (TestEqual(TEXT("two stored wheels"), Wheels.Num(), 2))
	{
		TestEqual(TEXT("the first is the first declared"), Wheels[0].Name, FString(TEXT("RimB")));
		TestEqual(TEXT("with its document's asset id"), Wheels[0].AssetId, FString(AssetWheelB));
		TestEqual(TEXT("the second is the last declared"), Wheels[1].Name, FString(TEXT("RimA")));
		TestEqual(TEXT("with its document's asset id"), Wheels[1].AssetId, FString(AssetWheelA));
	}
	const TArray<FString> Ids = FMantleStorage::StoredWheelAssetIds(Place);
	TestTrue(TEXT("the asset ids are in storage order"), Ids == TArray<FString>({ FString(AssetWheelB), FString(AssetWheelA) }));
	TestEqual(TEXT("the vehicle is not a wheel"), FMantleStorage::StoredVehicleAssetIds(Place).Num(), 1);

	// A wheel without an uploaded primary asset id is not kept.
	FMantlePlace Bad;
	Bad.Storage.Add(Declared(TEXT("RimA"), ItemWheelA, 3));
	FMantleStorageExpansion BadExpansion;
	BadExpansion.Entries.Add(Resolved(ItemWheelA, 3, TEXT("wheel"), TEXT("ast_cube")));
	FMantleStorage::Apply(Bad, BadExpansion);
	TestEqual(TEXT("a wheel with a non-uploaded primary id is dropped"), FMantleStorage::StoredWheels(Bad).Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
