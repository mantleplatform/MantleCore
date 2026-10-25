// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEmoteRetarget.h"

#include "MantleAnimClipWriter.h"
#include "MantleAvatarContract.h"
#include "MantleBuiltInMannequin.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticValidation.h"
#include "MantleRuntime.h"

#include "Animation/AnimSequence.h"
#include "Animation/IAnimationSequenceCompiler.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserItemPath.h"
#include "ContentBrowserModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/PlatformTime.h"
#include "IContentBrowserSingleton.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleEmoteRetargetPrivate
{
	const TCHAR* const BatchClassPath = TEXT("/Script/IKRigEditor.IKRetargetBatchOperation");
	const TCHAR* const BatchFunctionName = TEXT("RunBatchRetarget");
	const TCHAR* const RetargeterClassPath = TEXT("/Script/IKRig.IKRetargeter");
	const TCHAR* const EmoteMetaHumanId = TEXT("metahuman");
	const TCHAR* const EmoteUE5MannequinId = TEXT("ue5mannequin");
	const TCHAR* const EmoteUE4MannequinId = TEXT("ue4mannequin");

	/** FIKRetargetBatchOperationInputs' string and bool inputs, all set on every call. */
	const TCHAR* const BatchStringInputs[] = { TEXT("Search"), TEXT("Replace"), TEXT("Prefix"), TEXT("Suffix"), TEXT("TargetPath") };
	const TCHAR* const BatchBoolInputs[] = { TEXT("bUseSourcePath"), TEXT("bIncludeReferencedAssets"), TEXT("bOverwriteExistingFiles"), TEXT("bRetainAdditiveFlags") };

	/** RunBatchRetarget and the properties of its input struct, found by name. */
	struct FBatchCall
	{
		/** The class default object: the function is static, and a static UFunction runs on it. */
		UObject* Target = nullptr;
		UFunction* Function = nullptr;
		const FStructProperty* Inputs = nullptr;
		const FArrayProperty* Result = nullptr;
		const FArrayProperty* Assets = nullptr;
		const FObjectPropertyBase* SourceMesh = nullptr;
		const FObjectPropertyBase* TargetMesh = nullptr;
		const FObjectPropertyBase* Retargeter = nullptr;
		TMap<FName, const FStrProperty*> Strings;
		TMap<FName, const FBoolProperty*> Bools;
	};

	/** An array of FAssetData (the struct by name and size: CoreUObject's noexport FAssetData). */
	bool IsAssetDataArray(const FArrayProperty* Array)
	{
		const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
		return Inner && Inner->Struct && Inner->Struct->GetFName() == FName(TEXT("AssetData"))
			&& Inner->Struct->GetStructureSize() == static_cast<int32>(sizeof(FAssetData));
	}

	bool FindBatchCall(FBatchCall& Out, FString* OutError)
	{
		Out = FBatchCall();
		auto Fail = [OutError](const FString& Why)
		{
			if (OutError)
			{
				*OutError = Why;
			}
			return false;
		};
		UClass* Class = FindObject<UClass>(nullptr, BatchClassPath);
		if (!Class && FModuleManager::Get().ModuleExists(TEXT("IKRigEditor")))
		{
			// IKRigEditor loads at PostEngineInit; a commandlet may not have it yet.
			FModuleManager::Get().LoadModule(TEXT("IKRigEditor"));
			Class = FindObject<UClass>(nullptr, BatchClassPath);
		}
		if (!Class)
		{
			return Fail(FString::Printf(TEXT("%s is not loaded (the IK Rig plugin's editor module)"), BatchClassPath));
		}
		UFunction* Function = Class->FindFunctionByName(FName(BatchFunctionName));
		if (!Function || !Function->HasAnyFunctionFlags(FUNC_Static))
		{
			return Fail(FString::Printf(TEXT("%s has no static %s"), BatchClassPath, BatchFunctionName));
		}
		Out.Inputs = CastField<FStructProperty>(Function->FindPropertyByName(FName(TEXT("Inputs"))));
		Out.Result = CastField<FArrayProperty>(Function->GetReturnProperty());
		if (!Out.Inputs || !Out.Inputs->Struct || !IsAssetDataArray(Out.Result))
		{
			return Fail(FString::Printf(TEXT("%s does not take Inputs and return the assets it made"), BatchFunctionName));
		}
		const UScriptStruct* Struct = Out.Inputs->Struct;
		Out.Assets = FindFProperty<FArrayProperty>(Struct, TEXT("AssetsToRetarget"));
		Out.SourceMesh = FindFProperty<FObjectPropertyBase>(Struct, TEXT("SourceMesh"));
		Out.TargetMesh = FindFProperty<FObjectPropertyBase>(Struct, TEXT("TargetMesh"));
		Out.Retargeter = FindFProperty<FObjectPropertyBase>(Struct, TEXT("IKRetargetAsset"));
		if (!IsAssetDataArray(Out.Assets) || !Out.SourceMesh || !Out.TargetMesh || !Out.Retargeter)
		{
			return Fail(FString::Printf(TEXT("%s has no AssetsToRetarget, SourceMesh, TargetMesh or IKRetargetAsset"), *Struct->GetName()));
		}
		for (const TCHAR* Name : BatchStringInputs)
		{
			const FStrProperty* Property = FindFProperty<FStrProperty>(Struct, Name);
			if (!Property)
			{
				return Fail(FString::Printf(TEXT("%s has no string %s"), *Struct->GetName(), Name));
			}
			Out.Strings.Add(FName(Name), Property);
		}
		for (const TCHAR* Name : BatchBoolInputs)
		{
			const FBoolProperty* Property = FindFProperty<FBoolProperty>(Struct, Name);
			if (!Property)
			{
				return Fail(FString::Printf(TEXT("%s has no bool %s"), *Struct->GetName(), Name));
			}
			Out.Bools.Add(FName(Name), Property);
		}
		Out.Target = Class->GetDefaultObject();
		Out.Function = Function;
		return Out.Target != nullptr;
	}

	/**
	 * One RunBatchRetarget: Sequence alone (nothing it references), from SourceMesh onto TargetMesh through Retargeter,
	 * into Folder under its own name (a numbered one when taken), never overwriting anything. What it made in OutCreated.
	 */
	void RunBatch(const FBatchCall& Call, UAnimSequence* Sequence, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, UObject* Retargeter,
		const FString& Folder, TArray<FAssetData>& OutCreated)
	{
		UFunction* Function = Call.Function;
		const int32 Size = FMath::Max<int32>(1, Function->ParmsSize);
		uint8* Params = static_cast<uint8*>(FMemory::Malloc(Size, Function->GetMinAlignment()));
		FMemory::Memzero(Params, Size);
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Params);
		}

		void* Inputs = Call.Inputs->ContainerPtrToValuePtr<void>(Params);
		Call.Assets->ContainerPtrToValuePtr<TArray<FAssetData>>(Inputs)->Add(FAssetData(Sequence));
		Call.SourceMesh->SetObjectPropertyValue_InContainer(Inputs, SourceMesh);
		Call.TargetMesh->SetObjectPropertyValue_InContainer(Inputs, TargetMesh);
		Call.Retargeter->SetObjectPropertyValue_InContainer(Inputs, Retargeter);
		for (const TPair<FName, const FStrProperty*>& Pair : Call.Strings)
		{
			Pair.Value->SetPropertyValue_InContainer(Inputs, Pair.Key == FName(TEXT("TargetPath")) ? Folder : FString());
		}
		for (const TPair<FName, const FBoolProperty*>& Pair : Call.Bools)
		{
			// Only bRetainAdditiveFlags: never the source's folder, never what it references, never an overwrite.
			Pair.Value->SetPropertyValue_InContainer(Inputs, Pair.Key == FName(TEXT("bRetainAdditiveFlags")));
		}

		Call.Target->ProcessEvent(Function, Params);
		OutCreated = *Call.Result->ContainerPtrToValuePtr<TArray<FAssetData>>(Params);

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params);
		}
		FMemory::Free(Params);
	}

	/**
	 * Drops an asset the retarget made: gone from the asset registry, no longer kept, its package clean so nothing
	 * offers to save it; garbage collection frees it. Only ever an asset under Folder.
	 */
	void DiscardTemporary(UObject* Asset, const FString& Folder)
	{
		UPackage* Package = Asset ? Asset->GetPackage() : nullptr;
		if (!Package || !Package->GetName().StartsWith(Folder + TEXT("/"), ESearchCase::CaseSensitive))
		{
			return;
		}
		if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset))
		{
			// The copy's compression runs asynchronously; let it land before the copy goes (else it lands on garbage).
			UAnimSequence* const Sequences[] = { Sequence };
			UE::Anim::IAnimSequenceCompilingManager::FinishCompilation(MakeArrayView(Sequences));
		}
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		Package->SetDirtyFlag(false);
	}

	/** Takes Folder out of the Content Browser's tree once nothing in it is left (it was only ever in memory). */
	void RemoveFolderIfEmpty(const FString& Folder)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Left;
		Registry.GetAssetsByPath(FName(*Folder), Left, /*bRecursive*/ true);
		if (Left.Num() == 0)
		{
			Registry.RemovePath(Folder);
		}
	}

	/**
	 * One of the plugin's retargeters whose target retarget pose and FK chains are edited through IK Rig's
	 * UIKRetargeterController by reflection (MantleEditor links no IK Rig module). Make: a copy in the transient package and
	 * out of the undo history, which goes with this (the shipped asset untouched). Open: the asset itself, edited in place
	 * (Mantle.Dev.MatchRetargetHands). Game thread.
	 */
	class FRetargeterCopy
	{
	public:
		static constexpr uint8 Source = 0;
		static constexpr uint8 Target = 1;

		bool Make(UObject* Original, FString& OutError)
		{
			UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/IKRigEditor.IKRetargeterController"));
			UFunction* Get = Class ? Class->FindFunctionByName(TEXT("GetController")) : nullptr;
			if (!Original || !Get)
			{
				OutError = TEXT("IK Rig's retargeter controller is not loaded");
				return false;
			}
			Copy = DuplicateObject<UObject>(Original, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), Original->GetClass(),
				FName(*(Original->GetName() + TEXT("_RestHands")))));
			if (!Copy)
			{
				OutError = FString::Printf(TEXT("%s could not be copied"), *Original->GetName());
				return false;
			}
			bOwned = true;
			// Kept through any garbage collection the batch retarget runs; not transactional, so the controller's edits leave
			// nothing in the creator's undo history.
			Copy->AddToRoot();
			Copy->ClearFlags(RF_Transactional);
			ForEachObjectWithOuter(Copy, [](UObject* Inner) { Inner->ClearFlags(RF_Transactional); });
			return Attach(Class, Get, OutError);
		}

		/** Edits Asset itself; its package is marked dirty by the controller and saved by the caller. */
		bool Open(UObject* Asset, FString& OutError)
		{
			UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/IKRigEditor.IKRetargeterController"));
			UFunction* Get = Class ? Class->FindFunctionByName(TEXT("GetController")) : nullptr;
			if (!Asset || !Get)
			{
				OutError = TEXT("IK Rig's retargeter controller is not loaded");
				return false;
			}
			Copy = Asset;
			bOwned = false;
			return Attach(Class, Get, OutError);
		}

		bool Attach(UClass* Class, UFunction* Get, FString& OutError)
		{
			Call(Class->GetDefaultObject(), Get, [this](UFunction* Function, uint8* Params)
			{
				CastField<FObjectPropertyBase>(Function->FindPropertyByName(TEXT("InRetargeterAsset")))->SetObjectPropertyValue_InContainer(Params, Copy);
			}, [this](UFunction* Function, uint8* Params)
			{
				Controller = CastField<FObjectPropertyBase>(Function->GetReturnProperty())->GetObjectPropertyValue_InContainer(Params);
			});
			if (!Controller || !Find(TEXT("GetRotationOffsetForRetargetPoseBone")) || !Find(TEXT("SetRotationOffsetForRetargetPoseBone")))
			{
				OutError = TEXT("IK Rig gave no controller for the retargeter that can edit its retarget pose");
				return false;
			}
			return true;
		}

		UObject* Get() const { return Copy; }

		/**
		 * Every FK Chains op's chains that Which picks (by target chain name) edited by Edit (the chain's
		 * FRetargetFKChainSettings, its struct property and memory), through the op's UIKRetargetFKChainsController. The
		 * number of chains edited.
		 */
		int32 EditFKChains(TFunctionRef<bool(FName)> Which, TFunctionRef<void(const UScriptStruct& Chain, uint8* Element)> Edit) const
		{
			int32 Changed = 0;
			int32 Ops = 0;
			Call(Controller, Find(TEXT("GetNumRetargetOps")), [](UFunction*, uint8*) {}, [&](UFunction* Function, uint8* Params)
			{
				Ops = *Function->GetReturnProperty()->ContainerPtrToValuePtr<int32>(Params);
			});
			for (int32 Op = 0; Op < Ops; ++Op)
			{
				UObject* OpController = nullptr;
				Call(Controller, Find(TEXT("GetOpController")), [Op](UFunction* Function, uint8* Params)
				{
					*Function->FindPropertyByName(TEXT("InOpIndex"))->ContainerPtrToValuePtr<int32>(Params) = Op;
				}, [&](UFunction* Function, uint8* Params)
				{
					OpController = CastField<FObjectPropertyBase>(Function->GetReturnProperty())->GetObjectPropertyValue_InContainer(Params);
				});
				UFunction* GetSettings = OpController ? OpController->FindFunction(TEXT("GetSettings")) : nullptr;
				UFunction* SetSettings = OpController ? OpController->FindFunction(TEXT("SetSettings")) : nullptr;
				if (!GetSettings || !SetSettings || OpController->GetClass()->GetName() != TEXT("IKRetargetFKChainsController"))
				{
					continue;
				}
				const FStructProperty* Returned = CastField<FStructProperty>(GetSettings->GetReturnProperty());
				const FStructProperty* Given = CastField<FStructProperty>(SetSettings->FindPropertyByName(TEXT("InSettings")));
				const UScriptStruct* Struct = Returned ? Returned->Struct : nullptr;
				const FArrayProperty* Chains = Struct ? CastField<FArrayProperty>(Struct->FindPropertyByName(TEXT("ChainsToRetarget"))) : nullptr;
				const FStructProperty* Chain = Chains ? CastField<FStructProperty>(Chains->Inner) : nullptr;
				const FNameProperty* ChainName = Chain ? CastField<FNameProperty>(Chain->Struct->FindPropertyByName(TEXT("TargetChainName"))) : nullptr;
				if (!Given || !ChainName)
				{
					continue;
				}
				uint8* Settings = static_cast<uint8*>(FMemory::Malloc(Struct->GetStructureSize(), Struct->GetMinAlignment()));
				Struct->InitializeStruct(Settings);
				Call(OpController, GetSettings, [](UFunction*, uint8*) {}, [&](UFunction*, uint8* Params)
				{
					Struct->CopyScriptStruct(Settings, Returned->ContainerPtrToValuePtr<void>(Params));
				});
				FScriptArrayHelper Array(Chains, Chains->ContainerPtrToValuePtr<void>(Settings));
				for (int32 Index = 0; Index < Array.Num(); ++Index)
				{
					uint8* Element = Array.GetRawPtr(Index);
					if (Which(ChainName->GetPropertyValue_InContainer(Element)))
					{
						Edit(*Chain->Struct, Element);
						++Changed;
					}
				}
				Call(OpController, SetSettings, [&](UFunction*, uint8* Params)
				{
					Struct->CopyScriptStruct(Given->ContainerPtrToValuePtr<void>(Params), Settings);
				}, [](UFunction*, uint8*) {});
				Struct->DestroyStruct(Settings);
				FMemory::Free(Settings);
			}
			return Changed;
		}

		/** The copy, rooted, now the caller's (FMantleEmoteRetarget::ReleaseRestHandsCopy). */
		UObject* Detach()
		{
			UObject* Out = Copy;
			Copy = nullptr;
			Controller = nullptr;
			return Out;
		}

		/** The current pose's local rotation offset of Bone on one side (identity when it has none). */
		FQuat GetOffset(const FName Bone, const uint8 Side) const
		{
			FQuat Out = FQuat::Identity;
			Call(Controller, Find(TEXT("GetRotationOffsetForRetargetPoseBone")), [&](UFunction* Function, uint8* Params)
			{
				SetName(Function, Params, TEXT("BoneName"), Bone);
				SetSide(Function, Params, TEXT("SourceOrTarget"), Side);
			}, [&](UFunction* Function, uint8* Params)
			{
				Out = *Function->GetReturnProperty()->ContainerPtrToValuePtr<FQuat>(Params);
			});
			return Out;
		}

		/** Every bone's current offset on one side (bones without one are left out). */
		TMap<FName, FQuat> OffsetsOf(const FReferenceSkeleton& Skeleton, const uint8 Side) const
		{
			TMap<FName, FQuat> Out;
			for (int32 Bone = 0; Bone < Skeleton.GetRawBoneNum(); ++Bone)
			{
				const FQuat Offset = GetOffset(Skeleton.GetBoneName(Bone), Side);
				if (!Offset.Equals(FQuat::Identity, 1e-6))
				{
					Out.Add(Skeleton.GetBoneName(Bone), Offset);
				}
			}
			return Out;
		}

		void SetTargetOffset(const FName Bone, const FQuat& Offset) const
		{
			Call(Controller, Find(TEXT("SetRotationOffsetForRetargetPoseBone")), [&](UFunction* Function, uint8* Params)
			{
				SetName(Function, Params, TEXT("BoneName"), Bone);
				*Function->FindPropertyByName(TEXT("RotationOffset"))->ContainerPtrToValuePtr<FQuat>(Params) = Offset;
				SetSide(Function, Params, TEXT("SkeletonMode"), Target);
			}, [](UFunction*, uint8*) {});
		}

		~FRetargeterCopy()
		{
			if (!bOwned)
			{
				return;
			}
			if (Copy)
			{
				Copy->RemoveFromRoot();
				Copy->MarkAsGarbage();
			}
			if (Controller)
			{
				Controller->MarkAsGarbage();
			}
		}

	private:
		UObject* Copy = nullptr;
		UObject* Controller = nullptr;
		bool bOwned = true;

		UFunction* Find(const TCHAR* Name) const
		{
			return Controller ? Controller->GetClass()->FindFunctionByName(Name) : nullptr;
		}

		static void Call(UObject* Object, UFunction* Function, TFunctionRef<void(UFunction*, uint8*)> Set, TFunctionRef<void(UFunction*, uint8*)> Read)
		{
			if (!Object || !Function)
			{
				return;
			}
			uint8* Params = static_cast<uint8*>(FMemory::Malloc(FMath::Max<int32>(1, Function->ParmsSize), Function->GetMinAlignment()));
			FMemory::Memzero(Params, FMath::Max<int32>(1, Function->ParmsSize));
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->InitializeValue_InContainer(Params);
			}
			Set(Function, Params);
			Object->ProcessEvent(Function, Params);
			Read(Function, Params);
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(Params);
			}
			FMemory::Free(Params);
		}

		static void SetName(UFunction* Function, uint8* Params, const TCHAR* Name, const FName Value)
		{
			if (FNameProperty* Property = CastField<FNameProperty>(Function->FindPropertyByName(Name)))
			{
				Property->SetPropertyValue_InContainer(Params, Value);
			}
		}

		static void SetSide(UFunction* Function, uint8* Params, const TCHAR* Name, const uint8 Side)
		{
			FProperty* Property = Function->FindPropertyByName(Name);
			if (FEnumProperty* Enum = CastField<FEnumProperty>(Property))
			{
				Enum->GetUnderlyingProperty()->SetIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Params), static_cast<int64>(Side));
			}
			else if (FByteProperty* Byte = CastField<FByteProperty>(Property))
			{
				Byte->SetPropertyValue_InContainer(Params, Side);
			}
		}
	};
}

