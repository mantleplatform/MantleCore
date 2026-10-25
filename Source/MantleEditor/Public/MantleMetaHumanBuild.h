// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"
#include "MantleAvatarHeight.h"
#include "MantleMetaHumanConform.h"

class UObject;
class USkeletalMesh;

/** Whether this editor is signed in to Epic for MetaHuman's cloud (texture synthesis sources and the face auto-rig). */
enum class EMantleEpicSignIn : uint8
{
	SignedIn,
	SignedOut,
	/** No answer within FMantleMetaHumanBuild::SignInSeconds (the auth service is unreachable or not set up). */
	Unknown,
	/** Not asked yet: the fix list never asks Epic; the build asks when the creator presses Convert. */
	NotChecked,
};

/** What the build needs before it runs, checked before anything is sent to Epic. */
struct MANTLEEDITOR_API FMantleMetaHumanBuildPreflight
{
	/** MetaHuman Optional Content is installed (the texture synthesis model directory exists on disk). */
	bool bOptionalContent = false;

	EMantleEpicSignIn SignIn = EMantleEpicSignIn::NotChecked;

	/** The first thing in the way, as the fix list says it; empty when nothing is (NotChecked is not in the way). Pure (tested). */
	FString Problem() const;
};

/** What building the character made (pipeline step 05). */
struct MANTLEEDITOR_API FMantleMetaHumanBuildResult
{
	/** Where Epic's build put the built MetaHuman: <CharacterFolder>/Unpacked/<character>, found in the asset registry. */
	FString BuildRoot;

	/** The built body and face meshes (<character>_BodyMesh, _FaceMesh). */
	TWeakObjectPtr<USkeletalMesh> Body;
	TWeakObjectPtr<USkeletalMesh> Face;

	/** The underwear mask override read back after the skin commit and again before the build. */
	bool bMaskStored = false;

	/** Textures resized to MaxTextureSize ("<path> 8192 to 2048"), and the largest texture under the build root afterwards. */
	TArray<FString> Resized;
	int32 LargestTexture = 0;

	/** The packages this step saved (only under the character folder). */
	TArray<FString> Saved;

	/** The built body with its face, floor to head top (cm), as the script measures it; -1 unmeasured. */
	double BuiltHeightCm = -1.0;

	/**
	 * The lowest Z over the same vertices (cm; FMantleAvatarHeight::LowestZ), which the height can't see: a mesh sitting below its
	 * own skeleton has the right height and no feet on the floor. Unset when unmeasured.
	 */
	TOptional<double> LowestPointCm;

	/**
	 * Whether that is within FMantleAvatarHeight::ToleranceCm of the default height, and the line for it
	 * (FMantleAvatarHeight::BuiltHeightLine): outside it is a warning, never a stop.
	 */
	bool bHeightWithin = false;
	FString HeightLine;

	/** The guide's hand numbers on the built body (FMantleMetaHumanConform::MeasureHands). */
	FMantleHandMeasure Hands;

	/** The assets this run's build made under the build root (not ones already there), to record, or to discard on failure. */
	TArray<TWeakObjectPtr<UObject>> Created;
};

/**
 * #625, pipeline step 05 (build), the proven script's calls in C++, on the conformed character:
 *   0. the save scope checked (IsSaveScope): CharacterFolder is a /Game folder at least two levels deep holding the character;
 *   1. the preflight (Preflight, asking Epic): MetaHuman Optional Content, and Epic sign-in through
 *      CheckHasLoggedInUserAsync (never LoginToAuthEnvironment: no login window ever opens; not signed in is a clean stop);
 *   2. opened for editing (unless it already was); the skin: Epic's centre skin tone (U = V = 0.5), the
 *      top underwear off, and the bottom masked out by Epic's own all-black T_Flat_Black_M (referenced, never copied) as the
 *      Body_Underwear_Mask texture override on TextureMaterialOverrides (never the deprecated SkinSettings.TextureOverrides,
 *      not read in 5.8); CommitSkinSettings, then the override read back: not stored is a stop;
 *   3. Epic's cloud: RequestTextureSources, then RequestAutoRigging JointsAndBlendShapes (both blocking, as the script);
 *   4. the character saved, every compilation finished, CanBuildMetaHuman (Epic's message on refusal), then BuildMetaHuman
 *      Optimized / Medium on the default path (never a custom build or Common root);
 *   5. every dirty package under the character folder saved, nothing else (never save-all); a package the build left dirty
 *      anywhere else is a stop; then the build root found (<CharacterFolder>/Unpacked/<character>) and every Texture2D under
 *      it larger than MaxTextureSize resized with Epic's Resize Source and saved (one still larger is a stop);
 *   6. the built body measured: its height with its face against the default height (FMantleAvatarHeight::TargetCm; outside
 *      +- FMantleAvatarHeight::ToleranceCm a warning line, never a stop: the converter never refuses for size) and the
 *      hands, for the record; the character closed for editing if this step opened it.
 */
