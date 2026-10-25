// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleBodyTopologyData.h"
#include "MantleCosmeticExport.h"
#include "MantleRuntime.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "IMeshReductionInterfaces.h"
#include "IMeshReductionManagerModule.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "LODUtilities.h"
#include "MeshDescription.h"
#include "Modules/ModuleManager.h"
#include "MantleAssetCleanup.h"
#include "SkeletalMeshReductionSettings.h"
#include "SkeletalMeshTypes.h"
#include "SkinnedAssetCompiler.h"

const FName FMantleAvatarTrianglesFixer::KindMesh(TEXT("Mesh"));
const FName FMantleAvatarTrianglesFixer::KindFace(TEXT("Face"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarTrianglesPrivate
{
	/** What the upload would do with one mesh of a part, and what the fixer would do about it. */
	struct FTrianglePlan
	{
		int32 Limit = 0;
		/** The LOD the upload takes (FMantleCosmeticExport::Build's rule for the part's class). */
		int32 UploadLOD = 0;
		int32 UploadTriangles = 0;
		int32 LODZeroTriangles = 0;
		int32 NumLODs = 0;
		/** A hair piece's lifted section's material slot: only its triangles count. INDEX_NONE counts the whole LOD. */
		int32 MaterialSlot = INDEX_NONE;
		/** A lower LOD within the limit with geometry of its own, to make LOD 0; INDEX_NONE for none. */
		int32 PromoteLOD = INDEX_NONE;
		int32 PromoteTriangles = 0;
		int32 Target = 0;
		bool bOver = false;
		bool bHair = false;
	};

	void FinishTrianglesCompile(USkeletalMesh* Mesh)
	{
		TArray<USkinnedAsset*> Compiling = { Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	}

	/**
	 * Drops every LOD above 0 the way the editor's own "remove LOD" does (FLODUtilities::RemoveLOD keeps the LOD infos,
	 * source models and render data in step), highest first. SetNumSourceModels alone leaves LOD infos behind, which
	 * the rebuild then asserts on.
	 */
	void KeepOnlyLODZero(USkeletalMesh* Mesh)
	{
		FinishTrianglesCompile(Mesh);
		for (int32 LOD = Mesh->GetLODNum() - 1; LOD > 0; --LOD)
		{
			FSkeletalMeshUpdateContext Update;
			Update.SkeletalMesh = Mesh;
			FLODUtilities::RemoveLOD(Update, LOD);
			FinishTrianglesCompile(Mesh);
		}
	}

	bool HasSkeletalReduction()
	{
		IMeshReduction* Reduction = FModuleManager::Get().LoadModuleChecked<IMeshReductionManagerModule>(TEXT("MeshReductionInterface")).GetSkeletalMeshReductionInterface();
		return Reduction && Reduction->IsSupported();
	}

	int32 TrianglesOfSlot(const USkeletalMesh* Mesh, int32 LOD, int32 MaterialSlot)
	{
		int32 Triangles = 0;
		for (const FMantleSectionSpan& Span : FMantleCosmeticExport::ReadSectionSpans(Mesh, LOD))
		{
			if (Span.Slot == MaterialSlot)
			{
				Triangles += Span.NumTriangles;
			}
		}
		return Triangles;
	}

	int32 CountFor(const USkeletalMesh* Mesh, int32 LOD, int32 MaterialSlot)
	{
		return MaterialSlot == INDEX_NONE ? FMantleCosmeticExport::CountTriangles(Mesh, LOD) : TrianglesOfSlot(Mesh, LOD, MaterialSlot);
	}

	FString TrianglesNumber(int32 Value)
	{
		return FText::AsNumber(Value).ToString();
	}

	/**
	 * The plan for Mesh as Class uploads it (bFace: an avatar's face, on the face limit). Garments and faces take their
	 * first LOD within the limit, avatar bodies their reference-topology LOD else the first within, hair LOD 0 (and a
	 * lifted section only). False for no mesh.
	 */
	bool PlanTriangles(const USkeletalMesh* Mesh, const EMantleCosmeticClass Class, const FString& Slot, const bool bFace, const int32 Section,
		const FMantleAvatarContract& Contract, FTrianglePlan& Out)
	{
		Out = FTrianglePlan();
		if (!Mesh)
		{
			return false;
		}
		Out.bHair = Class == EMantleCosmeticClass::Hair;
		Out.Limit = bFace ? Contract.AvatarFace.MaxTriangles
			: Class == EMantleCosmeticClass::Avatar ? Contract.AvatarBody.MaxTriangles : Contract.SkinnedMaxTriangles(Slot);
		if (Out.Limit <= 0)
		{
			return false;
		}
		Out.NumLODs = UMantleBodyTopologyData::GetNumLODs(Mesh);
		if (Out.bHair && Section != INDEX_NONE)
		{
			const TArray<FMantleSectionSpan> Spans = FMantleCosmeticExport::ReadSectionSpans(Mesh, 0);
			Out.MaterialSlot = Spans.IsValidIndex(Section) ? Spans[Section].Slot : INDEX_NONE;
		}
		Out.LODZeroTriangles = CountFor(Mesh, 0, Out.MaterialSlot);
		if (Out.bHair)
		{
			Out.UploadLOD = 0;
		}
		else if (Class == EMantleCosmeticClass::Avatar && !bFace)
		{
			Out.UploadLOD = FMantleCosmeticExport::AvatarBodyLOD(Mesh, Contract);
		}
		else
		{
			Out.UploadLOD = 0;
			for (int32 LOD = 0; LOD < Out.NumLODs; ++LOD)
			{
				if (CountFor(Mesh, LOD, Out.MaterialSlot) <= Out.Limit)
				{
					Out.UploadLOD = LOD;
					break;
				}
			}
		}
		Out.UploadTriangles = CountFor(Mesh, Out.UploadLOD, Out.MaterialSlot);
		Out.bOver = Out.UploadTriangles > Out.Limit;
		Out.Target = FMantleAvatarTrianglesFixer::TargetTriangles(Out.Limit, FMantleAvatarTrianglesFixer::TargetShare);
		if (!Out.bOver)
		{
			return true;
		}
		for (int32 LOD = 1; LOD < Out.NumLODs; ++LOD)
		{
			const int32 Triangles = CountFor(Mesh, LOD, Out.MaterialSlot);
			if (Triangles > 0 && Triangles <= Out.Limit && Mesh->HasMeshDescription(LOD))
			{
				Out.PromoteLOD = LOD;
				Out.PromoteTriangles = Triangles;
				break;
			}
		}
		return true;
	}

	/** The section of Mesh's LOD 0 drawn with MaterialSlot (after a LOD moved), INDEX_NONE when none is. */
	int32 SectionOfSlot(const USkeletalMesh* Mesh, int32 MaterialSlot)
	{
		const TArray<FMantleSectionSpan> Spans = FMantleCosmeticExport::ReadSectionSpans(Mesh, 0);
		for (int32 Section = 0; Section < Spans.Num(); ++Section)
		{
			if (Spans[Section].Slot == MaterialSlot)
			{
				return Section;
			}
		}
		return INDEX_NONE;
	}
}

int32 FMantleAvatarTrianglesFixer::TargetTriangles(const int32 Limit, const float Share)
{
	return FMath::Max(1, FMath::RoundToInt(static_cast<double>(Limit) * FMath::Clamp(static_cast<double>(Share), 0.01, 1.0)));
}

void FMantleAvatarTrianglesFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	using namespace MantleAvatarTrianglesPrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract)
	{
		return;
	}
	const bool bReduction = HasSkeletalReduction();
	for (int32 Index = 0; Index < Context.Parts.Num(); ++Index)
	{
		const FMantleConvertPart& Part = Context.Parts[Index];
		EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
		if (!FMantleAvatarConverter::ClassOfSlot(Part.Slot, *Contract, Class))
		{
			continue;
		}
		for (const bool bFace : { false, true })
		{
			const USkeletalMesh* Mesh = bFace ? Part.Face.Get() : Part.Mesh.Get();
			if (!Mesh || (bFace && Class != EMantleCosmeticClass::Avatar))
			{
				continue;
			}
			FTrianglePlan Plan;
			if (!PlanTriangles(Mesh, Class, Part.Slot, bFace, Part.Section, *Contract, Plan) || !Plan.bOver)
			{
				continue;
			}
			FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
			Fix.FixerId = MantleAvatarFixerIds::Triangles;
			Fix.PartIndex = Index;
			Fix.Kind = bFace ? KindFace : KindMesh;
			const FString What = Plan.MaterialSlot != INDEX_NONE
				? FString::Printf(TEXT("%s (its section %d)"), *Mesh->GetName(), Part.Section) : Mesh->GetName();
			Fix.Problem = Plan.NumLODs > 1 && !Plan.bHair
				? FString::Printf(TEXT("%s: %s triangles, the limit is %s, and none of its %d LODs is within it."), *What,
					*TrianglesNumber(Plan.UploadTriangles), *TrianglesNumber(Plan.Limit), Plan.NumLODs)
				: FString::Printf(TEXT("%s: %s triangles, the limit is %s."), *What, *TrianglesNumber(Plan.UploadTriangles), *TrianglesNumber(Plan.Limit));
			if (Context.IsBuiltMetaHuman(Index))
			{
				// #625: Epic's build is tied to its DNA (face rig, blend shapes): reducing or re-LODing it would break them.
				Fix.bCanFix = false;
				Fix.bEnabled = false;
				Fix.Action = TEXT("Can't: a MetaHuman Convert built is never reduced or re-LODed here (its face rig and blend shapes would break).");
			}
			else if (Plan.PromoteLOD != INDEX_NONE)
			{
				Fix.Action = FString::Printf(TEXT("Makes its own LOD %d (%s triangles) LOD 0 of a converted copy: cheaper and cleaner than a new reduction."),
					Plan.PromoteLOD, *TrianglesNumber(Plan.PromoteTriangles));
			}
			else if (Plan.bHair)
			{
				Fix.bCanFix = false;
				Fix.Action = FString::Printf(TEXT("Can't: hair cards fall apart when they are reduced. Use a version of the hair with fewer cards, or give it a LOD under %s triangles."),
					*TrianglesNumber(Plan.Limit));
			}
			else if (!bReduction)
			{
				Fix.bCanFix = false;
				Fix.Action = TEXT("Can't: this editor has no skeletal mesh reduction (turn on the Skeletal Mesh Simplifier plugin), so reduce it in your modelling tool.");
			}
			else
			{
				Fix.Action = FString::Printf(TEXT("Reduces LOD 0 of a converted copy to about %s triangles (%d%% of the limit), keeping its skin weights."),
					*TrianglesNumber(Plan.Target), FMath::RoundToInt(TargetShare * 100.0f));
			}
		}
	}
}

bool FMantleAvatarTrianglesFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	using namespace MantleAvatarTrianglesPrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the part it was for is gone");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
	FMantleAvatarConverter::ClassOfSlot(Part.Slot, *Contract, Class);
	const bool bFace = Fix.Kind == KindFace;
	USkeletalMesh* Source = bFace ? Part.Face.Get() : Part.Mesh.Get();
	FTrianglePlan Plan;
	if (!PlanTriangles(Source, Class, Part.Slot, bFace, Part.Section, *Contract, Plan))
	{
		OutError = TEXT("the part has no mesh to reduce");
		return false;
	}
	if (!Plan.bOver)
	{
		Context.Notes.Add(FString::Printf(TEXT("%s is within the triangle limit already."), *Source->GetName()));
		return true;
	}
	// #625: checked first, as Analyze does: a built MetaHuman is refused for what it is, whatever else applies.
	if (Context.IsBuiltMetaHuman(Fix.PartIndex))
	{
		OutError = TEXT("a MetaHuman Convert built is never reduced or re-LODed (its face rig and blend shapes would break)");
		return false;
	}
	if (Plan.PromoteLOD == INDEX_NONE && Plan.bHair)
	{
		OutError = TEXT("hair cards are never reduced");
		return false;
	}

	// A copy the context may change: the creator's mesh is never touched.
	bool bNewCopy = true;
	for (const TWeakObjectPtr<UObject>& Created : Context.CreatedAssets)
	{
		bNewCopy &= Created.Get() != Source;
	}
	USkeletalMesh* Work = FMantleAvatarConverter::MakeWritable(Context, Source, OutError);
	if (!Work)
	{
		return false;
	}
	auto Discard = [&Context, Work, bNewCopy]()
	{
		// A copy made for a fix that failed is not left behind in Converted/.
		if (bNewCopy)
		{
			Context.CreatedAssets.RemoveAll([Work](const TWeakObjectPtr<UObject>& Created) { return Created.Get() == Work; });
			const TArray<UObject*> Doomed = { Work };
			FMantleAssetCleanup::MoveAside(Doomed);
		}
	};
	const bool bDone = Plan.PromoteLOD != INDEX_NONE ? PromoteLOD(Work, Plan.PromoteLOD, OutError) : ReduceLODZero(Work, Plan.Target, OutError);
	if (!bDone)
	{
		Discard();
		return false;
	}
	const int32 After = CountFor(Work, 0, Plan.MaterialSlot);
	if (After <= 0 || After > Plan.Limit)
	{
		OutError = FString::Printf(TEXT("LOD 0 of the copy has %s triangles, and the limit is %s"), *TrianglesNumber(After), *TrianglesNumber(Plan.Limit));
		Discard();
		return false;
	}
	if (bFace)
	{
		Part.Face = Work;
	}
	else
	{
		Part.Mesh = Work;
		if (Plan.MaterialSlot != INDEX_NONE)
		{
			// The lifted section moved with its LOD: find it again by its material.
			Part.Section = SectionOfSlot(Work, Plan.MaterialSlot);
		}
	}
	Context.Notes.Add(Plan.PromoteLOD != INDEX_NONE
		? FString::Printf(TEXT("%s: its LOD %d (%s triangles) is LOD 0 of %s."), *Source->GetName(), Plan.PromoteLOD, *TrianglesNumber(After), *Work->GetPathName())
		: FString::Printf(TEXT("%s: LOD 0 reduced from %s to %s triangles (the limit is %s) in %s."), *Source->GetName(),
			*TrianglesNumber(Plan.LODZeroTriangles), *TrianglesNumber(After), *TrianglesNumber(Plan.Limit), *Work->GetPathName()));
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT triangles %s: %d -> %d (limit %d) %s"), *Source->GetPathName(), Plan.UploadTriangles, After, Plan.Limit,
		*Work->GetPathName());
	return true;
}

bool FMantleAvatarTrianglesFixer::PromoteLOD(USkeletalMesh* Mesh, const int32 LOD, FString& OutError)
{
	using namespace MantleAvatarTrianglesPrivate;
	if (!Mesh || LOD <= 0 || LOD >= Mesh->GetNumSourceModels())
	{
		OutError = FString::Printf(TEXT("the mesh has no LOD %d"), LOD);
		return false;
	}
	FinishTrianglesCompile(Mesh);
	if (!Mesh->HasMeshDescription(LOD))
	{
		OutError = FString::Printf(TEXT("its LOD %d is generated from another LOD, so it has no geometry of its own to keep"), LOD);
		return false;
	}
	FMeshDescription Description;
	if (!Mesh->CloneMeshDescription(LOD, Description))
	{
		OutError = FString::Printf(TEXT("its LOD %d could not be read"), LOD);
		return false;
	}
	// The asset's LOD settings (MetaHuman meshes have one) must not reshape the copy's LODs on rebuild.
	Mesh->SetLODSettings(nullptr);
	// One LOD: the upload generates its own (A3.8).
	KeepOnlyLODZero(Mesh);
	{
		FScopedSkeletalMeshPostEditChange PostEdit(Mesh);
		Mesh->CreateMeshDescription(0, MoveTemp(Description));
		Mesh->CommitMeshDescription(0);
		if (FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(0))
		{
			Info->ReductionSettings = FSkeletalMeshOptimizationSettings();
			Info->LODMaterialMap.Reset();
			Info->bAllowCPUAccess = true;
		}
	}
	FinishTrianglesCompile(Mesh);
	if (UMantleBodyTopologyData::GetNumLODs(Mesh) < 1)
	{
		OutError = TEXT("the copy did not rebuild");
		return false;
	}
	return true;
}

bool FMantleAvatarTrianglesFixer::ReduceLODZero(USkeletalMesh* Mesh, const int32 Target, FString& OutError)
{
	using namespace MantleAvatarTrianglesPrivate;
	if (!Mesh || Target <= 0)
	{
		OutError = TEXT("there is nothing to reduce");
		return false;
	}
	if (!HasSkeletalReduction())
	{
		OutError = TEXT("this editor has no skeletal mesh reduction (the Skeletal Mesh Simplifier plugin is off)");
		return false;
	}
	FinishTrianglesCompile(Mesh);
	if (!Mesh->HasMeshDescription(0))
	{
		OutError = TEXT("LOD 0 has no source geometry to reduce");
		return false;
	}
	const int32 Before = FMantleCosmeticExport::CountTriangles(Mesh, 0);
	Mesh->SetLODSettings(nullptr);
	// One LOD: the upload generates its own (A3.8).
	KeepOnlyLODZero(Mesh);

	// Inline reduction of LOD 0 (the LOD reduces itself; its source geometry is kept in the asset).
	FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(0);
	if (!Info)
	{
		OutError = TEXT("the copy has no LOD 0");
		return false;
	}
	Info->bAllowCPUAccess = true;
	FSkeletalMeshOptimizationSettings& Settings = Info->ReductionSettings;
	Settings = FSkeletalMeshOptimizationSettings();
	Settings.BaseLOD = 0;
	Settings.TerminationCriterion = SMTC_AbsNumOfTriangles;
	Settings.MaxNumOfTriangles = static_cast<uint32>(Target);
	Settings.NumOfTrianglesPercentage = Before > 0 ? FMath::Clamp(static_cast<float>(Target) / static_cast<float>(Before), 0.01f, 1.0f) : 1.0f;
	// Skin weights as the source has them (the contract's limit is 8), the source's normals, no morphs (avatars carry none).
	Settings.MaxBonesPerVertex = 8;
	Settings.bRecalcNormals = false;
	Settings.bRemapMorphTargets = false;
	Settings.bLockEdges = false;
	Settings.bEnforceBoneBoundaries = false;

	FSkeletalMeshUpdateContext Update;
	Update.SkeletalMesh = Mesh;
	FLODUtilities::SimplifySkeletalMeshLOD(Update, 0, GetTargetPlatformManagerRef().GetRunningTargetPlatform());
	FinishTrianglesCompile(Mesh);
	int32 After = FMantleCosmeticExport::CountTriangles(Mesh, 0);
	const int32 Slack = FMath::Max(1, Target / 50);
	if (After <= 0 || After > Target + Slack)
	{
		// The build reduces a LOD whose settings say so; ask it once more before giving up.
		Mesh->PostEditChange();
		FinishTrianglesCompile(Mesh);
		After = FMantleCosmeticExport::CountTriangles(Mesh, 0);
	}
	if (After <= 0 || After > Target + Slack)
	{
		OutError = FString::Printf(TEXT("the reduction stopped at %d triangles (from %d; the target was %d)"), After, Before, Target);
		return false;
	}
	return true;
}
