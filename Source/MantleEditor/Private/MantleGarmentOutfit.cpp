// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGarmentOutfit.h"

#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleAvatarRefitFixer.h"
#include "MantleAvatarReposeFixer.h"
#include "MantleAvatarStripFixer.h"
#include "MantleCharacterSplit.h"
#include "MantleCosmeticExport.h"
#include "MantleGarmentClearance.h"
#include "MantleMetaHumanPose.h"
#include "MantleNailSeating.h"
#include "MantleOutfitSections.h"
#include "MantleParametricOutfitAuthoring.h"
#include "MantleSettings.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ChaosOutfitAsset/OutfitAsset.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Misc/ScopeExit.h"
#include "PreviewScene.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshAttributes.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "MeshDescription.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleGarmentOutfit, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGarmentOutfitPrivate
{
	/** Points hashed into cubic cells, for nearest-point searches that grow ring by ring. */
	struct FGarmentPointGrid
	{
		TConstArrayView<FVector3f> Points;
		double Cell = 1.5;
		TMap<FIntVector, TArray<int32>> Cells;

		FGarmentPointGrid(TConstArrayView<FVector3f> InPoints, const double InCell)
			: Points(InPoints), Cell(InCell)
		{
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				Cells.FindOrAdd(CellOf(Points[Index])).Add(Index);
			}
		}

		FIntVector CellOf(const FVector3f& Point) const
		{
			return FIntVector(FMath::FloorToInt32(Point.X / Cell), FMath::FloorToInt32(Point.Y / Cell), FMath::FloorToInt32(Point.Z / Cell));
		}

		/** The K nearest points within MaxCm, nearest first (fewer when there are not K within it). */
		void Nearest(const FVector3f& Point, const int32 K, const double MaxCm, TArray<TPair<double, int32>>& Out) const
		{
			Out.Reset();
			const FIntVector Center = CellOf(Point);
			const int32 MaxRing = FMath::CeilToInt32(MaxCm / Cell) + 1;
			for (int32 Ring = 0; Ring <= MaxRing; ++Ring)
			{
				for (int32 X = -Ring; X <= Ring; ++X)
				{
					for (int32 Y = -Ring; Y <= Ring; ++Y)
					{
						// Only the ring's shell: every Z on its sides, the top and bottom faces elsewhere.
						const bool bSide = FMath::Max(FMath::Abs(X), FMath::Abs(Y)) == Ring;
						const int32 ZStep = bSide || Ring == 0 ? 1 : 2 * Ring;
						for (int32 Z = -Ring; Z <= Ring; Z += ZStep)
						{
							const TArray<int32>* Found = Cells.Find(Center + FIntVector(X, Y, Z));
							if (!Found)
							{
								continue;
							}
							for (const int32 Index : *Found)
							{
								const double Distance = FVector3d::Dist(FVector3d(Point), FVector3d(Points[Index]));
								if (Distance <= MaxCm)
								{
									Out.Emplace(Distance, Index);
								}
							}
						}
					}
				}
				// Every point within Ring cells of the centre cell is now in: they are the nearest when the Kth is that close.
				if (Out.Num() >= K)
				{
					Out.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
					if (Out[K - 1].Key <= Ring * Cell)
					{
						Out.SetNum(K);
						return;
					}
				}
			}
			Out.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
			if (Out.Num() > K)
			{
				Out.SetNum(K);
			}
		}
	};

	/** Component-space transforms of Joints' reference pose (parents first). */
	TArray<FTransform> GarmentBindPose(const TArray<FMantleGlbJoint>& Joints)
	{
		TArray<FTransform> Out;
		Out.SetNum(Joints.Num());
		for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
		{
			const int32 Parent = Joints[Joint].Parent;
			Out[Joint] = Out.IsValidIndex(Parent) && Parent < Joint ? Joints[Joint].Local * Out[Parent] : Joints[Joint].Local;
		}
		return Out;
	}

	/**
	 * Each of Joints posed: the clip's bone of the same name (ByName), else the joint's own reference offset from its
	 * posed parent. OutMatched counts the joints the clip had.
	 */
	TArray<FTransform> GarmentPosedJoints(const TArray<FMantleGlbJoint>& Joints, const TMap<FName, FTransform>& ByName, int32& OutMatched)
	{
		OutMatched = 0;
		TArray<FTransform> Out;
		Out.SetNum(Joints.Num());
		for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
		{
			if (const FTransform* Found = ByName.Find(Joints[Joint].Name))
			{
				Out[Joint] = *Found;
				++OutMatched;
				continue;
			}
			const int32 Parent = Joints[Joint].Parent;
			Out[Joint] = Out.IsValidIndex(Parent) && Parent < Joint ? Joints[Joint].Local * Out[Parent] : Joints[Joint].Local;
		}
		return Out;
	}

	/** Every bone of Mesh posed by Clip (frame 0), component space, by name, evaluated on a component in a private scene. */
	bool GarmentClipBones(USkeletalMesh& Mesh, UAnimSequence& Clip, TMap<FName, FTransform>& Out, FString& OutError)
	{
		TArray<USkinnedAsset*> Compiling = { &Mesh };
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		FPreviewScene Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false).ShouldSimulatePhysics(false));
		USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->SetSkeletalMeshAsset(&Mesh);
		Scene.AddComponent(Component, FTransform::Identity);
		Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		Component->OverrideAnimationData(&Clip, /*bIsLooping*/ false, /*bIsPlaying*/ false, 0.0f, 1.0f);
		const TArray<FTransform>& Space = Component->GetComponentSpaceTransforms();
		const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
		for (int32 Bone = 0; Bone < Reference.GetRawBoneNum() && Bone < Space.Num(); ++Bone)
		{
			Out.Add(Reference.GetBoneName(Bone), Space[Bone]);
		}
		Scene.RemoveComponent(Component);
		if (Out.IsEmpty())
		{
			OutError = FString::Printf(TEXT("the A-pose clip gave no bones on %s"), *Mesh.GetName());
			return false;
		}
		return true;
	}

	bool GarmentSave(UObject* Asset)
	{
		UPackage* Package = Asset ? Asset->GetPackage() : nullptr;
		if (!Package)
		{
			return false;
		}
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		return UPackage::SavePackage(Package, Asset, *Filename, Args);
	}

	/** A new USkeleton asset Folder/<Name> built from Mesh's bones, set on Mesh. */
	USkeleton* GarmentGiveSkeleton(USkeletalMesh& Mesh, const FString& Folder, const FString& Name, FString& OutError)
	{
		const FString PackageName = FMantleMetaHumanSteps::UniquePackageName(Folder, Name);
		UPackage* Package = CreatePackage(*PackageName);
		USkeleton* Skeleton = Package ? NewObject<USkeleton>(Package, FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
		if (!Skeleton || !Skeleton->MergeAllBonesToBoneTree(&Mesh, false))
		{
			OutError = FString::Printf(TEXT("a skeleton could not be made for %s"), *Mesh.GetName());
			return nullptr;
		}
		Mesh.SetSkeleton(Skeleton);
		Mesh.MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Skeleton);
		if (!GarmentSave(Skeleton))
		{
			OutError = FString::Printf(TEXT("%s could not be saved"), *PackageName);
			return nullptr;
		}
		return Skeleton;
	}

	/** Mesh pushed out of Body's skin (FMantleAvatarRefit::PushOutOfBody) on its welded surface: render copies of a point move together. */
	bool GarmentPushOut(FMantleGlbMesh& Mesh, const FMantleSkinnedMeshLOD& Body, FMantleRefitPushOut& Out, FString& OutError)
	{
		FMantleSkinnedMeshLOD Surface;
		TArray<int32> PointOf;
		FMantleGarmentOutfit::WeldedSurface(Mesh, Surface, PointOf);
		if (!FMantleAvatarRefit::PushOutOfBody(Surface, Body, Out, OutError, /*bCoverSkin*/ true))
		{
			return false;
		}
		for (int32 Vertex = 0; Vertex < Mesh.Positions.Num(); ++Vertex)
		{
			Mesh.Positions[Vertex] = Surface.Positions[PointOf[Vertex]];
		}
		return true;
	}

	USkeletalMesh* GarmentLoadMesh(const FString& InPath)
	{
		FString Path = InPath.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (!Path.Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		}
		return LoadObject<USkeletalMesh>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	FString GarmentWriteJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	TArray<FString> GarmentKeys(const FString& List)
	{
		TArray<FString> Out;
		List.ParseIntoArray(Out, TEXT(","), true);
		for (FString& Key : Out)
		{
			Key.TrimStartAndEndInline();
		}
		Out.RemoveAll([](const FString& Key) { return Key.IsEmpty(); });
		return Out;
	}
}

USkeletalMesh* FMantleGarmentOutfit::SaveCopy(USkeletalMesh& Source, const FString& Folder, const FString& Name, FString& OutError,
	const TFunction<bool(FMeshDescription&, FReferenceSkeleton&, FString&)>& Edit)
{
	using namespace MantleGarmentOutfitPrivate;
	// A new mesh built from Source's LOD 0 geometry, never a duplicate: MetaHuman Creator's in-memory meshes don't duplicate.
	TArray<USkinnedAsset*> Compiling = { &Source };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	FMeshDescription Description;
	if (const FMeshDescription* Own = Source.HasMeshDescription(0) ? Source.GetMeshDescription(0) : nullptr)
	{
		Description = *Own;
	}
	else if (Source.GetImportedModel() && Source.GetImportedModel()->LODModels.IsValidIndex(0))
	{
		Source.GetImportedModel()->LODModels[0].GetMeshDescription(&Source, 0, Description);
	}
	if (Description.Vertices().Num() == 0)
	{
		OutError = FString::Printf(TEXT("%s has no geometry to copy"), *Source.GetName());
		return nullptr;
	}
	FReferenceSkeleton Reference = Source.GetRefSkeleton();
	if (Edit && !Edit(Description, Reference, OutError))
	{
		return nullptr;
	}
	// Bounds from the geometry: Creator's in-memory meshes report none, and a mesh with none is never drawn.
	FBox Bounds(ForceInit);
	for (const FVector3f& Position : Description.GetVertexPositions().GetRawArray())
	{
		Bounds += FVector(Position);
	}
	const FString PackageName = FMantleMetaHumanSteps::UniquePackageName(Folder, Name);
	const FString AssetName = FPackageName::GetShortName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	USkeletalMesh* Copy = Package ? NewObject<USkeletalMesh>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
	if (!Copy)
	{
		OutError = FString::Printf(TEXT("could not create %s"), *PackageName);
		return nullptr;
	}
	auto IsTransient = [](const UObject* Object) { return Object && (Object->HasAnyFlags(RF_Transient) || Object->GetPackage() == GetTransientPackage()); };
	Copy->PreEditChange(nullptr);
	Copy->SetRefSkeleton(Reference);
	Copy->GetRefBasesInvMatrix().Reset();
	Copy->CalculateInvRefMatrices();
	// Creator's preview materials are made in memory: a saved asset can't point at them.
	TArray<FSkeletalMaterial> Materials = Source.GetMaterials();
	for (FSkeletalMaterial& Slot : Materials)
	{
		UMaterialInterface* Material = Slot.MaterialInterface;
		while (IsTransient(Material))
		{
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
			Material = Instance ? Instance->Parent.Get() : nullptr;
		}
		Slot.MaterialInterface = Material;
	}
	Copy->SetMaterials(Materials);
	Copy->AddLODInfo();
	Copy->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel);
	Copy->CreateMeshDescription(0, MoveTemp(Description));
	Copy->CommitMeshDescription(0);
	Copy->SetImportedBounds(FBoxSphereBounds(Bounds));
	USkeleton* Skeleton = Source.GetSkeleton();
	if (Skeleton && !IsTransient(Skeleton))
	{
		Copy->SetSkeleton(Skeleton);
	}
	Copy->PostEditChange();
	TArray<USkinnedAsset*> Built = { Copy };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Built);
	FAssetRegistryModule::AssetCreated(Copy);
	if (!Copy->GetSkeleton() && !GarmentGiveSkeleton(*Copy, Folder, AssetName + TEXT("_Skeleton"), OutError))
	{
		return nullptr;
	}
	if (!GarmentSave(Copy))
	{
		OutError = FString::Printf(TEXT("%s could not be saved"), *Copy->GetPathName());
		return nullptr;
	}
	UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT saved %s from %s"), *Copy->GetPathName(), *Source.GetPathName());
	return Copy;
}

const FName FMantleGarmentOutfit::MadeFromGarmentKey(TEXT("MantleMadeFromGarment"));
const FName FMantleGarmentOutfit::MadeVersionKey(TEXT("MantleMadeFromGarmentVersion"));
// 3: the garment is carried with the skin onto the MetaHuman (CarryWithSkin), and skin coming through it between its vertices is covered (CoverSkin); 4: CoverSkin only answers skin a little through it.
const TCHAR* const FMantleGarmentOutfit::MadeVersion = TEXT("5");

const TArray<TPair<FName, FName>>& FMantleGarmentOutfit::Limbs()
{
	static const TArray<TPair<FName, FName>> Pairs = {
		{ FName(TEXT("clavicle_l")), FName(TEXT("upperarm_l")) }, { FName(TEXT("upperarm_l")), FName(TEXT("lowerarm_l")) }, { FName(TEXT("lowerarm_l")), FName(TEXT("hand_l")) },
		{ FName(TEXT("clavicle_r")), FName(TEXT("upperarm_r")) }, { FName(TEXT("upperarm_r")), FName(TEXT("lowerarm_r")) }, { FName(TEXT("lowerarm_r")), FName(TEXT("hand_r")) },
		{ FName(TEXT("thigh_l")), FName(TEXT("calf_l")) }, { FName(TEXT("calf_l")), FName(TEXT("foot_l")) },
		{ FName(TEXT("thigh_r")), FName(TEXT("calf_r")) }, { FName(TEXT("calf_r")), FName(TEXT("foot_r")) } };
	return Pairs;
}