TArray<FName> FMantleEmoteRetarget::HandBones()
{
	TArray<FName> Out;
	for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
	{
		Out.Add(FName(FString::Printf(TEXT("hand_%s"), Side)));
		for (const TCHAR* Finger : { TEXT("thumb"), TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky") })
		{
			for (const TCHAR* Joint : { TEXT("metacarpal"), TEXT("01"), TEXT("02"), TEXT("03") })
			{
				Out.Add(FName(FString::Printf(TEXT("%s_%s_%s"), Finger, Joint, Side)));
			}
		}
	}
	return Out;
}

TMap<FName, FQuat> FMantleEmoteRetarget::MatchRestRotations(const FReferenceSkeleton& Source, const TMap<FName, FQuat>& SourceOffsets,
	const FReferenceSkeleton& Target, const TMap<FName, FQuat>& TargetOffsets, TConstArrayView<FName> Bones, double* OutLargestDegrees)
{
	// A bone's retarget-pose local transform: reference local, its rotation * offset (IK Rig's retarget pose).
	auto LocalOf = [](const FReferenceSkeleton& Skeleton, const TMap<FName, FQuat>& Offsets, const int32 Bone)
	{
		FTransform Local = Skeleton.GetRawRefBonePose()[Bone];
		if (const FQuat* Offset = Offsets.Find(Skeleton.GetBoneName(Bone)))
		{
			Local.SetRotation((Local.GetRotation() * *Offset).GetNormalized());
		}
		return Local;
	};
	TArray<FTransform> SourceGlobal;
	for (int32 Bone = 0; Bone < Source.GetRawBoneNum(); ++Bone)
	{
		const int32 Parent = Source.GetRawParentIndex(Bone);
		const FTransform Local = LocalOf(Source, SourceOffsets, Bone);
		SourceGlobal.Add(Parent == INDEX_NONE ? Local : Local * SourceGlobal[Parent]);
	}
	// Where Descendant sits in Ancestor's frame, through the bones between (the retarget pose's locals).
	auto Within = [&LocalOf](const FReferenceSkeleton& Skeleton, const TMap<FName, FQuat>& Offsets, const int32 Ancestor, const int32 Descendant)
	{
		FTransform Out = FTransform::Identity;
		for (int32 Bone = Descendant; Bone != Ancestor; Bone = Skeleton.GetRawParentIndex(Bone))
		{
			if (Bone == INDEX_NONE)
			{
				return FVector::ZeroVector;
			}
			Out = Out * LocalOf(Skeleton, Offsets, Bone);
		}
		return Out.GetTranslation();
	};
	auto IsUnder = [](const FReferenceSkeleton& Skeleton, const int32 Ancestor, int32 Bone)
	{
		for (; Bone != INDEX_NONE; Bone = Skeleton.GetRawParentIndex(Bone))
		{
			if (Bone == Ancestor)
			{
				return true;
			}
		}
		return false;
	};

	// Parent first, each matched bone turned (from its own rotation, never copying the source's: the skeletons' bone axes
	// differ, the UE4 Mannequin's most) so that it points where the source's points: a bone with one matched child at that
	// child; a hand at its middle finger, its palm turned so the index-to-pinky line matches (a hand without those, as a bone). A bone the turn cannot be
	// read for (no matched child, no fingers) is left as it is.
	TArray<FQuat> TargetGlobal;
	TMap<FName, FQuat> Out;
	double Largest = 0.0;
	for (int32 Bone = 0; Bone < Target.GetRawBoneNum(); ++Bone)
	{
		const FName Name = Target.GetBoneName(Bone);
		const int32 SourceBone = Source.FindRawBoneIndex(Name);
		const int32 Parent = Target.GetRawParentIndex(Bone);
		const FQuat ParentGlobal = Parent == INDEX_NONE ? FQuat::Identity : TargetGlobal[Parent];
		const FQuat Global = (ParentGlobal * LocalOf(Target, TargetOffsets, Bone).GetRotation()).GetNormalized();
		TargetGlobal.Add(Global);
		if (!Bones.Contains(Name) || SourceBone == INDEX_NONE)
		{
			continue;
		}
		// The same bone's vector to another (by name) in each skeleton's component space; zero when either lacks it.
		auto Toward = [&](const FString& Other, FVector& OutTarget, FVector& OutSource)
		{
			const int32 TargetOther = Target.FindRawBoneIndex(FName(*Other));
			const int32 SourceOther = Source.FindRawBoneIndex(FName(*Other));
			OutTarget = OutSource = FVector::ZeroVector;
			if (TargetOther != INDEX_NONE && SourceOther != INDEX_NONE && IsUnder(Target, Bone, TargetOther) && IsUnder(Source, SourceBone, SourceOther))
			{
				OutTarget = Global.RotateVector(Within(Target, TargetOffsets, Bone, TargetOther));
				OutSource = SourceGlobal[SourceBone].GetRotation().RotateVector(Within(Source, SourceOffsets, SourceBone, SourceOther));
			}
			return !OutTarget.IsNearlyZero() && !OutSource.IsNearlyZero();
		};
		FQuat Turn = FQuat::Identity;
		bool bTurned = false;
		const FString Text = Name.ToString();
		if (Text.StartsWith(TEXT("hand_")))
		{
			const FString Side = Text.RightChop(5);
			FVector TargetMiddle, SourceMiddle, TargetIndex, SourceIndex, TargetPinky, SourcePinky;
			if (Toward(TEXT("middle_01_") + Side, TargetMiddle, SourceMiddle) && Toward(TEXT("index_01_") + Side, TargetIndex, SourceIndex)
				&& Toward(TEXT("pinky_01_") + Side, TargetPinky, SourcePinky))
			{
				const FQuat TargetFrame = FRotationMatrix::MakeFromXY(TargetMiddle, TargetIndex - TargetPinky).ToQuat();
				const FQuat SourceFrame = FRotationMatrix::MakeFromXY(SourceMiddle, SourceIndex - SourcePinky).ToQuat();
				Turn = SourceFrame * TargetFrame.Inverse();
				bTurned = true;
			}
		}
		if (!bTurned)
		{
			// The one matched child.
			FName Child;
			int32 Count = 0;
			for (int32 Each = Bone + 1; Each < Target.GetRawBoneNum(); ++Each)
			{
				if (Target.GetRawParentIndex(Each) == Bone && Bones.Contains(Target.GetBoneName(Each)) && Source.FindRawBoneIndex(Target.GetBoneName(Each)) != INDEX_NONE)
				{
					Child = Target.GetBoneName(Each);
					++Count;
				}
			}
			FVector TargetChild, SourceChild;
			if (Count == 1 && Toward(Child.ToString(), TargetChild, SourceChild))
			{
				Turn = FQuat::FindBetweenVectors(TargetChild, SourceChild);
				bTurned = true;
			}
		}
		if (!bTurned)
		{
			continue;
		}
		const FQuat Matched = (Turn * Global).GetNormalized();
		TargetGlobal[Bone] = Matched;
		const FQuat Offset = (Target.GetRawRefBonePose()[Bone].GetRotation().Inverse() * (ParentGlobal.Inverse() * Matched)).GetNormalized();
		const FQuat* Was = TargetOffsets.Find(Name);
		Largest = FMath::Max(Largest, FMath::RadiansToDegrees(Offset.AngularDistance(Was ? *Was : FQuat::Identity)));
		Out.Add(Name, Offset);
	}
	if (OutLargestDegrees)
	{
		*OutLargestDegrees = Largest;
	}
	return Out;
}

// Under /Game, a writable mount: IK Rig's batch retarget duplicates through the asset tools, which refuse to create an
// asset under /Temp (a read-only root for them): the first run made nothing there. In memory only, never saved, and
// emptied again after every retarget.
const TCHAR* const FMantleEmoteRetarget::TransientFolder = TEXT("/Game/__MantleEmoteRetarget");

const TCHAR* FMantleEmoteRetarget::RetargeterPathFor(const FString& SkeletonId)
{
	using namespace MantleEmoteRetargetPrivate;
	if (SkeletonId.Equals(EmoteMetaHumanId, ESearchCase::CaseSensitive))
	{
		return TEXT("/MantleCore/Avatars/EmoteRetarget/RTG_Mantle_MetaHuman_to_UEFN.RTG_Mantle_MetaHuman_to_UEFN");
	}
	if (SkeletonId.Equals(EmoteUE5MannequinId, ESearchCase::CaseSensitive))
	{
		return TEXT("/MantleCore/Avatars/EmoteRetarget/RTG_Mantle_UE5Mannequin_to_UEFN.RTG_Mantle_UE5Mannequin_to_UEFN");
	}
	if (SkeletonId.Equals(EmoteUE4MannequinId, ESearchCase::CaseSensitive))
	{
		return TEXT("/MantleCore/Avatars/EmoteRetarget/RTG_Mantle_UE4Mannequin_to_UEFN.RTG_Mantle_UE4Mannequin_to_UEFN");
	}
	return nullptr;
}

FString FMantleEmoteRetarget::IdentifySkeleton(const USkeleton* Skeleton, const FMantleAvatarContract& Contract, FString* OutWhy)
{
	using namespace MantleEmoteRetargetPrivate;
	if (!Skeleton)
	{
		return FString();
	}
	return IdentifyJoints(FMantleCosmeticChecks::JointsOf(Skeleton->GetReferenceSkeleton()), Contract, OutWhy);
}

bool FMantleEmoteRetarget::IsBodyBone(const FName Bone)
{
	const FString Name = Bone.ToString();
	static const TSet<FString> NotBody = { TEXT("interaction"), TEXT("center_of_mass"), TEXT("attach"), TEXT("props_root"), TEXT("prop_01"), TEXT("poi") };
	return !Name.StartsWith(TEXT("ik_"), ESearchCase::IgnoreCase) && !Name.StartsWith(TEXT("VB "), ESearchCase::CaseSensitive)
		&& !Name.Contains(TEXT("corrective"), ESearchCase::IgnoreCase) && !NotBody.Contains(Name);
}

FString FMantleEmoteRetarget::IdentifyJoints(TConstArrayView<FMantleSkinJointName> Joints, const FMantleAvatarContract& Contract, FString* OutWhy)
{
	using namespace MantleEmoteRetargetPrivate;
	auto Say = [OutWhy](const FString& Why)
	{
		if (OutWhy)
		{
			*OutWhy = Why;
		}
	};
	Say(FString());
	const FString Ids[] = { Contract.EmoteSkeleton, FString(EmoteMetaHumanId), FString(EmoteUE5MannequinId), FString(EmoteUE4MannequinId) };
	// Exactly one of them: every bone is one of its bones, under the same parent.
	for (const FString& Id : Ids)
	{
		const FMantleContractSkeleton* Fixture = Contract.FindSkeleton(Id);
		FString Error;
		if (Fixture && FMantleCosmeticChecks::MatchSkeleton(Joints, *Fixture, /*bRequireRoot*/ true, Error))
		{
			return Id;
		}
	}

	// One of them with bones of its own (extra twist, prop or IK bones, a tail): it carries every body bone of the
	// skeleton under the same parents. The retarget reads only the skeleton's chains, so the extra bones are left out.
	// The most specific skeleton wins (the most body bones carried); on a tie a retargeted one, so the clip's proportions
	// are carried over (the UEFN mannequin's body bones are the UE5 Mannequin's).
	const FString CarryIds[] = { FString(EmoteMetaHumanId), FString(EmoteUE5MannequinId), FString(EmoteUE4MannequinId), Contract.EmoteSkeleton };
	TMap<FName, FName> ParentOf;
	for (const FMantleSkinJointName& Joint : Joints)
	{
		ParentOf.Add(Joint.Name, Joints.IsValidIndex(Joint.Parent) ? Joints[Joint.Parent].Name : NAME_None);
	}
	FString Best;
	int32 BestCarried = 0;
	FString Nearest;
	FString NearestWhy;
	double NearestShare = -1.0;
	for (const FString& Id : CarryIds)
	{
		const FMantleContractSkeleton* Fixture = Contract.FindSkeleton(Id);
		if (!Fixture)
		{
			continue;
		}
		int32 Body = 0;
		int32 Carried = 0;
		FString FirstMiss;
		for (int32 Bone = 0; Bone < Fixture->Bones.Num(); ++Bone)
		{
			const FName Name = Fixture->Bones[Bone];
			if (!IsBodyBone(Name))
			{
				continue;
			}
			++Body;
			const FName* Parent = ParentOf.Find(Name);
			const FName Want = Fixture->Parents.IsValidIndex(Bone) && Fixture->Bones.IsValidIndex(Fixture->Parents[Bone]) ? Fixture->Bones[Fixture->Parents[Bone]] : NAME_None;
			if (Parent && *Parent == Want)
			{
				++Carried;
			}
			else if (FirstMiss.IsEmpty())
			{
				FirstMiss = Parent ? FString::Printf(TEXT("its %s is under %s, not %s"), *Name.ToString(), Parent->IsNone() ? TEXT("nothing") : *Parent->ToString(),
					Want.IsNone() ? TEXT("nothing") : *Want.ToString())
					: FString::Printf(TEXT("it has no %s"), *Name.ToString());
			}
		}
		if (Body > 0 && Carried == Body && Carried > BestCarried)
		{
			Best = Id;
			BestCarried = Carried;
		}
		const double Share = Body > 0 ? static_cast<double>(Carried) / Body : 0.0;
		if (Share > NearestShare)
		{
			NearestShare = Share;
			Nearest = Id;
			NearestWhy = FString::Printf(TEXT("it is nearest the %s skeleton (%d of its %d body bones), but %s"), *Id, Carried, Body, *FirstMiss);
		}
	}
	if (!Best.IsEmpty())
	{
		const FMantleContractSkeleton* Fixture = Contract.FindSkeleton(Best);
		TArray<FString> Extra;
		for (const FMantleSkinJointName& Joint : Joints)
		{
			if (Fixture->Find(Joint.Name) == INDEX_NONE)
			{
				Extra.Add(Joint.Name.ToString());
			}
		}
		const int32 Shown = FMath::Min(6, Extra.Num());
		Say(FString::Printf(TEXT("read as the %s skeleton; its %d bone(s) of its own (%s%s) are left out"), *Best, Extra.Num(),
			*FString::Join(TArrayView<const FString>(Extra.GetData(), Shown), TEXT(", ")), Extra.Num() > Shown ? TEXT(", ...") : TEXT("")));
		return Best;
	}
	Say(NearestWhy);
	return FString();
}

USkeletalMesh* FMantleEmoteRetarget::FindSourceMesh(USkeleton* Skeleton)
{
	if (!Skeleton)
	{
		return nullptr;
	}
	// The preview mesh only when it is already loaded: the const overload never loads, so a preview mesh the project does
	// not have (a git-ignored MetaHuman) is not looked for; the asset registry search below finds the meshes it has.
	USkeletalMesh* Preview = static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh();
	if (Preview && Preview->GetSkeleton() == Skeleton)
	{
		return Preview;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Meshes;
	Registry.GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), Meshes, true);
	const FString SkeletonPath = Skeleton->GetPathName();
	TArray<FAssetData> Candidates;
	for (const FAssetData& Data : Meshes)
	{
		FString Tag;
		Data.GetTagValue(TEXT("Skeleton"), Tag);
		if (FPackageName::ExportTextPathToObjectPath(Tag).Equals(SkeletonPath, ESearchCase::CaseSensitive))
		{
			Candidates.Add(Data);
		}
	}
	Candidates.StableSort([](const FAssetData& A, const FAssetData& B)
	{
		const bool bA = A.AssetName.ToString().Contains(TEXT("Body"));
		const bool bB = B.AssetName.ToString().Contains(TEXT("Body"));
		return bA != bB ? bA : A.PackageName.LexicalLess(B.PackageName);
	});
	for (int32 Index = 0; Index < FMath::Min(8, Candidates.Num()); ++Index)
	{
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Candidates[Index].GetAsset());
		if (Mesh && Mesh->GetSkeleton() == Skeleton)
		{
			return Mesh;
		}
	}
	return nullptr;
}

