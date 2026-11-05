// Copyright Joshua Eger. All Rights Reserved.
#include "Dom/JsonObject.h"
#include "MantleVehicleAsset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// A creator car's hood and two rear doors, opened from outside as the trunk is. The cook's own fixture (backend
// TestVehicleOpeningsFixtureMatchesWhatWeWriteToday) is the car parts car with a hood, a left rear door and a right rear door.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleOpeningsAssetTest,
	"Mantle.Vehicle.Asset.ReadsOpenings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleOpeningsAssetTest::RunTest(const FString&)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-car-openings-v1.mvh")));
	TArray<uint8> Bytes;
	if (!TestTrue(*FString::Printf(TEXT("the openings fixture exists (%s)"), *Path), FFileHelper::LoadFileToArray(Bytes, *Path)))
	{
		return false;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the reader takes a vehicle with a hood and rear doors (%s)"), *Error), FMantleVehicleAssetReader::Read(Bytes, Asset, Error)))
	{
		return false;
	}
	const FMantleVehicleDesc& D = Asset.Desc;
	TestTrue(TEXT("the trunk reads as before"), D.bTrunk);

	TestTrue(TEXT("the hood is set"), D.Hood.bSet);
	TestEqual(TEXT("the hood's bone"), D.Hood.Hinge.Bone, FName(TEXT("Hood")));
	TestEqual(TEXT("the hood's pivot"), D.Hood.Hinge.Pivot, FVector(110.0, 0.0, 95.0));
	TestEqual(TEXT("the hood's axis"), D.Hood.Hinge.Axis, FVector(0.0, -1.0, 0.0));
	TestEqual(TEXT("the hood's scale"), D.Hood.Hinge.Scale, 1.0);
	TestEqual(TEXT("the hood's stand is in front of the car"), D.Hood.Stand, FVector(260.0, 0.0, 0.0));
	TestEqual(TEXT("the hood's yaw faces the car"), D.Hood.Yaw, 180.0);

	TestTrue(TEXT("the left rear door is set"), D.RearLeftDoor.bSet);
	TestEqual(TEXT("the left rear door's bone is the body's spelling"), D.RearLeftDoor.Hinge.Bone, FName(TEXT("Door_RL")));
	TestEqual(TEXT("the left rear door's stand is on the -Y side"), D.RearLeftDoor.Stand, FVector(-60.0, -130.0, 0.0));
	TestEqual(TEXT("the left rear door's yaw"), D.RearLeftDoor.Yaw, 90.0);

	TestTrue(TEXT("the right rear door is set"), D.RearRightDoor.bSet);
	TestEqual(TEXT("the right rear door's bone"), D.RearRightDoor.Hinge.Bone, FName(TEXT("Door_RR")));
	TestEqual(TEXT("the right rear door's scale"), D.RearRightDoor.Hinge.Scale, 0.9);
	TestEqual(TEXT("the right rear door's stand is on the +Y side"), D.RearRightDoor.Stand, FVector(-60.0, 130.0, 0.0));
	TestEqual(TEXT("the right rear door's yaw"), D.RearRightDoor.Yaw, 0.0);

	// The earlier car parts fixture has none of them and still reads.
	const FString OldPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../schema/fixtures/vehicle-car-v1.mvh")));
	TArray<uint8> Old;
	FMantleVehicleAsset OldCar;
	if (TestTrue(TEXT("the car parts fixture exists"), FFileHelper::LoadFileToArray(Old, *OldPath)) && TestTrue(TEXT("and reads"), FMantleVehicleAssetReader::Read(Old, OldCar, Error)))
	{
		TestFalse(TEXT("no hood"), OldCar.Desc.Hood.bSet);
		TestFalse(TEXT("no left rear door"), OldCar.Desc.RearLeftDoor.bSet);
		TestFalse(TEXT("no right rear door"), OldCar.Desc.RearRightDoor.bSet);
	}

	// The reader holds the document to the schema: an opening needs its stand, its yaw within half a turn, and a bone of its own.
	const uint32 DocumentBytes = *reinterpret_cast<const uint32*>(Bytes.GetData() + 8);
	FString Json;
	FFileHelper::BufferToString(Json, Bytes.GetData() + 12, static_cast<int32>(DocumentBytes));
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("the fixture's document parses as JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid()))
	{
		return false;
	}
	const auto Parses = [&](const TFunctionRef<void(const TSharedRef<FJsonObject>&)>& Change)
	{
		// Each case starts from a fresh copy of the fixture's document.
		TSharedPtr<FJsonObject> Fresh;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Fresh);
		const TSharedRef<FJsonObject> Doc = Fresh.ToSharedRef();
		Change(Doc);
		FString Out;
		FJsonSerializer::Serialize(Doc, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
		const FTCHARToUTF8 Utf8(*Out);
		FMantleVehicleDesc Desc;
		FString Why;
		return FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Desc, Why);
	};
	TestTrue(TEXT("the unchanged document parses"), Parses([](const TSharedRef<FJsonObject>&) {}));
	TestFalse(TEXT("a hood without a stand is refused"), Parses([](const TSharedRef<FJsonObject>& Doc)
	{
		Doc->GetObjectField(TEXT("hood"))->RemoveField(TEXT("stand"));
	}));
	TestFalse(TEXT("a rear door yawed past half a turn is refused"), Parses([](const TSharedRef<FJsonObject>& Doc)
	{
		Doc->GetObjectField(TEXT("rearLeftDoor"))->SetNumberField(TEXT("yaw"), 200.0);
	}));
	TestFalse(TEXT("a rear door on the hood's bone is refused"), Parses([](const TSharedRef<FJsonObject>& Doc)
	{
		Doc->GetObjectField(TEXT("rearRightDoor"))->GetObjectField(TEXT("door"))->SetStringField(TEXT("bone"), TEXT("hood"));
	}));
	return true;
}

#endif
