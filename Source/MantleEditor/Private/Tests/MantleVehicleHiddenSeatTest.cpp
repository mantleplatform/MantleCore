// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleAsset.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// A seat that is hidden is for a model with no inside: the document's seat.hidden and passengerSeat.hidden, written only
// when set and read back into the desc, and refused when not a true or false.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleHiddenSeatTestPrivate
{
	/** A transient source with a placed driver seat and a passenger seat, the two hide flags as given. */
	UMantleVehicleSource* MakeSource(const bool bHideDriver, const bool bHidePassenger)
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		Source->Tuning.bSeatLocation = true;
		Source->Tuning.SeatLocation = FVector(-40.0, -34.0, 75.0);
		Source->Tuning.bPassengerSeat = true;
		Source->Tuning.PassengerSeatLocation = FVector(-40.0, 34.0, 75.0);
		Source->Tuning.bHideDriver = bHideDriver;
		Source->Tuning.bHidePassenger = bHidePassenger;
		return Source;
	}

	/**
	 * A resolved vehicle document (the schema's own vehicle-v1 fixture, with a passenger seat added), its seat and passenger seat
	 * given the extra text after their location, such as ,'hidden':true. Single quotes stand for double quotes.
	 */
	TArray<uint8> ResolvedBytes(const FString& SeatExtra, const FString& PassengerExtra)
	{
		FString Text = TEXT("{'schemaVersion':1,'mass':1500,'maxSpeed':4000,'dragCoefficient':0.3,'frontalArea':2.2,")
			TEXT("'engine':{'maxTorque':400,'maxRPM':6000,'idleRPM':900,'torqueCurve':[[0,0.6],[3000,1],[6000,0.8]]},")
			TEXT("'transmission':{'gearRatios':[3.2,2.1,1.4,1],'reverseRatio':3,'finalDrive':3.5,'changeUpRPM':5100,'changeDownRPM':2400},")
			TEXT("'suspension':{'maxRaise':10,'maxDrop':10,'springRate':250,'springPreload':50,'damping':0.5},")
			TEXT("'steering':{'maxAngle':35,'ackermann':true},'brakes':{'brakeTorque':1500,'handbrakeTorque':3000},'tires':{'friction':2},")
			TEXT("'wheels':[")
			TEXT("{'bone':'Wheel_FL','steering':true,'driven':false,'handbrake':false,'radius':35,'width':20},")
			TEXT("{'bone':'Wheel_FR','steering':true,'driven':false,'handbrake':false,'radius':35,'width':20},")
			TEXT("{'bone':'Wheel_BL','steering':false,'driven':true,'handbrake':true,'radius':35,'width':20},")
			TEXT("{'bone':'Wheel_BR','steering':false,'driven':true,'handbrake':true,'radius':35,'width':20}],")
			TEXT("'seat':{'location':[-40,-34,75]@SEAT@},'passengerSeat':{'location':[-40,34,75]@PASSENGER@},")
			TEXT("'camera':{'armLength':650,'height':150},'collision':[{'center':[0,0,80],'extent':[200,85,50]}],")
			TEXT("'materials':{'M_Body':'ast_u0123456789abcdef0123456789abcdef'}}");
		Text = Text.Replace(TEXT("@SEAT@"), *SeatExtra).Replace(TEXT("@PASSENGER@"), *PassengerExtra).Replace(TEXT("'"), TEXT("\""));
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleHiddenSeatDocumentTest,
	"Mantle.Vehicle.HiddenSeat.Document",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleHiddenSeatDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleHiddenSeatTestPrivate;

	// Written only when set: a vehicle that hides nobody writes the same document it always did.
	{
		const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*MakeSource(true, true), TMap<FString, FString>());
		bool bHidden = false;
		TestTrue(TEXT("a hidden driver writes seat.hidden"), Doc->GetObjectField(TEXT("seat"))->TryGetBoolField(TEXT("hidden"), bHidden) && bHidden);
		bHidden = false;
		TestTrue(TEXT("a hidden passenger writes passengerSeat.hidden"),
			Doc->GetObjectField(TEXT("passengerSeat"))->TryGetBoolField(TEXT("hidden"), bHidden) && bHidden);
	}
	{
		const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*MakeSource(false, false), TMap<FString, FString>());
		TestFalse(TEXT("a visible driver writes no seat.hidden"), Doc->GetObjectField(TEXT("seat"))->HasField(TEXT("hidden")));
		TestFalse(TEXT("a visible passenger writes no passengerSeat.hidden"), Doc->GetObjectField(TEXT("passengerSeat"))->HasField(TEXT("hidden")));
	}
	{
		const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*MakeSource(true, false), TMap<FString, FString>());
		TestTrue(TEXT("hiding only the driver writes seat.hidden"), Doc->GetObjectField(TEXT("seat"))->HasField(TEXT("hidden")));
		TestFalse(TEXT("and no passengerSeat.hidden"), Doc->GetObjectField(TEXT("passengerSeat"))->HasField(TEXT("hidden")));
	}

	// Read back into the desc.
	{
		FMantleVehicleDesc Desc;
		FString Error;
		TestTrue(TEXT("a document with both seats hidden is read"),
			FMantleVehicleAssetReader::ParseDocument(ResolvedBytes(TEXT(",'hidden':true"), TEXT(",'hidden':true")), Desc, Error));
		TestTrue(TEXT("the driver is hidden in the desc"), Desc.bSeatHidden);
		TestTrue(TEXT("the passenger is hidden in the desc"), Desc.bPassengerSeatHidden);
	}
	{
		FMantleVehicleDesc Desc;
		FString Error;
		TestTrue(TEXT("a document with no hidden keys is read"),
			FMantleVehicleAssetReader::ParseDocument(ResolvedBytes(TEXT(""), TEXT("")), Desc, Error));
		TestFalse(TEXT("the driver is not hidden by default"), Desc.bSeatHidden);
		TestFalse(TEXT("the passenger is not hidden by default"), Desc.bPassengerSeatHidden);
	}

	// A value that is not true or false is refused, naming the key.
	{
		FMantleVehicleDesc Desc;
		FString Error;
		TestFalse(TEXT("a seat.hidden that is not a boolean is refused"),
			FMantleVehicleAssetReader::ParseDocument(ResolvedBytes(TEXT(",'hidden':1"), TEXT("")), Desc, Error));
		TestTrue(TEXT("and the refusal names the key"), Error.Contains(TEXT("hidden")));
		Error.Reset();
		TestFalse(TEXT("a passengerSeat.hidden that is not a boolean is refused"),
			FMantleVehicleAssetReader::ParseDocument(ResolvedBytes(TEXT(""), TEXT(",'hidden':'yes'")), Desc, Error));
		TestTrue(TEXT("and the refusal names the key too"), Error.Contains(TEXT("hidden")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
