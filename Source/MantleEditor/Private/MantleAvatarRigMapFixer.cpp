// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarRigMapFixer.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleCosmeticValidation.h"
#include "MantleGlbWriter.h"

#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "BoneWeights.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkeletalMeshAttributes.h"
#include "SkinnedAssetCompiler.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleRigMap, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarRigMapPrivate
{
	using UE::AnimationCore::FBoneWeight;

	/** The contract skeleton a rig no profile knows is suggested onto: the three-spine layout most rigs have. */
	const TCHAR* const SuggestedTarget = TEXT("ue4mannequin");

	/** The slot an avatar body part has (limits.json slots[].id with itemClass avatar_body). */
	const TCHAR* const AvatarSlot = TEXT("avatar");

	/** A mesh taller or shorter than this (cm, after the scale) is not a person at 1 unit = 1 cm. */
	constexpr double MinPlausibleHeightCm = 50.0;
	constexpr double MaxPlausibleHeightCm = 300.0;
	constexpr double TypicalHeightCm = 175.0;

	/** Offsets below these are left alone: the upload tolerates 10 cm at the feet, and a centred pelvis is not required. */
	constexpr double GroundToleranceCm = 1.0;
	constexpr double CentreToleranceCm = 1.0;

	TMap<FString, FMantleRigMapping>& SessionMappings()
	{
		static TMap<FString, FMantleRigMapping> Mappings;
		return Mappings;
	}

	FString SessionKey(const USkeletalMesh* Mesh)
	{
		return Mesh ? Mesh->GetPathName() : FString();
	}

	/** Lower-case tokens of a bone name: split at anything not a letter or digit, at a lower-to-upper change and at a letter-digit change. */
	TArray<FString> RigNameTokens(const FString& Name)
	{
		TArray<FString> Out;
		FString Current;
		TCHAR Previous = 0;
		for (const TCHAR Char : Name)
		{
			if (!FChar::IsAlnum(Char))
			{
				if (!Current.IsEmpty())
				{
					Out.Add(Current.ToLower());
					Current.Reset();
				}
				Previous = 0;
				continue;
			}
			const bool bBreak = Previous != 0
				&& ((FChar::IsLower(Previous) && FChar::IsUpper(Char)) || (FChar::IsDigit(Previous) != FChar::IsDigit(Char)));
			if (bBreak && !Current.IsEmpty())
			{
				Out.Add(Current.ToLower());
				Current.Reset();
			}
			Current.AppendChar(Char);
			Previous = Char;
		}
		if (!Current.IsEmpty())
		{
			Out.Add(Current.ToLower());
		}
		return Out;
	}

	bool RigHasToken(const TArray<FString>& Tokens, std::initializer_list<const TCHAR*> Words)
	{
		for (const FString& Token : Tokens)
		{
			for (const TCHAR* Word : Words)
			{
				if (Token.Equals(Word, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
		}
		return false;
	}

	enum class ERigSide : uint8
	{
		None,
		Left,
		Right,
	};

	ERigSide RigSideOf(const TArray<FString>& Tokens)
	{
		if (RigHasToken(Tokens, { TEXT("l"), TEXT("left"), TEXT("lft") }))
		{
			return ERigSide::Left;
		}
		if (RigHasToken(Tokens, { TEXT("r"), TEXT("right"), TEXT("rt"), TEXT("rgt") }))
		{
			return ERigSide::Right;
		}
		return ERigSide::None;
	}

	/** Helpers, controls and leaf markers: never a body bone of their own. */
	bool RigIsHelperName(const TArray<FString>& Tokens)
	{
		return RigHasToken(Tokens, { TEXT("ik"), TEXT("ctrl"), TEXT("ctl"), TEXT("control"), TEXT("target"), TEXT("pole"), TEXT("end"), TEXT("nub"),
			TEXT("tip"), TEXT("top"), TEXT("twist"), TEXT("roll"), TEXT("helper"), TEXT("socket"), TEXT("attach"), TEXT("prop"), TEXT("weapon"),
			TEXT("grip"), TEXT("share"), TEXT("corrective"), TEXT("offset") });
	}

	bool RigIsFingerName(const TArray<FString>& Tokens)
	{
		return RigHasToken(Tokens, { TEXT("thumb"), TEXT("index"), TEXT("pointer"), TEXT("middle"), TEXT("mid"), TEXT("ring"), TEXT("pinky"),
			TEXT("pinkie"), TEXT("little"), TEXT("finger"), TEXT("carpal"), TEXT("metacarpal"), TEXT("palm") });
	}

	TArray<FTransform> RigComponentPoses(TConstArrayView<FMantleRigBone> Bones)
	{
		TArray<FTransform> Out;
		Out.SetNum(Bones.Num());
		for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
		{
			const int32 Parent = Bones[Bone].Parent;
			Out[Bone] = Parent >= 0 && Parent < Bone ? Bones[Bone].Local * Out[Parent] : Bones[Bone].Local;
		}
		return Out;
	}

	/** Parents (a parent at or after its child counts as none) and depths of a bone table. */
	void RigTree(TConstArrayView<FMantleRigBone> Bones, TArray<int32>& OutParent, TArray<int32>& OutDepth)
	{
		OutParent.SetNum(Bones.Num());
		OutDepth.SetNum(Bones.Num());
		for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
		{
			const int32 Parent = Bones[Bone].Parent;
			OutParent[Bone] = Parent >= 0 && Parent < Bone ? Parent : INDEX_NONE;
			OutDepth[Bone] = OutParent[Bone] == INDEX_NONE ? 0 : OutDepth[OutParent[Bone]] + 1;
		}
	}

	bool RigIsAncestor(const TArray<int32>& Parent, const int32 Ancestor, const int32 Bone)
	{
		if (Ancestor == INDEX_NONE || Bone == INDEX_NONE)
		{
			return false;
		}
		for (int32 Step = Parent[Bone]; Step != INDEX_NONE; Step = Parent[Step])
		{
			if (Step == Ancestor)
			{
				return true;
			}
		}
		return false;
	}

	int32 RigCommonAncestor(const TArray<int32>& Parent, const TArray<int32>& Depth, int32 A, int32 B)
	{
		if (A == INDEX_NONE || B == INDEX_NONE)
		{
			return INDEX_NONE;
		}
		while (Depth[A] > Depth[B])
		{
			A = Parent[A];
		}
		while (Depth[B] > Depth[A])
		{
			B = Parent[B];
		}
		while (A != B)
		{
			A = Parent[A];
			B = Parent[B];
			if (A == INDEX_NONE || B == INDEX_NONE)
			{
				return INDEX_NONE;
			}
		}
		return A;
	}

	/** The bones strictly between Top and Bottom, top first; empty when Top is not above Bottom. */
	TArray<int32> RigPathBetween(const TArray<int32>& Parent, const int32 Top, const int32 Bottom)
	{
		TArray<int32> Path;
		if (!RigIsAncestor(Parent, Top, Bottom))
		{
			return Path;
		}
		for (int32 Step = Parent[Bottom]; Step != Top; Step = Parent[Step])
		{
			Path.Insert(Step, 0);
		}
		return Path;
	}

	TArray<FString> RigRequiredWithoutRoot(const FMantleAvatarContract& Contract, const FString& TargetId)
	{
		TArray<FString> Out;
		const FMantleContractSkeleton* Target = Contract.FindSkeleton(TargetId);
		const TArray<FString>* Required = Contract.AvatarRequiredBones.Find(TargetId);
		if (!Required)
		{
			return Out;
		}
		for (const FString& Bone : *Required)
		{
			if (!Target || FName(*Bone) != Target->Root())
			{
				Out.Add(Bone);
			}
		}
		return Out;
	}

	bool RigIsFixtureDescendant(const FMantleContractSkeleton& Skeleton, const FName Bone, const FName Of)
	{
		for (FName Step = Skeleton.ParentOf(Bone); !Step.IsNone(); Step = Skeleton.ParentOf(Step))
		{
			if (Step == Of)
			{
				return true;
			}
		}
		return false;
	}

	/** The signed unit axis V points along most, SkipAxis (0, 1, 2) not considered. */
	FVector RigSnapAxis(const FVector& V, const int32 SkipAxis, int32& OutAxis)
	{
		OutAxis = INDEX_NONE;
		double Largest = -1.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Axis != SkipAxis && FMath::Abs(V[Axis]) > Largest)
			{
				Largest = FMath::Abs(V[Axis]);
				OutAxis = Axis;
			}
		}
		FVector Out = FVector::ZeroVector;
		Out[OutAxis] = V[OutAxis] >= 0.0 ? 1.0 : -1.0;
		return Out;
	}

	/**
	 * The turn, a multiple of 90 degrees about the axes, that takes a rig whose up is Up and whose left is Left to the
	 * skeleton fixtures' axes: left +X, forward +Y, up +Z. Forward is Up x Left, so the turn is never a mirror.
	 */
	bool RigAxisTurn(const FVector& Up, const FVector& Left, FQuat& OutTurn)
	{
		if (Up.SizeSquared() < UE_KINDA_SMALL_NUMBER || Left.SizeSquared() < UE_KINDA_SMALL_NUMBER)
		{
			return false;
		}
		int32 UpAxis = INDEX_NONE;
		int32 LeftAxis = INDEX_NONE;
		const FVector SnappedUp = RigSnapAxis(Up, INDEX_NONE, UpAxis);
		const FVector SnappedLeft = RigSnapAxis(Left, UpAxis, LeftAxis);
		if (FMath::Abs(Left[LeftAxis]) < 0.25 * Left.Size())
		{
			return false;
		}
		const FVector SnappedForward = FVector::CrossProduct(SnappedUp, SnappedLeft);
		// The matrix takes X, Y, Z to the rig's left, forward and up; its inverse is the turn.
		const FMatrix RigAxes(SnappedLeft, SnappedForward, SnappedUp, FVector::ZeroVector);
		OutTurn = FQuat(RigAxes).Inverse();
		OutTurn.Normalize();
		return true;
	}

	/** Height (cm after Factor) to Factor: 1 for a person-sized mesh, else the power of ten that makes it one. */
	double RigChooseScale(const double Height)
	{
		if (Height >= MinPlausibleHeightCm && Height <= MaxPlausibleHeightCm)
		{
			return 1.0;
		}
		const double Factors[] = { 100.0, 0.01, 10.0, 0.1, 1000.0, 0.001 };
		double Best = 1.0;
		double BestDistance = TNumericLimits<double>::Max();
		for (const double Factor : Factors)
		{
			const double Scaled = Height * Factor;
			if (Scaled >= MinPlausibleHeightCm && Scaled <= MaxPlausibleHeightCm && FMath::Abs(Scaled - TypicalHeightCm) < BestDistance)
			{
				Best = Factor;
				BestDistance = FMath::Abs(Scaled - TypicalHeightCm);
			}
		}
		return Best;
	}

	FTransform RigCorrected(const FTransform& Component, const FTransform& Correction)
	{
		FQuat Rotation = Correction.GetRotation() * Component.GetRotation();
		Rotation.Normalize();
		return FTransform(Rotation, Correction.TransformPosition(Component.GetTranslation()), FVector::OneVector);
	}

	/** An extra bone's name: its own without a namespace, "src_" before it when a supported skeleton has a bone of that name. */
	FName RigExtraName(const FName Original, const TSet<FName>& Reserved, const TSet<FName>& Used, bool& bOutCollided)
	{
		FString Base = FMantleRigProfiles::StripNamespace(Original.ToString());
		if (Base.IsEmpty())
		{
			Base = TEXT("bone");
		}
		FName Candidate(*Base);
		bOutCollided = Reserved.Contains(Candidate);
		if (bOutCollided)
		{
			Candidate = FName(*(TEXT("src_") + Base));
		}
		for (int32 Suffix = 2; Used.Contains(Candidate) || Reserved.Contains(Candidate); ++Suffix)
		{
			Candidate = FName(*FString::Printf(TEXT("src_%s_%d"), *Base, Suffix));
		}
		return Candidate;
	}

	FString RigListNames(TConstArrayView<FName> Names, const int32 Listed)
	{
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Names.Num() && Index < Listed; ++Index)
		{
			Parts.Add(Names[Index].ToString());
		}
		FString Out = FString::Join(Parts, TEXT(", "));
		if (Names.Num() > Listed)
		{
			Out += FString::Printf(TEXT(" and %d more"), Names.Num() - Listed);
		}
		return Out;
	}

	/** The source geometry of one LOD: its mesh description, or one made from its imported model; false for a generated LOD. */
	bool RigReadSourceGeometry(const USkeletalMesh* Source, const int32 LODIndex, FMeshDescription& Out)
	{
		if (Source->HasMeshDescription(LODIndex) && Source->CloneMeshDescription(LODIndex, Out))
		{
			return Out.Vertices().Num() > 0;
		}
		const FSkeletalMeshLODInfo* Info = Source->GetLODInfo(LODIndex);
		const FSkeletalMeshModel* Model = Source->GetImportedModel();
		if (!Info || Info->bHasBeenSimplified || !Model || !Model->LODModels.IsValidIndex(LODIndex) || Model->LODModels[LODIndex].NumVertices == 0)
		{
			return false;
		}
		Model->LODModels[LODIndex].GetMeshDescription(Source, LODIndex, Out);
		return Out.Vertices().Num() > 0;
	}

	/** One LOD's source geometry onto the plan's bones: skin weights (every profile), the bone table, the correction. */
	void RigRemapMeshDescription(FMeshDescription& Description, const FMantleRigPlan& Plan)
	{
		FSkeletalMeshAttributes Attributes(Description);

		TArray<FBoneWeight> Remapped;
		for (const FName Profile : Attributes.GetSkinWeightProfileNames())
		{
			FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights(Profile);
			if (!Weights.IsValid())
			{
				continue;
			}
			for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
			{
				Remapped.Reset();
				for (const FBoneWeight Weight : Weights.Get(Vertex))
				{
					const int32 Source = Weight.GetBoneIndex();
					const int32 Bone = Plan.WeightsOfSource.IsValidIndex(Source) ? Plan.WeightsOfSource[Source] : 0;
					FBoneWeight* Existing = Remapped.FindByPredicate([Bone](const FBoneWeight& Entry) { return Entry.GetBoneIndex() == Bone; });
					if (Existing)
					{
						const uint32 Sum = static_cast<uint32>(Existing->GetRawWeight()) + Weight.GetRawWeight();
						*Existing = FBoneWeight(static_cast<FBoneIndexType>(Bone), static_cast<uint16>(FMath::Min<uint32>(Sum, FBoneWeight::GetMaxRawWeight())));
					}
					else
					{
						Remapped.Add(FBoneWeight(static_cast<FBoneIndexType>(Bone), Weight.GetRawWeight()));
					}
				}
				// Sorted heaviest first and normalised by the attribute.
				Weights.Set(Vertex, TArrayView<const FBoneWeight>(Remapped));
			}
		}

		// The description's own bone table, when it carries one, is the new reference skeleton exactly.
		if (Attributes.HasBones() && Attributes.HasBoneNameAttribute() && Attributes.HasBoneParentIndexAttribute() && Attributes.HasBonePoseAttribute())
		{
			TArray<FBoneID> Existing;
			for (const FBoneID Bone : Attributes.Bones().GetElementIDs())
			{
				Existing.Add(Bone);
			}
			for (const FBoneID Bone : Existing)
			{
				Attributes.DeleteBone(Bone);
			}
			Attributes.ReserveNewBones(Plan.Bones.Num());
			FSkeletalMeshAttributes::FBoneNameAttributesRef Names = Attributes.GetBoneNames();
			FSkeletalMeshAttributes::FBoneParentIndexAttributesRef Parents = Attributes.GetBoneParentIndices();
			FSkeletalMeshAttributes::FBonePoseAttributesRef Poses = Attributes.GetBonePoses();
			for (int32 Index = 0; Index < Plan.Bones.Num(); ++Index)
			{
				const FBoneID Bone(Index);
				Attributes.CreateBone(Bone);
				Names.Set(Bone, Plan.Bones[Index].Name);
				Parents.Set(Bone, Plan.Bones[Index].Parent);
				Poses.Set(Bone, Plan.Bones[Index].Local);
			}
		}

		if (Plan.Correction.Equals(FTransform::Identity, 1.e-6))
		{
			return;
		}
		const FQuat Turn = Plan.Correction.GetRotation();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
		{
			Positions[Vertex] = FVector3f(Plan.Correction.TransformPosition(FVector(Positions[Vertex])));
		}
		if (Plan.bTurned)
		{
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
			for (const FVertexInstanceID Instance : Description.VertexInstances().GetElementIDs())
			{
				if (Normals.IsValid())
				{
					Normals[Instance] = FVector3f(Turn.RotateVector(FVector(Normals[Instance])));
				}
				if (Tangents.IsValid())
				{
					Tangents[Instance] = FVector3f(Turn.RotateVector(FVector(Tangents[Instance])));
				}
			}
		}
		// Morph targets are deltas in the same space: turned and scaled, never moved.
		for (const FName Morph : Attributes.GetMorphTargetNames())
		{
			TVertexAttributesRef<FVector3f> Deltas = Attributes.GetVertexMorphPositionDelta(Morph);
			if (Deltas.IsValid())
			{
				for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
				{
					Deltas[Vertex] = FVector3f(Turn.RotateVector(FVector(Deltas[Vertex])) * Plan.Scale);
				}
			}
			if (!Plan.bTurned)
			{
				continue;
			}
			TVertexInstanceAttributesRef<FVector3f> NormalDeltas = Attributes.GetVertexInstanceMorphNormalDelta(Morph);
			if (NormalDeltas.IsValid())
			{
				for (const FVertexInstanceID Instance : Description.VertexInstances().GetElementIDs())
				{
					NormalDeltas[Instance] = FVector3f(Turn.RotateVector(FVector(NormalDeltas[Instance])));
				}
			}
		}
	}

	/**
	 * A socket on a bone of the source mesh onto the plan's bones: kept where it is in the converted mesh's space, on its
	 * bone's new name, or on the bone that took a dropped bone's weights. False for a socket on no bone of the mesh.
	 */
	bool RigMoveSocket(USkeletalMeshSocket* Socket, const FReferenceSkeleton& SourceReference, const FMantleRigPlan& Plan)
	{
		const int32 Source = Socket ? SourceReference.FindRawBoneIndex(Socket->BoneName) : INDEX_NONE;
		if (!Plan.WeightsOfSource.IsValidIndex(Source) || !Plan.SourceComponent.IsValidIndex(Source))
		{
			return false;
		}
		const int32 Bone = Plan.NewIndexOfSource[Source] != INDEX_NONE ? Plan.NewIndexOfSource[Source] : Plan.WeightsOfSource[Source];
		if (!Plan.Bones.IsValidIndex(Bone))
		{
			return false;
		}
		const FTransform& OldBone = Plan.SourceComponent[Source];
		const FVector Position = Plan.Correction.TransformPosition(OldBone.TransformPosition(Socket->RelativeLocation));
		const FQuat Rotation = Plan.Correction.GetRotation() * OldBone.GetRotation() * Socket->RelativeRotation.Quaternion();
		const FTransform& NewBone = Plan.Bones[Bone].Component;
		Socket->BoneName = Plan.Bones[Bone].Name;
		Socket->RelativeLocation = NewBone.InverseTransformPosition(Position);
		Socket->RelativeRotation = (NewBone.GetRotation().Inverse() * Rotation).GetNormalized().Rotator();
		return true;
	}

	void RigRenameBoneReferences(TArray<FBoneReference>& References, const FReferenceSkeleton& SourceReference, const FMantleRigPlan& Plan)
	{
		for (int32 Index = References.Num() - 1; Index >= 0; --Index)
		{
			const int32 Source = SourceReference.FindRawBoneIndex(References[Index].BoneName);
			const int32 Bone = Plan.NewIndexOfSource.IsValidIndex(Source) ? Plan.NewIndexOfSource[Source] : INDEX_NONE;
			if (Plan.Bones.IsValidIndex(Bone))
			{
				References[Index].BoneName = Plan.Bones[Bone].Name;
			}
			else
			{
				References.RemoveAt(Index);
			}
		}
	}

	/** What Analyze and Apply decide for one part. */
	struct FRigPartDecision
	{
		/** The part is an avatar body on no supported skeleton: the fixer has a line for it. */
		bool bApplies = false;
		/** A clothing piece on the UE4 Mannequin skeleton, mapped onto the MetaHuman body skeleton. */
		bool bGarment = false;
		bool bKnown = false;
		bool bReady = false;
		FString Error;
		TArray<FMantleRigBone> Bones;
		FMantleRigMapping Mapping;
		FMantleRigPlan Plan;
	};

	FRigPartDecision RigDecide(const FMantleConvertPart& Part, const FMantleAvatarContract& Contract)
	{
		FRigPartDecision Out;
		const USkeletalMesh* Mesh = Part.Mesh.Get();
		if (!Mesh)
		{
			return Out;
		}
		// Clothing on the UE4 Mannequin skeleton: mapped onto the MetaHuman body skeleton, for the Refit fixer to carry it
		// onto the body. Every other garment is the Refit fixer's alone.
		if (!Part.Slot.Equals(AvatarSlot, ESearchCase::CaseSensitive))
		{
			EMantleCosmeticClass Class = EMantleCosmeticClass::Avatar;
			if (!FMantleAvatarConverter::ClassOfSlot(Part.Slot, Contract, Class) || Class != EMantleCosmeticClass::Garment)
			{
				return Out;
			}
			Out.Bones = FMantleAvatarRigMap::ReadBones(Mesh);
			if (!FMantleAvatarRigMap::IsUE4MannequinGarment(Out.Bones, Contract))
			{
				return Out;
			}
			Out.bApplies = true;
			Out.bGarment = true;
			Out.bKnown = true;
			Out.Mapping = FMantleAvatarRigMap::UE4MannequinOntoMetaHuman(Out.Bones, Contract);
			// No bounds: a garment's own bounds cover only part of the body, so the bones' extent sets the scale.
			Out.bReady = FMantleAvatarRigMap::MakePlan(Out.Bones, Out.Mapping, Contract, FBox(ForceInit), Out.Plan, Out.Error, /*bCompleteChains*/ true)
				&& FMantleAvatarRigMap::VerifyPlan(Out.Bones, Out.Plan, Contract, Out.Error, /*bGarment*/ true);
			return Out;
		}
		Out.Bones = FMantleAvatarRigMap::ReadBones(Mesh);
		if (Out.Bones.Num() == 0 || FMantleAvatarRigMap::IsOnSupportedSkeleton(Out.Bones, Contract))
		{
			return Out;
		}
		Out.bApplies = true;
		if (const FMantleRigMapping* Session = FMantleAvatarRigMap::FindSessionMapping(Mesh))
		{
			Out.Mapping = *Session;
			Out.bKnown = !Out.Mapping.ProfileId.IsEmpty();
		}
		else
		{
			Out.Mapping = FMantleAvatarRigMap::Detect(Out.Bones, Contract, FMantleRigProfiles::All(), Out.bKnown);
		}
		FBox Bounds = Mesh->GetImportedBounds().GetBox();
		if (Bounds.GetSize().GetMax() < UE_KINDA_SMALL_NUMBER)
		{
			Bounds = FBox(ForceInit);
		}
		Out.bReady = FMantleAvatarRigMap::MakePlan(Out.Bones, Out.Mapping, Contract, Bounds, Out.Plan, Out.Error)
			&& FMantleAvatarRigMap::VerifyPlan(Out.Bones, Out.Plan, Contract, Out.Error);
		return Out;
	}

	class FMantleAvatarRigMapFixer : public IMantleAvatarFixer
	{
	public:
		virtual FName GetId() const override
		{
			return MantleAvatarFixerIds::RigMap;
		}

		virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override
		{
			const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
			if (!Contract)
			{
				return;
			}
			for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
			{
				// #625: an avatar body keeps its rig only As-is; Convert makes it a MetaHuman (Epic's IK rigs take any bone names).
				if (Context.Route != EMantleAvatarRoute::AsIs && Context.Parts[PartIndex].Slot.Equals(AvatarSlot, ESearchCase::CaseSensitive))
				{
					continue;
				}
				// Chunk 7: an auto-rigged body is already on Epic's MetaHuman body skeleton (AsIsRig made it).
				if (Context.Parts[PartIndex].bAutoRigged)
				{
					continue;
				}
				const FRigPartDecision Decision = RigDecide(Context.Parts[PartIndex], *Contract);
				if (!Decision.bApplies)
				{
					continue;
				}
				FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
				Fix.FixerId = GetId();
				Fix.PartIndex = PartIndex;
				const FString Rig = Decision.bKnown && !Decision.Mapping.ProfileName.IsEmpty()
					? FString::Printf(TEXT("%s rig"), *Decision.Mapping.ProfileName) : FString(TEXT("Unknown rig"));
				Fix.Problem = Decision.bGarment
					? FString::Printf(TEXT("%s: on the UE4 Mannequin skeleton, and clothing must be on the MetaHuman body skeleton"),
						*Context.Parts[PartIndex].Mesh->GetName())
					: FString::Printf(TEXT("%s: not skinned to a supported skeleton"), *Rig);
				const TArray<FName> Missing = Decision.Mapping.MissingBones();
				if (Missing.Num() > 0)
				{
					Fix.bCanFix = false;
					Fix.Action = FString::Printf(TEXT("map %d bone%s to continue (%s)"), Missing.Num(), Missing.Num() == 1 ? TEXT("") : TEXT("s"),
						*RigListNames(Missing, 6));
				}
				else if (!Decision.bReady)
				{
					Fix.bCanFix = false;
					Fix.Action = Decision.Error;
				}
				else
				{
					Fix.Action = Decision.Plan.Describe(Decision.bKnown ? Decision.Mapping.ProfileName : FString());
					if (!Decision.bKnown)
					{
						Fix.Action = TEXT("check the suggested bone mapping; ") + Fix.Action;
					}
				}
			}
		}

		virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override
		{
			FString ContractError;
			const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
			if (!Contract)
			{
				OutError = ContractError;
				return false;
			}
			if (!Context.Parts.IsValidIndex(Fix.PartIndex))
			{
				OutError = TEXT("the rig fix names no part of this conversion");
				return false;
			}
			FMantleConvertPart& Part = Context.Parts[Fix.PartIndex];
			const FRigPartDecision Decision = RigDecide(Part, *Contract);
			if (!Decision.bApplies)
			{
				OutError = TEXT("the mesh is already on a supported skeleton");
				return false;
			}
			if (!Decision.bReady)
			{
				OutError = Decision.Error;
				return false;
			}
			TArray<UObject*> Created;
			USkeletalMesh* Converted = FMantleAvatarRigMap::ApplyPlan(Part.Mesh.Get(), Decision.Plan, Context.OutputFolder, Created, OutError);
			if (!Converted)
			{
				return false;
			}
			Part.Mesh = Converted;
			if (!Decision.bGarment)
			{
				Part.SkeletonId = Decision.Plan.TargetSkeletonId;
			}
			for (UObject* Asset : Created)
			{
				Context.CreatedAssets.Add(Asset);
			}
			Context.Notes.Add(FString::Printf(TEXT("%s: %s."), *Converted->GetName(),
				*Decision.Plan.Describe(Decision.bKnown ? Decision.Mapping.ProfileName : FString())));
			return true;
		}
	};
}