TArray<FTransform> FMantleGarmentOutfit::ComponentPose(const FReferenceSkeleton& Reference)
{
	const TArray<FTransform>& Local = Reference.GetRawRefBonePose();
	TArray<FTransform> Out;
	Out.SetNum(Local.Num());
	for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
	{
		const int32 Parent = Reference.GetRawParentIndex(Bone);
		Out[Bone] = Parent >= 0 && Parent < Bone ? Local[Bone] * Out[Parent] : Local[Bone];
	}
	return Out;
}

const TArray<TPair<FName, FName>>& FMantleGarmentOutfit::Feet()
{
	static const TArray<TPair<FName, FName>> Pairs = { { FName(TEXT("foot_l")), FName(TEXT("ball_l")) }, { FName(TEXT("foot_r")), FName(TEXT("ball_r")) } };
	return Pairs;
}

TArray<FTransform> FMantleGarmentOutfit::TurnToTemplate(const FReferenceSkeleton& Reference, TConstArrayView<FTransform> Component,
	const FReferenceSkeleton& Template, TConstArrayView<FTransform> TemplateComponent, TArray<FString>& OutLines, const TArray<TPair<FName, FName>>& Pairs)
{
	TArray<FTransform> Out(Component.GetData(), Component.Num());
	for (const TPair<FName, FName>& Limb : Pairs)
	{
		const int32 Bone = Reference.FindRawBoneIndex(Limb.Key);
		const int32 Child = Reference.FindRawBoneIndex(Limb.Value);
		const int32 TemplateBone = Template.FindRawBoneIndex(Limb.Key);
		const int32 TemplateChild = Template.FindRawBoneIndex(Limb.Value);
		if (!Out.IsValidIndex(Bone) || !Out.IsValidIndex(Child) || !TemplateComponent.IsValidIndex(TemplateBone) || !TemplateComponent.IsValidIndex(TemplateChild))
		{
			continue;
		}
		const FVector Joint = Out[Bone].GetLocation();
		const FVector Now = (Out[Child].GetLocation() - Joint).GetSafeNormal();
		const FVector Wanted = (TemplateComponent[TemplateChild].GetLocation() - TemplateComponent[TemplateBone].GetLocation()).GetSafeNormal();
		if (Now.IsNearlyZero() || Wanted.IsNearlyZero())
		{
			continue;
		}
		const FQuat Turn = FQuat::FindBetweenNormals(Now, Wanted);
		const double Degrees = FMath::RadiansToDegrees(Turn.GetAngle());
		OutLines.Add(FString::Printf(TEXT("%s %.1f"), *Limb.Key.ToString(), Degrees));
		if (Degrees < 0.01)
		{
			continue;
		}
		for (int32 Other = 0; Other < Out.Num(); ++Other)
		{
			if (Other == Bone || Reference.BoneIsChildOf(Other, Bone))
			{
				Out[Other].SetRotation((Turn * Out[Other].GetRotation()).GetNormalized());
				Out[Other].SetLocation(Joint + Turn.RotateVector(Out[Other].GetLocation() - Joint));
			}
		}
	}
	return Out;
}

bool FMantleGarmentOutfit::PoseMesh(FMantleGlbMesh& Mesh, TConstArrayView<FTransform> Posed, const double Scale, FString& OutError)
{
	using namespace MantleGarmentOutfitPrivate;
	if (Posed.Num() != Mesh.Joints.Num() || Mesh.NumInfluences <= 0 || !FMath::IsFinite(Scale) || Scale <= 0.0)
	{
		OutError = TEXT("the pose does not match the mesh's skeleton");
		return false;
	}
	const TArray<FTransform> Bind = GarmentBindPose(Mesh.Joints);
	TArray<FMatrix> Skin;
	Skin.SetNum(Bind.Num());
	for (int32 Joint = 0; Joint < Bind.Num(); ++Joint)
	{
		Skin[Joint] = Bind[Joint].ToMatrixWithScale().Inverse() * Posed[Joint].ToMatrixWithScale();
	}
	const int32 Slots = Mesh.NumInfluences;
	for (int32 Vertex = 0; Vertex < Mesh.Positions.Num(); ++Vertex)
	{
		FVector3d Position(0.0);
		FVector3d Normal(0.0);
		FVector3d Tangent(0.0);
		double Sum = 0.0;
		const FVector3d BindPosition(Mesh.Positions[Vertex]);
		const FVector3d BindNormal = Mesh.Normals.IsValidIndex(Vertex) ? FVector3d(Mesh.Normals[Vertex]) : FVector3d::ZeroVector;
		const FVector3d BindTangent = Mesh.Tangents.IsValidIndex(Vertex) ? FVector3d(Mesh.Tangents[Vertex].X, Mesh.Tangents[Vertex].Y, Mesh.Tangents[Vertex].Z) : FVector3d::ZeroVector;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const int32 Index = Vertex * Slots + Slot;
			if (!Mesh.InfluenceWeights.IsValidIndex(Index) || !Mesh.InfluenceJoints.IsValidIndex(Index))
			{
				OutError = TEXT("the mesh's skin weights are incomplete");
				return false;
			}
			const double Weight = Mesh.InfluenceWeights[Index];
			if (Weight <= 0.0)
			{
				continue;
			}
			const int32 Joint = Mesh.InfluenceJoints[Index];
			if (!Skin.IsValidIndex(Joint))
			{
				OutError = TEXT("a skin weight names a bone the mesh does not have");
				return false;
			}
			Position += Weight * Skin[Joint].TransformPosition(BindPosition);
			Normal += Weight * Skin[Joint].TransformVector(BindNormal);
			Tangent += Weight * Skin[Joint].TransformVector(BindTangent);
			Sum += Weight;
		}
		if (Sum <= UE_SMALL_NUMBER)
		{
			Position = BindPosition;
			Normal = BindNormal;
			Tangent = BindTangent;
		}
		else
		{
			Position /= Sum;
		}
		Mesh.Positions[Vertex] = FVector3f(Position * Scale);
		if (Mesh.Normals.IsValidIndex(Vertex))
		{
			Mesh.Normals[Vertex] = FVector3f(Normal.GetSafeNormal());
		}
		if (Mesh.Tangents.IsValidIndex(Vertex))
		{
			const FVector3f Turned(Tangent.GetSafeNormal());
			Mesh.Tangents[Vertex] = FVector4f(Turned.X, Turned.Y, Turned.Z, Mesh.Tangents[Vertex].W);
		}
	}
	return true;
}

FVector3f FMantleGarmentOutfit::AlignTranslation(TConstArrayView<FVector3f> From, TConstArrayView<FVector3f> To, double& OutMeanCm)
{
	using namespace MantleGarmentOutfitPrivate;
	OutMeanCm = 0.0;
	if (From.IsEmpty() || To.IsEmpty())
	{
		return FVector3f::ZeroVector;
	}
	constexpr double PairCm = 20.0;
	const FGarmentPointGrid Grid(To, 2.0);
	const int32 Stride = FMath::Max(1, From.Num() / 8000);
	FVector3d Offset(0.0);
	TArray<TPair<double, int32>> Found;
	for (int32 Iteration = 0; Iteration < 12; ++Iteration)
	{
		FVector3d Sum(0.0);
		int32 Pairs = 0;
		for (int32 Index = 0; Index < From.Num(); Index += Stride)
		{
			const FVector3f Point = From[Index] + FVector3f(Offset);
			Grid.Nearest(Point, 1, PairCm, Found);
			if (Found.Num() > 0)
			{
				Sum += FVector3d(To[Found[0].Value]) - FVector3d(Point);
				++Pairs;
			}
		}
		if (Pairs == 0)
		{
			break;
		}
		const FVector3d Step = Sum / Pairs;
		Offset += Step;
		if (Step.Size() < 0.005)
		{
			break;
		}
	}
	double Total = 0.0;
	int32 Count = 0;
	for (int32 Index = 0; Index < From.Num(); Index += Stride)
	{
		Grid.Nearest(From[Index] + FVector3f(Offset), 1, PairCm, Found);
		if (Found.Num() > 0)
		{
			Total += Found[0].Key;
			++Count;
		}
	}
	OutMeanCm = Count > 0 ? Total / Count : PairCm;
	return FVector3f(Offset);
}

bool FMantleGarmentOutfit::TransferWeights(FMantleGlbMesh& Garment, const FMantleGlbMesh& Body, int32& OutFar, double& OutMeanCm, FString& OutError)
{
	using namespace MantleGarmentOutfitPrivate;
	OutFar = 0;
	OutMeanCm = 0.0;
	if (!Body.IsSkinned() || Body.NumInfluences <= 0 || Body.Positions.IsEmpty())
	{
		OutError = TEXT("the MetaHuman body has no skin weights to take");
		return false;
	}
	constexpr int32 MaxSlots = 8;
	const FGarmentPointGrid Grid(Body.Positions, 1.5);
	TArray<uint16> Joints;
	TArray<float> Weights;
	Joints.Reserve(Garment.Positions.Num() * MaxSlots);
	Weights.Reserve(Garment.Positions.Num() * MaxSlots);
	TArray<TPair<double, int32>> Found;
	TArray<TPair<int32, double>> Blend;
	double Total = 0.0;
	int32 UsedSlots = 1;
	for (int32 Vertex = 0; Vertex < Garment.Positions.Num(); ++Vertex)
	{
		// The search widens until it finds skin: a garment far from the body still follows its nearest bones.
		const FVector3f Point = Garment.Positions[Vertex];
		Grid.Nearest(Point, TransferNeighbours, NearCm * 3.0, Found);
		if (Found.IsEmpty())
		{
			Grid.Nearest(Point, TransferNeighbours, 200.0, Found);
		}
		if (Found.IsEmpty())
		{
			OutError = TEXT("a garment vertex is more than 2 m from the MetaHuman body");
			return false;
		}
		Total += Found[0].Key;
		OutFar += Found[0].Key > NearCm ? 1 : 0;
		Blend.Reset();
		for (const TPair<double, int32>& Near : Found)
		{
			const double Share = 1.0 / (Near.Key + 0.5);
			for (int32 Slot = 0; Slot < Body.NumInfluences; ++Slot)
			{
				const int32 Index = Near.Value * Body.NumInfluences + Slot;
				const float Weight = Body.InfluenceWeights.IsValidIndex(Index) ? Body.InfluenceWeights[Index] : 0.0f;
				if (Weight <= 0.0f)
				{
					continue;
				}
				const int32 Joint = Body.InfluenceJoints[Index];
				TPair<int32, double>* Existing = Blend.FindByPredicate([Joint](const TPair<int32, double>& Entry) { return Entry.Key == Joint; });
				if (Existing)
				{
					Existing->Value += Share * Weight;
				}
				else
				{
					Blend.Emplace(Joint, Share * Weight);
				}
			}
		}
		Blend.Sort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value > B.Value; });
		if (Blend.Num() > MaxSlots)
		{
			Blend.SetNum(MaxSlots);
		}
		double Sum = 0.0;
		for (const TPair<int32, double>& Entry : Blend)
		{
			Sum += Entry.Value;
		}
		if (Sum <= 0.0)
		{
			OutError = TEXT("the MetaHuman skin near a garment vertex has no weights");
			return false;
		}
		UsedSlots = FMath::Max(UsedSlots, Blend.Num());
		for (int32 Slot = 0; Slot < MaxSlots; ++Slot)
		{
			Joints.Add(Blend.IsValidIndex(Slot) ? static_cast<uint16>(Blend[Slot].Key) : 0);
			Weights.Add(Blend.IsValidIndex(Slot) ? static_cast<float>(Blend[Slot].Value / Sum) : 0.0f);
		}
	}
	// Cut the slots to the most any vertex uses.
	Garment.InfluenceJoints.Reset();
	Garment.InfluenceWeights.Reset();
	for (int32 Vertex = 0; Vertex < Garment.Positions.Num(); ++Vertex)
	{
		for (int32 Slot = 0; Slot < UsedSlots; ++Slot)
		{
			Garment.InfluenceJoints.Add(Joints[Vertex * MaxSlots + Slot]);
			Garment.InfluenceWeights.Add(Weights[Vertex * MaxSlots + Slot]);
		}
	}
	Garment.NumInfluences = UsedSlots;
	Garment.Joints = Body.Joints;
	OutMeanCm = Garment.Positions.IsEmpty() ? 0.0 : Total / Garment.Positions.Num();
	return true;
}

bool FMantleGarmentOutfit::FollowsRootOnly(const FMantleGlbMesh& Mesh)
{
	if (!Mesh.IsSkinned() || Mesh.NumInfluences <= 0)
	{
		return true;
	}
	for (int32 Index = 0; Index < Mesh.InfluenceJoints.Num(); ++Index)
	{
		const int32 Joint = Mesh.InfluenceJoints[Index];
		if (Mesh.InfluenceWeights.IsValidIndex(Index) && Mesh.InfluenceWeights[Index] > 0.0f && Mesh.Joints.IsValidIndex(Joint) && Mesh.Joints[Joint].Parent != INDEX_NONE)
		{
			return false;
		}
	}
	return true;
}

