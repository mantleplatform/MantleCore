// Copyright Joshua Eger. All Rights Reserved.
#include "MantleUnriggedBody.h"
#include "MantleAvatarHeight.h"
#include "MantleAvatarRigMapFixer.h"
#include "MantleMetaHumanPose.h"
#include "MantleRuntime.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/CreateNewAssetUtilityFunctions.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Selections/MeshConnectedComponents.h"
#include "SkinnedAssetCompiler.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUnriggedBodyPrivate
{
	const TCHAR* const UnriggedAvatarSlot = TEXT("avatar");

	/** Every vertex of a dynamic mesh. */
	TArray<FVector> UnriggedPoints(const UDynamicMesh& Mesh)
	{
		TArray<FVector> Out;
		Mesh.ProcessMesh([&Out](const UE::Geometry::FDynamicMesh3& Read)
		{
			Out.Reserve(Read.VertexCount());
			for (const int32 Vertex : Read.VertexIndicesItr())
			{
				Out.Add(Read.GetVertex(Vertex));
			}
		});
		return Out;
	}

	/** Bounds of a dynamic mesh's vertices. */
	FBox3d UnriggedBounds(const UDynamicMesh& Mesh)
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
}

FString FMantleArmBend::ToString() const
{
	if (!bValid)
	{
		return TEXT("no arms found");
	}
	return FString::Printf(TEXT("%s: arms at %.2f of the height, span %.2f of the height either side"), bTPose ? TEXT("T-pose") : TEXT("not a T-pose"),
		ArmShare(), Height > 0.0 ? MaxX / Height : 0.0);
}

FMantleUnriggedWeld FMantleUnriggedBody::WeldVertices(UDynamicMesh& Mesh, const double Tolerance)
{
	FMantleUnriggedWeld Out;
	Mesh.EditMesh([&Out, Tolerance](UE::Geometry::FDynamicMesh3& Edit)
	{
		Out.VerticesBefore = Edit.VertexCount();
		// Matching open edges joined pairwise: in a soup every edge of the surface is two open edges, so this merges the soup's
		// copies of each vertex. Only unambiguous pairs, so stacked or doubled faces stay as they are instead of making an edge
		// with three triangles. The UV, normal and material layers keep their own splits.
		UE::Geometry::FMergeCoincidentMeshEdges Merge(&Edit);
		Merge.MergeVertexTolerance = Tolerance;
		Merge.OnlyUniquePairs = true;
		Merge.Apply();
		Out.VerticesAfter = Edit.VertexCount();
		Out.Triangles = Edit.TriangleCount();

		// What is still loose: triangles sharing no edge with another.
		UE::Geometry::FMeshConnectedComponents Components(&Edit);
		Components.FindConnectedTriangles();
		TSet<int32> LooseVertices;
		for (int32 Index = 0; Index < Components.Num(); ++Index)
		{
			const TArray<int32>& ComponentTriangles = Components.GetComponent(Index).Indices;
			if (ComponentTriangles.Num() != 1)
			{
				continue;
			}
			++Out.LooseTriangles;
			const UE::Geometry::FIndex3i Triangle = Edit.GetTriangle(ComponentTriangles[0]);
			LooseVertices.Add(Triangle.A);
			LooseVertices.Add(Triangle.B);
			LooseVertices.Add(Triangle.C);
		}
		Out.LooseVertices = LooseVertices.Num();
	});
	Out.bLoosePieces = Out.LooseShare() > MaxLooseVertexShare;
	return Out;
}

int32 FMantleUnriggedBody::RemoveUnusedVertices(UDynamicMesh& Mesh)
{
	int32 Removed = 0;
	Mesh.EditMesh([&Removed](UE::Geometry::FDynamicMesh3& Edit)
	{
		Removed = Edit.RemoveUnusedVertices();
	});
	return Removed;
}

FString FMantleUnriggedBody::LoosePiecesMessage()
{
	return TEXT("This model's surface is made of loose pieces, so it can't be fitted. Re-export it with its vertices merged.");
}