// ---------------------------------------------------------------------------------------------------------------------
// FMantleRigMapping

FName FMantleRigMapping::SourceFor(const FName Target) const
{
	for (const FMantleRigMappingEntry& Entry : Entries)
	{
		if (Entry.Target == Target)
		{
			return Entry.Source;
		}
	}
	return NAME_None;
}

FName FMantleRigMapping::TargetFor(const FName Source) const
{
	if (Source.IsNone())
	{
		return NAME_None;
	}
	for (const FMantleRigMappingEntry& Entry : Entries)
	{
		if (Entry.Source == Source)
		{
			return Entry.Target;
		}
	}
	return NAME_None;
}

void FMantleRigMapping::Set(const FName Target, const FName Source, const bool bRequired)
{
	if (Target.IsNone())
	{
		return;
	}
	if (!Source.IsNone())
	{
		for (FMantleRigMappingEntry& Entry : Entries)
		{
			if (Entry.Source == Source && Entry.Target != Target)
			{
				Entry.Source = NAME_None;
			}
		}
	}
	for (FMantleRigMappingEntry& Entry : Entries)
	{
		if (Entry.Target == Target)
		{
			Entry.Source = Source;
			Entry.bRequired = Entry.bRequired || bRequired;
			return;
		}
	}
	Entries.Add(FMantleRigMappingEntry{ Target, Source, bRequired });
}

