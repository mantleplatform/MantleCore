// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeadFit.h"

#include "Algo/AllOf.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MantleCosmeticExport.h"
#include "MantleGlbWriter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleHeadFit, Log, All);

namespace MantleHeadFitPrivate
{
	static const FName HatSocketName(TEXT("acc_hat"));
	static const FName HeadBoneName(TEXT("head"));
	// Below this many head vertices the box is not a head (a helmet-less stub, a broken skin).
	static constexpr int32 MinVertices = 200;
	// Percentiles per axis: a few stray vertices (a lash, a seam) do not move the box.
	static constexpr float LowPercentile = 0.005f;
	static constexpr float HighPercentile = 0.995f;
	// A vertex with more than this much weight on an ear is ear, not skull: a hat sits on the skull, so ears
	// (a goblin's or an elf's that stick out far) must not widen the box.
	static constexpr float MaxOnEar = 0.05f;
	// The front of the box is the forehead: only vertices at least this far up the cranium (of its height above the
	// joint) count for it. Below that the face (brow, nose, lips) sticks out further, but a hat's front edge rests on
	// the forehead (about 9 to 10 cm up on the reference head, the cranium being 16.9 cm tall).
	static constexpr float ForeheadFromHeight = 0.55f;

	/** The value at Fraction (0 to 1) of Values once sorted; Values is sorted in place. */
	static float Percentile(TArray<float>& Values, const float Fraction)
	{
		Values.Sort();
		const int32 Last = Values.Num() - 1;
		return Values[FMath::Clamp(Fraction < 0.5f ? FMath::FloorToInt32(Last * Fraction) : FMath::CeilToInt32(Last * Fraction), 0, Last)];
	}

	/** Appends Mesh's cranium vertices, in the socket's frame, to Out. */
	static bool AddCranium(const USkeletalMesh* Mesh, const FTransform& SocketInHead, TArray<FVector3f>& Out, FString& OutError)
	{
		FMantleGlbMesh Glb;
		TArray<UMaterialInterface*> Materials;
		int32 Triangles = 0;
		if (!FMantleCosmeticExport::ReadSkeletalMesh(Mesh, 0, Glb, Materials, Triangles, OutError))
		{
			return false;
		}
		const int32 Head = Glb.Joints.IndexOfByPredicate([](const FMantleGlbJoint& Joint) { return Joint.Name.IsEqual(HeadBoneName, ENameCase::IgnoreCase); });
		if (Head == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s has no head bone"), *Mesh->GetName());
			return false;
		}
		// Joints are parent-first: one pass gives each its bind pose and whether it is the head or under it.
		TArray<FTransform> Component;
		TArray<bool> UnderHead;
		TArray<bool> Ear;
		Component.SetNum(Glb.Joints.Num());
		UnderHead.SetNum(Glb.Joints.Num());
		Ear.SetNum(Glb.Joints.Num());
		for (int32 Index = 0; Index < Glb.Joints.Num(); ++Index)
		{
			const int32 Parent = Glb.Joints[Index].Parent;
			Component[Index] = Parent == INDEX_NONE ? Glb.Joints[Index].Local : Glb.Joints[Index].Local * Component[Parent];
			UnderHead[Index] = Index == Head || (Parent != INDEX_NONE && UnderHead[Parent]);
			Ear[Index] = UnderHead[Index] && FMantleHeadFit::IsEarBone(Glb.Joints[Index].Name);
		}
		const FTransform Frame = SocketInHead * Component[Head];
		const double JointZ = Component[Head].GetLocation().Z;
		const int32 Stride = Glb.NumInfluences;
		for (int32 Vertex = 0; Vertex < Glb.Positions.Num(); ++Vertex)
		{
			float OnHead = 0.0f;
			float OnEar = 0.0f;
			for (int32 Slot = 0; Slot < Stride; ++Slot)
			{
				const int32 Joint = Glb.InfluenceJoints[Vertex * Stride + Slot];
				if (UnderHead.IsValidIndex(Joint) && UnderHead[Joint])
				{
					OnHead += Glb.InfluenceWeights[Vertex * Stride + Slot];
					OnEar += Ear[Joint] ? Glb.InfluenceWeights[Vertex * Stride + Slot] : 0.0f;
				}
			}
			const FVector Position(Glb.Positions[Vertex]);
			if (!FMantleHeadFit::IsCranium(OnHead, OnEar) || Position.Z < JointZ)
			{
				continue;
			}
			const FVector Local = Frame.InverseTransformPositionNoScale(Position);
			Out.Add(FVector3f(Local));
		}
		return true;
	}

