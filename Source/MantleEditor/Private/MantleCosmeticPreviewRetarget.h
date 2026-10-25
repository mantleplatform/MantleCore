// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "UObject/ObjectPtr.h"
#include "MantleCosmeticPreviewRetarget.generated.h"

class USkeletalMeshComponent;

/**
 * The cosmetic panel's avatar preview pose (A2.4 #283): the client's body graph (UMantleBodyAnimInstance) without its
 * gameplay passes. A Retarget Pose From Mesh node copies the hidden UEFN driver's pose onto the avatar body through the
 * avatar skeleton's IK retargeter, as the client poses every avatar. No left-hand IK (no tool is held), no voice jaw,
 * and no RigLogic, so a MetaHuman body's DNA correctives do not run here.
 *
 * The node is IKRig's FAnimNode_RetargetPoseFromMesh, made and set through reflection the way an anim Blueprint holds
 * its nodes (UScriptStruct plus FProperty, driven through FAnimNode_Base), so MantleEditor takes no module dependency
 * on the IKRig plugin. IKRig is enabled by default in every project; when its module is not loaded, or the retargeter
 * or the driver is missing, the body holds its reference pose. Editor preview only.
 */
UCLASS(Transient, NotBlueprintable)
class UMantlePreviewRetargetAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** The IK retargeter (a UIKRetargeter, held as a UObject; the proxy checks its class against the node's property). */
	UPROPERTY(Transient)
	TObjectPtr<UObject> Retargeter;

	/** The component whose pose is retargeted: the preview's hidden UEFN driver. */
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> Source;

	/**
	 * True when IKRig's retarget node is loaded (its module is) and has the properties the preview sets, so a body can
	 * be posed through a retargeter; else false with what is missing.
	 */
	static bool IsRetargetNodeAvailable(FString* OutError = nullptr);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;
};