TArray<FName> FMantleRigMapping::MissingBones() const
{
	TArray<FName> Missing;
	for (const FName Bone : FMantleAvatarRigMap::MustMapBones())
	{
		if (SourceFor(Bone).IsNone())
		{
			Missing.Add(Bone);
		}
	}
	return Missing;
}

int32 FMantleRigMapping::NumMapped() const
{
	int32 Count = 0;
	for (const FMantleRigMappingEntry& Entry : Entries)
	{
		Count += Entry.Source.IsNone() ? 0 : 1;
	}
	return Count;
}

// ---------------------------------------------------------------------------------------------------------------------
// FMantleRigProfiles

bool FMantleRigProfiles::Parse(const FString& Json, TArray<FMantleRigProfile>& Out, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("the rig profile file is not JSON");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
	if (!Root->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles)
	{
		OutError = TEXT("the rig profile file has no \"profiles\" list");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Profiles)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
		{
			OutError = TEXT("a rig profile is not an object");
			return false;
		}
		FMantleRigProfile Profile;
		(*Object)->TryGetStringField(TEXT("id"), Profile.Id);
		(*Object)->TryGetStringField(TEXT("name"), Profile.Name);
		(*Object)->TryGetStringField(TEXT("target"), Profile.Target);
		const TArray<TSharedPtr<FJsonValue>>* Prefixes = nullptr;
		if ((*Object)->TryGetArrayField(TEXT("prefixes"), Prefixes) && Prefixes)
		{
			for (const TSharedPtr<FJsonValue>& Prefix : *Prefixes)
			{
				FString Text;
				if (Prefix.IsValid() && Prefix->TryGetString(Text) && !Text.IsEmpty())
				{
					Profile.Prefixes.Add(Text);
				}
			}
		}
		const TSharedPtr<FJsonObject>* Bones = nullptr;
		if ((*Object)->TryGetObjectField(TEXT("bones"), Bones) && Bones && Bones->IsValid())
		{
			for (const auto& Pair : (*Bones)->Values)
			{
				FString Target;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Target) && !Target.IsEmpty())
				{
					Profile.Bones.Add(FString(Pair.Key.ToView()).ToLower(), FName(*Target));
				}
			}
		}
		if (Profile.Id.IsEmpty() || Profile.Target.IsEmpty() || Profile.Bones.Num() == 0)
		{
			OutError = FString::Printf(TEXT("rig profile \"%s\" needs an id, a target and bones"), *Profile.Id);
			return false;
		}
		if (Profile.Name.IsEmpty())
		{
			Profile.Name = Profile.Id;
		}
		Out.Add(MoveTemp(Profile));
	}
	return true;
}

FString FMantleRigProfiles::BuiltInPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("AvatarConvert"), TEXT("rig-profiles.json")) : FString();
}

FString FMantleRigProfiles::SavedDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mantle"), TEXT("RigProfiles"));
}

const TArray<FMantleRigProfile>& FMantleRigProfiles::BuiltIn()
{
	static TArray<FMantleRigProfile> Profiles;
	static bool bLoaded = false;
	if (!bLoaded)
	{
		bLoaded = true;
		const FString Path = BuiltInPath();
		FString Text;
		FString Error;
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogMantleRigMap, Warning, TEXT("The rig profiles could not be read from '%s'."), *Path);
		}
		else if (!Parse(Text, Profiles, Error))
		{
			Profiles.Reset();
			UE_LOG(LogMantleRigMap, Warning, TEXT("The rig profiles at '%s' do not read: %s"), *Path, *Error);
		}
	}
	return Profiles;
}

