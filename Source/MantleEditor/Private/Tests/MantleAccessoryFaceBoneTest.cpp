// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticPackage.h"
#include "MantleParametricOutfit.h"

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

// #136: the face slots (ears, eyes, nose) hang from the MetaHuman face skeleton's FACIAL_* bones on the face mesh, at identity.
// There is no fallback: an avatar whose skeleton (or face) lacks a slot's bone does not have that slot, so the face slots exist
// only with a MetaHuman face, while wrist and ring are on every body skeleton. On the plugin's own contract (limits.json's "bone",
// "mesh" and "fingerBones"), and a broken copy of it; no assets. The ring thumbs are on thumb_02: thumb_01 is the palm on all four rigs.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAccessoryFaceBoneTestPrivate
{
	/** The face slots and their MetaHuman face bones. */
	const TPair<const TCHAR*, const TCHAR*> FaceBones[] = { { TEXT("acc_eyes"), TEXT("FACIAL_C_NoseBridge") }, { TEXT("acc_ear_l"), TEXT("FACIAL_L_Ear") },
		{ TEXT("acc_ear_r"), TEXT("FACIAL_R_Ear") }, { TEXT("acc_nose"), TEXT("FACIAL_C_Nose") } };
	/** The wrists and their body bones. */
	const TPair<const TCHAR*, const TCHAR*> BodyBones[] = { { TEXT("acc_wrist_l"), TEXT("hand_l") }, { TEXT("acc_wrist_r"), TEXT("hand_r") } };
	/** The four body skeletons an avatar can be. */
	const TCHAR* const BodySkeletons[] = { TEXT("metahuman"), TEXT("ue5mannequin"), TEXT("ue4mannequin"), TEXT("uefnmannequin") };

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
	FMantleAvatarContract Contract;
	FString Error;
	if (!TestTrue(FString::Printf(TEXT("the plugin's contract loads (%s)"), *Error), FMantleAvatarContract::LoadFromDirectory(FMantleAvatarContract::DefaultDirectory(), Contract, Error))) return false;
	const FMantleContractSlot* Ear = Contract.FindSlot(TEXT("acc_ear_l"));
	const FMantleContractSlot* Wrist = Contract.FindSlot(TEXT("acc_wrist_l"));
	const FMantleContractSlot* Hat = Contract.FindSlot(TEXT("acc_hat"));
	if (!TestNotNull(TEXT("acc_ear_l is there"), Ear) || !TestNotNull(TEXT("acc_wrist_l is there"), Wrist) || !TestNotNull(TEXT("acc_hat is there"), Hat)) return false;
	TestTrue(TEXT("acc_ear_l is on the face"), Ear->IsOnFace());
	TestEqual(TEXT("on its FACIAL bone"), Ear->SocketBone, FString(TEXT("FACIAL_L_Ear")));
	TestFalse(TEXT("acc_hat, with no mesh, is on the body"), Hat->IsOnFace());
	TestEqual(TEXT("a slot with no mesh says body"), Hat->Mesh, FString(TEXT("body")));

	// A face that has the bone carries the slot there.
	const FMantleAccessoryBone OnFace = FMantleAccessorySettings::ResolveBone(*Ear, [&EarBone](FName Bone) { return Bone == EarBone; });
	TestTrue(TEXT("with the bone on the face: available on the face"), OnFace.bAvailable && OnFace.bOnFace);
	TestTrue(TEXT("on FACIAL_L_Ear"), OnFace.Bone == EarBone);

	// A face without it (not a MetaHuman face), or no face at all: the slot is not there. No head fallback.
	const FMantleAccessoryBone NoBone = FMantleAccessorySettings::ResolveBone(*Ear, [](FName) { return false; });
	TestFalse(TEXT("a face without the bone: not available"), NoBone.bAvailable);
	TestTrue(TEXT("and not moved to the head"), NoBone.Bone == EarBone && NoBone.bOnFace);
	const FMantleAccessoryBone NoFace = FMantleAccessorySettings::ResolveBone(*Ear, static_cast<const USkeletalMesh*>(nullptr));
	TestFalse(TEXT("no face: not available"), NoFace.bAvailable);
	FTransform Nowhere;
	TestFalse(TEXT("no face: nowhere to place it"), FMantleAccessorySettings::BoneInComponent(*Ear, nullptr, nullptr, Nowhere));

	// A body slot stays on its body bone even when a face would have a bone of that name.
	const FMantleAccessoryBone HatBone = FMantleAccessorySettings::ResolveBone(*Hat, [](FName) { return true; });
	TestTrue(TEXT("acc_hat stays on the body's head"), HatBone.bAvailable && !HatBone.bOnFace && HatBone.Bone == FName(*Hat->SocketBone));

	// Offered per skeleton: the ear needs the MetaHuman face, so only a MetaHuman avatar has it; the wrist and the hat are on every
	// body skeleton.
	TestTrue(TEXT("the ear is offered on a MetaHuman"), FMantleAccessorySettings::SlotOffered(Contract, *Ear, TEXT("metahuman")));
	for (const TCHAR* Mannequin : { TEXT("ue5mannequin"), TEXT("ue4mannequin"), TEXT("uefnmannequin") })
	{
		TestFalse(FString::Printf(TEXT("the ear is hidden on %s"), Mannequin), FMantleAccessorySettings::SlotOffered(Contract, *Ear, Mannequin));
	}
	for (const TCHAR* Skeleton : { TEXT("metahuman"), TEXT("ue5mannequin"), TEXT("ue4mannequin"), TEXT("uefnmannequin") })
	{
		TestTrue(FString::Printf(TEXT("the wrist is offered on %s"), Skeleton), FMantleAccessorySettings::SlotOffered(Contract, *Wrist, Skeleton));
		TestTrue(FString::Printf(TEXT("the hat is offered on %s"), Skeleton), FMantleAccessorySettings::SlotOffered(Contract, *Hat, Skeleton));
	}

	// The whole approved list: every face slot's bone is on the MetaHuman face (offered on a MetaHuman, hidden on the mannequins);
	// every wrist and ring-finger bone, the thumbs on thumb_02, is on all four body skeletons.
	const FMantleContractSkeleton* FaceFixture = Contract.FindSkeleton(TEXT("metahuman_face"));
	if (!TestNotNull(TEXT("the MetaHuman face skeleton is in the contract"), FaceFixture)) return false;
	for (const TPair<const TCHAR*, const TCHAR*>& Entry : FaceBones)
	{
		const FMantleContractSlot* Slot = Contract.FindSlot(Entry.Key);
		if (!TestNotNull(FString::Printf(TEXT("%s is there"), Entry.Key), Slot)) continue;
		TestTrue(FString::Printf(TEXT("%s is on the face, on %s"), Entry.Key, Entry.Value), Slot->IsOnFace() && Slot->SocketBone == Entry.Value);
		TestTrue(FString::Printf(TEXT("%s is a bone of the MetaHuman face"), Entry.Value), FaceFixture->Find(FName(Entry.Value)) != INDEX_NONE);
		for (const TCHAR* Skeleton : BodySkeletons)
		{
			TestTrue(FString::Printf(TEXT("%s is offered on %s only if it is the MetaHuman"), Entry.Key, Skeleton),
				FMantleAccessorySettings::SlotOffered(Contract, *Slot, Skeleton) == (FCString::Strcmp(Skeleton, TEXT("metahuman")) == 0));
		}
	}
	TArray<FString> BodyBoneNames;
	for (const TPair<const TCHAR*, const TCHAR*>& Entry : BodyBones)
	{
		const FMantleContractSlot* Slot = Contract.FindSlot(Entry.Key);
		if (TestNotNull(FString::Printf(TEXT("%s is there"), Entry.Key), Slot))
		{
			TestTrue(FString::Printf(TEXT("%s is on the body, on %s"), Entry.Key, Entry.Value), !Slot->IsOnFace() && Slot->SocketBone == Entry.Value);
		}
		BodyBoneNames.Add(Entry.Value);
	}
	const FMantleContractSlot* Ring = Contract.FindSlot(TEXT("acc_ring"));
	if (TestNotNull(TEXT("acc_ring is there"), Ring))
	{
		TestEqual(TEXT("a ring has ten fingers"), Ring->FingerBones.Num(), 10);
		TestFalse(TEXT("a ring is on the body"), Ring->IsOnFace());
		for (const FMantleContractFingerBone& Finger : Ring->FingerBones)
		{
			if (Finger.Finger.StartsWith(TEXT("thumb")))
			{
				TestTrue(FString::Printf(TEXT("the %s ring is on thumb_02, not the palm's thumb_01 (%s)"), *Finger.Finger, *Finger.Bone), Finger.Bone.StartsWith(TEXT("thumb_02_")));
			}
			BodyBoneNames.Add(Finger.Bone);
		}
		TestTrue(TEXT("the ring is offered on every body skeleton"), FMantleAccessorySettings::SlotOffered(Contract, *Ring, TEXT("ue4mannequin")));
	}
	for (const TCHAR* Skeleton : BodySkeletons)
	{
		const FMantleContractSkeleton* Fixture = Contract.FindSkeleton(Skeleton);
		if (!TestNotNull(FString::Printf(TEXT("%s is in the contract"), Skeleton), Fixture)) continue;
		for (const FString& Bone : BodyBoneNames)
		{
			TestTrue(FString::Printf(TEXT("%s has %s"), Skeleton, *Bone), Fixture->Find(FName(*Bone)) != INDEX_NONE);
		}
	}

	// The panel's slot list follows it: the ear is in the MetaHuman list and not in a mannequin's.
	const TArray<FString> OnMetaHuman = CosmeticSlotOptions(Contract, EMantleCosmeticClass::Accessory, TEXT("metahuman"));
	const TArray<FString> OnMannequin = CosmeticSlotOptions(Contract, EMantleCosmeticClass::Accessory, TEXT("ue5mannequin"));
	TestTrue(TEXT("the MetaHuman slot list has the ear"), OnMetaHuman.Contains(TEXT("acc_ear_l")));
	TestFalse(TEXT("the mannequin slot list does not"), OnMannequin.Contains(TEXT("acc_ear_l")));
	TestTrue(TEXT("both have the wrist and the hat"), OnMannequin.Contains(TEXT("acc_wrist_l")) && OnMannequin.Contains(TEXT("acc_hat")) && OnMetaHuman.Contains(TEXT("acc_wrist_l")));
	TestTrue(TEXT("with no skeleton every slot is listed"), CosmeticSlotOptions(Contract, EMantleCosmeticClass::Accessory).Contains(TEXT("acc_ear_l")));

	// An Outfit piece cannot go on a face slot: a clear refusal.
	FMantleParametricOutfit::FRigidPivot Pivot;
	FString Refusal;
	TestFalse(TEXT("an Outfit piece on the ear is refused"), FMantleParametricOutfit::RigidPivot(Contract, TEXT("acc_ear_l"), Pivot, Refusal));
	TestTrue(FString::Printf(TEXT("saying why (%s)"), *Refusal), Refusal.Contains(TEXT("face")));
	TestTrue(TEXT("an Outfit piece on the hat is not"), FMantleParametricOutfit::RigidPivot(Contract, TEXT("acc_hat"), Pivot, Refusal));

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