namespace MantleEmoteRetargetPrivate
{
	/**
	 * The rest-hands match on an opened or made FRetargeterCopy (FMantleEmoteRetarget::MakeRestHandsCopy); what it changed.
	 * bTurnHands false leaves hand_l and hand_r as they are (only the fingers are matched), so whatever a hand holds keeps
	 * its place and turn.
	 */
	FString MatchHands(const FRetargeterCopy& Copy, const FReferenceSkeleton& Source, const FReferenceSkeleton& Target, const bool bTurnHands = true)
	{
		FString OutNote;
		double Largest = 0.0;
		TArray<FName> Bones = FMantleEmoteRetarget::HandBones();
		if (!bTurnHands)
		{
			Bones.Remove(FName(TEXT("hand_l")));
			Bones.Remove(FName(TEXT("hand_r")));
		}
		const TMap<FName, FQuat> Offsets = FMantleEmoteRetarget::MatchRestRotations(Source, Copy.OffsetsOf(Source, FRetargeterCopy::Source), Target,
			Copy.OffsetsOf(Target, FRetargeterCopy::Target), Bones, &Largest);
		for (const TPair<FName, FQuat>& Offset : Offsets)
		{
			Copy.SetTargetOffset(Offset.Key, Offset.Value);
		}
		OutNote = FString::Printf(TEXT("%d %s bones of the target retarget pose given the source's rest shape (largest change %.1f degrees)"),
			Offsets.Num(), bTurnHands ? TEXT("hand and finger") : TEXT("finger (the hands as they were)"), Largest);
		// The finger chains joint for joint (the default, Interpolated, samples the source finger by length, so each joint of
		// a finger with other proportions gets a mix of two joints' bend). A source without metacarpals (the UE4 Mannequin)
		// gives the target's metacarpal chains nothing to follow: the shipped retargeters map them to the whole finger, and a
		// one-bone chain takes the fingertip's bend, which folds the palm (40 degrees and more in the trials). They hold the
		// matched rest pose instead.
		const bool bSourceMetacarpals = Source.FindRawBoneIndex(FName(TEXT("index_metacarpal_l"))) != INDEX_NONE;
		const FString Mode(TEXT("OneToOne"));
		int32 Fingers = 0;
		int32 Held = 0;
		Copy.EditFKChains([](const FName Chain)
		{
			const FString Name = Chain.ToString();
			return Name.Contains(TEXT("Thumb")) || Name.Contains(TEXT("Index")) || Name.Contains(TEXT("Middle")) || Name.Contains(TEXT("Ring")) || Name.Contains(TEXT("Pinky"));
		}, [&](const UScriptStruct& Chain, uint8* Element)
		{
			const FNameProperty* ChainName = CastField<FNameProperty>(Chain.FindPropertyByName(TEXT("TargetChainName")));
			const bool bMetacarpal = ChainName && ChainName->GetPropertyValue_InContainer(Element).ToString().Contains(TEXT("Metacarpal"));
			if (bMetacarpal && !bSourceMetacarpals)
			{
				if (const FBoolProperty* EnableFK = CastField<FBoolProperty>(Chain.FindPropertyByName(TEXT("EnableFK"))))
				{
					EnableFK->SetPropertyValue_InContainer(Element, false);
					++Held;
				}
				return;
			}
			const FEnumProperty* RotationMode = CastField<FEnumProperty>(Chain.FindPropertyByName(TEXT("RotationMode")));
			const int64 Value = RotationMode ? RotationMode->GetEnum()->GetValueByNameString(Mode) : INDEX_NONE;
			if (Value != INDEX_NONE)
			{
				RotationMode->GetUnderlyingProperty()->SetIntPropertyValue(RotationMode->ContainerPtrToValuePtr<void>(Element), Value);
				++Fingers;
			}
		});
		OutNote += FString::Printf(TEXT("; %d finger chains retargeted joint for joint%s"), Fingers,
			Held > 0 ? *FString::Printf(TEXT(", %d metacarpal chains held at the rest pose (the source has no metacarpals)"), Held) : TEXT(""));
		return OutNote;
	}
}