	static FString ReferencePath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
		return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("AvatarConvert"), TEXT("head-reference.json")) : FString();
	}

	static bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector3f& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
		{
			return false;
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Out[Axis] = static_cast<float>((*Values)[Axis]->AsNumber());
		}
		return true;
	}
}

bool FMantleHeadFit::MeasureCranium(const USkeletalMesh* Body, const USkeletalMesh* Face, const FTransform& SocketInHead,
	FBox3f& OutBox, int32& OutVertices, FString& OutError)
{
	using namespace MantleHeadFitPrivate;
	OutBox = FBox3f(ForceInit);
	OutVertices = 0;
	TArray<FVector3f> Points;
	FString BodyError;
	FString FaceError;
	const bool bBody = Body && AddCranium(Body, SocketInHead, Points, BodyError);
	const bool bFace = Face && AddCranium(Face, SocketInHead, Points, FaceError);
	OutVertices = Points.Num();
	if ((!bBody && !bFace) || OutVertices < MinVertices)
	{
		OutError = !bBody && !bFace ? FString::Printf(TEXT("the head could not be measured (%s)"), *(BodyError.IsEmpty() ? FaceError : BodyError))
			: FString::Printf(TEXT("the head could not be measured: only %d vertices sit on the head above its joint"), OutVertices);
		return false;
	}
	OutBox = CraniumBox(Points);
	return true;
}

FBox3f FMantleHeadFit::CraniumBox(const TConstArrayView<FVector3f> Points)
{
	using namespace MantleHeadFitPrivate;
	FBox3f Box(ForceInit);
	if (Points.IsEmpty())
	{
		return Box;
	}
	TArray<float> Axis;
	Axis.Reserve(Points.Num());
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Axis.Reset();
		for (const FVector3f& Point : Points) { Axis.Add(Point[Index]); }
		Box.Min[Index] = Percentile(Axis, LowPercentile);
		Box.Max[Index] = Percentile(Axis, HighPercentile);
	}
	// The front (+Y) from the forehead up only, so a long nose or a jutting brow does not deepen the box.
	const float Forehead = ForeheadFromHeight * Box.Max.X;
	Axis.Reset();
	for (const FVector3f& Point : Points)
	{
		if (Point.X >= Forehead) { Axis.Add(Point.Y); }
	}
	if (!Axis.IsEmpty())
	{
		Box.Max.Y = FMath::Max(Box.Min.Y, Percentile(Axis, HighPercentile));
	}
	Box.IsValid = 1;
	return Box;
}

bool FMantleHeadFit::IsEarBone(const FName& Bone)
{
	// MetaHuman faces skin their ears to FACIAL_L_Ear, FACIAL_L_Ear1 to Ear4 and the same on the right: a name
	// part that is "ear" with only digits after it. Parts, not a substring, so Forehead or Nearside never match.
	TArray<FString> Parts;
	Bone.ToString().ParseIntoArray(Parts, TEXT("_"));
	for (const FString& Part : Parts)
	{
		if (Part.Len() >= 3 && Part.Left(3).Equals(TEXT("ear"), ESearchCase::IgnoreCase)
			&& Algo::AllOf(Part.Mid(3), [](const TCHAR Character) { return FChar::IsDigit(Character); }))
		{
			return true;
		}
	}
	return false;
}

bool FMantleHeadFit::IsCranium(const float OnHead, const float OnEar)
{
	return OnHead >= 0.5f && OnEar <= MantleHeadFitPrivate::MaxOnEar;
}

