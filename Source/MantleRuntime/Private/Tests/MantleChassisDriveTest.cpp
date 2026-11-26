// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"
#include "MantleVehicleChassisBuilder.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// #376 off-road follow-on: the builder's drive choice. Front, rear or all-wheel drive rewrites a type's driven axles and keeps its lift-off braking
// car-wide; the type's own drive changes nothing; bikes, aircraft and hovercraft aren't offered it. Named, not anonymous: unity builds.
namespace MantleChassisDriveTestPrivate
{
	struct FComposed
	{
		TArray<bool> Driven;
		double EngineBraking = 0.0;
	};

	bool Compose(const FMantleChassisSpec& Spec, FComposed& Out)
	{
		TSharedPtr<FJsonObject> Doc;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantleChassisSpec::ComposeDocument(Spec)), Doc) || !Doc.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Wheels = nullptr;
		if (!Doc->TryGetArrayField(TEXT("wheels"), Wheels))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Wheel : *Wheels)
		{
			bool bDriven = false;
			Wheel->AsObject()->TryGetBoolField(TEXT("driven"), bDriven);
			Out.Driven.Add(bDriven);
		}
		const TSharedPtr<FJsonObject>* Engine = nullptr;
		if (Doc->TryGetObjectField(TEXT("engine"), Engine))
		{
			(*Engine)->TryGetNumberField(TEXT("engineBraking"), Out.EngineBraking);
		}
		return true;
	}

	int32 DrivenCount(const FComposed& Composed)
	{
		int32 Count = 0;
		for (const bool bDriven : Composed.Driven) Count += bDriven ? 1 : 0;
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisDriveChoiceTest,
	"Mantle.Vehicle.Chassis.DriveChoice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisDriveChoiceTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisDriveTestPrivate;
	struct FCase { const TCHAR* Id; EMantleVehicleDrive Default; };
	// The types' own drives (their chassis.layout.axles): the sedan front-wheel drive (cars step 6), the sports car rear, the offroad all, the six-wheeler
	// both rear axles.
	for (const FCase& Case : { FCase{ TEXT("sedan"), EMantleVehicleDrive::Front }, FCase{ TEXT("sports"), EMantleVehicleDrive::Rear },
			 FCase{ TEXT("offroad"), EMantleVehicleDrive::All }, FCase{ TEXT("sixwheeler"), EMantleVehicleDrive::Rear } })
	{
		FMantleChassisSpec Base;
		FString Error;
		if (!TestTrue(*FString::Printf(TEXT("%s loads (%s)"), Case.Id, *Error), FMantleVehicleChassisBuilder::LoadSpec(Case.Id, Base, Error)))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: the drive is offered"), Case.Id), Base.IsDriveOffered());
		TestTrue(*FString::Printf(TEXT("%s: its own drive is named"), Case.Id), Base.DefaultDrive() == Case.Default);
		FComposed Plain;
		TestTrue(*FString::Printf(TEXT("%s composes"), Case.Id), Compose(Base, Plain));
		// The type's own drive: the spec and its document exactly as before.
		FMantleChassisSpec Same = Base;
		TestTrue(*FString::Printf(TEXT("%s: Type changes nothing"), Case.Id), Same.ApplyDrive(EMantleVehicleDrive::Type));
		TestEqual(*FString::Printf(TEXT("%s: Type composes the same document"), Case.Id), FMantleChassisSpec::ComposeDocument(Same), FMantleChassisSpec::ComposeDocument(Base));
		const int32 Axles = Base.Axles.Num();
		for (const EMantleVehicleDrive Drive : { EMantleVehicleDrive::Front, EMantleVehicleDrive::Rear, EMantleVehicleDrive::All })
		{
			FMantleChassisSpec Spec = Base;
			FComposed Chosen;
			if (!TestTrue(*FString::Printf(TEXT("%s: drive %d applies and composes"), Case.Id, int32(Drive)), Spec.ApplyDrive(Drive) && Compose(Spec, Chosen)))
			{
				continue;
			}
			const int32 WantAxles = Drive == EMantleVehicleDrive::Front ? 1 : Drive == EMantleVehicleDrive::Rear ? Axles - 1 : Axles;
			TestEqual(*FString::Printf(TEXT("%s: drive %d drives %d wheels"), Case.Id, int32(Drive), WantAxles * 2), DrivenCount(Chosen), WantAxles * 2);
			// The front pair is the first two wheels (the frontmost axle).
			TestTrue(*FString::Printf(TEXT("%s: drive %d, front wheels driven exactly when it isn't rear-wheel drive"), Case.Id, int32(Drive)),
				(Chosen.Driven[0] && Chosen.Driven[1]) == (Drive != EMantleVehicleDrive::Rear));
			TestEqual(*FString::Printf(TEXT("%s: drive %d names itself"), Case.Id, int32(Drive)), int32(Spec.DefaultDrive()), int32(Drive));
			// engineBraking is per driven wheel: the car-wide braking stays the type's.
			if (Plain.EngineBraking > 0.0)
			{
				TestTrue(*FString::Printf(TEXT("%s: drive %d keeps the car-wide engine braking (%g x %d vs %g x %d)"), Case.Id, int32(Drive), Chosen.EngineBraking,
					DrivenCount(Chosen), Plain.EngineBraking, DrivenCount(Plain)),
					FMath::IsNearlyEqual(Chosen.EngineBraking * DrivenCount(Chosen), Plain.EngineBraking * DrivenCount(Plain), 1e-6));
			}
			// Resized after the choice, the derived handling carries the scale once (not twice).
			const FMantleChassisSpec Big = FMantleChassisSpec::Scaled(Spec, 1.3);
			const FMantleChassisSpec BigPlain = FMantleChassisSpec::Scaled(Base, 1.3);
			FComposed BigChosen, BigBase;
			if (Compose(Big, BigChosen) && Compose(BigPlain, BigBase) && BigBase.EngineBraking > 0.0)
			{
				TestTrue(*FString::Printf(TEXT("%s@1.3: drive %d scales engine braking once"), Case.Id, int32(Drive)),
					FMath::IsNearlyEqual(BigChosen.EngineBraking * DrivenCount(BigChosen), BigBase.EngineBraking * DrivenCount(BigBase), 1e-6));
			}
		}
	}
	// Not offered where the drive isn't a choice: a bike on its centerline, a plane, a helicopter, a hovercraft.
	for (const TCHAR* Id : { TEXT("motorcycle"), TEXT("plane"), TEXT("helicopter"), TEXT("hovercraft") })
	{
		FMantleChassisSpec Spec;
		FString Error;
		if (!FMantleVehicleChassisBuilder::LoadSpec(Id, Spec, Error))
		{
			AddWarning(FString::Printf(TEXT("%s isn't a preset here (%s)"), Id, *Error));
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s: no drive choice"), Id), Spec.IsDriveOffered());
		TestTrue(*FString::Printf(TEXT("%s: no default drive to name"), Id), Spec.DefaultDrive() == EMantleVehicleDrive::Type);
		const FString Before = FMantleChassisSpec::ComposeDocument(Spec);
		TestFalse(*FString::Printf(TEXT("%s: a choice is refused"), Id), Spec.ApplyDrive(EMantleVehicleDrive::All));
		TestEqual(*FString::Printf(TEXT("%s: and changes nothing"), Id), FMantleChassisSpec::ComposeDocument(Spec), Before);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