class MANTLEEDITOR_API FMantleMetaHumanBuild
{
public:
	/** Epic's own all-black mask: the skin's Body_Underwear_Mask override, so the build bakes no underwear. Only read. */
	static const TCHAR* const UnderwearMaskPath;

	/** Epic's centre skin tone (the tone picker's U and V): players recolour in game. */
	static constexpr float SkinToneU = 0.5f;
	static constexpr float SkinToneV = 0.5f;

	/** The platform's texture limit: every built texture larger than this is resized to it. */
	static constexpr int32 MaxTextureSize = 2048;

	/** How long the build's sign-in check waits for Epic's answer (seconds). */
	static constexpr double SignInSeconds = 10.0;

	/** The note the fix list (and the panel) shows about Epic's cloud (a note, no tick box). */
	static const TCHAR* const ConsentLine;

	/**
	 * The preflight. bAskEpic: ask Epic for the sign-in (waits up to SignInSeconds; the build only). Otherwise the last
	 * answer (kept 30 s) or NotChecked, and Epic is never reached (the fix list).
	 */
	static FMantleMetaHumanBuildPreflight Preflight(bool bAskEpic);

	/**
	 * Whether CharacterFolder may be the build's save scope for the character package CharacterPackage: a /Game folder at
	 * least two levels deep (/Game/A/B) that holds the package. Pure (tested).
	 */
	static bool IsSaveScope(const FString& CharacterFolder, const FString& CharacterPackage);

	/**
	 * Step 05 for Character (a UMetaHumanCharacter) in CharacterFolder. False with why; OutResult.Created lists what was made.
	 * The skin is Epic's centre skin tone on every route (the upload is geometry only; players recolour it in game).
	 * bDeferHeightWarning records the measured height at Log severity, for a caller
	 * that retries and reports its final result; direct builds warn by default. A built body whose lowest point is further from the
	 * floor than the contract's feet tolerance (the upload's check) fails the step here (never retried as a height problem).
	 */
	static bool Build(UObject& Character, const FString& CharacterFolder, FMantleMetaHumanBuildResult& OutResult, FString& OutError,
		bool bDeferHeightWarning = false, double HeightToleranceCm = FMantleAvatarHeight::ToleranceCm);

	/**
	 * #625, the MetaHuman-source route: Character (a UMetaHumanCharacter, opened for editing here if it isn't) set to
	 * TargetCm tall through its "Height" body constraint (active, at the target), the body state committed. A fixed body type
	 * (imported whole from DNA, or a fixed compatibility body: UMetaHumanCharacter::bFixedBodyType) takes no constraint:
	 * bOutFixed, nothing changed, true. False
	 * with why when it can't be set (no Height constraint, MetaHuman Creator not running).
	 */
	static bool SetHeight(UObject& Character, double TargetCm, bool& bOutFixed, FString& OutError);

	/**
	 * The height of Creator's evaluated LOD0 body for Character's current body state (Y size of its first LOD0 vertices, cm:
	 * Creator's state is in DNA space, Y up),
	 * measured locally: no cloud, no build. Opens the character for editing here if it isn't. False with why (no state, no
	 * LOD0 vertices, invalid geometry).
	 */
	static bool MeasureEvaluatedHeight(UObject& Character, double& OutHeightCm, FString& OutError);

	/**
	 * SetHeight for the MetaHuman-source route, corrected before the first build. Creator's Height body constraint is not
	 * exact, so after setting TargetCm this measures Creator's evaluated LOD0 body (MeasureEvaluatedHeight: local, no cloud,
	 * no build) and re-sets the constraint proportionally (FMantleAvatarHeight::RetargetCm), at most
	 * FMantleAvatarHeight::MaxPredictionSteps times, so the build starts near the target. OutRequestedCm is the constraint
	 * finally set (TargetCm for a fixed body type); OutPredictedCm the last plausible measurement (plus
	 * FMantleAvatarHeight::PredictionOffsetCm), -1 when there is none. A measurement that fails or is implausible
	 * (FMantleAvatarHeight::IsPlausiblePrediction) is a warning, not an error: the build proceeds on the request so far. The
	 * build's own measurement and retry (Build, BuildAndHandOff) remain the authority. False with why only when the height
	 * can't be set.
	 */
	static bool SetPredictedHeight(UObject& Character, double TargetCm, double& OutRequestedCm, double& OutPredictedCm, bool& bOutFixed, FString& OutError);