void FMantleGarmentOutfit::WeldedSurface(const FMantleGlbMesh& Mesh, FMantleSkinnedMeshLOD& Out, TArray<int32>& OutPointOf)
{
	Out = FMantleSkinnedMeshLOD();
	OutPointOf.Reset();
	TMap<FVector3f, int32> Welded;
	for (const FVector3f& Position : Mesh.Positions)
	{
		const int32* Found = Welded.Find(Position);
		OutPointOf.Add(Found ? *Found : Welded.Add(Position, Out.Positions.Add(Position)));
	}
	Out.Normals.Init(FVector3f::ZeroVector, Out.Positions.Num());
	for (int32 Vertex = 0; Vertex < Mesh.Positions.Num(); ++Vertex)
	{
		if (Mesh.Normals.IsValidIndex(Vertex))
		{
			Out.Normals[OutPointOf[Vertex]] += Mesh.Normals[Vertex];
		}
	}
	for (FVector3f& Normal : Out.Normals)
	{
		Normal = Normal.GetSafeNormal();
	}
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		for (const uint32 Index : Primitive.Indices)
		{
			Out.Indices.Add(static_cast<uint32>(OutPointOf[Primitive.FirstVertex + static_cast<int32>(Index)]));
		}
	}
}

bool FMantleGarmentOutfit::CarryWithSkin(TArray<FVector3f>& Garment, const FMantleSkinnedMeshLOD& From, const FMantleSkinnedMeshLOD& To, double& OutMeanMoveCm, FString& OutError)
{
	using namespace MantleGarmentOutfitPrivate;
	OutMeanMoveCm = 0.0;
	if (Garment.IsEmpty())
	{
		return true;
	}
	// Each original skin point onto its closest point of the MetaHuman's skin.
	FMantleGarmentClearance SkinOnTo;
	if (!FMantleGarmentClearance::Compute(From.Positions, To, SkinOnTo, FMantleGarmentClearance::DefaultSearchCm, &OutError))
	{
		return false;
	}
	const int32 NumPoints = From.Positions.Num();
	TArray<FVector3f> Move;
	TArray<bool> Known;
	Move.Init(FVector3f::ZeroVector, NumPoints);
	Known.Init(false, NumPoints);
	for (int32 Point = 0; Point < NumPoints; ++Point)
	{
		if (!SkinOnTo.IsTracked(Point))
		{
			continue;
		}
		const int32 Triangle = SkinOnTo.Triangle[Point];
		const FVector3f& Bary = SkinOnTo.Barycentrics[Point];
		const FVector3f Closest = To.Positions[To.Indices[Triangle * 3]] * Bary.X + To.Positions[To.Indices[Triangle * 3 + 1]] * Bary.Y
			+ To.Positions[To.Indices[Triangle * 3 + 2]] * Bary.Z;
		Move[Point] = Closest - From.Positions[Point];
		Known[Point] = true;
	}
	// Smoothed over the skin's edges: half its own move, half its neighbours' mean; a point with no move takes its known neighbours'.
	TArray<TArray<int32>> Neighbours;
	Neighbours.SetNum(NumPoints);
	for (int32 First = 0; First + 2 < From.Indices.Num(); First += 3)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const int32 A = static_cast<int32>(From.Indices[First + Corner]);
			const int32 B = static_cast<int32>(From.Indices[First + (Corner + 1) % 3]);
			Neighbours[A].AddUnique(B);
			Neighbours[B].AddUnique(A);
		}
	}
	for (int32 Pass = 0; Pass < CarrySmoothIters; ++Pass)
	{
		TArray<FVector3f> Next = Move;
		TArray<bool> NextKnown = Known;
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			FVector3f Sum = FVector3f::ZeroVector;
			int32 Count = 0;
			for (const int32 Neighbour : Neighbours[Point])
			{
				if (Known[Neighbour])
				{
					Sum += Move[Neighbour];
					++Count;
				}
			}
			if (Count == 0)
			{
				continue;
			}
			Next[Point] = Known[Point] ? 0.5f * Move[Point] + 0.5f * Sum / Count : Sum / Count;
			NextKnown[Point] = true;
		}
		Move = MoveTemp(Next);
		Known = MoveTemp(NextKnown);
	}
	// Each garment point takes the move of the skin under it.
	FMantleGarmentClearance GarmentOnFrom;
	if (!FMantleGarmentClearance::Compute(Garment, From, GarmentOnFrom, FMantleGarmentClearance::DefaultSearchCm, &OutError))
	{
		return false;
	}
	TArray<FVector3f> KnownPoints;
	TArray<int32> KnownIndex;
	for (int32 Point = 0; Point < NumPoints; ++Point)
	{
		if (Known[Point])
		{
			KnownPoints.Add(From.Positions[Point]);
			KnownIndex.Add(Point);
		}
	}
	const FGarmentPointGrid Grid(KnownPoints, 2.0);
	TArray<TPair<double, int32>> Found;
	double Total = 0.0;
	for (int32 Vertex = 0; Vertex < Garment.Num(); ++Vertex)
	{
		FVector3f Shift = FVector3f::ZeroVector;
		if (GarmentOnFrom.IsTracked(Vertex))
		{
			const int32 Triangle = GarmentOnFrom.Triangle[Vertex];
			const FVector3f& Bary = GarmentOnFrom.Barycentrics[Vertex];
			Shift = Move[From.Indices[Triangle * 3]] * Bary.X + Move[From.Indices[Triangle * 3 + 1]] * Bary.Y + Move[From.Indices[Triangle * 3 + 2]] * Bary.Z;
		}
		else
		{
			Grid.Nearest(Garment[Vertex], TransferNeighbours, 200.0, Found);
			double Weights = 0.0;
			for (const TPair<double, int32>& Near : Found)
			{
				const double Share = 1.0 / (Near.Key + 0.5);
				Shift += Move[KnownIndex[Near.Value]] * static_cast<float>(Share);
				Weights += Share;
			}
			Shift = Weights > 0.0 ? Shift / static_cast<float>(Weights) : FVector3f::ZeroVector;
		}
		Garment[Vertex] += Shift;
		Total += Shift.Size();
	}
	OutMeanMoveCm = Total / Garment.Num();
	return true;
}

FString FMantleGarmentOutfit::DescribeClearance(TConstArrayView<FVector3f> Points, const FMantleSkinnedMeshLOD& Surface)
{
	FMantleGarmentClearance Clearance;
	FString Error;
	if (!FMantleGarmentClearance::Compute(Points, Surface, Clearance, FMantleGarmentClearance::DefaultSearchCm, &Error))
	{
		return FString::Printf(TEXT("not measured: %s"), *Error);
	}
	TArray<float> Tracked;
	int32 Under = 0;
	int32 Within = 0;
	double Sum = 0.0;
	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		if (!Clearance.IsTracked(Point))
		{
			continue;
		}
		const float Cm = Clearance.ClearanceCm[Point];
		Tracked.Add(Cm);
		Under += Cm < -0.05f ? 1 : 0;
		Within += FMath::Abs(Cm) <= 1.0f ? 1 : 0;
		Sum += Cm;
	}
	if (Tracked.IsEmpty())
	{
		return FString::Printf(TEXT("tracked 0/%d"), Points.Num());
	}
	Tracked.Sort();
	const auto Percentile = [&Tracked](const int32 Percent) { return Tracked[FMath::Min(Tracked.Num() - 1, Tracked.Num() * Percent / 100)]; };
	return FString::Printf(TEXT("tracked %d/%d, under %d, mean %.2f, p10 %.2f p50 %.2f p90 %.2f cm, within 1 cm %d%%"), Tracked.Num(), Points.Num(), Under,
		Sum / Tracked.Num(), Percentile(10), Percentile(50), Percentile(90), FMath::RoundToInt32(100.0 * Within / Tracked.Num()));
}

FString FMantleGarmentOutfit::DescribeShapeChange(TConstArrayView<FVector3f> Before, const FMantleGlbMesh& Mesh)
{
	if (Before.Num() != Mesh.Positions.Num())
	{
		return TEXT("not measured: the vertex count changed");
	}
	TArray<float> Changes;
	TMap<FName, TPair<double, int32>> ByBone;
	int32 Turned = 0;
	int32 Triangles = 0;
	const auto HeaviestBone = [&Mesh](const int32 Vertex)
	{
		int32 Best = INDEX_NONE;
		float BestWeight = 0.0f;
		for (int32 Slot = 0; Slot < Mesh.NumInfluences; ++Slot)
		{
			const int32 Index = Vertex * Mesh.NumInfluences + Slot;
			if (Mesh.InfluenceWeights.IsValidIndex(Index) && Mesh.InfluenceWeights[Index] > BestWeight)
			{
				BestWeight = Mesh.InfluenceWeights[Index];
				Best = Mesh.InfluenceJoints[Index];
			}
		}
		return Mesh.Joints.IsValidIndex(Best) ? Mesh.Joints[Best].Name : NAME_None;
	};
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		for (int32 First = 0; First + 2 < Primitive.Indices.Num(); First += 3)
		{
			int32 Corners[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				Corners[Corner] = Primitive.FirstVertex + static_cast<int32>(Primitive.Indices[First + Corner]);
			}
			if (!Before.IsValidIndex(Corners[0]) || !Before.IsValidIndex(Corners[1]) || !Before.IsValidIndex(Corners[2]))
			{
				continue;
			}
			++Triangles;
			const FVector3f NormalBefore = FVector3f::CrossProduct(Before[Corners[1]] - Before[Corners[0]], Before[Corners[2]] - Before[Corners[0]]);
			const FVector3f NormalAfter = FVector3f::CrossProduct(Mesh.Positions[Corners[1]] - Mesh.Positions[Corners[0]], Mesh.Positions[Corners[2]] - Mesh.Positions[Corners[0]]);
			Turned += FVector3f::DotProduct(NormalBefore, NormalAfter) < 0.0f ? 1 : 0;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 A = Corners[Corner];
				const int32 B = Corners[(Corner + 1) % 3];
				const float Length = FVector3f::Dist(Before[A], Before[B]);
				if (Length < 1.e-3f)
				{
					continue;
				}
				const float Change = FMath::Abs(FVector3f::Dist(Mesh.Positions[A], Mesh.Positions[B]) - Length) / Length;
				Changes.Add(Change);
				TPair<double, int32>& Entry = ByBone.FindOrAdd(HeaviestBone(A));
				Entry.Key += Change;
				++Entry.Value;
			}
		}
	}
	if (Changes.IsEmpty())
	{
		return TEXT("no edges");
	}
	Changes.Sort();
	const auto Percentile = [&Changes](const int32 Percent) { return 100.0f * Changes[FMath::Min(Changes.Num() - 1, Changes.Num() * Percent / 100)]; };
	TArray<TPair<FName, double>> Means;
	for (const TPair<FName, TPair<double, int32>>& Entry : ByBone)
	{
		if (Entry.Value.Value >= 20) { Means.Emplace(Entry.Key, 100.0 * Entry.Value.Key / Entry.Value.Value); }
	}
	Means.Sort([](const TPair<FName, double>& A, const TPair<FName, double>& B) { return A.Value > B.Value; });
	TArray<FString> Lines;
	for (int32 Index = 0; Index < Means.Num() && Index < 6; ++Index)
	{
		Lines.Add(FString::Printf(TEXT("%s %.0f%%"), *Means[Index].Key.ToString(), Means[Index].Value));
	}
	return FString::Printf(TEXT("edge length change p50 %.0f%% p90 %.0f%% largest %.0f%%, %d of %d triangles turned over; by bone (mean): %s"), Percentile(50), Percentile(90),
		100.0f * Changes.Last(), Turned, Triangles, *FString::Join(Lines, TEXT(", ")));
}

