// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleWeaponFit.h"
#include "MantleWeaponSource.h"
#include "MantleWeaponDocument.h"

/**
 * The weapon builder window's rules that need no window (#729): the holds a creator can choose today, the starting numbers
 * and allowed ranges for each hold, and the plain-words names. Kept apart from the Slate code so tests can run them.
 */
namespace MantleWeaponBuilderLogic
{
	struct FRange
	{
		float Min = 0.f;
		float Max = 0.f;
	};

	/** The persisted hold and the fit's hold list the same eight holds in the same order. */
	MANTLEEDITOR_API EMantleWeaponFitHold ToFitHold(EMantleWeaponHold Hold);
	MANTLEEDITOR_API EMantleWeaponHold FromFitHold(EMantleWeaponFitHold Hold);
	MANTLEEDITOR_API EMantleWeaponDocHold ToDocHold(EMantleWeaponHold Hold);
	/** The runtime socket name for a document point, shared by fit and export. */
	MANTLEEDITOR_API FName SocketRole(EMantleWeaponDocSocket Socket);
	MANTLEEDITOR_API bool DocSocket(FName Role, EMantleWeaponDocSocket& OutSocket);

	/** Whether Hold is a real hold (every hold is available; the Count sentinel and out-of-range values are not). */
	MANTLEEDITOR_API bool IsHoldAvailable(EMantleWeaponHold Hold);

	/** The delivery a hold uses when the creator picks none: the table's first, except the staff's, which is the held beam (the only staff the game plays so far). */
	MANTLEEDITOR_API EMantleWeaponDocDelivery UsualDelivery(EMantleWeaponDocHold Hold);

	/** Whether the panel offers this delivery for the hold: every one its table lists. */
	MANTLEEDITOR_API bool IsDeliveryOffered(EMantleWeaponDocHold Hold, EMantleWeaponDocDelivery Delivery);

	/** #905: the delivery a source attacks with: the creator's pick, or the hold's usual (UsualDelivery) when it is Default. */
	MANTLEEDITOR_API EMantleWeaponDocDelivery DeliveryOf(const UMantleWeaponSource& Source);

	/**
	 * #905: whether the hold's effect style choices are the elements (laser, fire, ice...) rather than the firearm styles (rifle, pistol,
	 * shotgun): every hold but a gun, and a gun that launches rounds or fires a beam. A gun's instant shot keeps the firearm styles.
	 */
	MANTLEEDITOR_API bool PicksElement(EMantleWeaponHold Hold, EMantleWeaponDocDelivery Delivery);

	/**
	 * #905: the creator picks an attack style for the source. A melee swing drops the effect choices. A gun that starts firing a beam
	 * starts from a laser rifle's numbers and the laser element; a gun that starts launching rounds starts from a launcher's numbers (its
	 * chosen round model is kept). A gun going back to the instant shot drops an element, which an instant shot cannot use.
	 */
	MANTLEEDITOR_API void SetDelivery(UMantleWeaponSource& Source, EMantleWeaponSourceDelivery Delivery);

	/** Plain words: "Pistol", "Marksman rifle". */
	MANTLEEDITOR_API FText HoldLabel(EMantleWeaponHold Hold);

	/** The name a new weapon gets: "My Rifle" ("My Knuckles" for the fist weapon). */
	MANTLEEDITOR_API FString DefaultName(EMantleWeaponHold Hold);

	/**
	 * Why the builder cannot go on without a 3D model, in a creator's words (#897): every weapon needs one, the fist weapon too
	 * ("Fist weapons need a 3D model: pick the model of the knuckles, claws or gauntlet the hand wears.").
	 */
	MANTLEEDITOR_API FText MissingModelMessage(EMantleWeaponHold Hold);

	/**
	 * The starting numbers for a hold. A small table for now: the shared preset and range format from the vehicle builder
	 * will replace it.
	 */
	MANTLEEDITOR_API FMantleWeaponTuning DefaultTuning(EMantleWeaponHold Hold);

	MANTLEEDITOR_API FRange DamageRange();
	MANTLEEDITOR_API FRange ShotsPerSecondRange();
	MANTLEEDITOR_API FRange MagazineSizeRange();
	MANTLEEDITOR_API FRange SpareAmmoRange();
	MANTLEEDITOR_API FRange ReloadSecondsRange();

	/** Every number pulled into its allowed range. */
	MANTLEEDITOR_API FMantleWeaponTuning ClampTuning(const FMantleWeaponTuning& Tuning);

	/** A name that is safe as an asset and folder name: letters, digits and underscores; spaces become underscores. Never empty. */
	MANTLEEDITOR_API FString SafeAssetName(const FString& Name);

	/**
	 * The display name to save a new weapon under so it never lands on an existing weapon: the name itself when its safe name is
	 * free, otherwise "<Name> 2", "<Name> 3" and so on. TakenSafeNames are the safe names already in use (compared ignoring case).
	 */
	MANTLEEDITOR_API FString FirstFreeDisplayName(const FString& DisplayName, const TArray<FString>& TakenSafeNames);

	/** The package path of the saved source asset: /Game/MantleWeapons/<Name>/<Name>_Source. */
	MANTLEEDITOR_API FString SourcePackagePath(const FString& Name);

	/** The hold a fit suggestion names ("Try the SMG or rifle preset.") that a creator can choose now and is not the current one. */
	MANTLEEDITOR_API TOptional<EMantleWeaponHold> AvailableHoldNamedIn(const FString& Suggestion, EMantleWeaponHold Current);

	/** The fit's sentences with the builder's own jargon in plain words: "mesh" is "model", "preset" is "weapon type". */
	MANTLEEDITOR_API FString PlainWords(const FString& Text);

	/**
	 * Everything about a weapon source that changes the files Create writes, as one string: two sources with the same signature
	 * make the same weapon. The window compares it with the one taken at Create to tell whether the weapon has been edited since.
	 */
	MANTLEEDITOR_API FString EditSignature(const UMantleWeaponSource& Source);

	/** Whether Test locally is offered: the weapon was created, and nothing was edited since (SignatureAtCreate is empty before any Create). */
	MANTLEEDITOR_API bool CanTestLocally(const FString& SignatureAtCreate, const FString& SignatureNow);
}
