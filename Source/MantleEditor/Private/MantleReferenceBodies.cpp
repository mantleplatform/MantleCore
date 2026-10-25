// Copyright Joshua Eger. All Rights Reserved.
#include "MantleReferenceBodies.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarReposeFixer.h"
#include "MantleCosmeticExport.h"
#include "MantleMetaHumanBuild.h"
#include "MantleMetaHumanPose.h"
#include "MantleRuntime.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterFactoryNew.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

const TCHAR* const FMantleReferenceBodies::Folder = TEXT("/Game/MantleReference/NeutralBody");
const TCHAR* const FMantleReferenceBodies::CharacterName = TEXT("MH_MantleNeutral");

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleReferenceBodiesPrivate
{
	const TCHAR* const RefBodiesNotBuilt =
		TEXT("Mantle's neutral reference body hasn't been built in this project yet (the Refit line builds it once, with Epic's MetaHuman cloud)");

	struct FRefBodiesState final : public FGCObject
	{
		TObjectPtr<USkeletalMesh> Body;
		TObjectPtr<USkeletalMesh> Face;

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			Collector.AddReferencedObject(Body);
			Collector.AddReferencedObject(Face);
		}

		virtual FString GetReferencerName() const override
		{
			return TEXT("FMantleReferenceBodies");
		}
	};

	/** Created on first use and deleted by Shutdown, while the editor's GC is still alive. */
	FRefBodiesState* GRefBodiesState = nullptr;

	FRefBodiesState& RefBodiesState()
	{
		if (!GRefBodiesState)
		{
			GRefBodiesState = new FRefBodiesState();
		}
		return *GRefBodiesState;
	}

	/** The built body and face under Folder/Unpacked (Epic's <character>_BodyMesh and _FaceMesh), as asset data. */
	void RefBodiesFindBuilt(FAssetData& OutBody, FAssetData& OutFace)
	{
		OutBody = FAssetData();
		OutFace = FAssetData();
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const FString Unpacked = FString(FMantleReferenceBodies::Folder) / TEXT("Unpacked");
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Unpacked), Assets, /*bRecursive*/ true);
		if (Assets.Num() == 0 && Registry.IsLoadingAssets())
		{
			// Early in an editor session the registry may not have reached this folder yet.
			Registry.ScanPathsSynchronous({ Unpacked }, /*bForceRescan*/ false);
			Registry.GetAssetsByPath(FName(*Unpacked), Assets, /*bRecursive*/ true);
		}
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName())
			{
				continue;
			}
			const FString Name = Asset.AssetName.ToString();
			if (Name.EndsWith(TEXT("_BodyMesh")))
			{
				OutBody = Asset;
			}
			else if (Name.EndsWith(TEXT("_FaceMesh")))
			{
				OutFace = Asset;
			}
		}
	}

	/** Loads the built body (and face) into the state when they are there and the body has the reference topology. */
	bool RefBodiesLoad(const FMantleAvatarContract& Contract, FString& OutWhyNot)
	{
		FRefBodiesState& State = RefBodiesState();
		if (State.Body)
		{
			if (FMantleCosmeticExport::FindReferenceLOD(State.Body, Contract) == INDEX_NONE)
			{
				OutWhyNot = FString::Printf(TEXT("Mantle's reference body %s does not match this plugin's reference topology"), *State.Body->GetPathName());
				return false;
			}
			return true;
		}
		FAssetData BodyAsset;
		FAssetData FaceAsset;
		RefBodiesFindBuilt(BodyAsset, FaceAsset);
		if (!BodyAsset.IsValid())
		{
			OutWhyNot = RefBodiesNotBuilt;
			return false;
		}
		USkeletalMesh* const Body = Cast<USkeletalMesh>(BodyAsset.GetAsset());
		if (!Body)
		{
			OutWhyNot = FString::Printf(TEXT("Mantle's reference body %s could not be loaded"), *BodyAsset.GetObjectPathString());
			return false;
		}
		if (FMantleCosmeticExport::FindReferenceLOD(Body, Contract) == INDEX_NONE)
		{
			OutWhyNot = FString::Printf(TEXT("Mantle's reference body %s does not match this plugin's reference topology (delete %s and let the Refit line build it again)"),
				*Body->GetPathName(), FMantleReferenceBodies::Folder);
			return false;
		}
		State.Body = Body;
		State.Face = FaceAsset.IsValid() ? Cast<USkeletalMesh>(FaceAsset.GetAsset()) : nullptr;
		return true;
	}
}