bool FMantleGarmentOutfit::Make(USkeletalMesh& InBody, TConstArrayView<USkeletalMesh*> Garments, const FString& TurnOff, const FString& Confirm,
	FMantleGarmentOutfitResult& OutResult, FString& OutConvertJson, FString& OutRefusalCode, FString& OutRefusal,
	TConstArrayView<TArray<USkeletalMesh*>> Trims)
{
	using namespace MantleGarmentOutfitPrivate;
	OutResult = FMantleGarmentOutfitResult();
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract)
	{
		OutRefusalCode = TEXT("no_contract");
		OutRefusal = ContractError.IsEmpty() ? FString(TEXT("Mantle's avatar rules (limits.json) could not be read.")) : ContractError;
		return false;
	}
	const FString BodyTag = FMantleMetaHumanSteps::AssetTag(InBody.GetName());
	const FString Folder = FMantleAvatarConverter::OutputFolderFor(FPackageName::GetLongPackagePath(InBody.GetPackage()->GetName())) / (BodyTag + TEXT("_Outfit"));
	OutResult.OutputFolder = Folder;
	UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT body=%s garments=%d folder=%s"), *InBody.GetPathName(), Garments.Num(), *Folder);

	// 1. A body with bones but no USkeleton gets one, on a copy: Epic's IK rig and retargeter need a skeleton.
	USkeletalMesh* Body = &InBody;
	if (!Body->GetSkeleton())
	{
		FString Error;
		USkeletalMesh* Copy = FMantleGarmentOutfit::SaveCopy(InBody, Folder, BodyTag + TEXT("_Skinned"), Error);
		if (!Copy)
		{
			OutRefusalCode = TEXT("no_skeleton");
			OutRefusal = FString::Printf(TEXT("%s has no skeleton asset, and one could not be made for it: %s."), *InBody.GetName(), *Error);
			return false;
		}
		if (!Copy->GetSkeleton() || Copy->GetSkeleton()->HasAnyFlags(RF_Transient))
		{
			if (!GarmentGiveSkeleton(*Copy, Folder, BodyTag + TEXT("_Skeleton"), Error) || !GarmentSave(Copy))
			{
				OutRefusalCode = TEXT("no_skeleton");
				OutRefusal = FString::Printf(TEXT("%s has no skeleton asset, and one could not be made for it: %s."), *InBody.GetName(), *Error);
				return false;
			}
		}
		OutResult.Notes.Add(FString::Printf(TEXT("%s has no skeleton asset, so a copy with its own skeleton was made: %s."), *InBody.GetName(), *Copy->GetPathName()));
		Body = Copy;
	}

	// Convert's MetaHuman steps on the body, and nothing after them.
	FMantleConvertOptions Options;
	Options.Slot = TEXT("avatar");
	Options.OutputFolder = Folder;
	Options.bSave = true;
	Options.bCheck = false;
	Options.OnlyFixers = { MantleAvatarFixerIds::Strip, MantleAvatarFixerIds::MHPose, MantleAvatarFixerIds::MHConform };
	FMantleConvertContext Context;
	FString Error;
	if (!FMantleAvatarConverter::MakeContext(Body, Options, *Contract, Context, Error))
	{
		OutRefusalCode = TEXT("not_convertible");
		OutRefusal = Error;
		return false;
	}
	if (Context.Parts.Num() != 1 || Context.Parts[0].bUnrigged)
	{
		OutRefusalCode = TEXT("not_convertible");
		OutRefusal = Context.Parts.Num() == 1 ? FString::Printf(TEXT("%s has no skeleton to pose, so its clothing can't be posed the way it is."), *Body->GetName())
			: FString::Printf(TEXT("%s is not one body: pass the character's bare body mesh."), *Body->GetName());
		return false;
	}
	TArray<FMantleConvertFix> Fixes = FMantleAvatarConverter::Analyze(Context);
	const TArray<FString> Keys = FMantleAvatarConverter::FixKeys(Fixes);
	TArray<TSharedPtr<FJsonValue>> UnknownTurnOff;
	TArray<TSharedPtr<FJsonValue>> UnknownConfirm;
	for (int32 Index = 0; Index < Fixes.Num(); ++Index)
	{
		// The garment was made for this body: its face never goes on another one.
		const FName Kind = Fixes[Index].Kind;
		if (Kind == FMantleAvatarStripFixer::KindFaceOnly || Kind == FMantleAvatarStripFixer::KindFeminineBody || Kind == FMantleAvatarStripFixer::KindMasculineBody)
		{
			Fixes[Index].bEnabled = false;
		}
	}
	for (const FString& Wanted : GarmentKeys(TurnOff))
	{
		const int32 Found = Keys.IndexOfByKey(Wanted);
		if (Found == INDEX_NONE) { UnknownTurnOff.Add(MakeShared<FJsonValueString>(Wanted)); }
		else { Fixes[Found].bEnabled = false; }
	}
	// Any line may be confirmed here, the bare-skin line too when no material read as skin (the creator kept the slots).
	for (const FString& Wanted : GarmentKeys(Confirm))
	{
		const int32 Found = Keys.IndexOfByKey(Wanted);
		if (Found == INDEX_NONE) { UnknownConfirm.Add(MakeShared<FJsonValueString>(Wanted)); }
		else { Fixes[Found].bEnabled = true; }
	}
	const FMantleConvertResult Converted = FMantleAvatarConverter::Convert(Context, Fixes, Options, *Contract);
	{
		TSharedRef<FJsonObject> Json = FMantleAvatarConverter::ToJson(Context, Fixes, &Converted, *Contract);
		Json->SetArrayField(TEXT("unknownTurnOff"), UnknownTurnOff);
		Json->SetArrayField(TEXT("unknownConfirm"), UnknownConfirm);
		OutConvertJson = GarmentWriteJson(Json);
	}
	const FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(0);
	if (Converted.Failed.Num() > 0 || !Work || !Work->Character.IsValid() || !Work->APoseClip.IsValid() || !Work->SkinOnly.IsValid() || !Work->PosedMesh.IsValid())
	{
		TArray<FString> Reasons;
		for (const FMantleConvertOutcome& Failed : Converted.Failed)
		{
			Reasons.Add(FString::Printf(TEXT("%s: %s"), *Failed.Key, *Failed.Reason));
		}
		OutRefusalCode = TEXT("convert_failed");
		OutRefusal = Reasons.Num() > 0 ? FString::Printf(TEXT("The body could not be made a MetaHuman: %s."), *FString::Join(Reasons, TEXT("; ")))
			: FString(TEXT("The body could not be made a MetaHuman: Strip, the A-pose or the conform did not run (see the fix list in \"convert\")."));
		return false;
	}
	return MakeFromWork(*Work, InBody.GetName(), Folder, Garments, OutResult, OutRefusalCode, OutRefusal, /*bCutFromOwnMesh*/ false, Trims);
}