bool FMantleHeadFit::HeadPose(const USkeletalMesh* Mesh, FTransform& OutPose)
{
	if (!Mesh)
	{
		return false;
	}
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	int32 Bone = INDEX_NONE;
	for (int32 Index = 0; Index < Skeleton.GetRawBoneNum(); ++Index)
	{
		if (Skeleton.GetBoneName(Index).IsEqual(MantleHeadFitPrivate::HeadBoneName, ENameCase::IgnoreCase))
		{
			Bone = Index;
			break;
		}
	}
	if (Bone == INDEX_NONE)
	{
		return false;
	}
	OutPose = FTransform::Identity;
	for (int32 Index = Bone; Index != INDEX_NONE; Index = Skeleton.GetRawParentIndex(Index))
	{
		OutPose = OutPose * Skeleton.GetRawRefBonePose()[Index];
	}
	return true;
}

bool FMantleHeadFit::ReferenceCranium(FBox3f& OutBox)
{
	using namespace MantleHeadFitPrivate;
	FString Text;
	const FString Path = ReferencePath();
	TSharedPtr<FJsonObject> Root;
	if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Cranium = nullptr;
	FVector3f Min;
	FVector3f Max;
	if (!Root->TryGetObjectField(TEXT("cranium"), Cranium) || !ReadVector(*Cranium, TEXT("min"), Min) || !ReadVector(*Cranium, TEXT("max"), Max))
	{
		return false;
	}
	OutBox = FBox3f(Min, Max);
	return OutBox.IsValid && OutBox.GetSize().GetMin() > 1.0f;
}

FMantleHeadFit::FFit FMantleHeadFit::FitTo(const FBox3f& Reference, const FBox3f& Measured)
{
	FFit Fit;
	const FVector3f From = Reference.GetSize();
	const FVector3f To = Measured.GetSize();
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Fit.Scale[Axis] = From[Axis] > KINDA_SMALL_NUMBER ? FMath::Clamp(To[Axis] / From[Axis], 0.5f, 2.0f) : 1.0f;
		// The boxes' centres meet: with a clamped scale the head is still centred under the hat.
		Fit.Offset[Axis] = Measured.GetCenter()[Axis] - Fit.Scale[Axis] * Reference.GetCenter()[Axis];
	}
	return Fit;
}

FMantleCosmeticSocketOverride FMantleHeadFit::HatSocket(const FBox3f& Reference, const FBox3f& Measured, const FTransform& SocketInHead)
{
	const FFit Fit = FitTo(Reference, Measured);
	FMantleCosmeticSocketOverride Override;
	Override.Socket = MantleHeadFitPrivate::HatSocketName.ToString();
	Override.Bone = MantleHeadFitPrivate::HeadBoneName.ToString();
	// A hat point u lands at socket(Scale * u + Offset): the socket's rotation stays, its location takes the offset.
	Override.bLocation = true;
	Override.bScale = true;
	Override.Location = FVector3f(SocketInHead.TransformPositionNoScale(FVector(Fit.Offset)));
	Override.Scale = Fit.Scale;
	return Override;
}

FTransform FMantleHeadFit::HatSocketInHead(const USkeletalMesh* Body)
{
	const USkeletalMeshSocket* Socket = Body ? Body->FindSocket(MantleHeadFitPrivate::HatSocketName) : nullptr;
	if (!Socket || !Socket->BoneName.IsEqual(MantleHeadFitPrivate::HeadBoneName, ENameCase::IgnoreCase))
	{
		return FTransform::Identity;
	}
	FTransform Local = Socket->GetSocketLocalTransform();
	Local.SetScale3D(FVector::OneVector);
	return Local;
}

FString FMantleHeadFit::Describe(const FBox3f& Box)
{
	const FVector3f Size = Box.GetSize();
	return FString::Printf(TEXT("%.1f x %.1f x %.1f cm"), Size.X, Size.Y, Size.Z);
}