// ---- The editor reference provider -----------------------------------------------------------------

FString FMantleReferenceBodies::UnavailableText(const FString& WhyNot)
{
	return FString(TEXT("Can't: ")) + (WhyNot.IsEmpty() ? TEXT("Mantle's neutral reference body could not be loaded.") : WhyNot);
}

bool FMantleReferenceBodies::GetAll(const FMantleAvatarContract& Contract, TArray<FMantleReferenceBody>& Out, FString& OutWhyNot)
{
	using namespace MantleReferenceBodiesPrivate;
	check(IsInGameThread());
	Out.Reset();
	OutWhyNot.Reset();
	if (!RefBodiesLoad(Contract, OutWhyNot))
	{
		return false;
	}
	FMantleReferenceBody& Body = Out.AddDefaulted_GetRef();
	Body.Body = RefBodiesState().Body;
	Body.Id = Body.Body->GetPathName();
	return true;
}

bool FMantleReferenceBodies::GetClosest(const USkeletalMesh& To, const FMantleAvatarContract& Contract, FMantleReferenceBody& Out, FString& OutWhyNot,
	FString* OutWhy)
{
	TArray<FMantleReferenceBody> All;
	if (!GetAll(Contract, All, OutWhyNot))
	{
		return false;
	}
	TArray<USkeletalMesh*> Meshes;
	Meshes.Reserve(All.Num());
	for (const FMantleReferenceBody& Body : All)
	{
		Meshes.Add(Body.Body);
	}
	FString Why;
	const int32 Chosen = ChooseClosest(To, Meshes, &Why);
	if (!All.IsValidIndex(Chosen))
	{
		OutWhyNot = TEXT("Mantle's reference body is missing the bones needed to measure it for Refit");
		return false;
	}
	Out = All[Chosen];
	if (OutWhy)
	{
		*OutWhy = Why;
	}
	return true;
}

TArray<USkeletalMesh*> FMantleReferenceBodies::FindBodies(const FMantleAvatarContract& Contract, FString* OutWhyNot)
{
	if (OutWhyNot)
	{
		OutWhyNot->Reset();
	}
	TArray<USkeletalMesh*> Out;
	TArray<FMantleReferenceBody> Bodies;
	FString WhyNot;
	if (!GetAll(Contract, Bodies, WhyNot))
	{
		if (OutWhyNot)
		{
			*OutWhyNot = WhyNot;
		}
		return Out;
	}
	for (const FMantleReferenceBody& Body : Bodies)
	{
		Out.Add(Body.Body);
	}
	return Out;
}

namespace MantleReferenceBodiesPrivate
{
	struct FRefBodiesMeasure
	{
		bool bOk = false;
		double Height = 0.0;
		double ShoulderToHip = 0.0;
	};