bool FMantleGarmentOutfit::MakeFromWork(const FMantleConvertMetaHumanWork& Work, const FString& BodyName, const FString& Folder,
	TConstArrayView<USkeletalMesh*> Garments, FMantleGarmentOutfitResult& OutResult, FString& OutRefusalCode, FString& OutRefusal, const bool bCutFromOwnMesh,
	TConstArrayView<TArray<USkeletalMesh*>> Trims)
{
	using namespace MantleGarmentOutfitPrivate;
	if (!Work.Character.IsValid() || !Work.APoseClip.IsValid() || !Work.SkinOnly.IsValid() || !Work.PosedMesh.IsValid())
	{
		OutRefusalCode = TEXT("no_metahuman");
		OutRefusal = FString::Printf(TEXT("Convert made no MetaHuman of %s (its A-pose or conform did not run), so there is nothing to fit its clothing on."), *BodyName);
		return false;
	}
	const FString BodyTag = FMantleMetaHumanSteps::AssetTag(BodyName);
	OutResult.OutputFolder = Folder;
	FString Error;
	OutResult.Character = Work.Character->GetPathName();
	OutResult.InputScale = Work.InputScale;

	// 2. The MetaHuman's merged body and head, as Epic's resize reads a character: the Outfit's authored body.
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(Work.Character.Get());
	UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
	if (!Character || !Subsystem)
	{
		OutRefusalCode = TEXT("no_metahuman");
		OutRefusal = TEXT("MetaHuman Creator's editor subsystem is unavailable.");
		return false;
	}
	const bool bAlreadyEditing = Subsystem->IsObjectAddedForEditing(Character);
	if (!bAlreadyEditing && !Subsystem->TryAddObjectToEdit(Character))
	{
		OutRefusalCode = TEXT("no_metahuman");
		OutRefusal = TEXT("MetaHuman Creator could not open the conformed character.");
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (!bAlreadyEditing)
		{
			Subsystem->RemoveObjectToEdit(Character);
		}
	};
	FMetaHumanCharacterPreviewAssetOptions PreviewOptions;
	PreviewOptions.bGenerateMergedHeadAndBodyMesh = true;
	PreviewOptions.bGenerateBodyMeasurements = true;
	FMetaHumanCharacterPreviewAssets Preview;
	if (!Subsystem->TryGetCharacterPreviewAssets(Character, PreviewOptions, Preview) || !Preview.MergedHeadAndBodyMesh)
	{
		OutRefusalCode = TEXT("no_metahuman");
		OutRefusal = TEXT("MetaHuman Creator could not provide the conformed character's merged body and head.");
		return false;
	}
	// The MetaHuman as conformed (its skin weights, and where the posed body lands on it), then the same turned onto
	// MetaHuman's own A-pose: the Outfit's authored body.
	USkeletalMesh* Conformed = FMantleGarmentOutfit::SaveCopy(*Preview.MergedHeadAndBodyMesh, Folder, TEXT("SKM_") + BodyTag + TEXT("_Conformed"), Error);
	USkeletalMesh* Template = Conformed ? FMantleMetaHumanPose::LoadTemplateBody(Error) : nullptr;
	if (!Conformed || !Template)
	{
		OutRefusalCode = TEXT("no_metahuman");
		OutRefusal = FString::Printf(TEXT("The MetaHuman's merged body and head could not be saved: %s."), *Error);
		return false;
	}
	const FReferenceSkeleton& ConformedReference = Conformed->GetRefSkeleton();
	const TArray<FTransform> ConformedPose = ComponentPose(ConformedReference);
	TArray<FString> TurnLines;
	const TArray<FTransform> APose = TurnToTemplate(ConformedReference, ConformedPose, Template->GetRefSkeleton(), ComponentPose(Template->GetRefSkeleton()), TurnLines);
	OutResult.APoseTurns = FString::Join(TurnLines, TEXT(", "));
	UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT A-pose turns (degrees): %s"), *OutResult.APoseTurns);
	FMantleGlbMesh MetaHumanSkin;
	TArray<UMaterialInterface*> SkinMaterials;
	int32 Triangles = 0;
	if (!FMantleCosmeticExport::ReadSkeletalMesh(Conformed, 0, MetaHumanSkin, SkinMaterials, Triangles, Error) || !MetaHumanSkin.IsSkinned())
	{
		OutRefusalCode = TEXT("no_metahuman");
		OutRefusal = FString::Printf(TEXT("The MetaHuman's merged body and head could not be read: %s."), *Error);
		return false;
	}
	// The A-pose, one transform per joint of the MetaHuman's skin (its joints are the reference skeleton's raw bones).
	TArray<FTransform> APoseJoints;
	for (const FMantleGlbJoint& Joint : MetaHumanSkin.Joints)
	{
		const int32 Bone = ConformedReference.FindRawBoneIndex(Joint.Name);
		APoseJoints.Add(APose.IsValidIndex(Bone) ? APose[Bone] : FTransform::Identity);
	}

	// The body posed by its own A-pose clip and scaled by k, as the conform's input was: the check that this pose is the
	// A-pose step's, and the offset that puts it on the MetaHuman's skin.
	TMap<FName, FTransform> ClipBones;
	if (!GarmentClipBones(*Work.SkinOnly.Get(), *Work.APoseClip.Get(), ClipBones, Error))
	{
		OutRefusalCode = TEXT("pose_failed");
		OutRefusal = Error;
		return false;
	}
	FMantleGlbMesh PosedBody;
	TArray<UMaterialInterface*> BodyMaterials;
	int32 Matched = 0;
	if (!FMantleCosmeticExport::ReadSkeletalMesh(Work.SkinOnly.Get(), 0, PosedBody, BodyMaterials, Triangles, Error)
		|| !PoseMesh(PosedBody, GarmentPosedJoints(PosedBody.Joints, ClipBones, Matched), Work.InputScale, Error))
	{
		OutRefusalCode = TEXT("pose_failed");
		OutRefusal = FString::Printf(TEXT("The body could not be posed: %s."), *Error);
		return false;
	}
	FBox3f PosedBox(ForceInit);
	for (const FVector3f& Position : PosedBody.Positions)
	{
		PosedBox += Position;
	}
	const FBox ConformBox = Work.PosedMesh->GetBoundingBox();
	const double PoseGap = FMath::Max((FVector(PosedBox.Min) - ConformBox.Min).GetAbsMax(), (FVector(PosedBox.Max) - ConformBox.Max).GetAbsMax());
	UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT posed body bounds %s vs conform input %s: %.3f cm"), *PosedBox.ToString(), *ConformBox.ToString(), PoseGap);
	if (PoseGap > PoseCheckCm)
	{
		OutRefusalCode = TEXT("pose_failed");
		OutRefusal = FString::Printf(TEXT("Posing the body again gave a shape %.1f cm from the one the MetaHuman was made from, so its clothing would not be posed the same way."), PoseGap);
		return false;
	}
	OutResult.Offset = AlignTranslation(PosedBody.Positions, MetaHumanSkin.Positions, OutResult.BodyGapCm);
	UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT k=%.5f offset=%s body gap %.3f cm"), OutResult.InputScale, *OutResult.Offset.ToString(), OutResult.BodyGapCm);
	OutResult.Notes.Add(FString::Printf(TEXT("The MetaHuman's skin is on average %.2f cm from the original body's (after the A-pose and the scale %.4f)."), OutResult.BodyGapCm, OutResult.InputScale));

	// Its feet turned to the MetaHuman's (a character in heels stands on tiptoe, the MetaHuman on flat feet): its clothing is
	// posed so. What a heel then reaches below the MetaHuman's sole is measured per piece (HeelCm).
	{
		const FReferenceSkeleton& BodyReference = Work.SkinOnly->GetRefSkeleton();
		TArray<FTransform> Clip;
		for (int32 Bone = 0; Bone < BodyReference.GetRawBoneNum(); ++Bone)
		{
			const FTransform* Found = ClipBones.Find(BodyReference.GetBoneName(Bone));
			Clip.Add(Found ? *Found : FTransform::Identity);
		}
		TArray<FString> FootLines;
		Clip = TurnToTemplate(BodyReference, Clip, ConformedReference, ConformedPose, FootLines, Feet());
		const auto Repose = [&]()
		{
			for (int32 Bone = 0; Bone < Clip.Num(); ++Bone)
			{
				if (ClipBones.Contains(BodyReference.GetBoneName(Bone)))
				{
					ClipBones[BodyReference.GetBoneName(Bone)] = Clip[Bone];
				}
			}
			PosedBody = FMantleGlbMesh();
			return FMantleCosmeticExport::ReadSkeletalMesh(Work.SkinOnly.Get(), 0, PosedBody, BodyMaterials, Triangles, Error)
				&& PoseMesh(PosedBody, GarmentPosedJoints(PosedBody.Joints, ClipBones, Matched), Work.InputScale, Error);
		};
		const bool bPosed = Repose();
		if (!bPosed)
		{
			OutRefusalCode = TEXT("pose_failed");
			OutRefusal = FString::Printf(TEXT("The body could not be posed with its feet turned to the MetaHuman's: %s."), *Error);
			return false;
		}
		OutResult.FeetTurns = FString::Join(FootLines, TEXT(", "));
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT feet turned to the MetaHuman's (degrees): %s"), *OutResult.FeetTurns);
	}

	// The original body (posed, on the MetaHuman) and the conformed MetaHuman as surfaces, for the fit log: how far the
	// original skin is outside the MetaHuman's, by the bone it follows (positive: the MetaHuman is smaller there).
	FMantleGlbMesh OriginalBody = PosedBody;
	for (FVector3f& Position : OriginalBody.Positions)
	{
		Position += OutResult.Offset;
	}
	FMantleSkinnedMeshLOD OriginalSurface, TwinSurface;
	TArray<int32> OriginalPointOf, TwinPointOf;
	WeldedSurface(OriginalBody, OriginalSurface, OriginalPointOf);
	WeldedSurface(MetaHumanSkin, TwinSurface, TwinPointOf);
	{
		FMantleGarmentClearance Gap;
		if (FMantleGarmentClearance::Compute(OriginalSurface.Positions, TwinSurface, Gap))
		{
			TMap<FName, TPair<double, int32>> ByBone;
			TBitArray<> Seen(false, OriginalSurface.Positions.Num());
			for (int32 Vertex = 0; Vertex < OriginalPointOf.Num(); ++Vertex)
			{
				const int32 Point = OriginalPointOf[Vertex];
				if (!Gap.IsTracked(Point) || Seen[Point])
				{
					continue;
				}
				Seen[Point] = true;
				int32 Best = 0;
				for (int32 Slot = 1; Slot < OriginalBody.NumInfluences; ++Slot)
				{
					Best = OriginalBody.InfluenceWeights[Vertex * OriginalBody.NumInfluences + Slot] > OriginalBody.InfluenceWeights[Vertex * OriginalBody.NumInfluences + Best] ? Slot : Best;
				}
				const int32 Joint = OriginalBody.InfluenceJoints[Vertex * OriginalBody.NumInfluences + Best];
				TPair<double, int32>& Entry = ByBone.FindOrAdd(OriginalBody.Joints.IsValidIndex(Joint) ? OriginalBody.Joints[Joint].Name : NAME_None);
				Entry.Key += Gap.ClearanceCm[Point];
				++Entry.Value;
			}
			TArray<TPair<FName, double>> Means;
			for (const TPair<FName, TPair<double, int32>>& Entry : ByBone)
			{
				if (Entry.Value.Value >= 20) { Means.Emplace(Entry.Key, Entry.Value.Key / Entry.Value.Value); }
			}
			Means.Sort([](const TPair<FName, double>& A, const TPair<FName, double>& B) { return FMath::Abs(A.Value) > FMath::Abs(B.Value); });
			TArray<FString> Lines;
			for (int32 Index = 0; Index < Means.Num() && Index < 14; ++Index)
			{
				Lines.Add(FString::Printf(TEXT("%s %+.2f"), *Means[Index].Key.ToString(), Means[Index].Value));
			}
			UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_FIT original skin over the MetaHuman: %s; by bone (cm): %s"),
				*DescribeClearance(OriginalSurface.Positions, TwinSurface), *FString::Join(Lines, TEXT(", ")));
		}
	}

	// 2. Each garment posed the way the body was, on the MetaHuman, before the body is shaped (it is shaped only under them).
	TArray<TSharedPtr<FMantleGlbMesh>> PosedGarments;
	TArray<TArray<UMaterialInterface*>> GarmentMaterials;
	TArray<FString> PoseErrors;
	TArray<FVector3f> Worn;
	for (USkeletalMesh* Garment : Garments)
	{
		TSharedPtr<FMantleGlbMesh>& Geometry = PosedGarments.Add_GetRef(MakeShared<FMantleGlbMesh>());
		TArray<UMaterialInterface*>& Materials = GarmentMaterials.AddDefaulted_GetRef();
		FString& PoseError = PoseErrors.AddDefaulted_GetRef();
		if (!Garment)
		{
			PoseError = TEXT("not a skeletal mesh");
			continue;
		}
		if (!FMantleCosmeticExport::ReadSkeletalMesh(Garment, 0, *Geometry, Materials, Triangles, PoseError))
		{
			continue;
		}
		const TArray<FTransform> Posed = GarmentPosedJoints(Geometry->Joints, ClipBones, Matched);
		if (Matched == 0)
		{
			PoseError = FString::Printf(TEXT("%s shares no bones with %s, so it can't be posed the way the body was"), *Garment->GetName(), *BodyName);
			continue;
		}
		if (!PoseMesh(*Geometry, Posed, Work.InputScale, PoseError))
		{
			continue;
		}
		// Its trims (a corset's lace), posed alike and made into its Outfit with it.
		const int32 GarmentIndex = PosedGarments.Num() - 1;
		for (USkeletalMesh* Trim : Trims.IsValidIndex(GarmentIndex) ? Trims[GarmentIndex] : TArray<USkeletalMesh*>())
		{
			FMantleGlbMesh TrimGeometry;
			TArray<UMaterialInterface*> TrimMaterials;
			FString TrimError;
			int32 TrimTriangles = 0;
			int32 TrimMatched = 0;
			if (!Trim || !FMantleCosmeticExport::ReadSkeletalMesh(Trim, 0, TrimGeometry, TrimMaterials, TrimTriangles, TrimError)
				|| !PoseMesh(TrimGeometry, GarmentPosedJoints(TrimGeometry.Joints, ClipBones, TrimMatched), Work.InputScale, TrimError) || TrimMatched == 0)
			{
				UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT trim %s left out of %s: %s"), Trim ? *Trim->GetName() : TEXT("?"), *Garment->GetName(),
					TrimError.IsEmpty() ? TEXT("it shares no bones with the body") : *TrimError);
				continue;
			}
			AppendTrim(*Geometry, TrimGeometry);
			Materials.Append(TrimMaterials);
			UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT trim %s goes into %s's Outfit"), *Trim->GetName(), *Garment->GetName());
		}
		for (FVector3f& Position : Geometry->Positions)
		{
			Position += OutResult.Offset;
		}
		Worn.Append(Geometry->Positions);
	}

	// The MetaHuman's skin under the clothing moved onto the original body's (CarryWithSkin, skin onto skin), fading out past
	// it (ShapeNearCm to ShapeFarCm from the clothing) and smoothed over its edges, as the body the Outfit is made on: the
	// clothing then stays as it was made, on the body it was made for, and the resize that fits an Outfit onto another body
	// (Epic's, smooth) takes it from that shape to theirs. Moving the clothing onto the MetaHuman's shape instead crushed it
	// where the MetaHuman is smaller (a corset's cups on a flatter bust); shaping all of the body tore its hands and arms.
	TArray<FVector3f> Wrapped = TwinSurface.Positions;
	{
		double WrapCm = 0.0;
		if (!CarryWithSkin(Wrapped, TwinSurface, OriginalSurface, WrapCm, Error))
		{
			OutRefusalCode = TEXT("no_metahuman");
			OutRefusal = FString::Printf(TEXT("The MetaHuman could not be shaped like the original body: %s."), *Error);
			return false;
		}
		const int32 NumPoints = TwinSurface.Positions.Num();
		TArray<FVector3f> Move;
		Move.SetNum(NumPoints);
		const FGarmentPointGrid WornGrid(Worn, 2.0);
		TArray<TPair<double, int32>> Near;
		int32 Shaped = 0;
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			WornGrid.Nearest(TwinSurface.Positions[Point], 1, ShapeFarCm, Near);
			const double Share = Near.IsEmpty() ? 0.0 : FMath::Clamp((ShapeFarCm - Near[0].Key) / (ShapeFarCm - ShapeNearCm), 0.0, 1.0);
			Move[Point] = (Wrapped[Point] - TwinSurface.Positions[Point]) * static_cast<float>(Share);
			Shaped += Share > 0.0 ? 1 : 0;
		}
		TArray<TArray<int32>> Neighbours;
		Neighbours.SetNum(NumPoints);
		for (int32 First = 0; First + 2 < TwinSurface.Indices.Num(); First += 3)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 A = static_cast<int32>(TwinSurface.Indices[First + Corner]);
				const int32 B = static_cast<int32>(TwinSurface.Indices[First + (Corner + 1) % 3]);
				Neighbours[A].AddUnique(B);
				Neighbours[B].AddUnique(A);
			}
		}
		for (int32 Pass = 0; Pass < ShapeSmoothIters; ++Pass)
		{
			TArray<FVector3f> Next = Move;
			for (int32 Point = 0; Point < NumPoints; ++Point)
			{
				if (Neighbours[Point].IsEmpty())
				{
					continue;
				}
				FVector3f Sum = FVector3f::ZeroVector;
				for (const int32 Neighbour : Neighbours[Point])
				{
					Sum += Move[Neighbour];
				}
				Next[Point] = 0.5f * Move[Point] + 0.5f * Sum / static_cast<float>(Neighbours[Point].Num());
			}
			Move = MoveTemp(Next);
		}
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			Wrapped[Point] = TwinSurface.Positions[Point] + Move[Point];
		}
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_FIT the MetaHuman shaped like the original body under the clothing (%d of %d points, %.2f cm on average where whole): %s"),
			Shaped, NumPoints, WrapCm, *DescribeClearance(Wrapped, OriginalSurface));
	}
	FMantleGlbMesh WrappedSkin = MetaHumanSkin;
	for (int32 Vertex = 0; Vertex < WrappedSkin.Positions.Num(); ++Vertex)
	{
		WrappedSkin.Positions[Vertex] = Wrapped[TwinPointOf[Vertex]];
	}
	UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_SHAPE the MetaHuman shaped like the original body: %s"), *DescribeShapeChange(MetaHumanSkin.Positions, WrappedSkin));
	{
		const int32 Turned = TurnedTriangles(MetaHumanSkin.Positions, WrappedSkin);
		int32 BodyTriangles = 0;
		for (const FMantleGlbPrimitive& Primitive : WrappedSkin.Primitives)
		{
			BodyTriangles += Primitive.Indices.Num() / 3;
		}
		if (bCutFromOwnMesh && !SplitsCleanly(Turned, BodyTriangles))
		{
			OutRefusalCode = TEXT("not_splittable");
			OutRefusal = FString::Printf(TEXT("%s can't be split cleanly: the skin under its clothing isn't a whole body (shaping a MetaHuman like it folds %.1f%% of it), so its clothing would come out crushed and isn't offered as items. The character itself still converts to a MetaHuman, without that clothing.%s"),
				*BodyName, BodyTriangles > 0 ? 100.0 * Turned / BodyTriangles : 0.0,
				UMantleSettings::AllowsAsIsAvatars() ? TEXT(" To keep it as it is, clothes and all, upload it with the As-is route.") : TEXT(""));
			UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_SPLIT %s: not splittable (%d of %d triangles turned over)"), *BodyName, Turned, BodyTriangles);
			return false;
		}
	}
	FMantleSkinnedMeshLOD WrappedSurface = TwinSurface;
	WrappedSurface.Positions = Wrapped;
	int32 Unmatched = 0;
	USkeletalMesh* Authoring = FMantleGarmentOutfit::SaveCopy(*Conformed, Folder, TEXT("SKM_") + BodyTag + TEXT("_OutfitBody"), Error,
		[&ConformedPose, &APose, &TwinSurface, &Wrapped, &Unmatched](FMeshDescription& Description, FReferenceSkeleton& Reference, FString& OutEditError)
		{
			// The MetaHuman's skin on the original body's shape (Wrapped, by welded point), then turned onto the A-pose.
			TMap<FVector3f, int32> PointAt;
			for (int32 Point = 0; Point < TwinSurface.Positions.Num(); ++Point)
			{
				PointAt.Add(TwinSurface.Positions[Point], Point);
			}
			const FGarmentPointGrid Grid(TwinSurface.Positions, 1.0);
			TArray<TPair<double, int32>> Found;
			FSkeletalMeshAttributes Attributes(Description);
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
			{
				const FVector3f Position = Positions.Get(Vertex);
				const int32* Point = PointAt.Find(Position);
				if (!Point)
				{
					Grid.Nearest(Position, 1, 1.0, Found);
					Unmatched += Found.IsEmpty() ? 1 : 0;
				}
				const int32 Index = Point ? *Point : (Found.Num() > 0 ? Found[0].Value : INDEX_NONE);
				if (Wrapped.IsValidIndex(Index))
				{
					Positions.Set(Vertex, Position + Wrapped[Index] - TwinSurface.Positions[Index]);
				}
			}
			TArray<FName> Names;
			TArray<FTransform> Local;
			Local.SetNum(APose.Num());
			for (int32 Bone = 0; Bone < APose.Num(); ++Bone)
			{
				Names.Add(Reference.GetBoneName(Bone));
				const int32 Parent = Reference.GetRawParentIndex(Bone);
				Local[Bone] = Parent >= 0 ? APose[Bone].GetRelativeTransform(APose[Parent]) : APose[Bone];
			}
			if (!FMantleAvatarRepose::RebindMeshDescription(Description, Names, ConformedPose, APose, Local, OutEditError))
			{
				return false;
			}
			FReferenceSkeletonModifier Modifier(Reference, nullptr);
			for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
			{
				Modifier.UpdateRefPoseTransform(Bone, Local[Bone]);
			}
			return true;
		});
	FMantleSkinnedMeshLOD AuthoringSurface;
	if (!Authoring || !FMantleNailSeating::ReadBodySurface(*Authoring, 0, AuthoringSurface, Error))
	{
		OutRefusalCode = TEXT("no_metahuman");
		OutRefusal = FString::Printf(TEXT("The MetaHuman could not be turned onto MetaHuman's A-pose: %s."), *Error);
		return false;
	}
	OutResult.AuthoringBody = Authoring->GetPathName();
	UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT Outfit body %s: %d vertices not within 1 cm of the MetaHuman's skin (left unshaped)"), *OutResult.AuthoringBody, Unmatched);

	// 3 and 4, per garment.
	for (int32 Index = 0; Index < Garments.Num(); ++Index)
	{
		USkeletalMesh* Garment = Garments[Index];
		FMantleGarmentOutfitPiece& Piece = OutResult.Pieces.AddDefaulted_GetRef();
		Piece.Garment = Garment ? Garment->GetPathName() : FString();
		if (!PoseErrors[Index].IsEmpty())
		{
			Piece.Error = PoseErrors[Index];
			continue;
		}
		const TSharedRef<FMantleGlbMesh> Geometry = PosedGarments[Index].ToSharedRef();
		const TArray<UMaterialInterface*>& Materials = GarmentMaterials[Index];
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_FIT %s over the original body: %s"), *Garment->GetName(), *DescribeClearance(Geometry->Positions, OriginalSurface));
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_FIT %s over the MetaHuman as posed: %s"), *Garment->GetName(), *DescribeClearance(Geometry->Positions, TwinSurface));
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_FIT %s over the MetaHuman shaped like the original body: %s"), *Garment->GetName(), *DescribeClearance(Geometry->Positions, WrappedSurface));
		if (!TransferWeights(*Geometry, WrappedSkin, Piece.FarVertices, Piece.MeanSkinCm, Error))
		{
			Piece.Error = Error;
			continue;
		}
		// Onto MetaHuman's A-pose with the body, then out of its skin where it sits under it.
		if (!PoseMesh(*Geometry, APoseJoints, 1.0, Error))
		{
			Piece.Error = Error;
			continue;
		}
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_FIT %s over the MetaHuman in its A-pose: %s"), *Garment->GetName(), *DescribeClearance(Geometry->Positions, AuthoringSurface));
		const TArray<FVector3f> BeforePush = Geometry->Positions;
		FMantleRefitPushOut Pushed;
		if (!GarmentPushOut(*Geometry, AuthoringSurface, Pushed, Error))
		{
			Piece.Error = Error;
			continue;
		}
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_SHAPE %s pushed out: %s"), *Garment->GetName(), *DescribeShapeChange(BeforePush, *Geometry));
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_FIT %s pushed out: %s"), *Garment->GetName(), *DescribeClearance(Geometry->Positions, AuthoringSurface));
		Piece.PushOut = Pushed.ToString();
		{
			float SoleZ = TNumericLimits<float>::Max();
			float LowestZ = TNumericLimits<float>::Max();
			for (const FVector3f& Position : AuthoringSurface.Positions) { SoleZ = FMath::Min(SoleZ, Position.Z); }
			for (const FVector3f& Position : Geometry->Positions) { LowestZ = FMath::Min(LowestZ, Position.Z); }
			Piece.HeelCm = FMath::Max(0.0, static_cast<double>(SoleZ - LowestZ));
			UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT %s reaches %.2f cm below the MetaHuman's sole"), *Garment->GetName(), Piece.HeelCm);
		}
		if (Piece.HeelCm >= HeelMinCm)
		{
			Piece.Error = FString::Printf(TEXT("%s is a high heel: made for a foot on tiptoe, it reaches %.1f cm below a flat foot, and MetaHuman bodies stand on flat feet. ")
				TEXT("Heeled shoes aren't offered until the game lifts the foot when they are worn (GhzGarage/mantle#1051)."), *Garment->GetName(), Piece.HeelCm);
			continue;
		}
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT push out %s: %s"), *Garment->GetName(), *Piece.PushOut);
		Piece.Vertices = Geometry->Positions.Num();
		FMantleOutfitAuthoredSource Source;
		Source.Id = TEXT("authored");
		Source.CombinedBodyHead = Authoring;
		Source.Geometry = Geometry;
		Source.Materials = Materials;
		const FString PackageName = FMantleMetaHumanSteps::UniquePackageName(Folder, TEXT("OA_") + FMantleMetaHumanSteps::AssetTag(Garment->GetName()));
		UPackage* Package = CreatePackage(*PackageName);
		TArray<TStrongObjectPtr<UObject>> KeepAlive;
		const FMantleOutfitAuthoredSource Sources[] = { Source };
		UChaosOutfitAsset* Outfit = Package ? FMantleParametricOutfitAuthoring::Create(Sources, Package, FName(*FPackageName::GetShortName(PackageName)), 1500, KeepAlive, Error) : nullptr;
		if (!Outfit)
		{
			Piece.Error = Error.IsEmpty() ? FString(TEXT("the Outfit could not be made")) : Error;
			continue;
		}
		FAssetRegistryModule::AssetCreated(Outfit);
		Package->GetMetaData().SetValue(Outfit, MadeFromGarmentKey, *Garment->GetPathName());
		Package->GetMetaData().SetValue(Outfit, MadeVersionKey, MadeVersion);
		if (!GarmentSave(Outfit))
		{
			Piece.Error = FString::Printf(TEXT("the Outfit was made but %s could not be saved"), *PackageName);
			continue;
		}
		Piece.Outfit = Outfit->GetPathName();
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT made %s from %s: %d vertices, %d far, skin %.2f cm away on average"),
			*Piece.Outfit, *Piece.Garment, Piece.Vertices, Piece.FarVertices, Piece.MeanSkinCm);
	}
	return true;
}