TArray<FMantleRigProfile> FMantleRigProfiles::All()
{
	TArray<FMantleRigProfile> Out;
	TArray<FString> Files;
	const FString Directory = SavedDirectory();
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
	Files.Sort();
	for (const FString& File : Files)
	{
		FString Text;
		FString Error;
		TArray<FMantleRigProfile> Saved;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Directory, File)) || !Parse(Text, Saved, Error))
		{
			UE_LOG(LogMantleRigMap, Warning, TEXT("Skipping the saved rig profile '%s': %s"), *File, *Error);
			continue;
		}
		for (FMantleRigProfile& Profile : Saved)
		{
			Profile.bSaved = true;
			Out.Add(MoveTemp(Profile));
		}
	}
	Out.Append(BuiltIn());
	return Out;
}

FString FMantleRigProfiles::StripNamespace(const FString& Bone)
{
	for (int32 Index = Bone.Len() - 1; Index >= 0; --Index)
	{
		if (Bone[Index] == TEXT(':') || Bone[Index] == TEXT('|'))
		{
			return Bone.RightChop(Index + 1);
		}
	}
	return Bone;
}

FString FMantleRigProfiles::StripName(const FMantleRigProfile& Profile, const FName Bone)
{
	FString Name = StripNamespace(Bone.ToString());
	for (const FString& Prefix : Profile.Prefixes)
	{
		const FRegexPattern Pattern(TEXT("^(?:") + Prefix + TEXT(")"), ERegexPatternFlags::CaseInsensitive);
		FRegexMatcher Matcher(Pattern, Name);
		if (Matcher.FindNext() && Matcher.GetMatchBeginning() == 0 && Matcher.GetMatchEnding() > 0 && Matcher.GetMatchEnding() < Name.Len())
		{
			Name.RightChopInline(Matcher.GetMatchEnding());
			break;
		}
	}
	return Name.ToLower();
}

int32 FMantleRigProfiles::Match(const FMantleRigProfile& Profile, TConstArrayView<FMantleRigBone> Bones, TConstArrayView<FString> Required,
	FMantleRigMapping& Out)
{
	Out = FMantleRigMapping();
	Out.ProfileId = Profile.Id;
	Out.ProfileName = Profile.Name;
	Out.TargetSkeletonId = Profile.Target;
	for (const FString& Bone : Required)
	{
		Out.Entries.Add(FMantleRigMappingEntry{ FName(*Bone), NAME_None, true });
	}
	int32 Matched = 0;
	for (const FMantleRigBone& Bone : Bones)
	{
		const FName* Target = Profile.Bones.Find(StripName(Profile, Bone.Name));
		if (!Target || !Out.SourceFor(*Target).IsNone() || !Out.TargetFor(Bone.Name).IsNone())
		{
			continue;
		}
		Out.Set(*Target, Bone.Name);
		++Matched;
	}
	return Matched;
}

FString FMantleRigProfiles::ToJson(const FMantleRigMapping& Mapping, const FString& Id, const FString& Name)
{
	const TSharedRef<FJsonObject> Bones = MakeShared<FJsonObject>();
	for (const FMantleRigMappingEntry& Entry : Mapping.Entries)
	{
		if (!Entry.Source.IsNone() && !Entry.Target.IsNone())
		{
			Bones->SetStringField(StripNamespace(Entry.Source.ToString()), Entry.Target.ToString());
		}
	}
	const TSharedRef<FJsonObject> Profile = MakeShared<FJsonObject>();
	Profile->SetStringField(TEXT("id"), Id);
	Profile->SetStringField(TEXT("name"), Name);
	Profile->SetStringField(TEXT("target"), Mapping.TargetSkeletonId);
	Profile->SetArrayField(TEXT("prefixes"), TArray<TSharedPtr<FJsonValue>>());
	Profile->SetObjectField(TEXT("bones"), Bones);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetStringField(TEXT("comment"), TEXT("A rig mapping saved from the Mantle avatar converter's bone mapping panel (#610)."));
	Root->SetArrayField(TEXT("profiles"), TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueObject>(Profile) });

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

bool FMantleRigProfiles::Save(const FMantleRigMapping& Mapping, const FString& Name, FString& OutPath, FString& OutError)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("name the profile first");
		return false;
	}
	if (Mapping.NumMapped() == 0 || Mapping.TargetSkeletonId.IsEmpty())
	{
		OutError = TEXT("map some bones first");
		return false;
	}
	FString Id = TEXT("saved_");
	for (const TCHAR Char : Trimmed)
	{
		Id.AppendChar(FChar::IsAlnum(Char) ? FChar::ToLower(Char) : TEXT('_'));
	}
	const FString Directory = SavedDirectory();
	IFileManager::Get().MakeDirectory(*Directory, true);
	OutPath = FPaths::Combine(Directory, Id + TEXT(".json"));
	if (!FFileHelper::SaveStringToFile(ToJson(Mapping, Id, Trimmed), *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("could not write %s"), *OutPath);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// FMantleRigPlan

FString FMantleRigPlan::Describe(const FString& RigName) const
{
	using namespace MantleAvatarRigMapPrivate;
	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("maps %d %sbones onto the %s skeleton"), NumMapped, RigName.IsEmpty() ? TEXT("") : *(RigName + TEXT(" ")),
		*FMantleAvatarRigMap::SkeletonDisplayName(TargetSkeletonId)));
	if (Inserted.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("adds %s with no weights"), *RigListNames(Inserted, 6)));
	}
	if (Dropped.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("drops %d bone%s nothing moves with (%s)"), Dropped.Num(), Dropped.Num() == 1 ? TEXT("") : TEXT("s"), *RigListNames(Dropped, 3)));
	}
	if (Unmapped.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("leaves %s to fold into their parents (the rig's hierarchy differs)"), *RigListNames(Unmapped, 3)));
	}
	if (!FMath::IsNearlyEqual(Scale, 1.0))
	{
		Parts.Add(FString::Printf(TEXT("scales it %gx to centimetres"), Scale));
	}
	if (bTurned)
	{
		Parts.Add(TEXT("turns it upright (Z up, facing +Y)"));
	}
	return FString::Join(Parts, TEXT(", "));
}

// ---------------------------------------------------------------------------------------------------------------------
// FMantleAvatarRigMap

const TArray<FName>& FMantleAvatarRigMap::MustMapBones()
{
	static const TArray<FName> Bones = {
		FName(TEXT("pelvis")), FName(TEXT("head")),
		FName(TEXT("upperarm_l")), FName(TEXT("lowerarm_l")), FName(TEXT("hand_l")),
		FName(TEXT("upperarm_r")), FName(TEXT("lowerarm_r")), FName(TEXT("hand_r")),
		FName(TEXT("thigh_l")), FName(TEXT("calf_l")), FName(TEXT("foot_l")),
		FName(TEXT("thigh_r")), FName(TEXT("calf_r")), FName(TEXT("foot_r")),
	};
	return Bones;
}

FString FMantleAvatarRigMap::SkeletonDisplayName(const FString& SkeletonId)
{
	if (SkeletonId.Equals(TEXT("metahuman"), ESearchCase::CaseSensitive))
	{
		return TEXT("MetaHuman");
	}
	if (SkeletonId.Equals(TEXT("ue5mannequin"), ESearchCase::CaseSensitive))
	{
		return TEXT("UE5 Mannequin");
	}
	if (SkeletonId.Equals(TEXT("ue4mannequin"), ESearchCase::CaseSensitive))
	{
		return TEXT("UE4 Mannequin");
	}
	if (SkeletonId.Equals(TEXT("uefnmannequin"), ESearchCase::CaseSensitive))
	{
		return TEXT("UEFN Mannequin");
	}
	return SkeletonId;
}

TArray<FMantleRigBone> FMantleAvatarRigMap::ReadBones(const USkeletalMesh* Mesh)
{
	using namespace MantleAvatarRigMapPrivate;
	TArray<FMantleRigBone> Out;
	if (!Mesh)
	{
		return Out;
	}
	const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
	const TArray<FMeshBoneInfo>& Info = Reference.GetRawRefBoneInfo();
	const TArray<FTransform>& Pose = Reference.GetRawRefBonePose();
	for (int32 Bone = 0; Bone < Info.Num(); ++Bone)
	{
		FMantleRigBone& Entry = Out.AddDefaulted_GetRef();
		Entry.Name = Info[Bone].Name;
		Entry.Parent = Info[Bone].ParentIndex;
		Entry.Local = Pose.IsValidIndex(Bone) ? Pose[Bone] : FTransform::Identity;
	}

	// Weighted: from the render data's CPU copies (every LOD), which the editor keeps.
	bool bRead = false;
	if (const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering())
	{
		for (const FSkeletalMeshLODRenderData& LOD : RenderData->LODRenderData)
		{
			const FSkinWeightVertexBuffer& Weights = LOD.SkinWeightVertexBuffer;
			const uint32 NumVertices = Weights.GetNumVertices();
			if (NumVertices == 0 || !Weights.GetDataVertexBuffer() || !Weights.GetDataVertexBuffer()->GetWeightData())
			{
				continue;
			}
			const int32 MaxSlots = FMath::Clamp(static_cast<int32>(Weights.GetMaxBoneInfluences()), 1, static_cast<int32>(MAX_TOTAL_INFLUENCES));
			for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
			{
				for (uint32 Offset = 0; Offset < Section.NumVertices; ++Offset)
				{
					const uint32 Vertex = Section.BaseVertexIndex + Offset;
					if (Vertex >= NumVertices)
					{
						break;
					}
					const FSkinWeightInfo Influences = Weights.GetVertexSkinWeights(Vertex);
					for (int32 Slot = 0; Slot < MaxSlots; ++Slot)
					{
						if (Influences.InfluenceWeights[Slot] == 0 || !Section.BoneMap.IsValidIndex(Influences.InfluenceBones[Slot]))
						{
							continue;
						}
						const int32 Bone = Section.BoneMap[Influences.InfluenceBones[Slot]];
						if (Out.IsValidIndex(Bone))
						{
							Out[Bone].bWeighted = true;
						}
					}
				}
			}
			bRead = true;
		}
	}
	if (!bRead)
	{
		FMeshDescription Description;
		if (RigReadSourceGeometry(Mesh, 0, Description))
		{
			FSkeletalMeshAttributes Attributes(Description);
			const FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
			if (Weights.IsValid())
			{
				for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
				{
					for (const FBoneWeight Weight : Weights.Get(Vertex))
					{
						if (Weight.GetRawWeight() > 0 && Out.IsValidIndex(Weight.GetBoneIndex()))
						{
							Out[Weight.GetBoneIndex()].bWeighted = true;
						}
					}
				}
				bRead = true;
			}
		}
	}
	if (!bRead)
	{
		// Nothing to read the weights from: every bone counts as weighted but the leaves named as markers (*_End).
		TArray<bool> HasChild;
		HasChild.Init(false, Out.Num());
		for (const FMantleRigBone& Bone : Out)
		{
			if (HasChild.IsValidIndex(Bone.Parent))
			{
				HasChild[Bone.Parent] = true;
			}
		}
		for (int32 Bone = 0; Bone < Out.Num(); ++Bone)
		{
			Out[Bone].bWeighted = HasChild[Bone]
				|| !RigHasToken(RigNameTokens(FMantleRigProfiles::StripNamespace(Out[Bone].Name.ToString())), { TEXT("end"), TEXT("nub"), TEXT("tip"), TEXT("top") });
		}
	}
	return Out;
}

