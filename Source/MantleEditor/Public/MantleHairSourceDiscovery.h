// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/** Reflected Groom binding evidence. Distinct source/target heads require an explicit authoring choice. */
struct FMantleHairBindingHead
{
	FString Groom;
	FString SourceHead;
	FString TargetHead;
};

class MANTLEEDITOR_API FMantleHairSourceDiscovery
{
public:
	/** Editor-only asset metadata retained on generated Grooms/cards. */
	static constexpr const TCHAR* OriginalGroomTag = TEXT("MantleHairOriginalGroom");
	static constexpr const TCHAR* AuthoringHeadTag = TEXT("MantleHairAuthoringHead");

	/** Exact association, never a global body fallback. Null with an actionable reason for ambiguous/unknown provenance. */
	static USkeletalMesh* FindHead(const UObject* CardsOrGroom, FString& Error, bool* HasAssociation = nullptr);
	/** A loaded MetaHuman authoring head, including a head-only reference rig used by native Groom bindings. */
	static bool IsHead(const USkeletalMesh* Mesh);
	/** Pure binding association selection. Repeated bindings to the same head agree; distinct heads refuse. */
	static bool ChooseHead(const FString& Groom, TConstArrayView<FMantleHairBindingHead> Bindings, FString& Head, FString& Error);
};
