// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// #136: the face slots (ears, eyes, nose) hang from the MetaHuman face skeleton's FACIAL_* bones on the face mesh, at identity;
// without that bone (no face, or not a MetaHuman face) they fall back to the body's head. Body slots stay on the body. On a copy
// of the plugin's own contract; no assets. The bone names here stand in until the lead's list lands in limits.json.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAccessoryFaceBoneTestPrivate
{
	/** The plugin's contract copied to a fresh folder, with Edit applied to each limits.json slot. Empty on failure. */
	FString CopyContract(const TFunctionRef<void(const TSharedPtr<FJsonObject>& Slot, const FString& Id)>& Edit)
	{
		const FString From = FMantleAvatarContract::DefaultDirectory();
		const FString To = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("MantleTests"), TEXT("FaceBones"), FGuid::NewGuid().ToString());
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(From, TEXT("*.json")), true, false);
		for (const FString& File : Files)
		{
			if (IFileManager::Get().Copy(*FPaths::Combine(To, File), *FPaths::Combine(From, File)) != COPY_OK) return FString();
		}
		const FString Limits = FPaths::Combine(To, TEXT("limits.json"));
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *Limits) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) return FString();
		const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
		if (!Root->TryGetArrayField(TEXT("slots"), Slots)) return FString();
		for (const TSharedPtr<FJsonValue>& Value : *Slots)
		{
			const TSharedPtr<FJsonObject> Slot = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Id;
			if (Slot.IsValid() && Slot->TryGetStringField(TEXT("id"), Id)) Edit(Slot, Id);
		}
		FString Out;
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Out)) || !FFileHelper::SaveStringToFile(Out, *Limits)) return FString();
		return To;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAccessoryFaceBoneTest,
	"Mantle.Editor.AccessorySockets.FaceBones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAccessoryFaceBoneTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryFaceBoneTestPrivate;
	const FName EarBone(TEXT("FACIAL_L_Ear"));

	// limits.json's "mesh": "face" puts a slot's bone on the face; a slot without it is on the body.
	const FString Directory = CopyContract([](const TSharedPtr<FJsonObject>& Slot, const FString& Id)
	{
		if (Id != TEXT("acc_ear_l")) return;
		Slot->SetStringField(TEXT("mesh"), TEXT("face"));
		Slot->SetStringField(TEXT("socketBone"), TEXT("FACIAL_L_Ear"));
	});
	if (!TestFalse(TEXT("the contract copies"), Directory.IsEmpty())) return false;
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };
	FMantleAvatarContract Contract;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the contract with a face slot loads (%s)"), *Error), FMantleAvatarContract::LoadFromDirectory(Directory, Contract, Error))) return false;
	const FMantleContractSlot* Ear = Contract.FindSlot(TEXT("acc_ear_l"));
	const FMantleContractSlot* Hat = Contract.FindSlot(TEXT("acc_hat"));
	if (!TestNotNull(TEXT("acc_ear_l is there"), Ear) || !TestNotNull(TEXT("acc_hat is there"), Hat)) return false;
	TestTrue(TEXT("acc_ear_l is on the face"), Ear->IsOnFace());
	TestEqual(TEXT("on its FACIAL bone"), Ear->SocketBone, FString(TEXT("FACIAL_L_Ear")));
	TestFalse(TEXT("acc_hat, with no mesh, is on the body"), Hat->IsOnFace());
	TestEqual(TEXT("a slot with no mesh says body"), Hat->Mesh, FString(TEXT("body")));

	// A face that has the bone carries the slot there.
	const FMantleAccessoryBone OnFace = FMantleAccessorySettings::ResolveBone(*Ear, [&EarBone](FName Bone) { return Bone == EarBone; });
	TestTrue(TEXT("with the bone on the face: on the face"), OnFace.bOnFace);
	TestTrue(TEXT("on FACIAL_L_Ear"), OnFace.Bone == EarBone);
	TestFalse(TEXT("not a fallback"), OnFace.bFellBack);

	// A face without it (not a MetaHuman face), or no face at all: the body's head.
	const FMantleAccessoryBone NoBone = FMantleAccessorySettings::ResolveBone(*Ear, [](FName) { return false; });
	TestFalse(TEXT("a face without the bone: on the body"), NoBone.bOnFace);
	TestTrue(TEXT("on the head"), NoBone.Bone == FName(FMantleAccessorySettings::FaceFallbackBone()));
	TestTrue(TEXT("as a fallback"), NoBone.bFellBack);
	const FMantleAccessoryBone NoFace = FMantleAccessorySettings::ResolveBone(*Ear, static_cast<const USkeletalMesh*>(nullptr));
	TestTrue(TEXT("no face: the head on the body, as a fallback"), !NoFace.bOnFace && NoFace.bFellBack && NoFace.Bone == FName(TEXT("head")));
	FTransform Nowhere;
	TestFalse(TEXT("no face and no body: nothing to place on"), FMantleAccessorySettings::BoneInComponent(*Ear, nullptr, nullptr, Nowhere));

	// A body slot stays on its body bone even when the face has a bone of that name.
	const FMantleAccessoryBone HatBone = FMantleAccessorySettings::ResolveBone(*Hat, [](FName) { return true; });
	TestTrue(TEXT("acc_hat stays on the body's head"), !HatBone.bOnFace && !HatBone.bFellBack && HatBone.Bone == FName(*Hat->SocketBone));

	// A mesh that is neither body nor face fails the contract, naming the slot.
	const FString Broken = CopyContract([](const TSharedPtr<FJsonObject>& Slot, const FString& Id)
	{
		if (Id == TEXT("acc_nose")) Slot->SetStringField(TEXT("mesh"), TEXT("hands"));
	});
	if (!TestFalse(TEXT("the broken contract copies"), Broken.IsEmpty())) return false;
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Broken, false, true); };
	FMantleAvatarContract Refused;
	FString Why;
	TestFalse(TEXT("an unknown mesh fails the contract"), FMantleAvatarContract::LoadFromDirectory(Broken, Refused, Why));
	TestTrue(FString::Printf(TEXT("and says which slot (%s)"), *Why), Why.Contains(TEXT("acc_nose")));
	return true;
}

#endif