bool FMantleAvatarRigMap::IsOnSupportedSkeleton(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract)
{
	TSet<FName> Names;
	for (const FMantleRigBone& Bone : Bones)
	{
		Names.Add(Bone.Name);
	}
	bool bNamed = false;
	for (const FString& Id : Contract.AvatarBody.Skeletons)
	{
		const TArray<FString>* Required = Contract.AvatarRequiredBones.Find(Id);
		if (!Required || Required->Num() == 0)
		{
			continue;
		}
		bool bAll = true;
		for (const FString& Bone : *Required)
		{
			if (!Names.Contains(FName(*Bone)))
			{
				bAll = false;
				break;
			}
		}
		if (bAll)
		{
			bNamed = true;
			break;
		}
	}
	if (!bNamed)
	{
		return false;
	}

	// #774: the names are not enough. A UE4-style rig (a Paragon hero) carries every required bone of the UE4 Mannequin and
	// also weighted bones of its own that are bones of another supported skeleton (upperarm_twist_02_l is a UE5 and MetaHuman
	// bone), which the upload never folds into a UE4 Mannequin body (#593): it refuses the body at export. Such a rig is not on
	// a supported skeleton; this fixer maps it and renames those bones so they fold. The test is the upload's own fit, on the bones.
	FMantleGlbMesh Glb;
	Glb.NumInfluences = 1;
	TArray<bool> Weighted;
	for (const FMantleRigBone& Bone : Bones)
	{
		FMantleGlbJoint& Joint = Glb.Joints.AddDefaulted_GetRef();
		Joint.Name = Bone.Name;
		Joint.Parent = Bone.Parent;
		Joint.Local = Bone.Local;
		Weighted.Add(Bone.bWeighted);
	}
	FMantleSkinFit Fit;
	FString Error;
	return FMantleCosmeticChecks::FitSkinToClass(Glb, TConstArrayView<FTransform>(), Weighted, Contract.AvatarBody, Contract, true, Fit, Error) != nullptr;
}

FMantleRigMapping FMantleAvatarRigMap::Detect(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract,
	TConstArrayView<FMantleRigProfile> Profiles, bool& bOutKnown)
{
	using namespace MantleAvatarRigMapPrivate;
	bOutKnown = false;
	bool bHaveBest = false;
	bool bBestComplete = false;
	int32 BestScore = 0;
	FMantleRigMapping Best;
	for (const FMantleRigProfile& Profile : Profiles)
	{
		if (!Contract.FindSkeleton(Profile.Target) || !Contract.AvatarBody.Skeletons.Contains(Profile.Target))
		{
			continue;
		}
		FMantleRigMapping Mapping;
		const int32 Score = FMantleRigProfiles::Match(Profile, Bones, RigRequiredWithoutRoot(Contract, Profile.Target), Mapping);
		if (Score == 0)
		{
			continue;
		}
		const bool bComplete = Mapping.MissingBones().Num() == 0;
		if (!bHaveBest || (bComplete && !bBestComplete) || (bComplete == bBestComplete && Score > BestScore))
		{
			bHaveBest = true;
			bBestComplete = bComplete;
			BestScore = Score;
			Best = MoveTemp(Mapping);
		}
	}
	if (bHaveBest && bBestComplete)
	{
		bOutKnown = true;
		return Best;
	}
	FMantleRigMapping Suggested = Suggest(Bones, Contract, SuggestedTarget);
	if (bHaveBest && Best.TargetSkeletonId.Equals(Suggested.TargetSkeletonId, ESearchCase::CaseSensitive))
	{
		// A profile that knows some of the bones knows them better than a guess.
		for (const FMantleRigMappingEntry& Entry : Best.Entries)
		{
			if (!Entry.Source.IsNone())
			{
				Suggested.Set(Entry.Target, Entry.Source, Entry.bRequired);
			}
		}
	}
	return Suggested;
}