FString FMantleGarmentOutfit::GarmentSlot(const FString& MeshName, TConstArrayView<FMantleOutfitSection> Sections, FString& OutWhy)
{
	OutWhy.Reset();
	// The pieces the name's words say, word by word ("SK_Clothes_Shirt" is a top). Hair is never clothing ("Hair_Short").
	TArray<EMantleOutfitPiece> NamePieces;
	bool bNotClothing = FMantleAvatarAssemblyFixer::IsHairName(MeshName);
	for (const FString& Word : FMantleOutfitSections::Words(MeshName))
	{
		FMantleOutfitSection ByWord;
		ByWord.SlotName = Word;
		const EMantleOutfitPiece Piece = FMantleOutfitSections::Classify(ByWord);
		if (!FMantleOutfitSections::PieceSlot(Piece).IsEmpty())
		{
			NamePieces.AddUnique(Piece);
		}
		bNotClothing |= Piece == EMantleOutfitPiece::Skin || FMantleOutfitSections::IsProp(ByWord);
	}
	TArray<EMantleOutfitPiece> SectionPieces;
	bool bSkin = false;
	for (const FMantleOutfitSection& Section : Sections)
	{
		const EMantleOutfitPiece Piece = FMantleOutfitSections::Classify(Section);
		if (!FMantleOutfitSections::PieceSlot(Piece).IsEmpty())
		{
			SectionPieces.AddUnique(Piece);
		}
		bSkin |= Piece == EMantleOutfitPiece::Skin;
	}
	auto Several = [](const TArray<EMantleOutfitPiece>& Pieces)
	{
		TArray<FString> Names;
		for (const EMantleOutfitPiece Piece : Pieces)
		{
			Names.Add(FMantleOutfitSections::PieceName(Piece));
		}
		return FString::Printf(TEXT("Holds more than one piece of clothing (%s) in one mesh, so it can't be one Outfit."), *FString::Join(Names, TEXT(" and ")));
	};
	const FString Merged = TEXT("Clothing merged with the body's skin in one mesh, so it can't be an Outfit. Use the character's separate clothing pieces.");
	if (NamePieces.Num() > 1)
	{
		OutWhy = Several(NamePieces);
		return FString();
	}
	if (NamePieces.Num() == 1 && !bNotClothing)
	{
		if (bSkin)
		{
			OutWhy = Merged;
			return FString();
		}
		return FMantleOutfitSections::PieceSlot(NamePieces[0]);
	}
	if (bNotClothing || SectionPieces.Num() == 0)
	{
		return FString();
	}
	if (bSkin || SectionPieces.Num() > 1)
	{
		OutWhy = bSkin ? Merged : Several(SectionPieces);
		return FString();
	}
	return FMantleOutfitSections::PieceSlot(SectionPieces[0]);
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGarmentOutfitPrivate
{
	/** Why Mesh can't be posed the way Body is (it shares too few of its bones); empty when it can. */
	FString GarmentNotRiggedTo(const USkeletalMesh& Body, const USkeletalMesh& Mesh)
	{
		const FReferenceSkeleton& BodyBones = Body.GetRefSkeleton();
		const FReferenceSkeleton& Bones = Mesh.GetRefSkeleton();
		int32 Shared = 0;
		for (int32 Bone = 0; Bone < Bones.GetRawBoneNum(); ++Bone)
		{
			Shared += BodyBones.FindRawBoneIndex(Bones.GetBoneName(Bone)) != INDEX_NONE ? 1 : 0;
		}
		return Shared * 2 < Bones.GetRawBoneNum() ? FString::Printf(TEXT("Isn't rigged to %s's skeleton, so it can't be posed the way the body is."), *Body.GetName()) : FString();
	}

	/** The reason every piece gets when Body wears clothing (Worn), naming BareBody to convert instead; then tops, bottoms, footwear. */
	void GarmentsSettle(const USkeletalMesh& Body, const FString& Worn, const FString& BareBody, TArray<FMantleConvertGarment>& OutGarments)
	{
		// The clothing was made for a bare body, not one that wears clothing (Convert would make the MetaHuman of what Strip leaves).
		if (!Worn.IsEmpty())
		{
			const FString Why = FString::Printf(TEXT("%s wears clothing (%s), so it isn't the bare body this was made for. Convert %s instead."),
				*FMantleAvatarAssemblyFixer::DisplayName(Body.GetName()), *Worn, BareBody.IsEmpty() ? TEXT("the character's bare body") : *BareBody);
			for (FMantleConvertGarment& Garment : OutGarments)
			{
				if (Garment.Why.IsEmpty())
				{
					Garment.Why = Why;
				}
			}
		}
		// Tops, bottoms, footwear, then merged clothing; within each, the pieces that can be Outfits first.
		auto Rank = [](const FMantleConvertGarment& Garment)
		{
			return Garment.Slot == TEXT("top") ? 0 : Garment.Slot == TEXT("bottom") ? 1 : Garment.Slot == TEXT("footwear") ? 2 : 3;
		};
		OutGarments.StableSort([&Rank](const FMantleConvertGarment& A, const FMantleConvertGarment& B)
		{
			return Rank(A) != Rank(B) ? Rank(A) < Rank(B) : (A.Why.IsEmpty() && !B.Why.IsEmpty());
		});
	}
}

void FMantleGarmentOutfit::FindGarments(USkeletalMesh& Body, TArray<FMantleConvertGarment>& OutGarments, TArray<FString>& OutNotOffered,
	TArray<FString>* OutPaired)
{
	using namespace MantleGarmentOutfitPrivate;
	OutGarments.Reset();
	OutNotOffered.Reset();
	const FString Folder = FPackageName::GetLongPackagePath(Body.GetPackage()->GetName());
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(*Folder);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	const FString Worn = WornClothing(Body);
	FString BareWhy;
	const USkeletalMesh* Bare = Worn.IsEmpty() ? nullptr : FindBareBody(Body, BareWhy);
	const FString BareBody = Bare ? FMantleAvatarAssemblyFixer::DisplayName(Bare->GetName()) : FString();
	for (const FAssetData& Asset : Assets)
	{
		// Convert's output (Converted folders) is never the creator's clothing.
		if (Asset.PackageName == Body.GetPackage()->GetFName() || (Asset.PackagePath.ToString() + TEXT("/")).Contains(TEXT("/Converted/")))
		{
			continue;
		}
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
		if (!Mesh)
		{
			continue;
		}
		TArray<int32> Slots;
		const TArray<FMantleOutfitSection> Sections = FMantleCosmeticExport::ReadOutfitSections(Mesh, 0, Slots);
		// Another whole body (the character dressed, in its nightwear): its skin says it isn't a garment.
		const bool bSkin = Sections.ContainsByPredicate([](const FMantleOutfitSection& Section) { return FMantleOutfitSections::Classify(Section) == EMantleOutfitPiece::Skin; });
		if (bSkin && Mesh->GetBounds().BoxExtent.Z >= BareHeightShare * Body.GetBounds().BoxExtent.Z)
		{
			continue;
		}
		FString Why;
		const FString Slot = GarmentSlot(Mesh->GetName(), Sections, Why);
		const FString Name = FMantleAvatarAssemblyFixer::DisplayName(Mesh->GetName());
		if (Slot.IsEmpty() && Why.IsEmpty())
		{
			// Not clothing: another body or head goes unsaid (a bare one, "Nude" first, is remembered), anything else
			// (hair, gloves, a belt) is named.
			FMantleOutfitSection ByName;
			ByName.SlotName = Mesh->GetName();
			bool bAllSkin = Sections.Num() > 0;
			for (const FMantleOutfitSection& Section : Sections)
			{
				bAllSkin &= FMantleOutfitSections::Classify(Section) == EMantleOutfitPiece::Skin;
			}
			const TArray<FString> Words = FMantleOutfitSections::Words(Mesh->GetName());
			const bool bHead = Words.Contains(TEXT("head")) || Words.Contains(TEXT("face"));
			const bool bNude = Words.Contains(TEXT("nude")) || Words.Contains(TEXT("naked"));
			if (bNude || (bAllSkin && !bHead))
			{
				// Another body, or a part of one: unsaid (FindBareBody names the bare body).
			}
			else if (FMantleOutfitSections::Classify(ByName) != EMantleOutfitPiece::Skin)
			{
				OutNotOffered.Add(Name);
			}
			continue;
		}
		if (Why.IsEmpty())
		{
			Why = GarmentNotRiggedTo(Body, *Mesh);
		}
		FMantleConvertGarment& Garment = OutGarments.AddDefaulted_GetRef();
		Garment.Mesh = Mesh;
		Garment.Slot = Slot;
		Garment.Name = Name;
		Garment.Why = Why;
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT found %s slot=%s why=%s"), *Mesh->GetPathName(), *Slot, *Why);
	}
	// A thin piece lying on another of the same slot (a corset's lace edging) goes with it, as one item.
	{
		TArray<FMantleGlbMesh> Read;
		Read.SetNum(OutGarments.Num());
		TBitArray<> bRead(false, OutGarments.Num());
		const auto Geometry = [&](const int32 Index) -> const FMantleGlbMesh*
		{
			if (!bRead[Index])
			{
				bRead[Index] = true;
				TArray<UMaterialInterface*> Materials;
				int32 Triangles = 0;
				FString Error;
				if (!OutGarments[Index].Mesh.IsValid() || !FMantleCosmeticExport::ReadSkeletalMesh(OutGarments[Index].Mesh.Get(), 0, Read[Index], Materials, Triangles, Error))
				{
					Read[Index] = FMantleGlbMesh();
				}
			}
			return Read[Index].Positions.Num() > 0 ? &Read[Index] : nullptr;
		};
		TArray<int32> Paired;
		for (int32 Trim = 0; Trim < OutGarments.Num(); ++Trim)
		{
			// The piece of the same slot it lies on most closely, if it lies on one.
			const FMantleConvertGarment& A = OutGarments[Trim];
			if (A.Trims.Num() > 0)
			{
				continue;
			}
			int32 Best = INDEX_NONE;
			double BestNear = TrimNearShare;
			for (int32 Other = 0; Other < OutGarments.Num(); ++Other)
			{
				const FMantleConvertGarment& B = OutGarments[Other];
				if (Other == Trim || Paired.Contains(Other) || A.Slot.IsEmpty() || A.Slot != B.Slot || !A.Why.IsEmpty() || !B.Why.IsEmpty())
				{
					continue;
				}
				const FMantleGlbMesh* TrimMesh = Geometry(Trim);
				const FMantleGlbMesh* OtherMesh = TrimMesh ? Geometry(Other) : nullptr;
				int32 TrimTriangles = 0;
				int32 OtherTriangles = 0;
				const double Near = OtherMesh ? TrimNear(*TrimMesh, *OtherMesh, TrimNearCm, &TrimTriangles, &OtherTriangles) : 0.0;
				// The closeness at a few distances too, so the rule can be set from the numbers rather than a guess.
				const double Near1 = OtherMesh ? TrimNear(*TrimMesh, *OtherMesh, 1.0f) : 0.0;
				const double Near3 = OtherMesh ? TrimNear(*TrimMesh, *OtherMesh, 3.0f) : 0.0;
				UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT trim check %s on %s: %.0f%% within %.0f cm (%.0f%% within 1 cm, %.0f%% within 3 cm; %d of %d triangles, at most %.0f%% to be trim)"),
					*A.Name, *B.Name, 100.0 * Near, TrimNearCm, 100.0 * Near1, 100.0 * Near3, TrimTriangles, OtherTriangles, 100.0 * TrimMaxTriangleShare);
				if (Near >= BestNear)
				{
					Best = Other;
					BestNear = Near;
				}
			}
			if (Best != INDEX_NONE)
			{
				OutGarments[Best].Trims.Add(A.Mesh);
				Paired.Add(Trim);
				UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT %s is trim on %s (%.0f%% within %.0f cm): one item"), *A.Name, *OutGarments[Best].Name,
					100.0 * BestNear, TrimNearCm);
				if (OutPaired)
				{
					OutPaired->Add(FString::Printf(TEXT("%s goes with %s"), *A.Name, *OutGarments[Best].Name));
				}
			}
		}
		Paired.Sort(TGreater<int32>());
		for (const int32 Index : Paired)
		{
			OutGarments.RemoveAt(Index);
		}
	}
	GarmentsSettle(Body, Worn, BareBody, OutGarments);
}