UObject* FMantleEmoteRetarget::MakeRestHandsCopy(UObject* Retargeter, const FReferenceSkeleton& Source, const FReferenceSkeleton& Target, FString& OutNote,
	FString& OutError)
{
	using namespace MantleEmoteRetargetPrivate;
	FRetargeterCopy Copy;
	if (!Copy.Make(Retargeter, OutError))
	{
		return nullptr;
	}
	OutNote = MatchHands(Copy, Source, Target);
	return Copy.Detach();
}

bool FMantleEmoteRetarget::MatchRestHandsInPlace(UObject* Retargeter, const FReferenceSkeleton& Source, const FReferenceSkeleton& Target, FString& OutNote,
	FString& OutError)
{
	using namespace MantleEmoteRetargetPrivate;
	FRetargeterCopy Asset;
	if (!Asset.Open(Retargeter, OutError))
	{
		return false;
	}
	OutNote = MatchHands(Asset, Source, Target, /*bTurnHands*/ false);
	return true;
}

bool FMantleEmoteRetarget::MatchWristsInPlace(UObject* Retargeter, const FReferenceSkeleton& Source, const FReferenceSkeleton& Target, FString& OutNote,
	FString& OutError)
{
	using namespace MantleEmoteRetargetPrivate;
	FRetargeterCopy Asset;
	if (!Asset.Open(Retargeter, OutError))
	{
		return false;
	}
	const TMap<FName, FQuat> SourceOffsets = Asset.OffsetsOf(Source, FRetargeterCopy::Source);
	const TMap<FName, FQuat> TargetOffsets = Asset.OffsetsOf(Target, FRetargeterCopy::Target);
	// Component-space bone transforms of a skeleton's reference pose, or its retarget pose (reference * offsets).
	auto Globals = [](const FReferenceSkeleton& Skeleton, const TMap<FName, FQuat>* Offsets)
	{
		TArray<FTransform> Out;
		for (int32 Bone = 0; Bone < Skeleton.GetRawBoneNum(); ++Bone)
		{
			FTransform Local = Skeleton.GetRawRefBonePose()[Bone];
			if (const FQuat* Offset = Offsets ? Offsets->Find(Skeleton.GetBoneName(Bone)) : nullptr)
			{
				Local.SetRotation((Local.GetRotation() * *Offset).GetNormalized());
			}
			const int32 Parent = Skeleton.GetRawParentIndex(Bone);
			Out.Add(Parent == INDEX_NONE ? Local : Local * Out[Parent]);
		}
		return Out;
	};
	// The hand's twist about the forearm, as Mantle.Dev.CheckEmote measures it.
	auto Twist = [](const TArray<FTransform>& Pose, const int32 Lower, const int32 Hand)
	{
		const FVector Axis = (Pose[Hand].GetLocation() - Pose[Lower].GetLocation()).GetSafeNormal();
		const FQuat Relative = (Pose[Hand].GetRotation() * Pose[Lower].GetRotation().Inverse()).GetNormalized();
		return FMath::RadiansToDegrees(2.0 * FMath::Atan2(FVector(Relative.X, Relative.Y, Relative.Z) | Axis, static_cast<double>(Relative.W)));
	};
	const TArray<FTransform> SourceRef = Globals(Source, nullptr), SourcePose = Globals(Source, &SourceOffsets);
	const TArray<FTransform> TargetRef = Globals(Target, nullptr);
	TArray<FTransform> TargetPose = Globals(Target, &TargetOffsets);
	OutNote.Reset();
	for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
	{
		auto Find = [Side](const FReferenceSkeleton& Skeleton, const TCHAR* Stem) { return Skeleton.FindRawBoneIndex(FName(*FString::Printf(TEXT("%s_%s"), Stem, Side))); };
		const int32 SL = Find(Source, TEXT("lowerarm")), SH = Find(Source, TEXT("hand"));
		const int32 TL = Find(Target, TEXT("lowerarm")), TH = Find(Target, TEXT("hand"));
		if (SL == INDEX_NONE || SH == INDEX_NONE || TL == INDEX_NONE || TH == INDEX_NONE)
		{
			continue;
		}
		// How far each retarget pose's wrist sits from its own reference pose; the animation carries the source's difference
		// from its reference onto the target, so what the two differ by is the error every frame.
		const double TwistGap = FMath::UnwindDegrees((Twist(TargetPose, TL, TH) - Twist(TargetRef, TL, TH)) - (Twist(SourcePose, SL, SH) - Twist(SourceRef, SL, SH)));
		const FVector Axis = (TargetPose[TH].GetLocation() - TargetPose[TL].GetLocation()).GetSafeNormal();
		const FQuat Turn(Axis, FMath::DegreesToRadians(-TwistGap));
		const int32 Parent = Target.GetRawParentIndex(TH);
		const FQuat ParentGlobal = Parent == INDEX_NONE ? FQuat::Identity : TargetPose[Parent].GetRotation();
		const FQuat NewLocal = (ParentGlobal.Inverse() * Turn * TargetPose[TH].GetRotation()).GetNormalized();
		const FName Hand = Target.GetBoneName(TH);
		const FQuat NewOffset = (Target.GetRawRefBonePose()[TH].GetRotation().Inverse() * NewLocal).GetNormalized();
		Asset.SetTargetOffset(Hand, NewOffset);
		OutNote += FString::Printf(TEXT("%shand_%s turned %.1f degrees about the forearm"), OutNote.IsEmpty() ? TEXT("") : TEXT("; "), Side, -TwistGap);
	}
	if (OutNote.IsEmpty())
	{
		OutError = TEXT("the source or target has no lowerarm and hand bones.");
		return false;
	}
	return true;
}