	/** MHSource only: remove semantic outfit selections from a distinct source copy before Epic builds it.
	 *  Character, hair and unrelated skeletal selections are preserved; the original is never edited. */
	static bool PrepareUnclothedSourceCopy(UObject& Copy, const UObject& Source, int32& OutRemovedSelections, FString& OutError);

	/** Verify Epic has not reintroduced selected clothing on an MHSource copy. Does not mutate assets. */
	static bool CheckUnclothedSource(const UObject& Character, FString& OutError);
	/** Complete native Optimized Medium body at every LOD; no count/CRC-only acceptance. */
	static bool CheckCompleteBody(const USkeletalMesh* Body, FString& OutError);

	/**
	 * Whether a build whose CheckCompleteBody answered bBodyComplete fails because it can't be made Mantle's body: a MetaHuman
	 * Creator character with a custom body (UMetaHumanCharacter::bFixedBodyType) that isn't Mantle's reference topology, or that
	 * Epic built off the canonical height (bHeightWithin false: a fixed body can't be retargeted, and Mantle refuses a converted
	 * avatar that isn't 165.6 cm). Pure (tested).
	 */
	static bool FixedBodyCantBuild(bool bFixedBody, bool bBodyComplete, bool bHeightWithin);

	/**
	 * #875: makes a MetaHuman Creator character at PackageName from an assembled MetaHuman's face and body meshes, as Epic's
	 * fits do: the face state fitted to the face mesh (by UVs, then by vertex order), the parametric body conformed to the body
	 * mesh and its joints, saved. OutCharacter is set as soon as it exists (the caller discards it on a failure). A refused fit
	 * fails with AssemblyFitRefusal.
	 *
	 * A face mesh the template fit refuses (a reduced mesh) is fitted from its face DNA instead, when it has one: the DNA the
	 * mesh carries as asset user data, else the one DNA asset in the mesh's own folder (FaceDnaAssetIndex). It is written to a
	 * temp .dna file for Epic's ImportFromFaceDna (the parametric fit, not a whole-rig import) and the file is deleted. The body
	 * is then fitted without the face mesh as its head template (it would be refused too).
	 *
	 * bKeepFaceOnly (#770's "keep the face, choose a body", Strip's FaceOnly line): the body is not fitted at all; the character
	 * keeps MetaHuman's own parametric body, on the Masculine/Feminine constraint bFeminineBody names
	 * (FMantleMetaHumanConform::ChooseParametricBody). A refused body fit is recorded (NoteBodyFitRefused) so the next Analyze offers it.
	 */
	static bool MakeCharacterFromAssembly(USkeletalMesh& Body, USkeletalMesh& Face, const FString& PackageName, UObject*& OutCharacter, FString& OutError,
		bool bKeepFaceOnly = false, bool bFeminineBody = false);

	/**
	 * #875: whether the part is an assembled MetaHuman whose body MetaHuman Creator's fit will refuse (the clothing is merged
	 * into the body mesh), so the creator is offered #770's face-only choice (Strip's FaceOnly / FeminineBody / MasculineBody
	 * lines) before Apply: the part is a character to build from an assembly (FMantleMetaHumanSourceFixer::IsCharacterToBuild)
	 * and the body mesh BodyLooksMergedWithClothing, or an earlier fit of this editor session refused it (NoteBodyFitRefused).
	 * OutWhy says what was seen.
	 */
	static bool AssemblyBodyNeedsFaceOnly(const FMantleConvertContext& Context, int32 PartIndex, FString& OutWhy);

	/**
	 * Whether a body mesh's LOD0 (BodyVertices render vertices) is far past a MetaHuman body: more than 1.5 times its own DNA's
	 * body mesh (DnaVertices, 0 when it carries none), or more than 4 times Mantle's reference LOD0 (ReferenceVertices, 0 when
	 * unknown; Optimized Medium, the smallest LOD0 a MetaHuman body is exported at). Pure (tested).
	 */
	static bool BodyLooksMergedWithClothing(int32 BodyVertices, int32 DnaVertices, int32 ReferenceVertices);

	/** Remembers, for this editor session, that Epic's fit refused this body mesh (its package path). */
	static void NoteBodyFitRefused(const FString& BodyPath);
	static bool BodyFitWasRefused(const FString& BodyPath);

	/**
	 * Which of the DNA assets in a face mesh's own folder clearly belongs to it: the only one (INDEX_NONE for none, or for
	 * several: nothing is guessed). Pure (tested).
	 */
	static int32 FaceDnaAssetIndex(TConstArrayView<FString> DnaAssetNames);

	/** #875: what a refused body fit tells the creator to do next (the face-only choice). Pure (tested). */
	static FString AssemblyFaceOnlyHint();

