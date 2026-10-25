// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPreviewRetarget.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticPreviewRetargetPrivate
{
	/** IKRig's Retarget Pose From Mesh node, the one UMantleBodyAnimInstance runs. */
	const TCHAR* const RetargetNodePath = TEXT("/Script/IKRig.AnimNode_RetargetPoseFromMesh");

	/** What the proxy sets on the node, found by name on its struct. */
	struct FRetargetNodeLayout
	{
		const UScriptStruct* Struct = nullptr;
		const FObjectPropertyBase* Asset = nullptr;
		const FObjectPropertyBase* Source = nullptr;
		const FEnumProperty* Mode = nullptr;
		/** ERetargetSourceMode::CustomSkeletalMeshComponent's value. */
		int64 CustomSource = INDEX_NONE;
	};

	/**
	 * The node's struct and the properties the proxy sets (IKRetargeterAsset, SourceMeshComponent, RetargetFrom), or
	 * false with what is missing: IKRig's module not loaded, or a node that is not the one this was written against.
	 */
	bool FindRetargetNode(FRetargetNodeLayout& Out, FString* OutError)
	{
		Out = FRetargetNodeLayout();
		auto Fail = [OutError](const FString& Why)
		{
			if (OutError)
			{
				*OutError = Why;
			}
			return false;
		};
		const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, RetargetNodePath);
		if (!Struct)
		{
			return Fail(FString::Printf(TEXT("%s is not loaded (the IK Rig plugin's module)"), RetargetNodePath));
		}
		if (!Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
		{
			return Fail(FString::Printf(TEXT("%s is not an anim node"), RetargetNodePath));
		}
		Out.Asset = FindFProperty<FObjectPropertyBase>(Struct, TEXT("IKRetargeterAsset"));
		Out.Source = FindFProperty<FObjectPropertyBase>(Struct, TEXT("SourceMeshComponent"));
		Out.Mode = FindFProperty<FEnumProperty>(Struct, TEXT("RetargetFrom"));
		if (!Out.Asset || !Out.Source || !Out.Mode || !Out.Mode->GetEnum() || !Out.Mode->GetUnderlyingProperty())
		{
			return Fail(TEXT("the retarget node has no IKRetargeterAsset, SourceMeshComponent or RetargetFrom property"));
		}
		const UEnum* ModeEnum = Out.Mode->GetEnum();
		Out.CustomSource = ModeEnum->GetValueByNameString(ModeEnum->GenerateFullEnumName(TEXT("CustomSkeletalMeshComponent")));
		if (Out.CustomSource == INDEX_NONE)
		{
			return Fail(FString::Printf(TEXT("%s has no CustomSkeletalMeshComponent"), *ModeEnum->GetName()));
		}
		Out.Struct = Struct;
		return true;
	}

	/**
	 * The client's proxy (FMantleBodyAnimInstanceProxy) reduced to its retarget: the node is initialised, fed on the game
	 * thread in PreUpdate and run from the proxy's own update and evaluate, reading the driver component. The node lives
	 * in memory the node's own struct constructs; its properties are set by name. Without a node, a retargeter or a
	 * driver the body holds its reference pose.
	 */
	class FMantlePreviewRetargetProxy : public FAnimInstanceProxy
	{
	public:
		explicit FMantlePreviewRetargetProxy(UAnimInstance* InInstance)
			: FAnimInstanceProxy(InInstance)
		{
			if (!FindRetargetNode(Layout, nullptr))
			{
				// No node, or not the one this was written against: the reference pose rather than a guess.
				return;
			}
			NodeMemory = FMemory::Malloc(FMath::Max(1, Layout.Struct->GetStructureSize()), Layout.Struct->GetMinAlignment());
			Layout.Struct->InitializeStruct(NodeMemory);
			// The driver is another component (the client's ERetargetSourceMode::CustomSkeletalMeshComponent).
			Layout.Mode->GetUnderlyingProperty()->SetIntPropertyValue(Layout.Mode->ContainerPtrToValuePtr<void>(NodeMemory), Layout.CustomSource);
		}

		virtual ~FMantlePreviewRetargetProxy() override
		{
			if (NodeMemory)
			{
				Layout.Struct->DestroyStruct(NodeMemory);
				FMemory::Free(NodeMemory);
				NodeMemory = nullptr;
			}
		}

		UE_NONCOPYABLE(FMantlePreviewRetargetProxy);

		virtual void Initialize(UAnimInstance* InAnimInstance) override
		{
			FAnimInstanceProxy::Initialize(InAnimInstance);
			SyncFromInstance(InAnimInstance);
			if (NodeMemory)
			{
				FAnimationInitializeContext Context(this);
				Node()->Initialize_AnyThread(Context);
			}
		}

		virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override
		{
			FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
			// Game thread: the only place the node may read UObjects (the driver's pose), so its inputs are set here.
			SyncFromInstance(InAnimInstance);
			if (bReady)
			{
				Node()->PreUpdate(InAnimInstance);
			}
		}

		virtual void CacheBones() override
		{
			FAnimInstanceProxy::CacheBones();
			if (NodeMemory)
			{
				FAnimationCacheBonesContext Context(this);
				Node()->CacheBones_AnyThread(Context);
			}
		}

		virtual void UpdateAnimationNode(const FAnimationUpdateContext& InContext) override
		{
			if (bReady)
			{
				Node()->Update_AnyThread(InContext);
			}
		}

		virtual bool Evaluate(FPoseContext& Output) override
		{
			if (bReady)
			{
				Node()->Evaluate_AnyThread(Output);
			}
			else
			{
				Output.ResetToRefPose();
			}
			return true;
		}

	private:
		FAnimNode_Base* Node() const
		{
			// The struct is a child of FAnimNode_Base by single inheritance: its memory starts with the base.
			return static_cast<FAnimNode_Base*>(NodeMemory);
		}

		void SyncFromInstance(const UAnimInstance* InAnimInstance)
		{
			bReady = false;
			const UMantlePreviewRetargetAnimInstance* Instance = Cast<UMantlePreviewRetargetAnimInstance>(InAnimInstance);
			if (!NodeMemory || !Instance)
			{
				return;
			}
			UObject* RetargetAsset = Instance->Retargeter;
			if (RetargetAsset && !RetargetAsset->IsA(Layout.Asset->PropertyClass))
			{
				RetargetAsset = nullptr;
			}
			USkeletalMeshComponent* Driver = Instance->Source;
			Layout.Asset->SetObjectPropertyValue_InContainer(NodeMemory, RetargetAsset);
			Layout.Source->SetObjectPropertyValue_InContainer(NodeMemory, Driver);
			bReady = RetargetAsset && Driver && Driver->GetSkeletalMeshAsset();
		}

		FRetargetNodeLayout Layout;
		void* NodeMemory = nullptr;
		/** A node, a retargeter and a driver with a mesh: the pose is retargeted; else the reference pose. */
		bool bReady = false;
	};
}

bool UMantlePreviewRetargetAnimInstance::IsRetargetNodeAvailable(FString* OutError)
{
	MantleCosmeticPreviewRetargetPrivate::FRetargetNodeLayout Layout;
	return MantleCosmeticPreviewRetargetPrivate::FindRetargetNode(Layout, OutError);
}

FAnimInstanceProxy* UMantlePreviewRetargetAnimInstance::CreateAnimInstanceProxy()
{
	return new MantleCosmeticPreviewRetargetPrivate::FMantlePreviewRetargetProxy(this);
}

void UMantlePreviewRetargetAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete InProxy;
}
