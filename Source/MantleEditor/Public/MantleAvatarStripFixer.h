// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"
#include "MantleFusedClothing.h"

class FMantleAvatarContract;
class UObject;
class USkeletalMesh;
class UStaticMesh;

/** One material slot of the body being stripped (LOD 0's source geometry). */
struct MANTLEEDITOR_API FMantleStripSlot
{
	/** The slot's index on the mesh (the material ID its triangles carry). */
	int32 Index = INDEX_NONE;

	FName SlotName;
	FString MaterialName;
	int32 Triangles = 0;

	/** Its triangles' bounds, component space, cm. */
	FBox3d Bounds = FBox3d(ForceInit);

	/** Read as bare skin (kept by default): FMantleAvatarStrip::LooksLikeSkin. */
	bool bSkin = false;

	/** For a skin slot, whether clothing is modelled into it (FMantleFusedClothing); Unknown for any other, and when Plan was given the creator's choices. */
	EMantleFusedVerdict Fused = EMantleFusedVerdict::Unknown;
	FString FusedWhy;
};

/** One connected piece of the head slot, the largest first. */
struct MANTLEEDITOR_API FMantleStripPiece
{
	int32 Triangles = 0;
	FBox3d Bounds = FBox3d(ForceInit);

	/** Sits inside the head skin's bounds (teeth, mouth interior, eye-corner pieces): removed by default. */
	bool bInside = false;
};

/** What stripping a body to bare skin would do, before the creator's choices. */
struct MANTLEEDITOR_API FMantleStripPlan
{
	TArray<FMantleStripSlot> Slots;

	/** The kept skin slot whose lowest point is highest (the head skin); INDEX_NONE without kept skin. */
	int32 HeadSlot = INDEX_NONE;

	/** One kept skin slot holds body and head: every piece is kept (the "inside the head" test is for a head slot only). */
	bool bOneSlot = false;

	/** The head slot's pieces, the largest (the head skin) first; empty in one-slot mode. */
	TArray<FMantleStripPiece> HeadPieces;

	const FMantleStripSlot* FindSlot(int32 Index) const;
};

/**
 * #625, pipeline step 01 ("strip to bare skin", the proven MetaHuman pipeline): Epic's conform fits whatever geometry it
 * is handed (hair cards made a lumpy scalp), so the body goes in as bare skin only. The steps, on Geometry Script as the
 * proven script ran them (01_strip_to_skin.py):
 *   - keep the bare-skin material slots, delete the others' triangles (hair cards, lashes, eyes, cornea, clothing);
 *   - in the head slot (the kept skin slot whose lowest point is highest), split into connected pieces: keep the head skin
 *     (the largest) and every piece outside its bounds, remove the pieces inside them (teeth, mouth interior, eye
 *     corners). With one kept skin slot every piece is kept, with a warning;
 *   - after every removal, drop the unused vertices and compact;
 *   - write it into a copy of the body (weights and materials kept), and check that Epic's conform reader sees exactly
 *     what renders (InputCheck, 0.000 cm).
 * The source is never modified. The plugin never fixes a body up: the creator confirms the kept parts are bare skin.
 */
class MANTLEEDITOR_API FMantleAvatarStrip
{
public:
	/** A conform input whose two readings are further apart than this (cm) is a mismatch. */
	static constexpr double InputMatchCm = 0.5;

	/**
	 * Whether a slot named SlotName with MaterialName (and its base material's name) is bare skin: FMantleOutfitSections
	 * reads it as skin (skin, body, head, face, flesh) and no word names hair, lashes, brows, eyes, the cornea, teeth, a
	 * tongue or a mouth. Pure (tested).
	 */
	static bool LooksLikeSkin(const FString& SlotName, const FString& MaterialName, const FString& BaseMaterialName);

	/** Whether either name has a word for hair, lashes, brows, eyes, the cornea, teeth, a tongue or a mouth (never bare skin). Pure. */
	static bool NamesFaceDetail(const FString& SlotName, const FString& MaterialName);

	/** Whether Inner lies within Outer (each side, with Margin cm of slack). Pure (tested). */
	static bool IsInside(const FBox3d& Inner, const FBox3d& Outer, double Margin = 0.0);