void FMantleGarmentOutfit::GarmentsFromReview(USkeletalMesh& Body, const FMantlePackReview& Review, TArray<FMantleConvertGarment>& OutGarments)
{
	using namespace MantleGarmentOutfitPrivate;
	OutGarments.Reset();
	auto MeshOf = [&Review](const int32 Piece) -> USkeletalMesh*
	{
		const int32 Mesh = Review.Pieces.IsValidIndex(Piece) ? Review.Pieces[Piece].MeshIndex : INDEX_NONE;
		return Review.Meshes.IsValidIndex(Mesh) ? Review.Meshes[Mesh].Get() : nullptr;
	};
	for (const FMantlePackReview::FItem& Item : Review.Items())
	{
		USkeletalMesh* Mesh = MeshOf(Item.Main);
		if (!Mesh)
		{
			continue;
		}
		FMantleConvertGarment& Garment = OutGarments.AddDefaulted_GetRef();
		Garment.Mesh = Mesh;
		Garment.Slot = Item.Slot;
		TArray<FString> With;
		for (const int32 Piece : Item.With)
		{
			if (USkeletalMesh* Trim = MeshOf(Piece))
			{
				Garment.Trims.Add(Trim);
				With.Add(Review.Pieces[Piece].Label);
			}
		}
		Garment.Name = With.IsEmpty() ? Review.Pieces[Item.Main].Label : FString::Printf(TEXT("%s with %s"), *Review.Pieces[Item.Main].Label, *FString::Join(With, TEXT(" and ")));
		Garment.Why = GarmentNotRiggedTo(Body, *Mesh);
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT from the rows: %s slot=%s mesh=%s with=%s why=%s"), *Garment.Name, *Garment.Slot, *Mesh->GetPathName(),
			*FString::Join(With, TEXT(", ")), *Garment.Why);
	}
	// The best bare body of the pack that isn't Body, to name when Body wears clothing.
	FString BareBody;
	for (const FMantlePackBody& Other : Review.Bodies)
	{
		if (Other.Rank >= 0 && BareBody.IsEmpty() && Review.Meshes.IsValidIndex(Other.MeshIndex) && Review.Meshes[Other.MeshIndex].Get() != &Body)
		{
			BareBody = Other.Label;
		}
	}
	GarmentsSettle(Body, WornClothing(Body), BareBody, OutGarments);
}

void FMantleGarmentOutfit::FindSectionGarments(USkeletalMesh& Body, TArray<FMantleConvertGarment>& OutGarments, FString* OutNote)
{
	// A bare body and separate clothing in the pack come first; the clothing in Body's own sections is cut only without them.
	FString BareWhy;
	const USkeletalMesh* Bare = OutNote && !WornClothing(Body).IsEmpty() ? FindBareBody(Body, BareWhy) : nullptr;
	const bool bSeparate = Bare && OutGarments.ContainsByPredicate([](const FMantleConvertGarment& Garment) { return Garment.Sections.IsEmpty() && Garment.Mesh.IsValid(); });
	if (bSeparate)
	{
		*OutNote = FString::Printf(TEXT("The character's bare body and its clothing as separate meshes are in the pack (%s), so those are used: convert %s to make its clothing Outfits. The clothing in this mesh isn't cut out."),
			*BareWhy, *FMantleAvatarAssemblyFixer::DisplayName(Bare->GetName()));
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_SPLIT %s: source = the separate meshes (%s)"), *Body.GetName(), *BareWhy);
		return;
	}
	if (OutNote)
	{
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_SPLIT %s: source = its own sections (%s)"), *Body.GetName(),
			Bare ? TEXT("no separate clothing beside its bare body") : TEXT("no bare body beside it"));
	}
	FMantleSplitSource Source;
	if (!FMantleCharacterSplit::Gather(Body, Source))
	{
		return;
	}
	const FMantleSplitProposal Proposal = FMantleCharacterSplit::Propose({ Source });
	for (const FString& Line : Proposal.Describe(Body.GetName()))
	{
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("%s"), *Line);
	}
	if (!Proposal.bSplittable)
	{
		return;
	}
	for (const FMantleSplitPiece& Piece : Proposal.Pieces)
	{
		if (Piece.Role != EMantleSplitRole::Garment || Piece.Sections.Num() == 0)
		{
			continue;
		}
		FMantleConvertGarment& Garment = OutGarments.AddDefaulted_GetRef();
		Garment.SourceMesh = &Body;
		Garment.Slot = Piece.Slot;
		Garment.Name = Piece.Name;
		for (const int32 Section : Piece.Sections)
		{
			if (Source.Slots.IsValidIndex(Section))
			{
				Garment.Sections.AddUnique(Source.Slots[Section]);
			}
		}
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_GARMENT_OUTFIT found a piece of %s: %s slot=%s material slots=%d"), *Body.GetName(), *Piece.Name, *Piece.Slot, Garment.Sections.Num());
	}
}

int32 FMantleGarmentOutfit::MakeSectionMeshes(FMantleConvertContext& Context)
{
	int32 Made = 0;
	const FString Folder = Context.OutputFolder / TEXT("Work");
	for (FMantleConvertGarment& Garment : Context.Garments)
	{
		USkeletalMesh* Source = Garment.SourceMesh.Get();
		if (!Source || Garment.Sections.Num() == 0 || Garment.Mesh.IsValid())
		{
			continue;
		}
		FString Error;
		USkeletalMesh* Piece = FMantleAvatarStrip::WriteSlots(*Source, TSet<int32>(Garment.Sections), Folder,
			FString::Printf(TEXT("SK_%s_%s"), *FMantleMetaHumanSteps::AssetTag(Source->GetName()), *FMantleMetaHumanSteps::AssetTag(Garment.Name)), Error);
		if (Piece && Error.IsEmpty())
		{
			Garment.Mesh = Piece;
			Context.CreatedAssets.AddUnique(Piece);
			++Made;
		}
		else
		{
			Garment.Error = Error.IsEmpty() ? FString(TEXT("the piece could not be cut from the mesh")) : Error;
		}
	}
	return Made;
}

FString FMantleGarmentOutfit::WhyNoOutfits(const FMantleConvertContext& Context, const int32 PartIndex, const bool bSectionPieces)
{
	const FMantleConvertPart* Part = Context.Parts.IsValidIndex(PartIndex) ? &Context.Parts[PartIndex] : nullptr;
	const FString BodyName = Part && Part->Original.IsValid() ? Part->Original->GetName() : FString(TEXT("The body"));
	const FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(PartIndex);
	if (Work && Work->bFaceOnly)
	{
		return FString::Printf(TEXT("%s was converted as a face on MetaHuman's own body, not its own body, so its clothing wouldn't fit. Convert the character's bare body to use its clothing."), *BodyName);
	}
	if (!Work || !Work->Character.IsValid() || !Work->APoseClip.IsValid() || !Work->SkinOnly.IsValid() || !Work->PosedMesh.IsValid())
	{
		return FString::Printf(TEXT("Convert made no MetaHuman of %s, so there is nothing to fit its clothing on."), *BodyName);
	}
	// A body wearing clothing: the MetaHuman is made of what Strip left, not the bare body the clothing was made for.
	const FString Worn = !bSectionPieces && Part && Part->Original.IsValid() ? WornClothing(*Part->Original.Get()) : FString();
	if (!Worn.IsEmpty())
	{
		return FString::Printf(TEXT("%s wears clothing (%s), so it isn't the bare body this clothing was made for. Convert the character's bare body to use its clothing."),
			*BodyName, *Worn);
	}
	return FString();
}

FString FMantleGarmentOutfit::WornClothing(const USkeletalMesh& Body)
{
	TArray<int32> Slots;
	TArray<FString> Worn;
	for (const FMantleOutfitSection& Section : FMantleCosmeticExport::ReadOutfitSections(&Body, 0, Slots))
	{
		if (!FMantleOutfitSections::PieceSlot(FMantleOutfitSections::Classify(Section)).IsEmpty())
		{
			Worn.Add(Section.SlotName);
		}
	}
	return FString::Join(Worn, TEXT(", "));
}

bool FMantleGarmentOutfit::IsTrimOn(const FMantleGlbMesh& Trim, const FMantleGlbMesh& Garment)
{
	return TrimNear(Trim, Garment) >= TrimNearShare;
}