double FMantleUnriggedBody::SmoothStep(const double T)
{
	const double Clamped = FMath::Clamp(T, 0.0, 1.0);
	return Clamped * Clamped * (3.0 - 2.0 * Clamped);
}

FMantleArmBend FMantleUnriggedBody::EstimateArmBend(const TConstArrayView<FVector> Points)
{
	FMantleArmBend Out;
	if (Points.Num() == 0)
	{
		return Out;
	}
	// The widest |x|, the median height of the points past TipShare of it, the shoulder pivot a tenth of
	// the height from the middle.
	double MaxX = 0.0;
	double Top = -TNumericLimits<double>::Max();
	double Bottom = TNumericLimits<double>::Max();
	for (const FVector& Point : Points)
	{
		MaxX = FMath::Max(MaxX, FMath::Abs(Point.X));
		Top = FMath::Max(Top, Point.Z);
		Bottom = FMath::Min(Bottom, Point.Z);
	}
	if (!(MaxX > 0.0) || !(Top > Bottom))
	{
		return Out;
	}
	TArray<double> TipHeights;
	for (const FVector& Point : Points)
	{
		if (FMath::Abs(Point.X) > TipShare * MaxX)
		{
			TipHeights.Add(Point.Z);
		}
	}
	TipHeights.Sort();
	Out.bValid = TipHeights.Num() > 0;
	if (!Out.bValid)
	{
		return Out;
	}
	Out.MaxX = MaxX;
	Out.Top = Top;
	Out.Bottom = Bottom;
	Out.Height = Top - Bottom;
	Out.ArmZ = TipHeights[TipHeights.Num() / 2];
	Out.PivotX = PivotShare * Out.Height;
	Out.Unit = Out.Height / ReferenceHeightCm;
	const double ArmShare = Out.ArmShare();
	Out.bTPose = MaxX >= TPoseMinSpan * Out.Height && ArmShare >= TPoseTipMin && ArmShare <= TPoseTipMax;
	return Out;
}

double FMantleUnriggedBody::BendWeight(const FMantleArmBend& Bend, const FVector& P)
{
	if (!Bend.bValid)
	{
		return 0.0;
	}
	// Across the shoulder, times up from the armpit; the head, neck and upper chest sit inside PivotX -
	// FadeInnerCm and are never touched, nothing below the armpit is.
	const double Unit = Bend.Unit;
	const double AbsX = FMath::Abs(P.X);
	const double WeightX = SmoothStep((AbsX - (Bend.PivotX - FadeInnerCm * Unit)) / (2.0 * FadeInnerCm * Unit));
	const double WeightZ = SmoothStep((P.Z - (Bend.ArmZ - ArmpitDropCm * Unit)) / (ArmpitFadeCm * Unit));
	return WeightX * WeightZ;
}

FVector FMantleUnriggedBody::BendPoint(const FMantleArmBend& Bend, const FVector& P, const double Degrees)
{
	const double Weight = BendWeight(Bend, P);
	if (Weight <= 0.0)
	{
		return P;
	}
	// A rotation in the XZ plane about (side * PivotX, ArmZ), down for a point on the arm.
	const double Angle = FMath::DegreesToRadians(Degrees) * Weight;
	const double Side = P.X >= 0.0 ? 1.0 : -1.0;
	const double Dx = FMath::Abs(P.X) - Bend.PivotX;
	const double Dz = P.Z - Bend.ArmZ;
	const double NewDx = Dx * FMath::Cos(Angle) + Dz * FMath::Sin(Angle);
	const double NewDz = -Dx * FMath::Sin(Angle) + Dz * FMath::Cos(Angle);
	return FVector(Side * (Bend.PivotX + NewDx), P.Y, Bend.ArmZ + NewDz);
}

