// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class UMaterialInterface;
class USkeletalMesh;
struct FMeshDescription;

/**
 * #46: an As-is avatar is uploaded as it is, so the weapons a game character holds and the cloth simulation it carries come
 * with it. A held weapon makes the body too deep for the upload (Wraith's rifle, Murdock's gun) and cloth simulation stops
 * the Atlas step merging sections. Lines in the fix list, each on a copy (the creator's mesh is never modified):
 *
 *   Cloth  the clothing (cloth-simulation) assets are dropped: an avatar never simulates cloth, the cloth keeps its shape and
 *          moves with its bones. A section that only exists to drive the simulation (its slot says cloth and sim,
 *          "Cloth_Sim_Mat") is removed with it.
 *   Props  hand-held props are removed: a prop bone is one whose name, or the name of a bone between it and the arm it hangs
 *          from, has a weapon or prop word (FMantleOutfitSections::IsPropName: weapon, gun, sword, dagger, shield ...), under
 *          an arm or hand bone (and, owner 2026-10-14, a chain or whip held in the hand: Riktor's chain whip); or one with a
 *          mouth-prop word (pipe, cigar, cigarette) under the head or jaw (Feng Mao's pipe).
 *          Owner 2026-10-09: gear worn on the back or hips is outfit and stays (Murdock's holstered shotgun hangs off
 *          spine_03). A section with PropSectionShare of its skin weight on prop bones goes whole; in the sections that stay,
 *          a triangle whose three corners each have PropCornerShare on prop bones goes (Murdock's gun shares M04_Weapons with
 *          his back gear).
 *
 *   Hidden sections whose material draws nothing at its own values are left out (Serath's wings: translucent, their opacity
 *          times a WingAlpha of 0 until her ability raises it). Invisible in the viewer, they still count toward the size
 *          limits. Read from the material's graph (KnownOpacity), never from a name.
 *
 * All are on by default and the creator can untick either (creator-friendliness: detected and named, never opt-in).
 * Hair, tails and plumes are never props (owner 2026-10-14): they hang off the head or spine, not a hand, so Terra's
 * helmet plume stays. Every copy keeps the mesh's morph targets (checked after each copy, LostMorphs).
 */
class MANTLEEDITOR_API FMantleAvatarPropsFixer : public IMantleAvatarFixer
{
public:
	static constexpr float PropSectionShare = 0.9f;
	static constexpr float PropCornerShare = 0.5f;

	inline static const FName KindProps = TEXT("Props");
	inline static const FName KindCloth = TEXT("Cloth");
	inline static const FName KindHidden = TEXT("Hidden");

	/** A translucent section whose opacity is this or less draws nothing. */
	static constexpr float HiddenOpacity = 1.0f / 255.0f;

	/**
	 * Whether a section draws nothing: a masked one whose mask stays under its clip value, a translucent one whose opacity
	 * is HiddenOpacity or less (MaxOpacity: KnownOpacity). Pure (tested).
	 */
	static bool IsHidden(bool bMasked, float MaxOpacity, float ClipValue);

	/**
	 * The opacity (the opacity mask, for a masked material) Material's graph gives everywhere at its own parameter values,
	 * when the graph proves it: constants and scalar parameters folded through the arithmetic that keeps a 0 a 0 (Serath's
	 * wings: opacity times WingAlpha 0, under a depth fade). Unset for an opaque, additive or modulated material, material
	 * attributes, or a value the graph doesn't prove (textures, functions). Read from the graph, not rendered: a bake of a
	 * depth fade reads 0 for any opacity (2026-10-14, Feng Mao's eyelashes).
	 */
	static TOptional<float> KnownOpacity(const UMaterialInterface* Material);

	/** Per bone, whether it is a hand-held prop's (see the class comment). Parents: each bone's parent index, -1 for a root. Pure (tested). */
	static TArray<bool> PropBones(TConstArrayView<FName> Names, TConstArrayView<int32> Parents);

	/** Whether a slot name says the section only drives cloth simulation ("Cloth_Sim_Mat", "ClothSim"). Pure (tested). */
	static bool IsClothSimSlot(const FString& SlotName);

	/** What would go from one LOD: whole sections (by material slot name) and the triangles of the sections that stay. */
	struct FPropCut
	{
		TArray<FName> Sections;
		TArray<int32> SectionTriangles;
		int32 LooseTriangles = 0;
		/** The prop bones that carry weight in the LOD, for the fix line. */
		TArray<FName> Bones;
		/** Where the LOD's points were before the cut: the ones that went with the props, and the ones that stay. */
		TArray<FVector3f> RemovedPoints;
		TArray<FVector3f> KeptPoints;
		bool IsEmpty() const { return Sections.Num() == 0 && LooseTriangles == 0; }
	};

	/**
	 * The props in Description (a LOD of a mesh whose raw reference skeleton is RefNames/RefParents); with bDelete, removes
	 * them from it (empty sections deleted, the description compacted). False when it has no skin weights.
	 *
	 * Reference: LOD 0's cut, for a lower LOD. A lower LOD may drop the prop's bones and skin the prop to their parent
	 * (Feng Mao's pipe is on "head" from LOD 2), so its own weights can't find it: a triangle also goes when each of its
	 * corners lies near a point that went from LOD 0 (within ReferenceReachCm) and clearly nearer it than any point that
	 * stayed (IsWhereRemoved). A corner that could be either stays: the body is never cut for a prop.
	 */
	static bool CutProps(FMeshDescription& Description, TConstArrayView<FName> RefNames, TConstArrayView<int32> RefParents, bool bDelete, FPropCut& OutCut,
		const FPropCut* Reference = nullptr);

	/** How far a lower LOD's point may lie from LOD 0's points and still be matched to them (CutProps' Reference). */
	static constexpr float ReferenceReachCm = 6.0f;

	/**
	 * How much nearer a lower LOD's point must be to LOD 0's removed points than to its kept ones to go with them. A lower
	 * LOD's reduction moves points by a centimetre or so, so a nearest-point match alone ate body under a thin shell over
	 * it (mantle#861, Sevarog's legs under an invisible shell).
	 */
	static constexpr float ReferenceMarginCm = 1.0f;

	/**
	 * Whether a lower LOD's point goes with LOD 0's cut: the nearest removed point within ReachCm (RemovedCm, unset for
	 * none) is at least MarginCm nearer than the nearest kept one (KeptCm, unset for none in reach). Pure (tested).
	 */
	static bool IsWhereRemoved(TOptional<float> RemovedCm, TOptional<float> KeptCm, float ReachCm = ReferenceReachCm, float MarginCm = ReferenceMarginCm);

	/**
	 * The morph targets Before had that After lacks, by name (each step writes a copy: mantle#861's proof of concept lost
	 * Terra's two morphs). Pure (tested).
	 */
	static TArray<FName> LostMorphs(TConstArrayView<FName> Before, TConstArrayView<FName> After);

	virtual FName GetId() const override { return MantleAvatarFixerIds::Props; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleAvatarPropsFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleAvatarPropsFixer();