void FMantleEmoteRetarget::ReleaseRestHandsCopy(UObject* Copy)
{
	if (Copy)
	{
		Copy->RemoveFromRoot();
		Copy->MarkAsGarbage();
	}
}

bool FMantleEmoteRetarget::RetargetAndSample(UAnimSequence* Sequence, const FString& SkeletonId, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh,
	const FMantleAvatarContract& Contract, FMantleAnimClipData& OutClip, FString& OutError, FString* OutHandsNote)
{
	using namespace MantleEmoteRetargetPrivate;
	OutClip = FMantleAnimClipData();
	const TCHAR* const RetargeterPath = RetargeterPathFor(SkeletonId);
	if (!Sequence)
	{
		OutError = TEXT("no animation was chosen.");
		return false;
	}
	if (!RetargeterPath)
	{
		OutError = FString::Printf(TEXT("Mantle retargets emotes from the MetaHuman, UE5 Mannequin and UE4 Mannequin skeletons only, not from '%s'. Retarget %s onto SK_UEFN_Mannequin (right-click it > Retarget Animations) and upload the retargeted copy."),
			*SkeletonId, *Sequence->GetName());
		return false;
	}
	if (!TargetMesh)
	{
		OutError = FString::Printf(TEXT("%s is on the %s skeleton and emotes play on the UEFN mannequin, but this project has none and the Mantle plugin could not make its own to retarget it onto (see the Output Log): reinstall the Mantle plugin, or add SKM_UEFN_Mannequin (from the Game Animation Sample) to the project, and upload again."),
			*Sequence->GetName(), *SkeletonId);
		return false;
	}
	if (!SourceMesh)
	{
		OutError = FString::Printf(TEXT("no skeletal mesh in this project uses %s's skeleton %s, so it cannot be retargeted. Give the skeleton a preview mesh and upload again."),
			*Sequence->GetName(), Sequence->GetSkeleton() ? *Sequence->GetSkeleton()->GetName() : TEXT("(none)"));
		return false;
	}
	if (SourceMesh->GetSkeleton() != Sequence->GetSkeleton() || SourceMesh == TargetMesh)
	{
		OutError = FString::Printf(TEXT("%s is not on %s's skeleton, so it cannot be retargeted from."), *SourceMesh->GetName(), *Sequence->GetName());
		return false;
	}
	// The plugin's built-in mannequin is made from the rig every emote retargeter targets, so it has every bone they
	// drive; it lacks only the fixture's prop bones, which then get no track and stay in their reference pose.
	const FMantleContractSkeleton* Uefn = Contract.FindSkeleton(Contract.EmoteSkeleton);
	FString MissingBone;
	if (!FMantleBuiltInMannequin::Is(TargetMesh) && (!Uefn || !FMantleCosmeticExport::HasFixtureBones(TargetMesh->GetSkeleton(), *Uefn, &MissingBone)))
	{
		OutError = FString::Printf(TEXT("%s is not a UEFN mannequin (its skeleton has no bone %s), so emotes cannot be retargeted onto it."),
			*TargetMesh->GetName(), Uefn ? *MissingBone : TEXT("of the uefn fixture"));
		return false;
	}

	const FSoftObjectPath Path(RetargeterPath);
	UObject* Retargeter = FPackageName::DoesPackageExist(Path.GetLongPackageName()) ? Path.TryLoad() : nullptr;
	const UClass* RetargeterClass = FindObject<UClass>(nullptr, RetargeterClassPath);
	if (!Retargeter || !RetargeterClass || !Retargeter->IsA(RetargeterClass))
	{
		OutError = FString::Printf(TEXT("the Mantle plugin's retargeter %s is missing or not an IK retargeter: reinstall the Mantle plugin."), RetargeterPath);
		return false;
	}
	// mantle #840 step 1, rest-pose match: Unreal's auto-align writes no offsets for fingers, so the FK finger chains would
	// carry the difference between the two rest hands into every frame (a constant 17 to 35 degree under-curl). This
	// retarget runs through a copy whose UEFN pose has the source's rest hands and fingers.
	FString MatchError;
	FString MatchNote;
	UObject* const Matched = MakeRestHandsCopy(Retargeter, SourceMesh->GetRefSkeleton(), TargetMesh->GetRefSkeleton(), MatchNote, MatchError);
	if (!Matched)
	{
		OutError = FString::Printf(TEXT("the retargeter's rest hands could not be matched (%s): enable the IK Rig plugin and restart the editor."), *MatchError);
		return false;
	}
	if (OutHandsNote)
	{
		*OutHandsNote = TEXT("Emote rest hands: ") + MatchNote;
	}
	ON_SCOPE_EXIT
	{
		ReleaseRestHandsCopy(Matched);
	};
	const double Frame = Contract.EmoteFrameRate > 0.0f ? 1.0 / Contract.EmoteFrameRate : 1.0 / 30.0;
	return RetargetThrough(Sequence, SourceMesh, TargetMesh, Matched, Frame,
		[&Contract, &OutClip, Sequence](UAnimSequence& Retargeted, FString& OutUseError)
		{
			FString SampleError;
			if (!FMantleAnimClipWriter::Sample(&Retargeted, Contract, OutClip, SampleError))
			{
				OutUseError = FString::Printf(TEXT("the retargeted %s could not be read: %s"), *Sequence->GetName(), *SampleError);
				return false;
			}
			return true;
		}, OutError);
}