int32 FMantleUnriggedBody::BendArms(UDynamicMesh& Mesh, const FMantleArmBend& Bend, const double Degrees)
{
	int32 Moved = 0;
	Mesh.EditMesh([&Bend, Degrees, &Moved](UE::Geometry::FDynamicMesh3& Edit)
	{
		for (const int32 Vertex : Edit.VertexIndicesItr())
		{
			const FVector From = Edit.GetVertex(Vertex);
			const FVector To = BendPoint(Bend, From, Degrees);
			if (To != From)
			{
				Edit.SetVertex(Vertex, To);
				++Moved;
			}
		}
	});
	return Moved;
}

bool FMantleUnriggedBody::IsUnriggedBody(const FMantleConvertContext& Context, const int32 PartIndex)
{
	if (Context.Route != EMantleAvatarRoute::Convert || !Context.AssemblyFolder.IsEmpty() || !Context.Parts.IsValidIndex(PartIndex))
	{
		return false;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	return Part.bUnrigged && Part.bIncluded && Part.Slot.Equals(MantleUnriggedBodyPrivate::UnriggedAvatarSlot, ESearchCase::CaseSensitive)
		&& (Part.StaticSource.IsValid() || Part.Original.IsValid()) && !Context.IsBuiltMetaHuman(PartIndex);
}

FString FMantleUnriggedBody::PlainMessage()
{
	return TEXT("This model has no skeleton, so we'll fit it to a MetaHuman body.");
}

FString FMantleUnriggedBody::NoUsableWeightsMessage()
{
	return TEXT("Skeleton found, but no usable weights: we'll fit it to a MetaHuman body.");
}

FString FMantleSkinWeightRead::ToString() const
{
	return FString::Printf(TEXT("%d vertices, %d bones weighted, %d dominant"), Vertices, WeightedBones, DominantBones);
}

int32 FMantleUnriggedBody::MinWeightedBones()
{
	return FMantleAvatarRigMap::MustMapBones().Num();
}

FMantleSkinWeightRead FMantleUnriggedBody::ReadSkinWeights(const TConstArrayView<FSkelMeshSection> Sections, const int32 NumBones)
{
	FMantleSkinWeightRead Out;
	if (NumBones <= 0)
	{
		return Out;
	}
	TBitArray<> Weighted(false, NumBones);
	TBitArray<> Dominant(false, NumBones);
	for (const FSkelMeshSection& Section : Sections)
	{
		for (const FSoftSkinVertex& Vertex : Section.SoftVertices)
		{
			++Out.Vertices;
			int32 Best = INDEX_NONE;
			int32 BestWeight = 0;
			for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
			{
				const int32 Weight = Vertex.InfluenceWeights[Influence];
				if (Weight == 0)
				{
					continue;
				}
				const int32 Local = Vertex.InfluenceBones[Influence];
				const int32 Bone = Section.BoneMap.IsValidIndex(Local) ? static_cast<int32>(Section.BoneMap[Local]) : INDEX_NONE;
				if (Bone < 0 || Bone >= NumBones)
				{
					continue;
				}
				Weighted[Bone] = true;
				if (Weight > BestWeight)
				{
					Best = Bone;
					BestWeight = Weight;
				}
			}
			if (Best != INDEX_NONE)
			{
				Dominant[Best] = true;
			}
		}
	}
	Out.WeightedBones = Weighted.CountSetBits();
	Out.DominantBones = Dominant.CountSetBits();
	return Out;
}

FMantleSkinWeightRead FMantleUnriggedBody::ReadSkinWeights(const USkeletalMesh& Mesh)
{
	const FSkeletalMeshModel* Imported = Mesh.GetImportedModel();
	if (!Imported || Imported->LODModels.Num() == 0)
	{
		return FMantleSkinWeightRead();
	}
	return ReadSkinWeights(Imported->LODModels[0].Sections, Mesh.GetRefSkeleton().GetRawBoneNum());
}

bool FMantleUnriggedBody::HasNoUsableWeights(const FMantleSkinWeightRead& Read, const int32 MinWeights)
{
	return Read.Vertices > 0 && (Read.DominantBones <= 1 || Read.WeightedBones < MinWeights);
}

bool FMantleUnriggedBody::MakePosedMesh(const FMantleConvertPart& Part, const FString& Folder, const FString& Name, const double DefaultHeightCm,
	FMantleUnriggedBodyResult& OutResult, FString& OutError)
{
	using namespace MantleUnriggedBodyPrivate;
	OutResult = FMantleUnriggedBodyResult();
	UStaticMesh* StaticSource = Part.StaticSource.Get();
	USkeletalMesh* SkeletalSource = StaticSource ? nullptr : Part.Original.Get();
	if (!StaticSource && !SkeletalSource)
	{
		OutError = TEXT("the model to fit is gone");
		return false;
	}

	// LOD 0's source geometry, read with Geometry Script's default options; material IDs are the source's
	// material slot numbers, so each slot below gets the source's material in the same place.
	UDynamicMesh* Copy = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> CopyHeld(Copy);
	FGeometryScriptCopyMeshFromAssetOptions CopyOptions;
	FGeometryScriptMeshReadLOD Read;
	Read.LODType = EGeometryScriptLODType::SourceModel;
	Read.LODIndex = 0;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	TArray<UMaterialInterface*> SourceMaterials;
	FString SourceName;
	if (StaticSource)
	{
		SourceName = StaticSource->GetName();
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(StaticSource, Copy, CopyOptions, Read, Outcome, /*bUseSectionMaterials*/ false);
		for (const FStaticMaterial& Material : StaticSource->GetStaticMaterials())
		{
			SourceMaterials.Add(Material.MaterialInterface.Get());
		}
	}
	else
	{
		SourceName = SkeletalSource->GetName();
		TArray<USkinnedAsset*> Compiling = { SkeletalSource };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromSkeletalMesh(SkeletalSource, Copy, CopyOptions, Read, Outcome);
		for (const FSkeletalMaterial& Material : SkeletalSource->GetMaterials())
		{
			SourceMaterials.Add(Material.MaterialInterface.Get());
		}
	}
	if (Outcome != EGeometryScriptOutcomePins::Success || Copy->GetTriangleCount() == 0)
	{
		OutError = FString::Printf(TEXT("%s's geometry could not be read"), *SourceName);
		return false;
	}

	// #165: vertices no triangle uses go first. Epic's conform reads every vertex of the posed mesh's description, its render
	// only the ones triangles draw, so one left over fails the conform's input check (ElfArden: 10 cm off); they would also
	// count in the height, the scale and the arm estimate below.
	const int32 UnusedVertices = RemoveUnusedVertices(*Copy);

	// Coincident vertices are welded before anything reads the shape (a triangle soup tears in the conform), by position alone,
	// WeldToleranceCm apart at the default height (the model's own units scaled by the height it is about to be scaled to).
	// Still mostly loose after that: refused, nothing made.
	const FBox3d RawBounds = UnriggedBounds(*Copy);
	const double RawHeight = RawBounds.IsValid ? RawBounds.Max.Z - RawBounds.Min.Z : 0.0;
	const double WeldTolerance = RawHeight > 0.0 && DefaultHeightCm > 0.0 ? WeldToleranceCm * RawHeight / DefaultHeightCm : WeldToleranceCm;
	OutResult.Weld = WeldVertices(*Copy, WeldTolerance);
	OutResult.Weld.UnusedRemoved = UnusedVertices;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_UNRIGGED weld: %d -> %d vertices (%s, %d triangles, tolerance %g; %d unused vertices removed first; %d vertices in %d loose triangles, %.2f%% of the vertices, the limit %.0f%%)"),
		OutResult.Weld.VerticesBefore, OutResult.Weld.VerticesAfter, *SourceName, OutResult.Weld.Triangles, WeldTolerance, OutResult.Weld.UnusedRemoved,
		OutResult.Weld.LooseVertices, OutResult.Weld.LooseTriangles, 100.0 * OutResult.Weld.LooseShare(), 100.0 * MaxLooseVertexShare);
	if (OutResult.Weld.bLoosePieces)
	{
		OutError = LoosePiecesMessage();
		return false;
	}

	// The arms: bent down 45 degrees about the shoulder when they stick out sideways (a T-pose), else left as they are.
	OutResult.Bend = EstimateArmBend(UnriggedPoints(*Copy));
	if (OutResult.Bend.bTPose)
	{
		OutResult.BentVertices = BendArms(*Copy, OutResult.Bend, BendDegrees);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_UNRIGGED %s: %s; pivot %.2f cm, arm height %.2f, widest %.2f; %s (%d vertices moved)"), *SourceName,
		*OutResult.Bend.ToString(), OutResult.Bend.PivotX, OutResult.Bend.ArmZ, OutResult.Bend.MaxX,
		OutResult.Bend.bTPose ? TEXT("arms bent 45 degrees down") : TEXT("arms left as they are"), OutResult.BentVertices);

	// The default height: one uniform scale about the origin, k = target / (h + 0.5), h the height after the bend.
	const FBox3d Bounds = UnriggedBounds(*Copy);
	OutResult.HeightCm = Bounds.IsValid ? Bounds.Max.Z - Bounds.Min.Z : 0.0;
	if (!(OutResult.HeightCm > 0.0))
	{
		OutError = FString::Printf(TEXT("%s has no height (its vertices are all at one level)"), *SourceName);
		return false;
	}
	OutResult.InputScale = FMantleAvatarHeight::ConformInputScale(OutResult.HeightCm, DefaultHeightCm);
	UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(Copy, FVector(OutResult.InputScale), FVector::ZeroVector, /*bFixOrientationForNegativeScale*/ true);
	const FBox3d ScaledBounds = UnriggedBounds(*Copy);
	OutResult.ScaledHeightCm = ScaledBounds.IsValid ? ScaledBounds.Max.Z - ScaledBounds.Min.Z : 0.0;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_UNRIGGED default height %s: target %.1f cm, model %.3f cm, scale %.5f, scaled %.3f cm"), *SourceName,
		DefaultHeightCm, OutResult.HeightCm, OutResult.InputScale, OutResult.ScaledHeightCm);

	FGeometryScriptCreateNewStaticMeshAssetOptions MeshOptions;
	MeshOptions.bEnableRecomputeNormals = false;
	MeshOptions.bEnableRecomputeTangents = false;
	MeshOptions.bEnableNanite = false;
	MeshOptions.bEnableCollision = false;
	const FString PackageName = FMantleMetaHumanSteps::UniquePackageName(Folder, FString::Printf(TEXT("SM_%s_MHUnrigged"), *FMantleMetaHumanSteps::AssetTag(Name)));
	UStaticMesh* Posed = UGeometryScriptLibrary_CreateNewAssetFunctions::CreateNewStaticMeshAssetFromMesh(Copy, PackageName, MeshOptions, Outcome);
	if (Posed)
	{
		OutResult.Created.Add(Posed);
	}
	if (Outcome != EGeometryScriptOutcomePins::Success || !Posed)
	{
		OutError = TEXT("the posed static mesh could not be made");
		return false;
	}
	// Each slot the source's material in the same place.
	TArray<FStaticMaterial> Materials = Posed->GetStaticMaterials();
	for (int32 Slot = 0; Slot < Materials.Num(); ++Slot)
	{
		UMaterialInterface* Material = SourceMaterials.IsValidIndex(Slot) ? SourceMaterials[Slot] : nullptr;
		Materials[Slot].MaterialInterface = Material;
		Materials[Slot].MaterialSlotName = Material ? FName(*Material->GetName()) : FName(*FString::Printf(TEXT("Slot_%d"), Slot));
	}
	Posed->SetStaticMaterials(Materials);
	// k on the mesh, as the A-pose step writes it (the conform's head-joint hint would be scaled by it; here there is none).
	FMantleAvatarHeight::WriteInputScale(*Posed, OutResult.InputScale);
	Posed->MarkPackageDirty();
	OutResult.PosedMesh = Posed;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_UNRIGGED posed mesh %s: %d triangles"), *Posed->GetPathName(), Copy->GetTriangleCount());
	return true;
}

void FMantleMetaHumanUnriggedFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		if (!FMantleUnriggedBody::IsUnriggedBody(Context, PartIndex))
		{
			continue;
		}
		// #50: a model that keeps only its face has no body to fit: its head is cut and conformed alone.
		if (const FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(PartIndex); Work && Work->bFaceOnly)
		{
			continue;
		}
		FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Problem = TEXT("it has no skeleton to pose into MetaHuman's A-pose");
		Fix.Action = FMantleUnriggedBody::PlainMessage()
			+ TEXT(" If its arms stick out sideways, they are bent down into an A-pose first. It is scaled to the default height and saved as a mesh in the Work folder.");
	}
}

bool FMantleMetaHumanUnriggedFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !FMantleUnriggedBody::IsUnriggedBody(Context, Fix.PartIndex))
	{
		OutError = TEXT("there is no model without a skeleton to fit");
		return false;
	}
	FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
	// Nothing of Epic's, MetaHuman Creator's or the source's may change; the step writes only into <output>/Work.
	const FString WorkFolder = Context.OutputFolder / TEXT("Work");
	const TArray<FString> Roots = FMantleMetaHumanSteps::ProtectedRoots(Context, Fix.PartIndex);
	const TSet<FString> DirtyBefore = FMantleMetaHumanSteps::DirtyUnder(Roots, WorkFolder);
	FMantleUnriggedBodyResult Result;
	bool bMade = FMantleUnriggedBody::MakePosedMesh(Part, WorkFolder, Part.Name, FMantleAvatarHeight::TargetCm(), Result, OutError);
	// Checked whether the step finished or not: one that stopped half-way may have changed something too.
	FString Dirtied;
	if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, WorkFolder, DirtyBefore, TEXT("The fitting step"), Dirtied))
	{
		OutError = bMade ? Dirtied : FString::Printf(TEXT("%s. %s"), *OutError, *Dirtied);
		bMade = false;
	}
	if (!bMade)
	{
		// The fixer contract: a failed Apply leaves nothing behind and the context as it was.
		FMantleMetaHumanSteps::Discard(Result.Created);
		return false;
	}
	for (const TWeakObjectPtr<UObject>& Created : Result.Created)
	{
		Context.CreatedAssets.AddUnique(Created);
	}
	// The conform's input, as the A-pose step leaves it: no skin-only body, rig or clip, and no head joint (the portrait takes the
	// head to start NoHeadDropCm under the top).
	FMantleConvertMetaHumanWork& Work = Context.MetaHuman.FindOrAdd(Fix.PartIndex);
	Work.PosedMesh = Result.PosedMesh;
	Work.InputScale = Result.InputScale;
	const UStaticMesh* Posed = Result.PosedMesh.Get();
	const FString ArmsText = Result.Bend.bTPose
		? FString::Printf(TEXT("arms bent 45 degrees down (%d vertices moved)"), Result.BentVertices)
		: FString(TEXT("arms left as they were (not in a T-pose)"));
	const FString WeldText = Result.Weld.VerticesAfter < Result.Weld.VerticesBefore
		? FString::Printf(TEXT(", its loose vertices merged (%d to %d)"), Result.Weld.VerticesBefore, Result.Weld.VerticesAfter)
		: FString();
	Context.Notes.Add(FString::Printf(TEXT("%s: %s Fitted as %s, %s%s, scaled by %.4f from %.1f cm toward the default height."), *Part.Name,
		*FMantleUnriggedBody::PlainMessage(), Posed ? *Posed->GetName() : TEXT("?"), *ArmsText, *WeldText, Result.InputScale, Result.HeightCm));
	return true;
}

void RegisterMantleMetaHumanUnriggedFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleMetaHumanUnriggedFixer>());
}