	/**
	 * The plan for Mesh's LOD 0 (its source geometry, read with Geometry Script): its slots read as skin or not, the head
	 * slot among RemovedSlots' complement (null: the slots read as skin) and its pieces. Reads only. False with why when
	 * the mesh has no source geometry.
	 */
	static bool Plan(USkeletalMesh& Mesh, const TSet<int32>* RemovedSlots, FMantleStripPlan& OutPlan, FString& OutError);

	/**
	 * #50: the slots of a model with no skeleton (a static mesh), read and judged as Plan does (skin or not, and with fused clothing),
	 * without the head pieces. False with why when the mesh has no source geometry.
	 */
	static bool PlanStatic(UStaticMesh& Mesh, FMantleStripPlan& OutPlan, FString& OutError);

	/**
	 * The skin-only copy: Mesh's LOD 0 without RemovedSlots and without the head pieces RemovedPieces names (by their
	 * index in the plan, head skin never), written into a new skeletal mesh Folder/Name (a duplicate of Mesh; weights and
	 * materials kept). Null with why. The new package is dirty, not saved; Mesh is never modified.
	 */
	static USkeletalMesh* WriteSkinOnly(USkeletalMesh& Mesh, const TSet<int32>& RemovedSlots, const TSet<int32>& RemovedPieces, const FString& Folder,
		const FString& Name, FString& OutError);

	/**
	 * #55: a copy of Mesh's LOD 0 with only the triangles of the material slots in KeepSlots (a piece of clothing cut from the
	 * character's own mesh; weights and materials kept), written into a new skeletal mesh Folder/Name. Null with why. The new
	 * package is dirty, not saved; Mesh is never modified.
	 */
	static USkeletalMesh* WriteSlots(USkeletalMesh& Mesh, const TSet<int32>& KeepSlots, const FString& Folder, const FString& Name, FString& OutError);

	/**
	 * Epic's conform reader (UMetaHumanCharacterEditorSubsystem::GetMeshDataForConforming) against the mesh as it renders
	 * (LOD 0 render vertices), nearest point both ways: true when both are within InputMatchCm. OutLine: "INPUTCHECK
	 * <mesh>: N verts M tris | conform->render max X cm | render->conform max Y cm: MATCH". OutVertices and
	 * OutTriangleIndices, when given, get what the conform reads.
	 */
	static bool InputCheck(UObject* Mesh, FString& OutLine, TArray<FVector3f>* OutVertices = nullptr, TArray<int32>* OutTriangleIndices = nullptr);

	/**
	 * The largest distance from any point of From to its nearest point of To (a uniform grid, CellCm cells), cm; capped at
	 * SearchCm (returned as SearchCm when a point has nothing within it). 0 for an empty From. Pure (tested).
	 */
	static double LargestNearestDistance(TConstArrayView<FVector3f> From, TConstArrayView<FVector3f> To, double CellCm = 1.0, double SearchCm = 10.0);
};

/**
 * MantleAvatarFixerIds::Strip (the Convert route): for an avatar body that is not already a MetaHuman (no assembly, no
 * Mantle reference topology), one line per material slot ("removes 'Hair'", ticked for a slot that isn't bare skin,
 * unticked for one that is), one per piece inside the head skin (ticked), a warning in one-slot mode, and last the body row
 * (AddBodyRow) with its bare-skin line (bNeedsConfirm): "the kept parts are bare skin", ticked only when a bare body is
 * clearly found (BareBodyFound). While the body is kept, Convert runs nothing until it is ticked. The slot
 * and piece lines record the creator's choice (FMantleConvertContext::StripSlots, StripPieces); the bare-skin line strips
 * (FMantleAvatarStrip::WriteSkinOnly into <output>/Work, then InputCheck) and points the part at the skin-only copy.
 * Clothing pieces are listed and removed; clothing items from them come later.
 */
class MANTLEEDITOR_API FMantleAvatarStripFixer final : public IMantleAvatarFixer
{
public:
	/** The bare-skin line's kind ("Strip.<part>.BareSkin"). */
	static const FName KindBareSkin;