FMantleRigMapping FMantleAvatarRigMap::Suggest(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract, const FString& TargetId)
{
	using namespace MantleAvatarRigMapPrivate;
	FMantleRigMapping Out;
	Out.TargetSkeletonId = TargetId;
	for (const FString& Bone : RigRequiredWithoutRoot(Contract, TargetId))
	{
		Out.Entries.Add(FMantleRigMappingEntry{ FName(*Bone), NAME_None, true });
	}
	const FMantleContractSkeleton* Target = Contract.FindSkeleton(TargetId);
	const int32 NumBones = Bones.Num();
	if (!Target || NumBones == 0)
	{
		return Out;
	}

	TArray<int32> Parent;
	TArray<int32> Depth;
	RigTree(Bones, Parent, Depth);
	TArray<TArray<FString>> Tokens;
	Tokens.Reserve(NumBones);
	for (const FMantleRigBone& Bone : Bones)
	{
		Tokens.Add(RigNameTokens(FMantleRigProfiles::StripNamespace(Bone.Name.ToString())));
	}
	// The shallowest bone a predicate accepts.
	const auto Shallowest = [&Depth, NumBones](TFunctionRef<bool(int32)> Accept)
	{
		int32 Best = INDEX_NONE;
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			if (Accept(Bone) && (Best == INDEX_NONE || Depth[Bone] < Depth[Best]))
			{
				Best = Bone;
			}
		}
		return Best;
	};
	const auto Named = [&Tokens](const int32 Bone, std::initializer_list<const TCHAR*> Words, const ERigSide Side)
	{
		return RigHasToken(Tokens[Bone], Words) && RigSideOf(Tokens[Bone]) == Side && !RigIsHelperName(Tokens[Bone]) && !RigIsFingerName(Tokens[Bone]);
	};

	const int32 Head = Shallowest([&](int32 Bone) { return Named(Bone, { TEXT("head") }, ERigSide::None) && !RigHasToken(Tokens[Bone], { TEXT("neck") }); });
	int32 Hand[2] = { INDEX_NONE, INDEX_NONE };
	int32 Foot[2] = { INDEX_NONE, INDEX_NONE };
	const ERigSide Sides[2] = { ERigSide::Left, ERigSide::Right };
	for (int32 Side = 0; Side < 2; ++Side)
	{
		Hand[Side] = Shallowest([&](int32 Bone) { return Named(Bone, { TEXT("hand"), TEXT("wrist") }, Sides[Side]); });
		Foot[Side] = Shallowest([&](int32 Bone)
		{
			return Named(Bone, { TEXT("foot"), TEXT("ankle") }, Sides[Side]) && !RigHasToken(Tokens[Bone], { TEXT("toe"), TEXT("toes"), TEXT("ball") });
		});
	}

	// The pelvis is where the legs and the spine meet; by name when the feet or head are not found.
	int32 Pelvis = RigCommonAncestor(Parent, Depth, RigCommonAncestor(Parent, Depth, Foot[0], Foot[1]), Head);
	if (Pelvis == INDEX_NONE || Pelvis == Head)
	{
		Pelvis = Shallowest([&](int32 Bone) { return Named(Bone, { TEXT("hips"), TEXT("hip"), TEXT("pelvis") }, ERigSide::None); });
	}
	// The top spine bone is where the arms and the neck meet.
	int32 Chest = RigCommonAncestor(Parent, Depth, RigCommonAncestor(Parent, Depth, Hand[0], Hand[1]), Head);
	if (!RigIsAncestor(Parent, Pelvis, Chest))
	{
		Chest = INDEX_NONE;
	}

	TMap<FName, int32> Suggested;
	const auto Propose = [&Suggested](const TCHAR* TargetBone, const int32 Bone)
	{
		if (Bone != INDEX_NONE)
		{
			Suggested.Add(FName(TargetBone), Bone);
		}
	};
	Propose(TEXT("pelvis"), Pelvis);
	Propose(TEXT("head"), Head);
	if (Chest != INDEX_NONE)
	{
		const TArray<int32> Spine = RigPathBetween(Parent, Pelvis, Chest);
		if (Spine.Num() >= 1)
		{
			Propose(TEXT("spine_01"), Spine[0]);
		}
		if (Spine.Num() >= 2)
		{
			Propose(TEXT("spine_02"), Spine[Spine.Num() / 2]);
		}
		Propose(TEXT("spine_03"), Chest);
		const TArray<int32> Neck = RigPathBetween(Parent, Chest, Head);
		if (Neck.Num() >= 1)
		{
			Propose(TEXT("neck_01"), Neck[0]);
		}
	}
	const TCHAR* const ArmNames[2][3] = { { TEXT("clavicle_l"), TEXT("upperarm_l"), TEXT("lowerarm_l") }, { TEXT("clavicle_r"), TEXT("upperarm_r"), TEXT("lowerarm_r") } };
	const TCHAR* const LegNames[2][4] = { { TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l"), TEXT("ball_l") }, { TEXT("thigh_r"), TEXT("calf_r"), TEXT("foot_r"), TEXT("ball_r") } };
	const TCHAR* const HandNames[2] = { TEXT("hand_l"), TEXT("hand_r") };
	for (int32 Side = 0; Side < 2; ++Side)
	{
		// The limb bones are the chain above the hand and the foot, twist and roll bones left out when that leaves enough.
		const auto Limb = [&](const int32 Top, const int32 End)
		{
			TArray<int32> Chain = RigPathBetween(Parent, Top, End);
			TArray<int32> Kept = Chain.FilterByPredicate([&Tokens](const int32 Link) { return !RigIsHelperName(Tokens[Link]); });
			return Kept.Num() >= 2 ? Kept : Chain;
		};
		if (Hand[Side] != INDEX_NONE)
		{
			Propose(HandNames[Side], Hand[Side]);
			const TArray<int32> Arm = Limb(Chest != INDEX_NONE ? Chest : Pelvis, Hand[Side]);
			for (int32 Step = 0; Step < 3 && Step < Arm.Num(); ++Step)
			{
				Propose(ArmNames[Side][2 - Step], Arm[Arm.Num() - 1 - Step]);
			}
		}
		if (Foot[Side] != INDEX_NONE)
		{
			Propose(LegNames[Side][2], Foot[Side]);
			const TArray<int32> Leg = Limb(Pelvis, Foot[Side]);
			for (int32 Step = 0; Step < 2 && Step < Leg.Num(); ++Step)
			{
				Propose(LegNames[Side][1 - Step], Leg[Leg.Num() - 1 - Step]);
			}
			// The ball: the foot's toe child, else its only child that is no marker.
			int32 Ball = INDEX_NONE;
			int32 Plain = INDEX_NONE;
			int32 PlainCount = 0;
			for (int32 Bone = Foot[Side] + 1; Bone < NumBones; ++Bone)
			{
				if (Parent[Bone] != Foot[Side])
				{
					continue;
				}
				if (Ball == INDEX_NONE && RigHasToken(Tokens[Bone], { TEXT("toe"), TEXT("toes"), TEXT("toebase"), TEXT("ball") }) && !RigIsHelperName(Tokens[Bone]))
				{
					Ball = Bone;
				}
				if (!RigIsHelperName(Tokens[Bone]))
				{
					Plain = Bone;
					++PlainCount;
				}
			}
			Propose(LegNames[Side][3], Ball != INDEX_NONE ? Ball : (PlainCount == 1 ? Plain : INDEX_NONE));
		}
	}

	// Required bones first, each rig bone once.
	for (const FString& Bone : RigRequiredWithoutRoot(Contract, TargetId))
	{
		const int32* Source = Suggested.Find(FName(*Bone));
		if (Source && Out.TargetFor(Bones[*Source].Name).IsNone())
		{
			Out.Set(FName(*Bone), Bones[*Source].Name, true);
		}
	}

	// Fingers, optional: the first bone below the hand named for the finger, and its first two children.
	struct FFingerKind
	{
		const TCHAR* Target;
		const TCHAR* Words[3];
	};
	const FFingerKind Fingers[] = {
		{ TEXT("thumb"), { TEXT("thumb"), nullptr, nullptr } },
		{ TEXT("index"), { TEXT("index"), TEXT("pointer"), nullptr } },
		{ TEXT("middle"), { TEXT("middle"), TEXT("mid"), nullptr } },
		{ TEXT("ring"), { TEXT("ring"), nullptr, nullptr } },
		{ TEXT("pinky"), { TEXT("pinky"), TEXT("pinkie"), TEXT("little") } },
	};
	const auto IsFingerKind = [](const TArray<FString>& BoneTokens, const FFingerKind& Kind)
	{
		for (const TCHAR* Word : Kind.Words)
		{
			if (Word && RigHasToken(BoneTokens, { Word }))
			{
				return true;
			}
		}
		return false;
	};
	const TCHAR* const SideSuffix[2] = { TEXT("l"), TEXT("r") };
	for (int32 Side = 0; Side < 2; ++Side)
	{
		if (Hand[Side] == INDEX_NONE)
		{
			continue;
		}
		for (const FFingerKind& Finger : Fingers)
		{
			int32 Bone = INDEX_NONE;
			for (int32 Candidate = Hand[Side] + 1; Candidate < NumBones; ++Candidate)
			{
				if (RigIsAncestor(Parent, Hand[Side], Candidate) && IsFingerKind(Tokens[Candidate], Finger)
					&& !RigHasToken(Tokens[Candidate], { TEXT("carpal"), TEXT("metacarpal"), TEXT("palm") }) && !RigIsHelperName(Tokens[Candidate]))
				{
					Bone = Candidate;
					break;
				}
			}
			for (int32 Segment = 1; Segment <= 3 && Bone != INDEX_NONE; ++Segment)
			{
				const FName TargetBone(*FString::Printf(TEXT("%s_%02d_%s"), Finger.Target, Segment, SideSuffix[Side]));
				if (Target->Find(TargetBone) != INDEX_NONE && Out.TargetFor(Bones[Bone].Name).IsNone() && Out.SourceFor(TargetBone).IsNone())
				{
					Out.Set(TargetBone, Bones[Bone].Name);
				}
				const int32 Current = Bone;
				Bone = INDEX_NONE;
				for (int32 Child = Current + 1; Child < NumBones; ++Child)
				{
					if (Parent[Child] == Current && !RigIsHelperName(Tokens[Child]))
					{
						Bone = Child;
						break;
					}
				}
			}
		}
	}
	return Out;
}

bool FMantleAvatarRigMap::MakePlan(TConstArrayView<FMantleRigBone> Bones, const FMantleRigMapping& Mapping, const FMantleAvatarContract& Contract,
	const FBox& Bounds, FMantleRigPlan& Out, FString& OutError, const bool bCompleteChains)
{
	using namespace MantleAvatarRigMapPrivate;
	Out = FMantleRigPlan();
	const FMantleContractSkeleton* Target = Contract.FindSkeleton(Mapping.TargetSkeletonId);
	if (!Target || Target->Bones.Num() == 0)
	{
		OutError = FString::Printf(TEXT("the %s skeleton is not available in this plugin"), *SkeletonDisplayName(Mapping.TargetSkeletonId));
		return false;
	}
	Out.TargetSkeletonId = Target->Id;
	const int32 NumSource = Bones.Num();
	if (NumSource == 0)
	{
		OutError = TEXT("the mesh has no bones");
		return false;
	}
	for (int32 Bone = 0; Bone < NumSource; ++Bone)
	{
		if (Bones[Bone].Parent >= Bone || Bones[Bone].Parent < INDEX_NONE)
		{
			OutError = TEXT("the rig's bones are not listed parents first; reimport the mesh");
			return false;
		}
	}
	Out.SourceComponent = RigComponentPoses(Bones);
	const FName Root = Target->Root();

	// The mapping on the rig's bones, in the fixture's spelling: the first claim on a target or a bone wins.
	TMap<FName, int32> SourceByName;
	for (int32 Bone = 0; Bone < NumSource; ++Bone)
	{
		if (!SourceByName.Contains(Bones[Bone].Name))
		{
			SourceByName.Add(Bones[Bone].Name, Bone);
		}
	}
	TArray<FName> TargetOfSource;
	TargetOfSource.Init(NAME_None, NumSource);
	TMap<FName, int32> SourceOfTarget;
	for (const FMantleRigMappingEntry& Entry : Mapping.Entries)
	{
		const int32 TargetIndex = Entry.Target.IsNone() ? INDEX_NONE : Target->Find(Entry.Target);
		const int32* Source = Entry.Source.IsNone() ? nullptr : SourceByName.Find(Entry.Source);
		if (TargetIndex == INDEX_NONE || Target->Bones[TargetIndex] == Root || !Source || !TargetOfSource[*Source].IsNone()
			|| SourceOfTarget.Contains(Target->Bones[TargetIndex]))
		{
			continue;
		}
		TargetOfSource[*Source] = Target->Bones[TargetIndex];
		SourceOfTarget.Add(Target->Bones[TargetIndex], *Source);
	}
	TArray<FName> Missing;
	for (const FName Bone : MustMapBones())
	{
		if (!SourceOfTarget.Contains(Bone))
		{
			Missing.Add(Bone);
		}
	}
	if (Missing.Num() > 0)
	{
		OutError = FString::Printf(TEXT("map %d more bone%s to continue: %s"), Missing.Num(), Missing.Num() == 1 ? TEXT("") : TEXT("s"), *RigListNames(Missing, 6));
		return false;
	}
	TSet<FName> Required;
	if (const TArray<FString>* RequiredList = Contract.AvatarRequiredBones.Find(Target->Id))
	{
		for (const FString& Bone : *RequiredList)
		{
			Required.Add(FName(*Bone));
		}
	}

	// Each mapped bone must sit below the mapped bone the skeleton has above it. An optional one that does not is unmapped
	// (it folds instead); a required one refuses. Unmapping can change what is above other bones, so repeat until stable.
	for (bool bChanged = true; bChanged; )
	{
		bChanged = false;
		for (const FName Bone : Target->Bones)
		{
			const int32* SourcePtr = SourceOfTarget.Find(Bone);
			if (!SourcePtr)
			{
				continue;
			}
			const int32 Source = *SourcePtr;
			FName Expected = Target->ParentOf(Bone);
			while (!Expected.IsNone() && Expected != Root && !SourceOfTarget.Contains(Expected))
			{
				Expected = Target->ParentOf(Expected);
			}
			if (Expected == Root)
			{
				Expected = NAME_None;
			}
			int32 Ancestor = Bones[Source].Parent;
			while (Ancestor != INDEX_NONE && TargetOfSource[Ancestor].IsNone())
			{
				Ancestor = Bones[Ancestor].Parent;
			}
			const FName Actual = Ancestor == INDEX_NONE ? NAME_None : TargetOfSource[Ancestor];
			if (Actual == Expected)
			{
				continue;
			}
			if (Required.Contains(Bone) || MustMapBones().Contains(Bone))
			{
				OutError = FString::Printf(TEXT("%s (the rig's %s) must be below %s, but the rig has it below %s: map bones whose hierarchy matches the %s skeleton"),
					*Bone.ToString(), *Bones[Source].Name.ToString(), Expected.IsNone() ? *Root.ToString() : *Expected.ToString(),
					Actual.IsNone() ? TEXT("no mapped bone") : *Actual.ToString(), *SkeletonDisplayName(Target->Id));
				return false;
			}
			SourceOfTarget.Remove(Bone);
			TargetOfSource[Source] = NAME_None;
			Out.Unmapped.Add(Bone);
			bChanged = true;
		}
	}

	// The scale and axis correction: up from the feet to the head, left from the right limbs to the left ones.
	const auto SourcePosition = [&Out, &SourceOfTarget](const TCHAR* Bone) { return Out.SourceComponent[SourceOfTarget.FindChecked(FName(Bone))].GetTranslation(); };
	const FVector Feet = (SourcePosition(TEXT("foot_l")) + SourcePosition(TEXT("foot_r"))) * 0.5;
	const FVector Up = SourcePosition(TEXT("head")) - Feet;
	const FVector Left = (SourcePosition(TEXT("upperarm_l")) - SourcePosition(TEXT("upperarm_r"))) + (SourcePosition(TEXT("thigh_l")) - SourcePosition(TEXT("thigh_r")));
	FQuat Turn = FQuat::Identity;
	if (!RigAxisTurn(Up, Left, Turn))
	{
		OutError = TEXT("the rig's head is not above its feet, or its left side cannot be told from its right: check the mapping");
		return false;
	}
	double MinUp = TNumericLimits<double>::Max();
	double MaxUp = TNumericLimits<double>::Lowest();
	const bool bBounds = Bounds.IsValid != 0;
	if (bBounds)
	{
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Point((Corner & 1) ? Bounds.Max.X : Bounds.Min.X, (Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
			const double Height = Turn.RotateVector(Point).Z;
			MinUp = FMath::Min(MinUp, Height);
			MaxUp = FMath::Max(MaxUp, Height);
		}
	}
	else
	{
		for (const FTransform& Pose : Out.SourceComponent)
		{
			const double Height = Turn.RotateVector(Pose.GetTranslation()).Z;
			MinUp = FMath::Min(MinUp, Height);
			MaxUp = FMath::Max(MaxUp, Height);
		}
	}
	// A head bone sits below the top of the head: the bones' extent undershoots the mesh's.
	const double MeasuredHeight = (MaxUp - MinUp) * (bBounds ? 1.0 : 1.08);
	Out.Scale = RigChooseScale(MeasuredHeight);
	const FVector Pelvis = Turn.RotateVector(SourcePosition(TEXT("pelvis"))) * Out.Scale;
	FVector Offset = FVector::ZeroVector;
	if (FVector2D(Pelvis.X, Pelvis.Y).Size() > CentreToleranceCm)
	{
		Offset.X = -Pelvis.X;
		Offset.Y = -Pelvis.Y;
	}
	if (bBounds && FMath::Abs(MinUp * Out.Scale) > GroundToleranceCm)
	{
		Offset.Z = -MinUp * Out.Scale;
	}
	Out.Correction = FTransform(Turn, Offset, FVector(Out.Scale));
	Out.bTurned = !Turn.Equals(FQuat::Identity, 1.e-4);

	// Which rig bones stay: the mapped ones, and below the pelvis any weighted bone or one with a kept bone below it. The
	// pelvis's unmapped ancestors (an armature or scene root) give way to the inserted root.
	const int32 PelvisSource = SourceOfTarget.FindChecked(FName(TEXT("pelvis")));
	TArray<bool> AbovePelvis;
	AbovePelvis.Init(false, NumSource);
	for (int32 Ancestor = Bones[PelvisSource].Parent; Ancestor != INDEX_NONE; Ancestor = Bones[Ancestor].Parent)
	{
		AbovePelvis[Ancestor] = true;
	}
	TArray<bool> Keep;
	Keep.Init(false, NumSource);
	TArray<bool> KeptBelow;
	KeptBelow.Init(false, NumSource);
	for (int32 Bone = NumSource - 1; Bone >= 0; --Bone)
	{
		const bool bMapped = !TargetOfSource[Bone].IsNone();
		Keep[Bone] = bMapped || (!AbovePelvis[Bone] && (Bones[Bone].bWeighted || KeptBelow[Bone]));
		if ((Keep[Bone] || KeptBelow[Bone]) && Bones[Bone].Parent != INDEX_NONE)
		{
			KeptBelow[Bones[Bone].Parent] = true;
		}
	}

	struct FPlanNode
	{
		FName Name;
		int32 Parent = INDEX_NONE;
		FTransform Component;
		int32 Source = INDEX_NONE;
		bool bMapped = false;
		bool bInserted = false;
	};
	TArray<FPlanNode> Nodes;
	Nodes.Add(FPlanNode{ Root, INDEX_NONE, FTransform::Identity, INDEX_NONE, false, true });
	Out.Inserted.Add(Root);

	// Names an extra bone may not take: every bone of every skeleton the contract has, and every target mapped.
	TSet<FName> Reserved;
	for (const TPair<FString, FMantleContractSkeleton>& Pair : Contract.Skeletons)
	{
		Reserved.Append(Pair.Value.Bones);
	}
	TSet<FName> Used;
	Used.Add(Root);
	for (const TPair<FName, int32>& Pair : SourceOfTarget)
	{
		Used.Add(Pair.Key);
	}

	TArray<int32> NodeOfSource;
	NodeOfSource.Init(INDEX_NONE, NumSource);
	for (int32 Bone = 0; Bone < NumSource; ++Bone)
	{
		if (!Keep[Bone])
		{
			Out.Dropped.Add(Bones[Bone].Name);
			continue;
		}
		int32 ParentNode = 0;
		for (int32 Ancestor = Bones[Bone].Parent; Ancestor != INDEX_NONE; Ancestor = Bones[Ancestor].Parent)
		{
			if (NodeOfSource[Ancestor] != INDEX_NONE)
			{
				ParentNode = NodeOfSource[Ancestor];
				break;
			}
		}
		FPlanNode Node;
		Node.Parent = ParentNode;
		Node.Source = Bone;
		Node.Component = RigCorrected(Out.SourceComponent[Bone], Out.Correction);
		Node.bMapped = !TargetOfSource[Bone].IsNone();
		if (Node.bMapped)
		{
			Node.Name = TargetOfSource[Bone];
		}
		else
		{
			bool bCollided = false;
			Node.Name = RigExtraName(Bones[Bone].Name, Reserved, Used, bCollided);
			Used.Add(Node.Name);
			if (bCollided)
			{
				Out.RenamedExtras.Add(TPair<FName, FName>(Bones[Bone].Name, Node.Name));
			}
		}
		NodeOfSource[Bone] = Nodes.Add(Node);
	}

	const auto FindNode = [&Nodes](const FName Name)
	{
		for (int32 Node = 0; Node < Nodes.Num(); ++Node)
		{
			if (Nodes[Node].Name == Name)
			{
				return Node;
			}
		}
		return static_cast<int32>(INDEX_NONE);
	};

	// bCompleteChains: every skeleton bone above a mapped one is inserted too, so each mapped bone's parent is its own.
	TSet<FName> Insert = Required;
	if (bCompleteChains)
	{
		for (const TPair<FName, int32>& Pair : SourceOfTarget)
		{
			for (FName Step = Target->ParentOf(Pair.Key); !Step.IsNone(); Step = Target->ParentOf(Step))
			{
				Insert.Add(Step);
			}
		}
	}

	// Required bones the rig lacks, parents first: between their skeleton parent and the bones that belong below them, or
	// for a ball below the foot, forward and down at the toes.
	for (const FName Bone : Target->Bones)
	{
		if (!Insert.Contains(Bone) || Bone == Root || FindNode(Bone) != INDEX_NONE)
		{
			continue;
		}
		int32 ParentNode = INDEX_NONE;
		for (FName Step = Target->ParentOf(Bone); !Step.IsNone() && ParentNode == INDEX_NONE; Step = Target->ParentOf(Step))
		{
			ParentNode = FindNode(Step);
		}
		if (ParentNode == INDEX_NONE)
		{
			ParentNode = 0;
		}
		TArray<int32> Adopted;
		for (int32 Node = 1; Node < Nodes.Num(); ++Node)
		{
			if (Nodes[Node].Parent == ParentNode && RigIsFixtureDescendant(*Target, Nodes[Node].Name, Bone))
			{
				Adopted.Add(Node);
			}
		}
		const FVector ParentPosition = Nodes[ParentNode].Component.GetTranslation();
		FVector Position;
		if (Adopted.Num() > 0)
		{
			FVector Sum = FVector::ZeroVector;
			for (const int32 Node : Adopted)
			{
				Sum += Nodes[Node].Component.GetTranslation();
			}
			Position = FMath::Lerp(ParentPosition, Sum / Adopted.Num(), 0.5);
		}
		else if (Bone == FName(TEXT("ball_l")) || Bone == FName(TEXT("ball_r")))
		{
			const double AnkleHeight = FMath::Max(ParentPosition.Z, 2.0);
			Position = ParentPosition + FVector(0.0, AnkleHeight, -0.75 * AnkleHeight);
		}
		else
		{
			const int32 Grand = Nodes[ParentNode].Parent;
			const FVector Along = Grand != INDEX_NONE ? ParentPosition - Nodes[Grand].Component.GetTranslation() : FVector(0.0, 0.0, 10.0);
			Position = ParentPosition + Along * 0.5;
		}
		const int32 NewNode = Nodes.Add(FPlanNode{ Bone, ParentNode, FTransform(Nodes[ParentNode].Component.GetRotation(), Position), INDEX_NONE, false, true });
		for (const int32 Node : Adopted)
		{
			Nodes[Node].Parent = NewNode;
		}
		Out.Inserted.Add(Bone);
	}

	// Parent-first order from the root, children in the order they were made.
	TArray<TArray<int32>> Children;
	Children.SetNum(Nodes.Num());
	for (int32 Node = 1; Node < Nodes.Num(); ++Node)
	{
		Children[Nodes[Node].Parent].Add(Node);
	}
	TArray<int32> Order;
	TArray<int32> Stack = { 0 };
	while (Stack.Num() > 0)
	{
		const int32 Node = Stack.Pop(EAllowShrinking::No);
		Order.Add(Node);
		for (int32 Child = Children[Node].Num() - 1; Child >= 0; --Child)
		{
			Stack.Add(Children[Node][Child]);
		}
	}
	TArray<int32> IndexOfNode;
	IndexOfNode.Init(INDEX_NONE, Nodes.Num());
	for (int32 Index = 0; Index < Order.Num(); ++Index)
	{
		IndexOfNode[Order[Index]] = Index;
	}
	for (const int32 NodeIndex : Order)
	{
		const FPlanNode& Node = Nodes[NodeIndex];
		FMantleRigPlanBone& Bone = Out.Bones.AddDefaulted_GetRef();
		Bone.Name = Node.Name;
		Bone.Parent = Node.Parent == INDEX_NONE ? INDEX_NONE : IndexOfNode[Node.Parent];
		Bone.Component = Node.Component;
		Bone.Local = Bone.Parent == INDEX_NONE ? Node.Component : Node.Component.GetRelativeTransform(Out.Bones[Bone.Parent].Component);
		Bone.Local.SetScale3D(FVector::OneVector);
		Bone.Local.NormalizeRotation();
		Bone.Source = Node.Source;
		Bone.bMapped = Node.bMapped;
		Bone.bInserted = Node.bInserted;
		Out.NumMapped += Node.bMapped ? 1 : 0;
	}

	// Each rig bone's weights: its own bone; a dropped bone's go to its nearest kept ancestor, the pelvis's ancestors' to
	// the pelvis.
	Out.NewIndexOfSource.Init(INDEX_NONE, NumSource);
	Out.WeightsOfSource.Init(0, NumSource);
	const int32 PelvisIndex = IndexOfNode[NodeOfSource[PelvisSource]];
	for (int32 Bone = 0; Bone < NumSource; ++Bone)
	{
		if (NodeOfSource[Bone] != INDEX_NONE)
		{
			Out.NewIndexOfSource[Bone] = IndexOfNode[NodeOfSource[Bone]];
			Out.WeightsOfSource[Bone] = Out.NewIndexOfSource[Bone];
			continue;
		}
		if (AbovePelvis[Bone])
		{
			Out.WeightsOfSource[Bone] = PelvisIndex;
			continue;
		}
		for (int32 Ancestor = Bones[Bone].Parent; Ancestor != INDEX_NONE; Ancestor = Bones[Ancestor].Parent)
		{
			if (NodeOfSource[Ancestor] != INDEX_NONE)
			{
				Out.WeightsOfSource[Bone] = IndexOfNode[NodeOfSource[Ancestor]];
				break;
			}
		}
	}
	return true;
}

bool FMantleAvatarRigMap::VerifyPlan(TConstArrayView<FMantleRigBone> Bones, const FMantleRigPlan& Plan, const FMantleAvatarContract& Contract, FString& OutError,
	const bool bGarment)
{
	FMantleGlbMesh Glb;
	Glb.NumInfluences = 1;
	TArray<FTransform> Pose;
	TArray<bool> Weighted;
	Weighted.Init(false, Plan.Bones.Num());
	for (const FMantleRigPlanBone& Bone : Plan.Bones)
	{
		FMantleGlbJoint& Joint = Glb.Joints.AddDefaulted_GetRef();
		Joint.Name = Bone.Name;
		Joint.Parent = Bone.Parent;
		Joint.Local = Bone.Local;
		Pose.Add(Bone.Component);
	}
	for (int32 Bone = 0; Bone < Bones.Num(); ++Bone)
	{
		if (Bones[Bone].bWeighted && Plan.WeightsOfSource.IsValidIndex(Bone) && Weighted.IsValidIndex(Plan.WeightsOfSource[Bone]))
		{
			Weighted[Plan.WeightsOfSource[Bone]] = true;
		}
	}
	FMantleSkinFit Fit;
	FString Error;
	const FMantleContractSkeleton* Skeleton = FMantleCosmeticChecks::FitSkinToClass(Glb, Pose, Weighted, bGarment ? Contract.SkinnedGarment : Contract.AvatarBody,
		Contract, true, Fit, Error);
	if (!Skeleton)
	{
		OutError = Error;
		return false;
	}
	if (!Skeleton->Id.Equals(Plan.TargetSkeletonId, ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(TEXT("the converted rig would upload as the %s skeleton, not the %s one"), *SkeletonDisplayName(Skeleton->Id),
			*SkeletonDisplayName(Plan.TargetSkeletonId));
		return false;
	}
	return true;
}

bool FMantleAvatarRigMap::IsUE4MannequinGarment(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract)
{
	const TArray<FString>* Required = Contract.AvatarRequiredBones.Find(TEXT("ue4mannequin"));
	if (!Required || Required->Num() == 0)
	{
		return false;
	}
	TSet<FName> Names;
	for (const FMantleRigBone& Bone : Bones)
	{
		Names.Add(Bone.Name);
	}
	for (const FString& Bone : *Required)
	{
		if (!Names.Contains(FName(*Bone)))
		{
			return false;
		}
	}
	return !Names.Contains(FName(TEXT("spine_04"))) && !Names.Contains(FName(TEXT("spine_05")));
}

FMantleRigMapping FMantleAvatarRigMap::UE4MannequinOntoMetaHuman(TConstArrayView<FMantleRigBone> Bones, const FMantleAvatarContract& Contract)
{
	using namespace MantleAvatarRigMapPrivate;
	FMantleRigMapping Out;
	Out.ProfileId = TEXT("ue4mannequin");
	Out.ProfileName = SkeletonDisplayName(TEXT("ue4mannequin"));
	Out.TargetSkeletonId = TEXT("metahuman");
	const FMantleContractSkeleton* Target = Contract.FindSkeleton(Out.TargetSkeletonId);
	if (!Target)
	{
		return Out;
	}
	TSet<FName> Required;
	for (const FString& Bone : RigRequiredWithoutRoot(Contract, Out.TargetSkeletonId))
	{
		Required.Add(FName(*Bone));
	}
	// Three spine bones over the MetaHuman's five, by height: the chest one carries the clavicles and the neck.
	const TMap<FName, FName> Spine = {
		{ FName(TEXT("spine_01")), FName(TEXT("spine_02")) },
		{ FName(TEXT("spine_02")), FName(TEXT("spine_04")) },
		{ FName(TEXT("spine_03")), FName(TEXT("spine_05")) } };
	for (const FMantleRigBone& Bone : Bones)
	{
		const FName* Onto = Spine.Find(Bone.Name);
		const FName Name = Onto ? *Onto : Bone.Name;
		if (Target->Find(Name) != INDEX_NONE && Name != Target->Root())
		{
			Out.Set(Name, Bone.Name, Required.Contains(Name));
		}
	}
	return Out;
}

USkeletalMesh* FMantleAvatarRigMap::ApplyPlan(USkeletalMesh* Source, const FMantleRigPlan& Plan, const FString& OutputFolder,
	TArray<UObject*>& OutCreated, FString& OutError)
{
	using namespace MantleAvatarRigMapPrivate;
	OutCreated.Reset();
	if (!Source)
	{
		OutError = TEXT("no mesh to convert");
		return nullptr;
	}
	const FReferenceSkeleton& SourceReference = Source->GetRefSkeleton();
	if (Plan.Bones.Num() == 0 || Plan.NewIndexOfSource.Num() != SourceReference.GetRawBoneNum())
	{
		OutError = TEXT("the bone mapping was made for another mesh; analyse again");
		return nullptr;
	}
	FString Folder = OutputFolder;
	while (Folder.EndsWith(TEXT("/")))
	{
		Folder.LeftChopInline(1);
	}
	if (Folder.IsEmpty() || !FPackageName::IsValidLongPackageName(Folder / Source->GetName(), /*bIncludeReadOnlyRoots*/ true))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a folder the converted assets can go in"), *OutputFolder);
		return nullptr;
	}

	TArray<USkinnedAsset*> Compiling = { Source };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);

	IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
	FString MeshPackageName;
	FString MeshName;
	AssetTools.CreateUniqueAssetName(Folder / Source->GetName(), FString(), MeshPackageName, MeshName);
	FString SkeletonPackageName;
	FString SkeletonName;
	AssetTools.CreateUniqueAssetName(Folder / (Source->GetName() + TEXT("_Skeleton")), FString(), SkeletonPackageName, SkeletonName);

	const auto Discard = [&OutCreated]()
	{
		for (UObject* Object : OutCreated)
		{
			if (Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}
		}
		OutCreated.Reset();
	};

	UPackage* MeshPackage = CreatePackage(*MeshPackageName);
	USkeletalMesh* Mesh = MeshPackage ? DuplicateObject<USkeletalMesh>(Source, MeshPackage, FName(*MeshName)) : nullptr;
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("%s could not be copied to %s"), *Source->GetName(), *MeshPackageName);
		return nullptr;
	}
	OutCreated.Add(Mesh);
	Mesh->ClearFlags(RF_Transient);
	Mesh->SetFlags(RF_Public | RF_Standalone);
	Compiling = { Mesh };
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	Mesh->SetMaterials(Source->GetMaterials());

	// The new bone table and its bind matrices.
	FReferenceSkeleton Reference;
	{
		FReferenceSkeletonModifier Modifier(Reference, nullptr);
		for (const FMantleRigPlanBone& Bone : Plan.Bones)
		{
			Modifier.Add(FMeshBoneInfo(Bone.Name, Bone.Name.ToString(), Bone.Parent), Bone.Local);
		}
	}
	Mesh->SetRefSkeleton(Reference);
	Mesh->GetRefBasesInvMatrix().Reset();
	Mesh->CalculateInvRefMatrices();

	// Every LOD with source geometry; a generated LOD is generated again from its base when the mesh builds.
	bool bConvertedLOD0 = false;
	for (int32 LODIndex = 0; LODIndex < Source->GetLODNum(); ++LODIndex)
	{
		FMeshDescription Description;
		if (!RigReadSourceGeometry(Source, LODIndex, Description))
		{
			continue;
		}
		RigRemapMeshDescription(Description, Plan);
		Mesh->CreateMeshDescription(LODIndex, MoveTemp(Description));
		Mesh->CommitMeshDescription(LODIndex);
		bConvertedLOD0 = bConvertedLOD0 || LODIndex == 0;
	}
	if (!bConvertedLOD0)
	{
		Discard();
		OutError = FString::Printf(TEXT("%s has no source geometry to convert; reimport it and try again"), *Source->GetName());
		return nullptr;
	}

	// Settings that name bones, and the mesh's sockets.
	// Per LOD through GetLODInfo: GetLODInfoArray is deprecated in 5.8.
	for (int32 LODIndex = 0; LODIndex < Mesh->GetLODNum(); ++LODIndex)
	{
		if (FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(LODIndex))
		{
			RigRenameBoneReferences(Info->BonesToRemove, SourceReference, Plan);
			RigRenameBoneReferences(Info->BonesToPrioritize, SourceReference, Plan);
		}
	}
	for (USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList())
	{
		RigMoveSocket(Socket, SourceReference, Plan);
	}
	Mesh->RebuildSocketMap();

	// A skeleton of its own, with the source skeleton's sockets moved onto it.
	UPackage* SkeletonPackage = CreatePackage(*SkeletonPackageName);
	USkeleton* Skeleton = SkeletonPackage ? NewObject<USkeleton>(SkeletonPackage, FName(*SkeletonName), RF_Public | RF_Standalone) : nullptr;
	if (!Skeleton)
	{
		Discard();
		OutError = FString::Printf(TEXT("the skeleton %s could not be created"), *SkeletonPackageName);
		return nullptr;
	}
	OutCreated.Add(Skeleton);
	if (!Skeleton->MergeAllBonesToBoneTree(Mesh, false))
	{
		Discard();
		OutError = TEXT("the converted bones could not make a skeleton");
		return nullptr;
	}
	if (const USkeleton* SourceSkeleton = Source->GetSkeleton())
	{
		for (const TObjectPtr<USkeletalMeshSocket>& Socket : SourceSkeleton->Sockets)
		{
			USkeletalMeshSocket* Copy = Socket ? DuplicateObject<USkeletalMeshSocket>(Socket.Get(), Skeleton) : nullptr;
			if (Copy && RigMoveSocket(Copy, SourceReference, Plan))
			{
				Skeleton->Sockets.Add(Copy);
			}
			else if (Copy)
			{
				Copy->MarkAsGarbage();
			}
		}
	}
	Mesh->SetSkeleton(Skeleton);
	// The physics asset and post-process animation name the old bones.
	Mesh->SetPhysicsAsset(nullptr);
	Mesh->SetShadowPhysicsAsset(nullptr);
	Mesh->SetPostProcessAnimBlueprint(TSubclassOf<UAnimInstance>());

	Mesh->PostEditChange();
	FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
	Skeleton->SetPreviewMesh(Mesh);

	FAssetRegistryModule::AssetCreated(Mesh);
	FAssetRegistryModule::AssetCreated(Skeleton);
	MeshPackage->MarkPackageDirty();
	SkeletonPackage->MarkPackageDirty();
	return Mesh;
}

void FMantleAvatarRigMap::SetSessionMapping(const USkeletalMesh* Mesh, const FMantleRigMapping& Mapping)
{
	if (Mesh)
	{
		MantleAvatarRigMapPrivate::SessionMappings().Add(MantleAvatarRigMapPrivate::SessionKey(Mesh), Mapping);
	}
}

const FMantleRigMapping* FMantleAvatarRigMap::FindSessionMapping(const USkeletalMesh* Mesh)
{
	return Mesh ? MantleAvatarRigMapPrivate::SessionMappings().Find(MantleAvatarRigMapPrivate::SessionKey(Mesh)) : nullptr;
}

void RegisterMantleAvatarRigMapFixer()
{
	FMantleAvatarFixers::Register(MakeShared<MantleAvatarRigMapPrivate::FMantleAvatarRigMapFixer>());
}