	FRefBodiesMeasure RefBodiesMeasureBody(const USkeletalMesh& Mesh)
	{
		const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
		const TArray<FMeshBoneInfo>& Info = Reference.GetRawRefBoneInfo();
		const TArray<FTransform>& Pose = Reference.GetRawRefBonePose();
		TArray<int32> Parents;
		TArray<FTransform> Local;
		TMap<FName, int32> IndexOf;
		for (int32 Bone = 0; Bone < Info.Num(); ++Bone)
		{
			Parents.Add(Info[Bone].ParentIndex);
			Local.Add(Pose.IsValidIndex(Bone) ? Pose[Bone] : FTransform::Identity);
			IndexOf.Add(Info[Bone].Name, Bone);
		}
		TArray<FTransform> Component;
		FMantleAvatarRepose::ComposeComponent(Parents, Local, Component);
		auto Find = [&IndexOf, &Component](const TCHAR* Name) -> const FTransform*
		{
			const int32* Index = IndexOf.Find(FName(Name));
			return Index && Component.IsValidIndex(*Index) ? &Component[*Index] : nullptr;
		};
		FRefBodiesMeasure Out;
		const FTransform* Head = Find(TEXT("head"));
		const FTransform* FootL = Find(TEXT("foot_l"));
		const FTransform* FootR = Find(TEXT("foot_r"));
		const FTransform* ArmL = Find(TEXT("upperarm_l"));
		const FTransform* ArmR = Find(TEXT("upperarm_r"));
		const FTransform* ThighL = Find(TEXT("thigh_l"));
		const FTransform* ThighR = Find(TEXT("thigh_r"));
		if (!Head || !FootL || !FootR || !ArmL || !ArmR || !ThighL || !ThighR)
		{
			return Out;
		}
		Out.Height = Head->GetLocation().Z - FMath::Min(FootL->GetLocation().Z, FootR->GetLocation().Z);
		const double Shoulders = FVector::Dist(ArmL->GetLocation(), ArmR->GetLocation());
		const double Hips = FVector::Dist(ThighL->GetLocation(), ThighR->GetLocation());
		Out.ShoulderToHip = Hips > UE_KINDA_SMALL_NUMBER ? Shoulders / Hips : 0.0;
		Out.bOk = Out.Height > 1.0 && Out.ShoulderToHip > UE_KINDA_SMALL_NUMBER;
		return Out;
	}
}

int32 FMantleReferenceBodies::ChooseClosest(const USkeletalMesh& Source, TConstArrayView<USkeletalMesh*> References, FString* OutWhy)
{
	using namespace MantleReferenceBodiesPrivate;
	const FRefBodiesMeasure Measure = RefBodiesMeasureBody(Source);
	int32 Best = INDEX_NONE;
	double BestScore = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < References.Num(); ++Index)
	{
		if (!References[Index])
		{
			continue;
		}
		const FRefBodiesMeasure Other = RefBodiesMeasureBody(*References[Index]);
		if (!Other.bOk)
		{
			continue;
		}
		// Without a source measure (a body missing a limb joint), the first reference that measures.
		const double Score = Measure.bOk ? FMath::Abs(FMath::Loge(Measure.Height / Other.Height)) + FMath::Abs(FMath::Loge(Measure.ShoulderToHip / Other.ShoulderToHip)) : 0.0;
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Index;
			if (OutWhy)
			{
				*OutWhy = Measure.bOk
					? FString::Printf(TEXT("%.1f%% %s, shoulder-to-hip ratio %.1f%% off"), FMath::Abs(Measure.Height / Other.Height - 1.0) * 100.0,
						Measure.Height >= Other.Height ? TEXT("taller") : TEXT("shorter"), FMath::Abs(Measure.ShoulderToHip / Other.ShoulderToHip - 1.0) * 100.0)
					: FString(TEXT("the source's limbs could not be measured"));
			}
		}
	}
	return Best;
}

USkeletalMesh* FMantleReferenceBodies::GetFace(FString* OutNote)
{
	using namespace MantleReferenceBodiesPrivate;
	check(IsInGameThread());
	FString WhyNot;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&WhyNot);
	USkeletalMesh* Face = Contract && RefBodiesLoad(*Contract, WhyNot) ? RefBodiesState().Face.Get() : nullptr;
	if (OutNote)
	{
		*OutNote = Face ? FString() : WhyNot.IsEmpty() ? FString(TEXT("Mantle's reference body was built without a face")) : WhyNot;
	}
	return Face;
}