double FMantleGarmentOutfit::TrimNear(const FMantleGlbMesh& Trim, const FMantleGlbMesh& Garment, float WithinCm, int32* OutTrimTriangles, int32* OutGarmentTriangles)
{
	int32 TrimTriangles = 0;
	int32 GarmentTriangles = 0;
	for (const FMantleGlbPrimitive& Primitive : Trim.Primitives) { TrimTriangles += Primitive.Indices.Num() / 3; }
	for (const FMantleGlbPrimitive& Primitive : Garment.Primitives) { GarmentTriangles += Primitive.Indices.Num() / 3; }
	if (OutTrimTriangles) { *OutTrimTriangles = TrimTriangles; }
	if (OutGarmentTriangles) { *OutGarmentTriangles = GarmentTriangles; }
	if (TrimTriangles == 0 || GarmentTriangles == 0 || TrimTriangles > TrimMaxTriangleShare * GarmentTriangles)
	{
		return 0.0;
	}
	FMantleSkinnedMeshLOD Surface;
	TArray<int32> PointOf;
	WeldedSurface(Garment, Surface, PointOf);
	return ShareOn(Trim.Positions, Surface, WithinCm);
}

double FMantleGarmentOutfit::ShareOn(const TConstArrayView<FVector3f> Points, const FMantleSkinnedMeshLOD& Surface, const float WithinCm)
{
	FMantleGarmentClearance Near;
	if (Points.IsEmpty() || !FMantleGarmentClearance::Compute(Points, Surface, Near, FMantleGarmentClearance::DefaultSearchCm))
	{
		return 0.0;
	}
	int32 Close = 0;
	for (int32 Point = 0; Point < Points.Num(); ++Point)
	{
		Close += Near.IsTracked(Point) && FMath::Abs(Near.ClearanceCm[Point]) <= WithinCm ? 1 : 0;
	}
	return static_cast<double>(Close) / Points.Num();
}

void FMantleGarmentOutfit::AppendTrim(FMantleGlbMesh& Garment, const FMantleGlbMesh& Trim)
{
	const int32 First = Garment.Positions.Num();
	const int32 FirstMaterial = Garment.MaterialNames.Num();
	const int32 Count = Trim.Positions.Num();
	Garment.Positions.Append(Trim.Positions);
	// Per-vertex streams the garment has: the trim's, or a neutral value when it lacks one.
	const auto Fill = [Count](auto& To, const auto& From, const auto& Neutral, const int32 Before)
	{
		if (To.Num() != Before)
		{
			return;
		}
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			To.Add(From.IsValidIndex(Vertex) ? From[Vertex] : Neutral);
		}
	};
	Fill(Garment.Normals, Trim.Normals, FVector3f(0, 0, 1), First);
	Fill(Garment.Tangents, Trim.Tangents, FVector4f(1, 0, 0, 1), First);
	Fill(Garment.UVs, Trim.UVs, FVector2f::ZeroVector, First);
	Fill(Garment.UV1s, Trim.UV1s, FVector2f::ZeroVector, First);
	Fill(Garment.Colors, Trim.Colors, FColor::White, First);
	if (Garment.NumInfluences > 0)
	{
		Garment.InfluenceJoints.AddZeroed(Count * Garment.NumInfluences);
		Garment.InfluenceWeights.AddZeroed(Count * Garment.NumInfluences);
	}
	Garment.MaterialNames.Append(Trim.MaterialNames);
	for (const FMantleGlbPrimitive& Primitive : Trim.Primitives)
	{
		FMantleGlbPrimitive& Added = Garment.Primitives.Add_GetRef(Primitive);
		Added.FirstVertex = First + Primitive.FirstVertex;
		Added.MaterialIndex = FirstMaterial + Primitive.MaterialIndex;
	}
}

bool FMantleGarmentOutfit::SplitsCleanly(const int32 Turned, const int32 Triangles)
{
	return Triangles > 0 && Turned <= MaxFoldShare * Triangles;
}

int32 FMantleGarmentOutfit::TurnedTriangles(TConstArrayView<FVector3f> Before, const FMantleGlbMesh& Mesh)
{
	int32 Turned = 0;
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		for (int32 First = 0; First + 2 < Primitive.Indices.Num(); First += 3)
		{
			const int32 A = Primitive.FirstVertex + static_cast<int32>(Primitive.Indices[First]);
			const int32 B = Primitive.FirstVertex + static_cast<int32>(Primitive.Indices[First + 1]);
			const int32 C = Primitive.FirstVertex + static_cast<int32>(Primitive.Indices[First + 2]);
			if (!Before.IsValidIndex(A) || !Before.IsValidIndex(B) || !Before.IsValidIndex(C) || !Mesh.Positions.IsValidIndex(FMath::Max3(A, B, C)))
			{
				continue;
			}
			const FVector3f Was = FVector3f::CrossProduct(Before[B] - Before[A], Before[C] - Before[A]);
			const FVector3f Now = FVector3f::CrossProduct(Mesh.Positions[B] - Mesh.Positions[A], Mesh.Positions[C] - Mesh.Positions[A]);
			Turned += FVector3f::DotProduct(Was, Now) < 0.0f ? 1 : 0;
		}
	}
	return Turned;
}

bool FMantleGarmentOutfit::IsUnderwearName(const FString& Name)
{
	const TArray<FString> Words = FMantleOutfitSections::Words(Name);
	for (const TCHAR* Word : { TEXT("underwear"), TEXT("lingerie"), TEXT("bra"), TEXT("panties"), TEXT("briefs"), TEXT("knickers"), TEXT("thong"), TEXT("bikini") })
	{
		if (Words.Contains(Word))
		{
			return true;
		}
	}
	return false;
}

int32 FMantleGarmentOutfit::BareBodyRank(const FString& MeshName, const int32 CoveredSlots, const double HeightShare)
{
	if (HeightShare < BareHeightShare)
	{
		return -1;
	}
	const TArray<FString> Words = FMantleOutfitSections::Words(MeshName);
	const bool bNude = Words.Contains(TEXT("nude")) || Words.Contains(TEXT("naked")) || Words.Contains(TEXT("bare"));
	if (CoveredSlots == 0)
	{
		return 100 + (bNude ? 1 : 0);
	}
	// Something on it that isn't skin: still the bare body when its name says it is (nude, or underwear Convert takes off).
	return bNude || IsUnderwearName(MeshName) ? 10 - FMath::Min(CoveredSlots, 9) : -1;
}

USkeletalMesh* FMantleGarmentOutfit::FindBareBody(const USkeletalMesh& Body, FString& OutWhy)
{
	OutWhy.Reset();
	const FString Folder = FPackageName::GetLongPackagePath(Body.GetPackage()->GetName());
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(*Folder);
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
	const double Height = 2.0 * Body.GetBounds().BoxExtent.Z;
	const FReferenceSkeleton& BodyBones = Body.GetRefSkeleton();
	USkeletalMesh* Best = nullptr;
	int32 BestRank = -1;
	int32 BestWorn = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.PackageName == Body.GetPackage()->GetFName() || (Asset.PackagePath.ToString() + TEXT("/")).Contains(TEXT("/Converted/")))
		{
			continue;
		}
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
		if (!Mesh || Height <= UE_SMALL_NUMBER)
		{
			continue;
		}
		const FReferenceSkeleton& Bones = Mesh->GetRefSkeleton();
		int32 Shared = 0;
		for (int32 Bone = 0; Bone < Bones.GetRawBoneNum(); ++Bone)
		{
			Shared += BodyBones.FindRawBoneIndex(Bones.GetBoneName(Bone)) != INDEX_NONE ? 1 : 0;
		}
		if (Shared * 2 < Bones.GetRawBoneNum())
		{
			continue;
		}
		// Strip's own skin check, section by section: skin, or a face detail (eyes, mouth, lashes, hair), is the body;
		// anything else covers it (clothing, underwear, armour).
		TArray<FString> Covering;
		TArray<int32> SlotIndices;
		for (const FMantleOutfitSection& Section : FMantleCosmeticExport::ReadOutfitSections(Mesh, 0, SlotIndices))
		{
			if (!FMantleAvatarStrip::LooksLikeSkin(Section.SlotName, Section.MaterialName, Section.BaseMaterialName)
				&& !FMantleAvatarStrip::NamesFaceDetail(Section.SlotName, Section.MaterialName))
			{
				Covering.Add(Section.SlotName);
			}
		}
		const int32 Rank = BareBodyRank(Mesh->GetName(), Covering.Num(), 2.0 * Mesh->GetBounds().BoxExtent.Z / Height);
		UE_LOG(LogMantleGarmentOutfit, Display, TEXT("MANTLE_SPLIT bare body candidate %s: rank %d (covered by: %s)"), *Mesh->GetName(), Rank,
			Covering.IsEmpty() ? TEXT("nothing") : *FString::Join(Covering, TEXT(", ")));
		if (Rank > BestRank)
		{
			Best = Mesh;
			BestRank = Rank;
			BestWorn = Covering.Num();
		}
	}
	if (Best)
	{
		OutWhy = BestWorn == 0 ? FString::Printf(TEXT("%s is %s's whole body, every part of it skin"), *Best->GetName(), *Body.GetName())
			: FString::Printf(TEXT("%s is %s's whole body with %d piece%s on it that Convert takes off"), *Best->GetName(), *Body.GetName(), BestWorn, BestWorn == 1 ? TEXT("") : TEXT("s"));
	}
	return Best;
}

FString FMantleGarmentOutfit::RunForAgent(const FString& BodyPath, const FString& GarmentPaths, const FString& TurnOff, const FString& Confirm)
{
	using namespace MantleGarmentOutfitPrivate;
	auto Refuse = [](const FString& Code, const FString& Message, const FString& ConvertJson = FString())
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("ok"), false);
		Object->SetStringField(TEXT("refused"), Code);
		Object->SetStringField(TEXT("message"), Message);
		TSharedPtr<FJsonObject> Convert;
		if (!ConvertJson.IsEmpty() && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ConvertJson), Convert) && Convert.IsValid())
		{
			Object->SetObjectField(TEXT("convert"), Convert);
		}
		return GarmentWriteJson(Object);
	};
	USkeletalMesh* Body = GarmentLoadMesh(BodyPath);
	if (!Body)
	{
		return Refuse(TEXT("not_found"), FString::Printf(TEXT("%s is not a skeletal mesh."), *BodyPath));
	}
	TArray<USkeletalMesh*> Garments;
	for (const FString& Path : GarmentKeys(GarmentPaths))
	{
		USkeletalMesh* Garment = GarmentLoadMesh(Path);
		if (!Garment)
		{
			return Refuse(TEXT("not_found"), FString::Printf(TEXT("%s is not a skeletal mesh."), *Path));
		}
		Garments.Add(Garment);
	}
	if (Garments.IsEmpty())
	{
		return Refuse(TEXT("no_garment"), TEXT("Pass at least one garment mesh."));
	}
	FMantleGarmentOutfitResult Result;
	FString ConvertJson;
	FString Code;
	FString Message;
	if (!Make(*Body, Garments, TurnOff, Confirm, Result, ConvertJson, Code, Message))
	{
		return Refuse(Code, Message, ConvertJson);
	}
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), Result.Pieces.ContainsByPredicate([](const FMantleGarmentOutfitPiece& Piece) { return !Piece.Outfit.IsEmpty(); }));
	Object->SetStringField(TEXT("body"), Body->GetPathName());
	Object->SetStringField(TEXT("outputFolder"), Result.OutputFolder);
	Object->SetStringField(TEXT("character"), Result.Character);
	Object->SetStringField(TEXT("authoringBody"), Result.AuthoringBody);
	Object->SetNumberField(TEXT("inputScale"), Result.InputScale);
	TArray<TSharedPtr<FJsonValue>> Offset;
	Offset.Add(MakeShared<FJsonValueNumber>(Result.Offset.X));
	Offset.Add(MakeShared<FJsonValueNumber>(Result.Offset.Y));
	Offset.Add(MakeShared<FJsonValueNumber>(Result.Offset.Z));
	Object->SetArrayField(TEXT("offsetCm"), Offset);
	Object->SetNumberField(TEXT("bodyGapCm"), Result.BodyGapCm);
	Object->SetStringField(TEXT("aPoseTurns"), Result.APoseTurns);
	TArray<TSharedPtr<FJsonValue>> Outfits;
	for (const FMantleGarmentOutfitPiece& Piece : Result.Pieces)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("garment"), Piece.Garment);
		Row->SetStringField(TEXT("outfit"), Piece.Outfit);
		Row->SetStringField(TEXT("error"), Piece.Error);
		Row->SetNumberField(TEXT("vertices"), Piece.Vertices);
		Row->SetNumberField(TEXT("farVertices"), Piece.FarVertices);
		Row->SetNumberField(TEXT("meanSkinCm"), Piece.MeanSkinCm);
		Row->SetStringField(TEXT("pushOut"), Piece.PushOut);
		Outfits.Add(MakeShared<FJsonValueObject>(Row));
	}
	Object->SetArrayField(TEXT("outfits"), Outfits);
	TArray<TSharedPtr<FJsonValue>> Notes;
	for (const FString& Note : Result.Notes)
	{
		Notes.Add(MakeShared<FJsonValueString>(Note));
	}
	Object->SetArrayField(TEXT("notes"), Notes);
	TSharedPtr<FJsonObject> Convert;
	if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ConvertJson), Convert) && Convert.IsValid())
	{
		Object->SetObjectField(TEXT("convert"), Convert);
	}
	return GarmentWriteJson(Object);
}
