// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarRigMapFixer.h"
#include "Widgets/SCompoundWidget.h"

class USkeletalMesh;

DECLARE_DELEGATE_OneParam(FOnMantleRigMappingChanged, const FMantleRigMapping&);

/**
 * The guided bone mapping for a rig the RigMap fixer does not know (FMantleAvatarRigMap). One dropdown per required
 * bone of the target skeleton, listing the rig's bones, pre-filled with the saved or suggested mapping; the bones the
 * converter cannot insert must be chosen, the others may be left empty (they are added with no weights). Every change
 * becomes the session's mapping for the mesh (FMantleAvatarRigMap::SetSessionMapping), which the fixer's Analyze and Apply
 * use, and is reported through OnMappingChanged so the fix list can analyse again. Save as profile writes the mapping to
 * <Project>/Saved/Mantle/RigProfiles, so the rig is known from then on. The fix list embeds it under an unknown rig's line.
 */
class SMantleRigMappingPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantleRigMappingPanel)
		: _Mesh(nullptr)
	{}
		/** The creator's mesh, as the fix list has it (the part's mesh before the RigMap fix). */
		SLATE_ARGUMENT(USkeletalMesh*, Mesh)
		SLATE_EVENT(FOnMantleRigMappingChanged, OnMappingChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TSharedRef<SWidget> MakeRow(FName Target);
	void Choose(FName Target, FName Source);
	FText StatusText() const;
	FReply SaveProfile();

	TWeakObjectPtr<USkeletalMesh> Mesh;
	FMantleRigMapping Mapping;
	FOnMantleRigMappingChanged OnMappingChanged;

	/** "none" first, then the rig's bones in its order. */
	TArray<TSharedPtr<FName>> SourceOptions;

	FString ProfileName;
	FText SaveResult;
};