	/** The one-slot warning's kind. */
	static const FName KindOneSlot;
	/**
	 * #770, "keep the face, choose a body" ("Strip.<part>.FaceOnly"): ticked when the body reads as covered
	 * (FMantleMetaHumanHeadOnly::IsCovered), then the bare-skin line needs no confirming; the creator may tick it on any body.
	 * Applied, it marks the part's work face-only (FMantleConvertMetaHumanWork::bFaceOnly) and the bare-skin line strips nothing.
	 * #875: an assembled MetaHuman whose body MetaHuman Creator can't fit (FMantleMetaHumanBuild::AssemblyBodyNeedsFaceOnly) is offered the
	 * same three lines (ticked face, both bodies unticked) with no strip lines; MHSource then keeps its face on MetaHuman's own body.
	 */
	static const FName KindFaceOnly;
	/**
	 * #770: the body the face goes on, Women's or Men's, which the creator picks ("Strip.<part>.FeminineBody" / "Strip.<part>.MasculineBody"):
	 * both start unticked, and with the face line ticked exactly one must be (MissingBodyChoice). Nothing is guessed.
	 */
	static const FName KindFeminineBody;
	static const FName KindMasculineBody;

	/** Why Convert can't run yet for PartIndex: its face line is ticked and not exactly one body line is; empty when it can. */
	static FString MissingBodyChoice(TConstArrayView<FMantleConvertFix> Fixes, int32 PartIndex);

	/** What the body row says for one part: the pre-picked choice with its reason, and whether a bare body was clearly found. */
	struct FBodyRow
	{
		/** Pre-picked: put the face on a MetaHuman body (true) or keep this character's body (false). */
		bool bFaceOnBody = false;

		/** Why it is pre-picked, shown next to it ("the body is covered by clothing (...)"). */
		FString Why;

		/** A bare body was clearly found: the bare-skin line starts ticked. Otherwise the creator ticks it. */
		bool bBareFound = false;

		/** False when this body can't be kept (MetaHuman Creator can't fit it): keeping it stops the conversion. */
		bool bCanKeep = true;

		/** The bare-skin line's text: what keeping the body needs to be bare skin. */
		FString BareProblem;
	};

	/**
	 * Chunk 5: the one place the body row is written, for all three cases (an assembled MetaHuman, a model with no rig, a
	 * stripped skeletal body): the face line ("put the face on a MetaHuman body", ticked when Row.bFaceOnBody), the two body
	 * lines (Men's, Women's: both unticked, the creator picks), and the bare-skin line (bNeedsConfirm; ticked only when
	 * Row.bBareFound). The keys stay Strip.<part>.FaceOnly / MasculineBody / FeminineBody / BareSkin.
	 */
	static void AddBodyRow(TArray<FMantleConvertFix>& OutFixes, int32 PartIndex, const FBodyRow& Row);

	/**
	 * The one bare-skin rule: a bare body is clearly found when the plan doesn't read as covered (FMantleMetaHumanHeadOnly::
	 * IsCovered) and every kept slot reads as skin (bAllKept: nothing is stripped, so every slot is kept). False with why.
	 */
	static bool BareBodyFound(const FMantleStripPlan& Plan, bool bAllKept, FString& OutWhy);

	/** A line that must be confirmed and isn't (bNeedsConfirm, unticked); a part's bare-skin line counts only while its body is kept. */
	static bool IsUnconfirmed(TConstArrayView<FMantleConvertFix> Fixes, const FMantleConvertFix& Fix);

	/** Whether Fix is one of the body row's lines (face, Men's, Women's, bare skin). */
	static bool IsBodyRowLine(const FMantleConvertFix& Fix);

	/** "Slot<index>": a slot line. */
	static FName SlotKind(int32 SlotIndex);

	/** "Piece<slot>_<piece>": a head piece line. */
	static FName PieceKind(int32 HeadSlot, int32 Piece);

	/** Whether Context.Parts[PartIndex] is a body Strip is for (the class comment's rule). */
	static bool IsBodyToStrip(const FMantleConvertContext& Context, int32 PartIndex, const FMantleAvatarContract& Contract);

	virtual FName GetId() const override { return MantleAvatarFixerIds::Strip; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleAvatarStripFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleAvatarStripFixer();