bool FMantleReferenceBodies::IsBuilt()
{
	using namespace MantleReferenceBodiesPrivate;
	if (GRefBodiesState && GRefBodiesState->Body)
	{
		return true;
	}
	FAssetData Body;
	FAssetData Face;
	RefBodiesFindBuilt(Body, Face);
	return Body.IsValid() && Face.IsValid();
}

bool FMantleReferenceBodies::Build(FString& OutError)
{
	using namespace MantleReferenceBodiesPrivate;
	check(IsInGameThread());
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&OutError);
	if (!Contract)
	{
		return false;
	}
	FString WhyNot;
	if (IsBuilt() && RefBodiesLoad(*Contract, WhyNot) && RefBodiesState().Face)
	{
		return true;
	}

	// A character a failed build left behind is built again; otherwise a blank one: Epic's template, nobody in particular.
	TArray<TWeakObjectPtr<UObject>> Made;
	const FString CharacterPath = FString(Folder) / CharacterName;
	UMetaHumanCharacter* Character = LoadObject<UMetaHumanCharacter>(nullptr, *(CharacterPath + TEXT(".") + CharacterName), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Character)
	{
		UMetaHumanCharacterFactoryNew* Factory = NewObject<UMetaHumanCharacterFactoryNew>(GetTransientPackage());
		Character = Cast<UMetaHumanCharacter>(FMantleMetaHumanSteps::CreateAsset(Folder, CharacterName, UMetaHumanCharacter::StaticClass(), Factory));
		if (!Character)
		{
			OutError = FString::Printf(TEXT("the reference MetaHuman Character could not be made in %s"), Folder);
			return false;
		}
		Made.Add(Character);
	}

	UE_LOG(LogMantle, Log, TEXT("MANTLE_REFERENCE_BODIES building %s (once per project)"), *Character->GetPathName());
	FMantleMetaHumanBuildResult Result;
	FString Error;
	if (!FMantleMetaHumanBuild::Build(*Character, Folder, Result, Error, /*bDeferHeightWarning*/ true)
		|| !Result.Body.IsValid() || !Result.Face.IsValid())
	{
		OutError = FString::Printf(TEXT("Mantle's neutral reference body could not be built (%s)"),
			Error.IsEmpty() ? TEXT("Epic's build made no body or no face") : *Error);
		Made.Append(Result.Created);
		FMantleMetaHumanSteps::Discard(Made);
		return false;
	}

	Shutdown();
	if (!RefBodiesLoad(*Contract, WhyNot))
	{
		OutError = WhyNot;
		return false;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_REFERENCE_BODIES built %s and %s"), *Result.Body->GetPathName(), *Result.Face->GetPathName());
	return true;
}

bool FMantleReferenceBodies::IsReferenceMesh(const UObject* Object, FString* OutId)
{
	using namespace MantleReferenceBodiesPrivate;
	if (!Object || !GRefBodiesState)
	{
		return false;
	}
	const FRefBodiesState& State = *GRefBodiesState;
	if (State.Body && Object == State.Body.Get())
	{
		if (OutId) { *OutId = State.Body->GetPathName(); }
		return true;
	}
	if (State.Face && Object == State.Face.Get())
	{
		if (OutId) { *OutId = State.Body ? State.Body->GetPathName() + TEXT(":face") : State.Face->GetPathName(); }
		return true;
	}
	return false;
}

FString FMantleReferenceBodies::Describe(const UObject* Object)
{
	if (!Object)
	{
		return FString();
	}
	FString Id;
	return IsReferenceMesh(Object, &Id) ? Id : Object->GetPathName();
}

void FMantleReferenceBodies::Shutdown()
{
	using namespace MantleReferenceBodiesPrivate;
	delete GRefBodiesState;
	GRefBodiesState = nullptr;
}