	/**
	 * #875: whether an assembly really is a MetaHuman: its body skinned to Epic's MetaHuman body skeleton (metahuman_base_skel, as
	 * FMantleCosmeticExport::CouldBeAuthoringBody reads it) and its face to Epic's face skeleton (Face_Archetype_Skeleton). Skeleton
	 * asset paths or names. Pure (tested).
	 */
	static bool IsMetaHumanAssembly(const FString& BodySkeleton, const FString& FaceSkeleton);

	/** #875: the message for a mesh MetaHuman Creator's fit refused (What: "face", "body" or "body joints"; Code: Epic's error code). Pure (tested). */
	static FString AssemblyFitRefusal(const FString& MeshName, const FString& What, const FString& Code);

	/** #875: the message for an assembled MetaHuman no build step could run on (Why: what is missing). Pure (tested). */
	static FString AssemblyNotBuiltMessage(const FString& Name, const FString& Why);

	/**
	 * The hand-off to the Assembly fixer (no remap yet): BuildRoot read as a MetaHuman Creator character
	 * (FMantleAvatarAssemblyFixer::Scan on Epic's built names, its Converted folder allowed), which must pick BuiltBody and
	 * BuiltFace; then Context.Parts[PartIndex] becomes that body with that face on the metahuman skeleton (its Original stays
	 * the source), Context.AssemblyFolder the build root, and the work records the build (BuildRoot, BuiltBody, BuiltFace:
	 * FMantleConvertContext::IsBuiltMetaHuman), so the Assembly, Refit and Bake fixers see the built MetaHuman (Triangles and
	 * Atlas leave it alone). The body and face are stamped FMantleAvatarConverter::BuiltKey (nothing else writes it). The
	 * build's other meshes, if any, are named in OutNote, not uploaded. False with why, the context unchanged.
	 */
	static bool HandOff(FMantleConvertContext& Context, int32 PartIndex, const FString& BuildRoot, const USkeletalMesh* BuiltBody,
		const USkeletalMesh* BuiltFace, FString& OutNote, FString& OutError);
};

/**
 * MantleAvatarFixerIds::MHBuild (the Convert route): one line for a body Convert makes a MetaHuman of, with the preflight as
 * far as it is known without asking Epic, and the cloud note; Apply needs the conform's character, builds it
 * (FMantleMetaHumanBuild::Build, which asks Epic; #751: a build outside FMantleAvatarHeight::ToleranceCm of the default
 * height is deleted, the posed body rescaled by target / built and conformed again into a new character, which is built
 * instead, once), records the build root and the built meshes, and hands the part on to
 * the Assembly fixer (FMantleMetaHumanBuild::HandOff). A failed Apply deletes the built assets this run made and leaves
 * the context as it was; the character keeps what was committed and saved to it
 * (its skin, textures and rig: it is the conform's asset). So does one that dirtied a protected package outside
 * <output>/<Name>.
 */
class MANTLEEDITOR_API FMantleMetaHumanBuildFixer final : public IMantleAvatarFixer
{
public:
	virtual FName GetId() const override { return MantleAvatarFixerIds::MHBuild; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/**
 * MantleAvatarFixerIds::MHSource (the Convert route, #625): a MetaHuman Creator character picked as the source (an Epic preset
 * under /MetaHumanCharacter/Optional/Presets, or the creator's own) keeps its Epic shape and skips Strip, MHPose and MHConform.
 * One line, with the preflight as far as it is known without asking Epic, and the cloud note. Apply copies the character to
 * <output>/<Name>/<Name> (the source is never changed: a dirtied protected package stops it), sets the copy to the default
 * height (FMantleMetaHumanBuild::SetHeight; a fixed body type builds at its own height, with a warning line), builds it as
 * MHBuild does (Epic's centre skin tone), and hands the part on (FMantleMetaHumanBuild::HandOff). Creator's Height
 * constraint is not exact, so the built height is measured and, when it is further than FMantleAvatarHeight::AimCm from the
 * default, the character is rebuilt with a corrected constraint (at most FMantleAvatarHeight::MaxHeightBuilds builds; not for
 * a fixed body type). A failed Apply deletes the copy and what the build made, the context as it was.
 */
class MANTLEEDITOR_API FMantleMetaHumanSourceFixer final : public IMantleAvatarFixer
{
public:
	/**
	 * Whether Context.Parts[PartIndex] is a MetaHuman Creator character to build: the Convert route, the context's source a
	 * UMetaHumanCharacter or an assembled MetaHuman's body and face (#875, made into a character first), an included avatar
	 * part with no mesh yet (not built).
	 */
	static bool IsCharacterToBuild(const FMantleConvertContext& Context, int32 PartIndex);

	virtual FName GetId() const override { return MantleAvatarFixerIds::MHSource; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleMetaHumanBuildFixer and FMantleMetaHumanSourceFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleMetaHumanBuildFixer();
