// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarStripFixer.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleBodyTopologyData.h"
#include "MantleCosmeticExport.h"
#include "MantleFusedClothing.h"
#include "MantleMetaHumanBuild.h"
#include "MetaHumanCharacter.h"
#include "MantleMetaHumanHeadOnly.h"
#include "MantleOutfitSections.h"
#include "MantleUnriggedBody.h"
#include "MantleRuntime.h"

#include "AssetToolsModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshDecompositionFunctions.h"
#include "GeometryScript/MeshMaterialFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "IAssetTools.h"
#include "Materials/MaterialInterface.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "SkinnedAssetCompiler.h"
#include "StaticMeshResources.h"
#include "UDynamicMesh.h"
#include "UObject/StrongObjectPtr.h"

const FName FMantleAvatarStripFixer::KindBareSkin(TEXT("BareSkin"));
const FName FMantleAvatarStripFixer::KindFaceOnly(TEXT("FaceOnly"));
const FName FMantleAvatarStripFixer::KindFeminineBody(TEXT("FeminineBody"));
const FName FMantleAvatarStripFixer::KindMasculineBody(TEXT("MasculineBody"));

FString FMantleAvatarStripFixer::MissingBodyChoice(TConstArrayView<FMantleConvertFix> Fixes, const int32 PartIndex)
{
	bool bFace = false;
	int32 Bodies = 0;
	for (const FMantleConvertFix& Fix : Fixes)
	{
		if (Fix.FixerId != MantleAvatarFixerIds::Strip || Fix.PartIndex != PartIndex || !Fix.bEnabled)
		{
			continue;
		}
		bFace |= Fix.Kind == KindFaceOnly;
		Bodies += Fix.Kind == KindFeminineBody || Fix.Kind == KindMasculineBody ? 1 : 0;
	}
	if (!bFace || Bodies == 1)
	{
		return FString();
	}
	return Bodies == 0 ? TEXT("pick the MetaHuman body the face goes on: Women's or Men's")
		: TEXT("pick only one MetaHuman body: Women's or Men's");
}

void FMantleAvatarStripFixer::AddBodyRow(TArray<FMantleConvertFix>& OutFixes, const int32 PartIndex, const FBodyRow& Row)
{
	auto Add = [&OutFixes, PartIndex](const FName Kind) -> FMantleConvertFix&
	{
		FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
		Fix.FixerId = MantleAvatarFixerIds::Strip;
		Fix.PartIndex = PartIndex;
		Fix.Kind = Kind;
		return Fix;
	};
	// Applied in this order: the face line, then the body it goes on, then the bare-skin line (which strips while the body is kept).
	FMantleConvertFix& Face = Add(KindFaceOnly);
	Face.bEnabled = Row.bFaceOnBody;
	Face.Problem = Row.Why;
	Face.Action = Row.bCanKeep
		? TEXT("Puts the face on a MetaHuman body (Women's or Men's) at Mantle's default height; players shape it with the Weight, Muscle, Chest and Hips sliders. Everything below the face (clothing, armour, hair) is left out of the body. Untick it to keep this character's body, which needs bare skin from head to feet.")
		: TEXT("Puts the face on a MetaHuman body (Women's or Men's) at Mantle's default height; players shape it with the Weight, Muscle, Chest and Hips sliders. This body can't be kept: untick it and the conversion stops.");
	for (const bool bFeminine : { false, true })
	{
		FMantleConvertFix& Body = Add(bFeminine ? KindFeminineBody : KindMasculineBody);
		Body.bEnabled = false;
		Body.Problem = TEXT("With the face on a MetaHuman body: which one? Pick one");
		Body.Action = bFeminine ? TEXT("Women's body.") : TEXT("Men's body.");
	}
	FMantleConvertFix& Bare = Add(KindBareSkin);
	Bare.bNeedsConfirm = true;
	// A body that can't be kept has no bare skin to confirm: the panel greys "Keep this character's body".
	Bare.bCanFix = Row.bCanKeep;
	Bare.bEnabled = Row.bBareFound;
	Bare.Problem = Row.BareProblem;
	Bare.Action = Row.bBareFound
		? TEXT("Ticked: a bare body was found. Untick it if any kept part is really clothing. Convert won't fix a body for you.")
		: TEXT("Tick to confirm the kept parts are bare skin. Convert won't fix a body for you: a character whose clothing is part of its body, with no skin under it, can't keep its body.");
}

bool FMantleAvatarStripFixer::BareBodyFound(const FMantleStripPlan& Plan, const bool bAllKept, FString& OutWhy)
{
	if (FMantleMetaHumanHeadOnly::IsCovered(Plan, OutWhy))
	{
		return false;
	}
	for (const FMantleStripSlot& Slot : Plan.Slots)
	{
		if (bAllKept && !Slot.bSkin)
		{
			OutWhy = FString::Printf(TEXT("'%s' doesn't read as skin"), *Slot.SlotName.ToString());
			return false;
		}
	}
	OutWhy.Reset();
	return true;
}

bool FMantleAvatarStripFixer::IsBodyRowLine(const FMantleConvertFix& Fix)
{
	return Fix.FixerId == MantleAvatarFixerIds::Strip
		&& (Fix.Kind == KindFaceOnly || Fix.Kind == KindFeminineBody || Fix.Kind == KindMasculineBody || Fix.Kind == KindBareSkin);
}

bool FMantleAvatarStripFixer::IsUnconfirmed(TConstArrayView<FMantleConvertFix> Fixes, const FMantleConvertFix& Fix)
{
	if (!Fix.bNeedsConfirm || Fix.bEnabled)
	{
		return false;
	}
	// With the face on a MetaHuman body nothing is stripped, so there is no bare skin to confirm.
	if (Fix.FixerId == MantleAvatarFixerIds::Strip && Fix.Kind == KindBareSkin)
	{
		return !Fixes.ContainsByPredicate([&Fix](const FMantleConvertFix& Other)
		{
			return Other.FixerId == MantleAvatarFixerIds::Strip && Other.PartIndex == Fix.PartIndex && Other.Kind == KindFaceOnly && Other.bEnabled;
		});
	}
	return true;
}
const FName FMantleAvatarStripFixer::KindOneSlot(TEXT("OneSlot"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarStripPrivate
{
	const TCHAR* const StripAvatarSlot = TEXT("avatar");

	/** StripPieces' entries: the head slot and the piece in one number. */
	int32 StripPieceKey(const int32 HeadSlot, const int32 Piece)
	{
		return HeadSlot * 65536 + Piece;
	}

	FString StripNumber(const int32 Value)
	{
		return FText::AsNumber(Value).ToString();
	}

	FString StripBoxText(const FBox3d& Box)
	{
		return Box.IsValid
			? FString::Printf(TEXT("(%.1f, %.1f, %.1f)..(%.1f, %.1f, %.1f)"), Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z)
			: FString(TEXT("(empty)"));
	}

	FBox3d StripBoundsOf(const UDynamicMesh& Mesh)
	{
		FBox3d Out(ForceInit);
		Mesh.ProcessMesh([&Out](const UE::Geometry::FDynamicMesh3& Read)
		{
			for (const int32 Vertex : Read.VertexIndicesItr())
			{
				Out += Read.GetVertex(Vertex);
			}
		});
		return Out;
	}

	/** LOD 0's source geometry through Geometry Script (default options, the source model). */
	UDynamicMesh* StripRead(USkeletalMesh& Mesh, FString& OutError)
	{
		UDynamicMesh* Out = NewObject<UDynamicMesh>(GetTransientPackage());
		FGeometryScriptCopyMeshFromAssetOptions Options;
		FGeometryScriptMeshReadLOD Read;
		Read.LODType = EGeometryScriptLODType::SourceModel;
		Read.LODIndex = 0;
		EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(&Mesh, Out, Options, Read, Outcome);
		if (Outcome != EGeometryScriptOutcomePins::Success || Out->GetTriangleCount() == 0)
		{
			OutError = FString::Printf(TEXT("%s's LOD 0 source geometry could not be read"), *Mesh.GetName());
			return nullptr;
		}
		return Out;
	}

	/** Deleting triangles leaves their vertices behind, unattached: drop them, then compact. */
	void StripClean(UDynamicMesh& Mesh)
	{
		UGeometryScriptLibrary_MeshRepairFunctions::RemoveUnusedVertices(&Mesh);
		UGeometryScriptLibrary_MeshRepairFunctions::CompactMesh(&Mesh);
	}

	/** A fresh read of Mesh with only the triangles of the material IDs in Keep. */
	UDynamicMesh* StripOnly(USkeletalMesh& Mesh, const TSet<int32>& Keep, FString& OutError)
	{
		UDynamicMesh* Out = StripRead(Mesh, OutError);
		if (!Out)
		{
			return nullptr;
		}
		bool bHasIds = false;
		const int32 Top = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(Out, bHasIds);
		for (int32 Id = 0; Id <= Top; ++Id)
		{
			if (!Keep.Contains(Id))
			{
				int32 Deleted = 0;
				UGeometryScriptLibrary_MeshMaterialFunctions::DeleteTrianglesByMaterialID(Out, Id, Deleted);
			}
		}
		StripClean(*Out);
		return Out;
	}

	/** Triangles and bounds per material ID of a read. */
	void StripSlotStats(const UDynamicMesh& Mesh, TMap<int32, int32>& OutTriangles, TMap<int32, FBox3d>& OutBounds)
	{
		Mesh.ProcessMesh([&OutTriangles, &OutBounds](const UE::Geometry::FDynamicMesh3& Read)
		{
			const UE::Geometry::FDynamicMeshMaterialAttribute* Ids = Read.HasAttributes() ? Read.Attributes()->GetMaterialID() : nullptr;
			for (const int32 Triangle : Read.TriangleIndicesItr())
			{
				const int32 Id = Ids ? Ids->GetValue(Triangle) : 0;
				OutTriangles.FindOrAdd(Id)++;
				FBox3d& Box = OutBounds.FindOrAdd(Id, FBox3d(ForceInit));
				const UE::Geometry::FIndex3i Corners = Read.GetTriangle(Triangle);
				Box += Read.GetVertex(Corners.A);
				Box += Read.GetVertex(Corners.B);
				Box += Read.GetVertex(Corners.C);
			}
		});
	}

	/**
	 * #50: for each skin slot of Plan (read from Mesh), whether clothing is modelled into it (FMantleFusedClothing on its triangles);
	 * the character's height is the whole mesh's.
	 */
	void StripJudgeFused(const UDynamicMesh& Mesh, FMantleStripPlan& Plan)
	{
		FBox3d All(ForceInit);
		for (const FMantleStripSlot& Slot : Plan.Slots)
		{
			if (Slot.Bounds.IsValid)
			{
				All += Slot.Bounds;
			}
		}
		const double Height = All.IsValid ? All.Max.Z - All.Min.Z : 0.0;
		TMap<int32, TArray<FVector3f>> Vertices;
		TMap<int32, TArray<int32>> Indices;
		for (const FMantleStripSlot& Slot : Plan.Slots)
		{
			if (Slot.bSkin)
			{
				Vertices.Add(Slot.Index);
				Indices.Add(Slot.Index);
			}
		}
		Mesh.ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Read)
		{
			const UE::Geometry::FDynamicMeshMaterialAttribute* Ids = Read.HasAttributes() ? Read.Attributes()->GetMaterialID() : nullptr;
			for (const int32 Triangle : Read.TriangleIndicesItr())
			{
				const int32 Id = Ids ? Ids->GetValue(Triangle) : 0;
				TArray<int32>* SlotIndices = Indices.Find(Id);
				if (!SlotIndices)
				{
					continue;
				}
				TArray<FVector3f>& SlotVertices = Vertices.FindChecked(Id);
				const UE::Geometry::FIndex3i Corners = Read.GetTriangle(Triangle);
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					SlotIndices->Add(SlotVertices.Add(FVector3f(Read.GetVertex(Corners[Corner]))));
				}
			}
		});
		for (FMantleStripSlot& Slot : Plan.Slots)
		{
			if (!Slot.bSkin)
			{
				continue;
			}
			const FMantleFusedResult Result = FMantleFusedClothing::CheckSlot({ Slot.SlotName.ToString(), Vertices.FindChecked(Slot.Index), Indices.FindChecked(Slot.Index) }, Height);
			Slot.Fused = Result.Verdict;
			Slot.FusedWhy = Result.Reason;
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_STRIP fused-clothing check '%s': %s (%.0f%% layered) %s"), *Slot.SlotName.ToString(),
				Result.Verdict == EMantleFusedVerdict::Bare ? TEXT("bare") : Result.Verdict == EMantleFusedVerdict::FusedClothing ? TEXT("fused clothing") : TEXT("unknown"),
				Result.Layered * 100.0, *Result.Reason);
		}
	}

	/** A copy of Mesh (weights and materials kept) in Folder/Name whose LOD 0 source model is Out. Null with why; the copy with why when only the write failed. */
	USkeletalMesh* StripWriteCopy(USkeletalMesh& Mesh, UDynamicMesh* Out, const FString& Folder, const FString& Name, const TCHAR* What, FString& OutError)
	{
	// A copy of the body takes the skin-only geometry (its LOD 0 source model; weights and materials kept).
	if (Folder.IsEmpty() || !FPackageName::IsValidLongPackageName(Folder, /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder the copy can go in"), *Folder);
		return nullptr;
	}
	TArray<USkinnedAsset*> Compiling = { &Mesh };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	FString PackageName;
	FString AssetName;
	AssetTools.CreateUniqueAssetName(Folder / Name, FString(), PackageName, AssetName);
	USkeletalMesh* Copy = Cast<USkeletalMesh>(AssetTools.DuplicateAsset(AssetName, FPackageName::GetLongPackagePath(PackageName), &Mesh));
	if (!Copy)
	{
		OutError = FString::Printf(TEXT("%s could not be copied to %s"), *Mesh.GetName(), *Folder);
		return nullptr;
	}
	Compiling = { Copy };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	FGeometryScriptCopyMeshToAssetOptions Options;
	Options.bEnableRecomputeNormals = false;
	Options.bEnableRecomputeTangents = false;
	FGeometryScriptMeshWriteLOD Write;
	Write.LODIndex = 0;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToSkeletalMesh(Out, Copy, Options, Write, Outcome);
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	if (Outcome != EGeometryScriptOutcomePins::Success)
	{
		OutError = FString::Printf(TEXT("the geometry could not be written into %s"), *Copy->GetName());
		return Copy; // The caller records and discards it.
	}
		return Copy;
	}

	/** The head slot's pieces, the largest first (stable on a tie), each held. */
	bool StripHeadPieces(USkeletalMesh& Mesh, const int32 HeadSlot, TArray<TStrongObjectPtr<UDynamicMesh>>& OutPieces, FString& OutError)
	{
		OutPieces.Reset();
		UDynamicMesh* Head = StripOnly(Mesh, TSet<int32>({ HeadSlot }), OutError);
		if (!Head)
		{
			return false;
		}
		const TStrongObjectPtr<UDynamicMesh> HeadHeld(Head);
		UDynamicMeshPool* Pool = NewObject<UDynamicMeshPool>(GetTransientPackage());
		const TStrongObjectPtr<UDynamicMeshPool> PoolHeld(Pool);
		TArray<UDynamicMesh*> Pieces;
		UGeometryScriptLibrary_MeshDecompositionFunctions::SplitMeshByComponents(Head, Pieces, Pool);
		for (UDynamicMesh* Piece : Pieces)
		{
			if (Piece && Piece->GetTriangleCount() > 0)
			{
				OutPieces.Emplace(Piece);
			}
		}
		OutPieces.StableSort([](const TStrongObjectPtr<UDynamicMesh>& A, const TStrongObjectPtr<UDynamicMesh>& B)
		{
			return A->GetTriangleCount() > B->GetTriangleCount();
		});
		if (OutPieces.Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s's head material has no triangles"), *Mesh.GetName());
			return false;
		}
		return true;
	}

	/** Render vertex positions of LOD 0 (a skeletal or static mesh): what draws. */
	bool StripRenderPositions(UObject* Mesh, TArray<FVector3f>& OutPositions, FString& OutError)
	{
		OutPositions.Reset();
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Mesh))
		{
			FMantleBodyLODTopology Topology;
			return UMantleBodyTopologyData::ReadLOD(Skeletal, 0, Topology, &OutPositions, nullptr, &OutError);
		}
		if (const UStaticMesh* Static = Cast<UStaticMesh>(Mesh))
		{
			const FStaticMeshRenderData* RenderData = Static->GetRenderData();
			if (!RenderData || RenderData->LODResources.Num() == 0)
			{
				OutError = FString::Printf(TEXT("%s has no render data"), *Static->GetName());
				return false;
			}
			const FPositionVertexBuffer& Positions = RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer;
			if (Positions.GetNumVertices() == 0 || Positions.GetVertexData() == nullptr)
			{
				OutError = FString::Printf(TEXT("%s's LOD 0 has no CPU copy of its vertices"), *Static->GetName());
				return false;
			}
			OutPositions.Reserve(Positions.GetNumVertices());
			for (uint32 Vertex = 0; Vertex < Positions.GetNumVertices(); ++Vertex)
			{
				OutPositions.Add(Positions.VertexPosition(Vertex));
			}
			return true;
		}
		OutError = TEXT("the conform input is neither a skeletal nor a static mesh");
		return false;
	}
}

const FMantleStripSlot* FMantleStripPlan::FindSlot(const int32 Index) const
{
	return Slots.FindByPredicate([Index](const FMantleStripSlot& Slot) { return Slot.Index == Index; });
}

bool FMantleAvatarStrip::LooksLikeSkin(const FString& SlotName, const FString& MaterialName, const FString& BaseMaterialName)
{
	FMantleOutfitSection Section;
	Section.SlotName = SlotName;
	Section.MaterialName = MaterialName;
	Section.BaseMaterialName = BaseMaterialName;
	return FMantleOutfitSections::Classify(Section) == EMantleOutfitPiece::Skin && !NamesFaceDetail(SlotName, MaterialName);
}

bool FMantleAvatarStrip::NamesFaceDetail(const FString& SlotName, const FString& MaterialName)
{
	static const TCHAR* const NotSkin[] = { TEXT("hair"), TEXT("hairs"), TEXT("lash"), TEXT("lashes"), TEXT("eyelash"), TEXT("eyelashes"),
		TEXT("brow"), TEXT("brows"), TEXT("eyebrow"), TEXT("eyebrows"), TEXT("eye"), TEXT("eyes"), TEXT("cornea"), TEXT("iris"),
		TEXT("teeth"), TEXT("tooth"), TEXT("tongue"), TEXT("mouth"), TEXT("gum"), TEXT("gums"), TEXT("beard"), TEXT("moustache"),
		TEXT("mustache"), TEXT("fuzz"), TEXT("peach") };
	for (const FString* Name : { &SlotName, &MaterialName })
	{
		for (const FString& Word : FMantleOutfitSections::Words(*Name))
		{
			for (const TCHAR* Not : NotSkin)
			{
				if (Word.Equals(Not, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool FMantleAvatarStrip::IsInside(const FBox3d& Inner, const FBox3d& Outer, const double Margin)
{
	return Inner.IsValid && Outer.IsValid
		&& Inner.Min.X >= Outer.Min.X - Margin && Inner.Min.Y >= Outer.Min.Y - Margin && Inner.Min.Z >= Outer.Min.Z - Margin
		&& Inner.Max.X <= Outer.Max.X + Margin && Inner.Max.Y <= Outer.Max.Y + Margin && Inner.Max.Z <= Outer.Max.Z + Margin;
}

bool FMantleAvatarStrip::Plan(USkeletalMesh& Mesh, const TSet<int32>* RemovedSlots, FMantleStripPlan& OutPlan, FString& OutError)
{
	using namespace MantleAvatarStripPrivate;
	OutPlan = FMantleStripPlan();
	UDynamicMesh* Full = StripRead(Mesh, OutError);
	if (!Full)
	{
		return false;
	}
	const TStrongObjectPtr<UDynamicMesh> FullHeld(Full);
	TMap<int32, int32> Triangles;
	TMap<int32, FBox3d> Bounds;
	StripSlotStats(*Full, Triangles, Bounds);
	const TArray<FSkeletalMaterial>& Materials = Mesh.GetMaterials();
	TArray<int32> Ids;
	Triangles.GetKeys(Ids);
	Ids.Sort();
	for (const int32 Id : Ids)
	{
		FMantleStripSlot& Slot = OutPlan.Slots.AddDefaulted_GetRef();
		Slot.Index = Id;
		const FSkeletalMaterial* Material = Materials.IsValidIndex(Id) ? &Materials[Id] : nullptr;
		Slot.SlotName = Material ? Material->MaterialSlotName : FName(*FString::Printf(TEXT("Slot_%d"), Id));
		UMaterialInterface* Interface = Material ? Material->MaterialInterface.Get() : nullptr;
		Slot.MaterialName = Interface ? Interface->GetName() : FString();
		const UMaterialInterface* Base = Interface ? Interface->GetBaseMaterial() : nullptr;
		Slot.Triangles = Triangles.FindRef(Id);
		Slot.Bounds = Bounds.FindRef(Id);
		Slot.bSkin = LooksLikeSkin(Slot.SlotName.ToString(), Slot.MaterialName, Base ? Base->GetName() : FString());
	}
	// Strip's own Apply asks for the creator's choices (RemovedSlots): only the first look judges the shapes.
	if (!RemovedSlots)
	{
		StripJudgeFused(*Full, OutPlan);
	}

	// The kept skin: the creator's choice when given (every slot not removed), else the slots read as skin.
	TArray<const FMantleStripSlot*> Kept;
	for (const FMantleStripSlot& Slot : OutPlan.Slots)
	{
		if (RemovedSlots ? !RemovedSlots->Contains(Slot.Index) : Slot.bSkin)
		{
			Kept.Add(&Slot);
		}
	}
	if (Kept.Num() == 0)
	{
		return true;
	}
	// The head skin is the kept slot whose lowest point is highest (by geometry, never by slot number or name).
	const FMantleStripSlot* Head = Kept[0];
	for (const FMantleStripSlot* Slot : Kept)
	{
		if (Slot->Bounds.Min.Z > Head->Bounds.Min.Z)
		{
			Head = Slot;
		}
	}
	OutPlan.HeadSlot = Head->Index;
	OutPlan.bOneSlot = Kept.Num() == 1;
	if (OutPlan.bOneSlot)
	{
		// One skin slot for body and head: it is declared skin-only, so every piece is kept (on a one-slot mesh made of
		// parts the largest piece can be an arm, and pieces inside its bounds, knuckles, would be removed).
		return true;
	}
	TArray<TStrongObjectPtr<UDynamicMesh>> Pieces;
	if (!StripHeadPieces(Mesh, OutPlan.HeadSlot, Pieces, OutError))
	{
		return false;
	}
	const FBox3d SkinBox = StripBoundsOf(*Pieces[0]);
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		FMantleStripPiece& Piece = OutPlan.HeadPieces.AddDefaulted_GetRef();
		Piece.Triangles = Pieces[Index]->GetTriangleCount();
		Piece.Bounds = StripBoundsOf(*Pieces[Index]);
		Piece.bInside = Index > 0 && IsInside(Piece.Bounds, SkinBox);
	}
	return true;
}

bool FMantleAvatarStrip::PlanStatic(UStaticMesh& Mesh, FMantleStripPlan& OutPlan, FString& OutError)
{
	using namespace MantleAvatarStripPrivate;
	OutPlan = FMantleStripPlan();
	UDynamicMesh* Full = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> FullHeld(Full);
	FGeometryScriptCopyMeshFromAssetOptions Options;
	FGeometryScriptMeshReadLOD Read;
	Read.LODType = EGeometryScriptLODType::SourceModel;
	Read.LODIndex = 0;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(&Mesh, Full, Options, Read, Outcome, /*bUseSectionMaterials*/ false);
	if (Outcome != EGeometryScriptOutcomePins::Success || Full->GetTriangleCount() == 0)
	{
		OutError = FString::Printf(TEXT("%s's LOD 0 source geometry could not be read"), *Mesh.GetName());
		return false;
	}
	TMap<int32, int32> Triangles;
	TMap<int32, FBox3d> Bounds;
	StripSlotStats(*Full, Triangles, Bounds);
	const TArray<FStaticMaterial>& Materials = Mesh.GetStaticMaterials();
	TArray<int32> Ids;
	Triangles.GetKeys(Ids);
	Ids.Sort();
	for (const int32 Id : Ids)
	{
		FMantleStripSlot& Slot = OutPlan.Slots.AddDefaulted_GetRef();
		Slot.Index = Id;
		const FStaticMaterial* Material = Materials.IsValidIndex(Id) ? &Materials[Id] : nullptr;
		Slot.SlotName = Material ? Material->MaterialSlotName : FName(*FString::Printf(TEXT("Slot_%d"), Id));
		UMaterialInterface* Interface = Material ? Material->MaterialInterface.Get() : nullptr;
		Slot.MaterialName = Interface ? Interface->GetName() : FString();
		const UMaterialInterface* Base = Interface ? Interface->GetBaseMaterial() : nullptr;
		Slot.Triangles = Triangles.FindRef(Id);
		Slot.Bounds = Bounds.FindRef(Id);
		Slot.bSkin = LooksLikeSkin(Slot.SlotName.ToString(), Slot.MaterialName, Base ? Base->GetName() : FString());
	}
	StripJudgeFused(*Full, OutPlan);
	return true;
}

USkeletalMesh* FMantleAvatarStrip::WriteSkinOnly(USkeletalMesh& Mesh, const TSet<int32>& RemovedSlots, const TSet<int32>& RemovedPieces, const FString& Folder,
	const FString& Name, FString& OutError)
{
	using namespace MantleAvatarStripPrivate;
	FMantleStripPlan Plan;
	if (!FMantleAvatarStrip::Plan(Mesh, &RemovedSlots, Plan, OutError))
	{
		return nullptr;
	}
	if (Plan.HeadSlot == INDEX_NONE)
	{
		OutError = TEXT("every material is ticked for removal: keep the bare-skin ones (untick them)");
		return nullptr;
	}
	TSet<int32> KeptOthers;
	for (const FMantleStripSlot& Slot : Plan.Slots)
	{
		if (!RemovedSlots.Contains(Slot.Index) && (Plan.bOneSlot || Slot.Index != Plan.HeadSlot))
		{
			KeptOthers.Add(Slot.Index);
		}
	}
	// The body: the kept slots but the head's (in one-slot mode, the one slot, whole).
	UDynamicMesh* Out = StripOnly(Mesh, KeptOthers, OutError);
	if (!Out)
	{
		return nullptr;
	}
	const TStrongObjectPtr<UDynamicMesh> OutHeld(Out);
	if (!Plan.bOneSlot)
	{
		TArray<TStrongObjectPtr<UDynamicMesh>> Pieces;
		if (!StripHeadPieces(Mesh, Plan.HeadSlot, Pieces, OutError))
		{
			return nullptr;
		}
		for (int32 Index = 0; Index < Pieces.Num(); ++Index)
		{
			// The head skin always; every other piece unless removed (skin weights and material IDs travel with it).
			if (Index > 0 && RemovedPieces.Contains(Index))
			{
				continue;
			}
			UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(Out, Pieces[Index].Get(), FTransform::Identity);
		}
	}
	StripClean(*Out);
	if (Out->GetTriangleCount() == 0)
	{
		OutError = TEXT("nothing is left once the removed materials and pieces are taken out");
		return nullptr;
	}

	USkeletalMesh* Copy = StripWriteCopy(Mesh, Out, Folder, Name, TEXT("skin-only"), OutError);
	if (!Copy || !OutError.IsEmpty())
	{
		return Copy;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_STRIP %s -> %s: %d triangles kept (head slot %d, one slot %d)"), *Mesh.GetName(), *Copy->GetPathName(),
		Out->GetTriangleCount(), Plan.HeadSlot, Plan.bOneSlot ? 1 : 0);
	return Copy;
}

USkeletalMesh* FMantleAvatarStrip::WriteSlots(USkeletalMesh& Mesh, const TSet<int32>& KeepSlots, const FString& Folder, const FString& Name, FString& OutError)
{
	using namespace MantleAvatarStripPrivate;
	UDynamicMesh* Out = StripOnly(Mesh, KeepSlots, OutError);
	if (!Out)
	{
		return nullptr;
	}
	const TStrongObjectPtr<UDynamicMesh> OutHeld(Out);
	StripClean(*Out);
	if (Out->GetTriangleCount() == 0)
	{
		OutError = TEXT("none of the kept material slots has any triangles");
		return nullptr;
	}
	USkeletalMesh* Copy = StripWriteCopy(Mesh, Out, Folder, Name, TEXT("piece"), OutError);
	if (Copy && OutError.IsEmpty())
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_STRIP %s -> %s: %d triangles of %d slots"), *Mesh.GetName(), *Copy->GetPathName(), Out->GetTriangleCount(), KeepSlots.Num());
	}
	return Copy;
}

double FMantleAvatarStrip::LargestNearestDistance(TConstArrayView<FVector3f> From, TConstArrayView<FVector3f> To, const double CellCm, const double SearchCm)
{
	if (From.Num() == 0)
	{
		return 0.0;
	}
	if (To.Num() == 0 || CellCm <= 0.0)
	{
		return SearchCm;
	}
	auto CellOf = [CellCm](const FVector3f& Point)
	{
		return FIntVector(FMath::FloorToInt32(Point.X / CellCm), FMath::FloorToInt32(Point.Y / CellCm), FMath::FloorToInt32(Point.Z / CellCm));
	};
	TMap<FIntVector, TArray<int32>> Cells;
	for (int32 Index = 0; Index < To.Num(); ++Index)
	{
		Cells.FindOrAdd(CellOf(To[Index])).Add(Index);
	}
	const int32 MaxRing = FMath::CeilToInt32(SearchCm / CellCm) + 1;
	double Largest = 0.0;
	for (const FVector3f& Point : From)
	{
		const FIntVector Cell = CellOf(Point);
		double Best = TNumericLimits<double>::Max();
		for (int32 Ring = 0; Ring <= MaxRing; ++Ring)
		{
			for (int32 X = -Ring; X <= Ring; ++X)
			{
				for (int32 Y = -Ring; Y <= Ring; ++Y)
				{
					for (int32 Z = -Ring; Z <= Ring; ++Z)
					{
						if (FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z)) != Ring)
						{
							continue;
						}
						if (const TArray<int32>* Members = Cells.Find(Cell + FIntVector(X, Y, Z)))
						{
							for (const int32 Member : *Members)
							{
								Best = FMath::Min(Best, static_cast<double>(FVector3f::DistSquared(Point, To[Member])));
							}
						}
					}
				}
			}
			if (Best <= FMath::Square(Ring * CellCm))
			{
				break;
			}
		}
		Largest = FMath::Max(Largest, Best == TNumericLimits<double>::Max() ? SearchCm : FMath::Min(SearchCm, FMath::Sqrt(Best)));
		if (Largest >= SearchCm)
		{
			break;
		}
	}
	return Largest;
}

bool FMantleAvatarStrip::InputCheck(UObject* Mesh, FString& OutLine, TArray<FVector3f>* OutVertices, TArray<int32>* OutTriangleIndices)
{
	using namespace MantleAvatarStripPrivate;
	TArray<FVector3f> Conform;
	TArray<int32> Indices;
	const bool bRead = Mesh && UMetaHumanCharacterEditorSubsystem::GetMeshDataForConforming(Mesh, Conform, Indices);
	TArray<FVector3f> Render;
	FString Error;
	if (!bRead || Conform.Num() == 0 || !StripRenderPositions(Mesh, Render, Error))
	{
		OutLine = FString::Printf(TEXT("INPUTCHECK %s: %s"), Mesh ? *Mesh->GetName() : TEXT("(none)"),
			!bRead || Conform.Num() == 0 ? TEXT("Epic's conform reader read nothing") : *Error);
		return false;
	}
	const double ConformToRender = LargestNearestDistance(Conform, Render);
	const double RenderToConform = LargestNearestDistance(Render, Conform);
	const bool bMatch = ConformToRender < InputMatchCm && RenderToConform < InputMatchCm;
	OutLine = FString::Printf(TEXT("INPUTCHECK %s: %d verts %d tris | conform->render max %.3f cm | render->conform max %.3f cm: %s"), *Mesh->GetName(),
		Conform.Num(), Indices.Num() / 3, ConformToRender, RenderToConform, bMatch ? TEXT("MATCH") : TEXT("MISMATCH"));
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_%s"), *OutLine);
	if (OutVertices)
	{
		*OutVertices = MoveTemp(Conform);
	}
	if (OutTriangleIndices)
	{
		*OutTriangleIndices = MoveTemp(Indices);
	}
	return bMatch;
}

FName FMantleAvatarStripFixer::SlotKind(const int32 SlotIndex)
{
	return FName(*FString::Printf(TEXT("Slot%d"), SlotIndex));
}

FName FMantleAvatarStripFixer::PieceKind(const int32 HeadSlot, const int32 Piece)
{
	return FName(*FString::Printf(TEXT("Piece%d_%d"), HeadSlot, Piece));
}

bool FMantleAvatarStripFixer::IsBodyToStrip(const FMantleConvertContext& Context, const int32 PartIndex, const FMantleAvatarContract& Contract)
{
	using namespace MantleAvatarStripPrivate;
	if (Context.Route != EMantleAvatarRoute::Convert || !Context.AssemblyFolder.IsEmpty() || !Context.Parts.IsValidIndex(PartIndex))
	{
		return false;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	const USkeletalMesh* Mesh = Part.Mesh.Get();
	// #776: a model with no skeleton is fitted by the MHUnrigged step instead (there is no body to strip).
	if (!Mesh || Part.bUnrigged || !Part.bIncluded || !Part.Slot.Equals(StripAvatarSlot, ESearchCase::CaseSensitive))
	{
		return false;
	}
	// A MetaHuman body (Mantle's reference topology) is not converted, and a mesh this conversion wrote is past the strip.
	const bool bCreated = Context.CreatedAssets.ContainsByPredicate([Mesh](const TWeakObjectPtr<UObject>& Created) { return Created.Get() == Mesh; });
	return !bCreated && FMantleCosmeticExport::FindReferenceLOD(Mesh, Contract) == INDEX_NONE;
}

void FMantleAvatarStripFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	using namespace MantleAvatarStripPrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract)
	{
		return;
	}
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		// #875: an assembled MetaHuman (no mesh on its part yet: MHSource makes it a character) gets the body row too. The face goes on
		// a MetaHuman body when MetaHuman Creator can't fit its body (the clothing is merged into the body mesh), and then the body
		// can't be kept; otherwise its body is kept, and a MetaHuman body with separate clothing is a bare body. Nothing is stripped.
		FString CantFitWhy;
		const bool bCantFit = FMantleMetaHumanBuild::AssemblyBodyNeedsFaceOnly(Context, PartIndex, CantFitWhy);
		if (bCantFit || (Context.AssemblyBodyToFit.IsValid() && Context.AssemblyFaceToFit.IsValid() && !Cast<UMetaHumanCharacter>(Context.Source.Get())
			&& FMantleMetaHumanSourceFixer::IsCharacterToBuild(Context, PartIndex)))
		{
			FBodyRow Row;
			Row.bFaceOnBody = bCantFit;
			Row.bCanKeep = !bCantFit;
			Row.bBareFound = !bCantFit;
			Row.Why = bCantFit ? FString::Printf(TEXT("MetaHuman Creator can't fit this body (%s)"), *CantFitWhy)
				: FString(TEXT("a MetaHuman body with its clothing separate is a bare body"));
			Row.BareProblem = TEXT("Keeping the body needs it bare: its clothing in separate pieces, none merged into the body");
			AddBodyRow(OutFixes, PartIndex, Row);
			continue;
		}
		// #50: a model with no skeleton has no body to strip; its head is cut by height (MakeCleanHeadUnrigged). Nothing is removed,
		// so every slot is kept and must read as skin for the bare body to be found.
		if (FMantleUnriggedBody::IsUnriggedBody(Context, PartIndex))
		{
			const FMantleConvertPart& Unrigged = Context.Parts[PartIndex];
			FMantleStripPlan UnriggedPlan;
			FString PlanError;
			UStaticMesh* StaticSource = Unrigged.StaticSource.Get();
			USkeletalMesh* SkeletalSource = Unrigged.Original.Get();
			const bool bRead = StaticSource ? FMantleAvatarStrip::PlanStatic(*StaticSource, UnriggedPlan, PlanError)
				: SkeletalSource ? FMantleAvatarStrip::Plan(*SkeletalSource, nullptr, UnriggedPlan, PlanError) : false;
			FString CoveredWhy;
			const bool bCovered = bRead && FMantleMetaHumanHeadOnly::IsCovered(UnriggedPlan, CoveredWhy);
			FString BareWhy;
			FBodyRow Row;
			Row.bFaceOnBody = bCovered;
			Row.bBareFound = bRead && BareBodyFound(UnriggedPlan, /*bAllKept*/ true, BareWhy);
			Row.Why = bCovered ? FString::Printf(TEXT("the body is covered by clothing (%s)"), *CoveredWhy)
				: Row.bBareFound ? FString(TEXT("a bare body was found"))
				: FString::Printf(TEXT("the body may be bare, but %s: tick the bare-skin line yourself if it is"), bRead ? *BareWhy : *PlanError);
			Row.BareProblem = TEXT("Keeping the body fits the whole model to a MetaHuman body: it must be bare skin from head to feet");
			AddBodyRow(OutFixes, PartIndex, Row);
			continue;
		}
		if (!IsBodyToStrip(Context, PartIndex, *Contract))
		{
			continue;
		}
		USkeletalMesh* Mesh = Context.Parts[PartIndex].Mesh.Get();
		auto AddFix = [&OutFixes, PartIndex, this](const FName Kind) -> FMantleConvertFix&
		{
			FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
			Fix.FixerId = GetId();
			Fix.PartIndex = PartIndex;
			Fix.Kind = Kind;
			return Fix;
		};
		FMantleStripPlan Plan;
		FString Error;
		if (!FMantleAvatarStrip::Plan(*Mesh, nullptr, Plan, Error))
		{
			FMantleConvertFix& Fix = AddFix(KindBareSkin);
			Fix.Problem = FString::Printf(TEXT("%s can't be read to strip it to bare skin"), *Mesh->GetName());
			Fix.Action = FString::Printf(TEXT("Can't: %s."), *Error);
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			continue;
		}

		TArray<FString> KeptNames;
		for (const FMantleStripSlot& Slot : Plan.Slots)
		{
			FMantleConvertFix& Fix = AddFix(SlotKind(Slot.Index));
			Fix.bEnabled = !Slot.bSkin;
			if (Slot.bSkin)
			{
				KeptNames.Add(FString::Printf(TEXT("'%s'"), *Slot.SlotName.ToString()));
				Fix.Problem = FString::Printf(TEXT("'%s' (%s triangles) reads as bare skin: kept"), *Slot.SlotName.ToString(), *StripNumber(Slot.Triangles));
				Fix.Action = TEXT("Tick it only if it isn't bare skin: it is then removed.");
			}
			else
			{
				Fix.Problem = FString::Printf(TEXT("'%s' (%s triangles) isn't bare skin"), *Slot.SlotName.ToString(), *StripNumber(Slot.Triangles));
				Fix.Action = TEXT("Removes it from the copy that becomes the MetaHuman (Epic gives it its own eyes, teeth and skin). Untick it only if it is bare skin. Clothing taken off here can still be an item: the clothing Convert finds (separate pieces beside the body, or the clothing in its own sections) is listed after Convert, under What to publish.");
			}
		}
		if (Plan.bOneSlot)
		{
			const FMantleStripSlot* Slot = Plan.FindSlot(Plan.HeadSlot);
			FMantleConvertFix& Fix = AddFix(KindOneSlot);
			Fix.Problem = FString::Printf(TEXT("the body and the head share one skin material ('%s')"), Slot ? *Slot->SlotName.ToString() : TEXT("?"));
			Fix.Action = TEXT("Every piece of it is kept. If it also holds eyes, teeth or a mouth, use a skin-only version of the character instead.");
			Fix.bCanFix = false;
			Fix.bEnabled = false;
		}
		else
		{
			for (int32 Piece = 1; Piece < Plan.HeadPieces.Num(); ++Piece)
			{
				if (!Plan.HeadPieces[Piece].bInside)
				{
					continue;
				}
				const FMantleStripSlot* Slot = Plan.FindSlot(Plan.HeadSlot);
				FMantleConvertFix& Fix = AddFix(PieceKind(Plan.HeadSlot, Piece));
				Fix.Problem = FString::Printf(TEXT("a piece of '%s' sits inside the head (%s triangles, %s)"), Slot ? *Slot->SlotName.ToString() : TEXT("?"),
					*StripNumber(Plan.HeadPieces[Piece].Triangles), *StripBoxText(Plan.HeadPieces[Piece].Bounds));
				Fix.Action = TEXT("Removes it (teeth, a mouth's inside or eye corners sit there). Untick it if it is skin, such as a nail or an ear piece.");
			}
		}

		// #770: a body with no skin under its clothing puts its face on a MetaHuman body instead.
		FString CoveredWhy;
		const bool bCovered = FMantleMetaHumanHeadOnly::IsCovered(Plan, CoveredWhy);
		FString BareWhy;
		FBodyRow Row;
		Row.bFaceOnBody = bCovered;
		// The kept slots are the ones read as skin (the slot lines' defaults): only the covered check can say no here.
		Row.bBareFound = BareBodyFound(Plan, /*bAllKept*/ false, BareWhy);
		Row.Why = bCovered ? FString::Printf(TEXT("the body is covered by clothing (%s)"), *CoveredWhy) : FString(TEXT("a bare body was found"));
		Row.BareProblem = KeptNames.Num() > 0
			? FString::Printf(TEXT("Keeping the body needs it bare: the kept parts (%s) must be skin, with no clothing merged into the body"), *FString::Join(KeptNames, TEXT(", ")))
			: FString(TEXT("Keeping the body needs it bare, and no material here reads as bare skin: untick the ones that are"));
		AddBodyRow(OutFixes, PartIndex, Row);
	}
}

bool FMantleAvatarStripFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	using namespace MantleAvatarStripPrivate;
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the strip names no part");
		return false;
	}
	const FString Kind = Fix.Kind.ToString();
	// The choice lines: recorded for the bare-skin line, which comes after them.
	if (Kind.StartsWith(TEXT("Slot"), ESearchCase::CaseSensitive))
	{
		Context.StripSlots.FindOrAdd(Fix.PartIndex).Add(FCString::Atoi(*Kind.RightChop(4)));
		return true;
	}
	if (Kind.StartsWith(TEXT("Piece"), ESearchCase::CaseSensitive))
	{
		FString SlotText;
		FString PieceText;
		if (!Kind.RightChop(5).Split(TEXT("_"), &SlotText, &PieceText))
		{
			OutError = FString::Printf(TEXT("the strip line %s names no piece"), *Kind);
			return false;
		}
		Context.StripPieces.FindOrAdd(Fix.PartIndex).Add(StripPieceKey(FCString::Atoi(*SlotText), FCString::Atoi(*PieceText)));
		return true;
	}
	if (Fix.Kind == KindFaceOnly)
	{
		FMantleConvertMetaHumanWork& Work = Context.MetaHuman.FindOrAdd(Fix.PartIndex);
		Work.bFaceOnly = true;
		Work.FaceOnlyWhy = Fix.Problem;
		Context.Notes.Add(FString::Printf(TEXT("%s: keeping the face only; it goes on MetaHuman's own body. %s"), *Context.Parts[Fix.PartIndex].Name, *Fix.Problem));
		return true;
	}
	if (Fix.Kind == KindFeminineBody || Fix.Kind == KindMasculineBody)
	{
		// Applied after the face line (it comes first in the list); without it there's no face route to choose a body for.
		if (FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(Fix.PartIndex); Work && Work->bFaceOnly)
		{
			Work->bFeminineBody = Fix.Kind == KindFeminineBody;
			Context.Notes.Add(FString::Printf(TEXT("%s: the face goes on a MetaHuman %s body."), *Context.Parts[Fix.PartIndex].Name,
				Work->bFeminineBody ? TEXT("Women's") : TEXT("Men's")));
		}
		return true;
	}
	if (Fix.Kind != KindBareSkin)
	{
		OutError = FString::Printf(TEXT("the Strip fixer has no fix of kind %s"), *Kind);
		return false;
	}

	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	if (const FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(Fix.PartIndex); Work && Work->bFaceOnly)
	{
		// #770: the face route takes the head from the source itself; there is no body to strip.
		return true;
	}
	if (Part.bUnrigged || !Context.AssemblyFolder.IsEmpty())
	{
		// The body row's bare-skin line on a model with no rig or an assembled MetaHuman: the creator's word, nothing to strip.
		Context.Notes.Add(FString::Printf(TEXT("%s: its body is kept; the kept parts are bare skin."), *Part.Name));
		return true;
	}
	USkeletalMesh* Mesh = Part.Mesh.Get();
	if (!Mesh)
	{
		OutError = TEXT("the part has no mesh to strip");
		return false;
	}
	const TSet<int32> RemovedSlots = Context.StripSlots.FindRef(Fix.PartIndex);
	FMantleStripPlan Plan;
	if (!FMantleAvatarStrip::Plan(*Mesh, &RemovedSlots, Plan, OutError))
	{
		return false;
	}
	// Pieces recorded for the head slot as it now stands (a creator who changed the kept slots changed the head too).
	TSet<int32> RemovedPieces;
	for (const int32 Key : Context.StripPieces.FindRef(Fix.PartIndex))
	{
		if (Key / 65536 == Plan.HeadSlot)
		{
			RemovedPieces.Add(Key % 65536);
		}
	}
	USkeletalMesh* SkinOnly = FMantleAvatarStrip::WriteSkinOnly(*Mesh, RemovedSlots, RemovedPieces, Context.OutputFolder / TEXT("Work"),
		Mesh->GetName() + TEXT("_SkinOnly"), OutError);
	if (!SkinOnly)
	{
		return false;
	}
	Context.CreatedAssets.Add(SkinOnly);
	if (!OutError.IsEmpty())
	{
		return false;
	}
	FString Line;
	if (!FMantleAvatarStrip::InputCheck(SkinOnly, Line))
	{
		OutError = FString::Printf(TEXT("Epic's conform would not read the skin-only body as it renders (%s); recorded, not patched"), *Line);
		return false;
	}
	TArray<FString> Removed;
	for (const FMantleStripSlot& Slot : Plan.Slots)
	{
		if (RemovedSlots.Contains(Slot.Index))
		{
			Removed.Add(FString::Printf(TEXT("'%s' (%s triangles)"), *Slot.SlotName.ToString(), *StripNumber(Slot.Triangles)));
		}
	}
	Context.Notes.Add(FString::Printf(TEXT("%s: stripped to bare skin in %s; removed %s%s. %s"), *Part.Name, *SkinOnly->GetName(),
		Removed.Num() > 0 ? *FString::Join(Removed, TEXT(", ")) : TEXT("no material"),
		RemovedPieces.Num() > 0 ? *FString::Printf(TEXT(" and %d piece(s) inside the head"), RemovedPieces.Num()) : TEXT(""), *Line));
	Part.Mesh = SkinOnly;
	return true;
}

void RegisterMantleAvatarStripFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleAvatarStripFixer>());
}