bool FMantleEmoteRetarget::RetargetThrough(UAnimSequence* Sequence, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, UObject* Retargeter,
	const double LengthTolerance, TFunctionRef<bool(UAnimSequence& Retargeted, FString& OutUseError)> Use, FString& OutError)
{
	using namespace MantleEmoteRetargetPrivate;
	if (!Sequence || !SourceMesh || !TargetMesh || !Retargeter)
	{
		OutError = TEXT("the retarget needs a clip, a mesh to play it on, a mesh to retarget it onto and a retargeter.");
		return false;
	}
	const FString RetargeterPath = Retargeter->GetPathName();
	FBatchCall Call;
	FString CallError;
	if (!FindBatchCall(Call, &CallError))
	{
		OutError = FString::Printf(TEXT("IK Rig's batch retarget cannot be called (%s): enable the IK Rig plugin and restart the editor."), *CallError);
		return false;
	}
	if (!Call.Retargeter->PropertyClass || !Retargeter->IsA(Call.Retargeter->PropertyClass))
	{
		OutError = FString::Printf(TEXT("%s is not the retargeter class IK Rig's batch retarget takes."), *RetargeterPath);
		return false;
	}

	// IK Rig's batch retarget points the Content Browser at what it made (SyncBrowserToAssets): a copy that is gone a moment
	// later, so the creator's browser would jump to the transient folder and keep a deleted asset selected. It goes back to
	// what was selected, else to the folder it was showing, once the copy is gone.
	IContentBrowserSingleton& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get();
	TArray<FAssetData> WasSelected;
	Browser.GetSelectedAssets(WasSelected);
	const FContentBrowserItemPath WasPath = Browser.GetCurrentPath();
	const FString WasFolder = WasPath.HasInternalPath() ? WasPath.GetInternalPathString() : FString();

	TArray<FAssetData> Created;
	{
		// The batch retarget makes TargetMesh the copy's retarget source, which UAnimSequence refuses for a transient mesh
		// with an error and an ensure: the plugin's built-in UEFN mannequin is one (made in memory). It lives in the transient
		// package all the same, so it is never saved; the flag is back as soon as the retarget is done.
		const bool bTransientTarget = TargetMesh->HasAnyFlags(RF_Transient);
		TargetMesh->ClearFlags(RF_Transient);
		ON_SCOPE_EXIT
		{
			if (bTransientTarget)
			{
				TargetMesh->SetFlags(RF_Transient);
			}
		};
		RunBatch(Call, Sequence, SourceMesh, TargetMesh, Retargeter, TransientFolder, Created);
	}

	// Whatever it made is dropped, whatever happens next.
	TArray<UObject*> Made;
	UAnimSequence* Retargeted = nullptr;
	for (const FAssetData& Data : Created)
	{
		if (UObject* Asset = Data.FastGetAsset(/*bLoad*/ false))
		{
			Made.Add(Asset);
			Retargeted = Retargeted ? Retargeted : Cast<UAnimSequence>(Asset);
		}
	}
	const FString Folder(TransientFolder);
	ON_SCOPE_EXIT
	{
		for (UObject* Asset : Made)
		{
			MantleEmoteRetargetPrivate::DiscardTemporary(Asset, Folder);
		}
		MantleEmoteRetargetPrivate::RemoveFolderIfEmpty(Folder);
		if (WasSelected.Num() > 0)
		{
			Browser.SyncBrowserToAssets(WasSelected, /*bAllowLockedBrowsers*/ false, /*bFocusContentBrowser*/ false);
		}
		else
		{
			Browser.SyncBrowserToFolders({ WasFolder.IsEmpty() || WasFolder.StartsWith(Folder) ? FString(TEXT("/Game")) : WasFolder },
				/*bAllowLockedBrowsers*/ false, /*bFocusContentBrowser*/ false);
		}
	};
	if (Made.Num() != 1 || !Retargeted || !Retargeted->GetPackage()->GetName().StartsWith(Folder + TEXT("/"), ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(TEXT("IK Rig's batch retarget made %d assets from %s (played on %s, onto %s, through %s), not one animation in %s: see the Output Log."),
			Created.Num(), *Sequence->GetName(), *SourceMesh->GetPathName(), *TargetMesh->GetPathName(), *RetargeterPath, TransientFolder);
		return false;
	}
	if (Retargeted->GetSkeleton() != TargetMesh->GetSkeleton())
	{
		OutError = FString::Printf(TEXT("the retargeted %s is on %s, not on %s's skeleton."), *Sequence->GetName(),
			Retargeted->GetSkeleton() ? *Retargeted->GetSkeleton()->GetName() : TEXT("no skeleton"), *TargetMesh->GetName());
		return false;
	}
	const double SourceLength = Sequence->GetPlayLength();
	const double RetargetedLength = Retargeted->GetPlayLength();
	if (FMath::Abs(SourceLength - RetargetedLength) > LengthTolerance + 1e-3)
	{
		OutError = FString::Printf(TEXT("the retargeted %s is %.3f s long; the clip is %.3f s."), *Sequence->GetName(), RetargetedLength, SourceLength);
		return false;
	}
	return Use(*Retargeted, OutError);
}

USkeletalMesh* FMantleEmoteRetarget::ChooseSourceMesh(UAnimSequence* Sequence, USkeletalMesh* PickedBody, FString* OutFrom)
{
	USkeleton* Skeleton = Sequence ? Sequence->GetSkeleton() : nullptr;
	auto Say = [OutFrom](const TCHAR* From)
	{
		if (OutFrom)
		{
			*OutFrom = From;
		}
	};
	Say(TEXT("none"));
	if (!Skeleton)
	{
		return nullptr;
	}
	// #584: the body the clip was animated on, when it says: its own preview mesh (loaded if it has to be; a mesh the
	// project does not have loads as nothing and is passed over).
	if (USkeletalMesh* Preview = Sequence->GetPreviewMesh(/*bFindIfNotSet*/ false))
	{
		if (Preview->GetSkeleton() == Skeleton)
		{
			Say(TEXT("the animation's preview mesh"));
			return Preview;
		}
	}
	// Then the body the creator picked in the panel, when it is on the clip's skeleton.
	if (PickedBody && PickedBody->GetSkeleton() == Skeleton)
	{
		Say(TEXT("the body picked in the panel"));
		return PickedBody;
	}
	USkeletalMesh* Found = FindSourceMesh(Skeleton);
	if (Found)
	{
		Say(TEXT("a mesh on the clip's skeleton"));
	}
	return Found;
}

bool FMantleEmoteRetarget::Sample(UAnimSequence* Sequence, const FString& SkeletonId, const FMantleAvatarContract& Contract, FMantleAnimClipData& OutClip,
	FString& OutError, FString& OutNote, USkeletalMesh* PickedBody)
{
	FString SourceFrom;
	USkeletalMesh* Source = ChooseSourceMesh(Sequence, PickedBody, &SourceFrom);
	USkeletalMesh* Target = FMantleBuiltInMannequin::UEFNMannequin(Contract);
	const double Start = FPlatformTime::Seconds();
	FString HandsNote;
	if (!RetargetAndSample(Sequence, SkeletonId, Source, Target, Contract, OutClip, OutError, &HandsNote))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_EMOTE_RETARGET failed %s skeleton=%s: %s"), Sequence ? *Sequence->GetPathName() : TEXT("(none)"), *SkeletonId, *OutError);
		return false;
	}
	const double Seconds = FPlatformTime::Seconds() - Start;
	OutNote = FString::Printf(TEXT("Emote retargeted from the %s skeleton (played on %s, %s) onto %s through %s in %.1f s"), *SkeletonId, *Source->GetName(),
		*SourceFrom, *Target->GetName(), *FPackageName::ObjectPathToObjectName(FString(RetargeterPathFor(SkeletonId))), Seconds);
	if (!HandsNote.IsEmpty())
	{
		OutNote += TEXT(". ") + HandsNote;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_EMOTE_RETARGET %s skeleton=%s source=%s (%s) target=%s frames=%d tracks=%d seconds=%.2f"), *Sequence->GetPathName(),
		*SkeletonId, *Source->GetPathName(), *SourceFrom, *Target->GetPathName(), OutClip.FrameCount, OutClip.Tracks.Num(), Seconds);
	return true;
}
