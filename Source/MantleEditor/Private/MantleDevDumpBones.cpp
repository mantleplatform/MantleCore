// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.DumpBones <OutJson> <body> <bone> [<bone> ...]
// For authoring test accessories on their bones (Joshua, 2026-11-24: a piece is designed for the bone it hangs from, at zero
// offset). <body> is a standard body id (m_med_nrw, f_med_nrw ...), made in memory (FMantleFitBodies) with its MetaHuman face.
// For each bone, on the mesh that has it (the face first: the #136 face slots hang from its FACIAL_* bones; else the body), writes
// its reference pose in component space (cm; Unreal axes: X to the body's left, Y forward, Z up): location, rotation (quaternion
// x, y, z, w) and the bone's own X, Y and Z axes, and the skin point of that mesh nearest the bone (LOD0, its position, its normal
// and the distance), so a modelling script can build a piece in the bone's frame, its pivot on the bone, touching the skin.
// Reads only; logs MANTLE_DUMP_BONES lines (each bone, then "done" or "failed").
#include "MantleFitBodies.h"
#include "MantleSkinnedMesh.h"

#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleDumpBones, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevDumpBonesPrivate
{
	TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const double Value : Values) Out.Add(MakeShared<FJsonValueNumber>(Value));
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> Vector(const FVector& V)
	{
		return Numbers({ V.X, V.Y, V.Z });
	}

	/** Bone's reference pose in Mesh's component space; false when Mesh lacks it. */
	bool ComponentPose(const USkeletalMesh* Mesh, const FName Bone, FTransform& Out)
	{
		if (!Mesh) return false;
		const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
		const TArray<FTransform>& Pose = Reference.GetRefBonePose();
		int32 Index = Reference.FindBoneIndex(Bone);
		if (!Pose.IsValidIndex(Index)) return false;
		Out = Pose[Index];
		while ((Index = Reference.GetParentIndex(Index)) != INDEX_NONE) Out = Out * Pose[Index];
		return true;
	}

	void Run(const TArray<FString>& Args)
	{
		UE_LOG(LogMantleDumpBones, Display, TEXT("MANTLE_DUMP_BONES start"));
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantleDumpBones, Error, TEXT("MANTLE_DUMP_BONES failed: usage: Mantle.Dev.DumpBones <OutJson> <body> <bone> [<bone> ...]"));
			return;
		}
		EMetaHumanBodyType Type;
		FMantleFitBody Body;
		FString Error;
		if (!FMantleFitBodies::ParseBodyType(Args[1], Type) || !FMantleFitBodies::Make(Args[1], Type, Body, Error) || !Body.BodyMesh.IsValid())
		{
			UE_LOG(LogMantleDumpBones, Error, TEXT("MANTLE_DUMP_BONES failed: the body %s could not be made (%s)"), *Args[1], *Error);
			return;
		}
		ON_SCOPE_EXIT { FMantleFitBodies::Release(Body); };
		FMantleSkinnedMesh BodySkin, FaceSkin;
		FMantleSkinnedMesh::ExtractFromSkeletalMesh(Body.BodyMesh.Get(), BodySkin, &Error);
		if (Body.FaceMesh.IsValid()) FMantleSkinnedMesh::ExtractFromSkeletalMesh(Body.FaceMesh.Get(), FaceSkin, &Error);

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("body"), Args[1]);
		Root->SetStringField(TEXT("axes"), TEXT("Unreal component space, cm: X to the body's left, Y forward, Z up"));
		TSharedRef<FJsonObject> Bones = MakeShared<FJsonObject>();
		int32 Missing = 0;
		for (int32 Index = 2; Index < Args.Num(); ++Index)
		{
			const FName Name(*Args[Index]);
			FTransform Pose;
			const bool bFace = ComponentPose(Body.FaceMesh.Get(), Name, Pose);
			if (!bFace && !ComponentPose(Body.BodyMesh.Get(), Name, Pose))
			{
				UE_LOG(LogMantleDumpBones, Error, TEXT("MANTLE_DUMP_BONES %s: on neither the face nor the body"), *Args[Index]);
				++Missing;
				continue;
			}
			// The skin of that mesh nearest the bone.
			const FMantleSkinnedMesh& Skin = bFace ? FaceSkin : BodySkin;
			int32 Nearest = INDEX_NONE;
			double Best = TNumericLimits<double>::Max();
			if (Skin.LODs.Num() > 0)
			{
				const TArray<FVector3f>& Positions = Skin.LODs[0].Positions;
				for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
				{
					const double D = FVector::DistSquared(FVector(Positions[Vertex]), Pose.GetLocation());
					if (D < Best) { Best = D; Nearest = Vertex; }
				}
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("mesh"), bFace ? TEXT("face") : TEXT("body"));
			Entry->SetArrayField(TEXT("location"), Vector(Pose.GetLocation()));
			const FQuat Q = Pose.GetRotation();
			Entry->SetArrayField(TEXT("rotation"), Numbers({ Q.X, Q.Y, Q.Z, Q.W }));
			Entry->SetArrayField(TEXT("axisX"), Vector(Q.GetAxisX()));
			Entry->SetArrayField(TEXT("axisY"), Vector(Q.GetAxisY()));
			Entry->SetArrayField(TEXT("axisZ"), Vector(Q.GetAxisZ()));
			if (Nearest != INDEX_NONE)
			{
				const FMantleSkinnedMeshLOD& LOD = Skin.LODs[0];
				Entry->SetArrayField(TEXT("nearestSkin"), Vector(FVector(LOD.Positions[Nearest])));
				if (LOD.Normals.IsValidIndex(Nearest)) Entry->SetArrayField(TEXT("nearestSkinNormal"), Vector(FVector(LOD.Normals[Nearest])));
				Entry->SetNumberField(TEXT("nearestSkinCm"), FMath::Sqrt(Best));
			}
			Bones->SetObjectField(Args[Index], Entry);
			UE_LOG(LogMantleDumpBones, Display, TEXT("MANTLE_DUMP_BONES %s on the %s: at %s, nearest skin %.2f cm"), *Args[Index], bFace ? TEXT("face") : TEXT("body"),
				*Pose.GetLocation().ToString(), Nearest != INDEX_NONE ? FMath::Sqrt(Best) : -1.0);
		}
		Root->SetObjectField(TEXT("bones"), Bones);
		FString Text;
		if (!FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text)) || !FFileHelper::SaveStringToFile(Text, *Args[0]))
		{
			UE_LOG(LogMantleDumpBones, Error, TEXT("MANTLE_DUMP_BONES failed: %s could not be written"), *Args[0]);
			return;
		}
		UE_LOG(LogMantleDumpBones, Display, TEXT("MANTLE_DUMP_BONES %s %s"), Missing > 0 ? TEXT("failed") : TEXT("done"), *Args[0]);
	}

	FAutoConsoleCommand GMantleDevDumpBonesCommand(
		TEXT("Mantle.Dev.DumpBones"),
		TEXT("Mantle.Dev.DumpBones <OutJson> <body> <bone> ... -- writes each bone's reference pose on a standard body (its face first) and the skin nearest it, ")
		TEXT("for authoring test pieces on their bones. Reads only."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
