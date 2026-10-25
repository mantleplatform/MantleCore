// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyOffsetsBuilder.h"
#include "MantleAvatarConverter.h"
#include "MantleRuntime.h"

#include "DNAReader.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterBodyIdentity.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterIdentity.h"
#include "MetaHumanRigEvaluatedState.h"
#include "SkelMeshDNAUtils.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBodyOffsetsBuilderPrivate
{
	/** The grid cell of a point; cells are wider than any tolerance worth asking for, so a match is in the 27 around. */
	FIntVector CellOf(const FVector3f& P, float CellCm)
	{
		return FIntVector(FMath::FloorToInt(P.X / CellCm), FMath::FloorToInt(P.Y / CellCm), FMath::FloorToInt(P.Z / CellCm));
	}

	/** One LOD out of the state's concatenated vertex array. */
	TConstArrayView<FVector3f> Slice(const TArray<FVector3f>& All, const int32 Start, const int32 Count)
	{
		return TConstArrayView<FVector3f>(All.GetData() + Start, Count);
	}
}

bool FMantleBodyOffsetsBuilder::LearnVertexMap(const TConstArrayView<FVector3f> Built, const TConstArrayView<FVector3f> StateLod,
	TArray<int32>& OutMap, FString& OutError, const float TolCm)
{
	using namespace MantleBodyOffsetsBuilderPrivate;

	OutMap.Reset();
	if (Built.Num() == 0 || StateLod.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%d built vertices against %d state vertices; nothing to match"), Built.Num(), StateLod.Num());
		return false;
	}
	const float CellCm = FMath::Max(TolCm * 4.0f, 0.01f);

	TMap<FIntVector, TArray<int32>> Grid;
	Grid.Reserve(StateLod.Num());
	TArray<FVector3f> Mapped;
	Mapped.Reserve(StateLod.Num());
	for (int32 Index = 0; Index < StateLod.Num(); ++Index)
	{
		const FVector3f P = StateToBuilt(StateLod[Index]);
		Mapped.Add(P);
		Grid.FindOrAdd(CellOf(P, CellCm)).Add(Index);
	}

	OutMap.Reserve(Built.Num());
	for (int32 Vertex = 0; Vertex < Built.Num(); ++Vertex)
	{
		const FVector3f& P = Built[Vertex];
		const FIntVector Cell = CellOf(P, CellCm);
		int32 Best = INDEX_NONE;
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DZ = -1; DZ <= 1; ++DZ)
				{
					const TArray<int32>* Neighbours = Grid.Find(FIntVector(Cell.X + DX, Cell.Y + DY, Cell.Z + DZ));
					if (!Neighbours)
					{
						continue;
					}
					for (const int32 Candidate : *Neighbours)
					{
						if (FVector3f::Distance(Mapped[Candidate], P) > TolCm)
						{
							continue;
						}
						if (Best == INDEX_NONE)
						{
							Best = Candidate;
						}
						else if (Mapped[Candidate] != Mapped[Best])
						{
							OutMap.Reset();
							OutError = FString::Printf(TEXT("built vertex %d matches state vertices %d and %d at different positions (within %g cm)"),
								Vertex, FMath::Min(Best, Candidate), FMath::Max(Best, Candidate), TolCm);
							return false;
						}
						else
						{
							Best = FMath::Min(Best, Candidate);
						}
					}
				}
			}
		}
		if (Best == INDEX_NONE)
		{
			OutMap.Reset();
			OutError = FString::Printf(TEXT("built vertex %d (%g, %g, %g) has no state vertex within %g cm"), Vertex, P.X, P.Y, P.Z, TolCm);
			return false;
		}
		OutMap.Add(Best);
	}
	return true;
}

TArray<FVector3f> FMantleBodyOffsetsBuilder::PositionDeltas(const TConstArrayView<FVector3f> NormalState,
	const TConstArrayView<FVector3f> ShapeState, const TConstArrayView<int32> Map)
{
	TArray<FVector3f> Out;
	Out.Reserve(Map.Num());
	for (const int32 StateIndex : Map)
	{
		Out.Add(StateToBuilt(ShapeState[StateIndex] - NormalState[StateIndex]));
	}
	return Out;
}

float FMantleBodyOffsetsBuilder::StripVerticalOffset(const TConstArrayView<FVector3f> NormalLod0, TArray<TArray<FVector3f>>& LodDeltas,
	const TConstArrayView<int32> Parents, TArray<FVector3f>& LocalJointDeltas)
{
	if (LodDeltas.Num() == 0 || NormalLod0.Num() == 0 || LodDeltas[0].Num() != NormalLod0.Num())
	{
		return 0.0f;
	}
	float NormalMin = NormalLod0[0].Z;
	float ShapeMin = NormalLod0[0].Z + LodDeltas[0][0].Z;
	for (int32 Vertex = 1; Vertex < NormalLod0.Num(); ++Vertex)
	{
		NormalMin = FMath::Min(NormalMin, NormalLod0[Vertex].Z);
		ShapeMin = FMath::Min(ShapeMin, NormalLod0[Vertex].Z + LodDeltas[0][Vertex].Z);
	}
	const float Shift = ShapeMin - NormalMin;
	ShiftVertical(Shift, LodDeltas, Parents, LocalJointDeltas);
	return Shift;
}

bool FMantleBodyOffsetsBuilder::ComposeComponentRotations(const TConstArrayView<int32> Parents, const TConstArrayView<FQuat4f> LocalRotations,
	TArray<FQuat4f>& OutComponentRotations, FString& OutError)
{
	OutComponentRotations.Reset();
	if (Parents.Num() != LocalRotations.Num())
	{
		OutError = FString::Printf(TEXT("%d parents for %d rotations"), Parents.Num(), LocalRotations.Num());
		return false;
	}
	OutComponentRotations.Reserve(Parents.Num());
	for (int32 Joint = 0; Joint < Parents.Num(); ++Joint)
	{
		const int32 Parent = Parents[Joint];
		if (Parent == INDEX_NONE)
		{
			OutComponentRotations.Add(LocalRotations[Joint]);
		}
		else if (Parent < 0 || Parent >= Joint)
		{
			OutComponentRotations.Reset();
			OutError = FString::Printf(TEXT("joint %d's parent %d does not come before it"), Joint, Parent);
			return false;
		}
		else
		{
			OutComponentRotations.Add((OutComponentRotations[Parent] * LocalRotations[Joint]).GetNormalized());
		}
	}
	return true;
}

bool FMantleBodyOffsetsBuilder::LocalJointDeltas(const TConstArrayView<int32> Parents, const TConstArrayView<FQuat4f> ComponentRotations,
	const TConstArrayView<FVector3f> ComponentDeltas, TArray<FVector3f>& OutLocalDeltas, FString& OutError)
{
	OutLocalDeltas.Reset();
	if (Parents.Num() != ComponentRotations.Num() || Parents.Num() != ComponentDeltas.Num())
	{
		OutError = FString::Printf(TEXT("%d parents, %d rotations and %d deltas; they must agree"),
			Parents.Num(), ComponentRotations.Num(), ComponentDeltas.Num());
		return false;
	}
	OutLocalDeltas.Reserve(Parents.Num());
	for (int32 Joint = 0; Joint < Parents.Num(); ++Joint)
	{
		const int32 Parent = Parents[Joint];
		if (Parent == INDEX_NONE)
		{
			OutLocalDeltas.Add(ComponentDeltas[Joint]);
		}
		else if (Parent < 0 || Parent >= Parents.Num())
		{
			OutLocalDeltas.Reset();
			OutError = FString::Printf(TEXT("joint %d's parent %d is not a joint"), Joint, Parent);
			return false;
		}
		else
		{
			OutLocalDeltas.Add(ComponentRotations[Parent].UnrotateVector(ComponentDeltas[Joint] - ComponentDeltas[Parent]));
		}
	}
	return true;
}

bool FMantleBodyOffsetsBuilder::LearnNearestVertexMap(const TConstArrayView<FVector3f> Built, const TConstArrayView<FVector3f> StateAll,
	TArray<int32>& OutMap, FString& OutError, const float TolCm, int32* OutExact, float* OutWorstCm)
{
	using namespace MantleBodyOffsetsBuilderPrivate;

	OutMap.Reset();
	if (OutExact)
	{
		*OutExact = 0;
	}
	if (OutWorstCm)
	{
		*OutWorstCm = 0.0f;
	}
	if (Built.Num() == 0 || StateAll.Num() == 0)
	{
		OutError = FString::Printf(TEXT("%d built vertices against %d state vertices; nothing to match"), Built.Num(), StateAll.Num());
		return false;
	}
	// A cell as wide as the reach, so the nearest vertex within it is in the 27 cells around the point.
	const float CellCm = FMath::Max(TolCm, 0.01f);
	TMap<FIntVector, TArray<int32>> Grid;
	Grid.Reserve(StateAll.Num());
	TArray<FVector3f> Mapped;
	Mapped.Reserve(StateAll.Num());
	for (int32 Index = 0; Index < StateAll.Num(); ++Index)
	{
		const FVector3f P = StateToBuilt(StateAll[Index]);
		Mapped.Add(P);
		Grid.FindOrAdd(CellOf(P, CellCm)).Add(Index);
	}

	int32 Exact = 0;
	float Worst = 0.0f;
	OutMap.Reserve(Built.Num());
	for (int32 Vertex = 0; Vertex < Built.Num(); ++Vertex)
	{
		const FVector3f& P = Built[Vertex];
		const FIntVector Cell = CellOf(P, CellCm);
		int32 Best = INDEX_NONE;
		float BestDistance = TNumericLimits<float>::Max();
		for (int32 DX = -1; DX <= 1; ++DX)
		{
			for (int32 DY = -1; DY <= 1; ++DY)
			{
				for (int32 DZ = -1; DZ <= 1; ++DZ)
				{
					const TArray<int32>* Neighbours = Grid.Find(FIntVector(Cell.X + DX, Cell.Y + DY, Cell.Z + DZ));
					if (!Neighbours)
					{
						continue;
					}
					for (const int32 Candidate : *Neighbours)
					{
						const float Distance = FVector3f::Distance(Mapped[Candidate], P);
						if (Distance < BestDistance || (Distance == BestDistance && Candidate < Best))
						{
							Best = Candidate;
							BestDistance = Distance;
						}
					}
				}
			}
		}
		if (Best == INDEX_NONE || BestDistance > TolCm)
		{
			OutMap.Reset();
			OutError = FString::Printf(TEXT("built vertex %d (%g, %g, %g) has no state vertex within %g cm (the nearest is %g cm away)"), Vertex, P.X, P.Y, P.Z,
				TolCm, Best == INDEX_NONE ? -1.0f : BestDistance);
			return false;
		}
		Exact += BestDistance <= 0.001f ? 1 : 0;
		Worst = FMath::Max(Worst, BestDistance);
		OutMap.Add(Best);
	}
	if (OutExact)
	{
		*OutExact = Exact;
	}
	if (OutWorstCm)
	{
		*OutWorstCm = Worst;
	}
	return true;
}

void FMantleBodyOffsetsBuilder::ShiftVertical(const float Shift, TArray<TArray<FVector3f>>& LodDeltas, const TConstArrayView<int32> Parents,
	TArray<FVector3f>& LocalJointDeltas)
{
	for (TArray<FVector3f>& Lod : LodDeltas)
	{
		for (FVector3f& Delta : Lod)
		{
			Delta.Z -= Shift;
		}
	}
	for (int32 Joint = 0; Joint < LocalJointDeltas.Num() && Joint < Parents.Num(); ++Joint)
	{
		if (Parents[Joint] == INDEX_NONE)
		{
			LocalJointDeltas[Joint].Z -= Shift;
		}
	}
}

int32 FMantleBodyOffsetsBuilder::ComponentDeltasByName(const TConstArrayView<FName> JointNames, const TConstArrayView<int32> Parents,
	const TMap<FName, FVector3f>& Named, TArray<FVector3f>& OutComponentDeltas)
{
	OutComponentDeltas.Reset();
	if (JointNames.Num() != Parents.Num())
	{
		return INDEX_NONE;
	}
	int32 Taken = 0;
	OutComponentDeltas.Reserve(Parents.Num());
	for (int32 Joint = 0; Joint < Parents.Num(); ++Joint)
	{
		const int32 Parent = Parents[Joint];
		if (Parent != INDEX_NONE && (Parent < 0 || Parent >= Joint))
		{
			OutComponentDeltas.Reset();
			return INDEX_NONE;
		}
		if (const FVector3f* Delta = Named.Find(JointNames[Joint]))
		{
			OutComponentDeltas.Add(*Delta);
			++Taken;
		}
		else
		{
			// A copy first: Add may grow the array the parent's delta lives in.
			const FVector3f Inherited = Parent == INDEX_NONE ? FVector3f::ZeroVector : OutComponentDeltas[Parent];
			OutComponentDeltas.Add(Inherited);
		}
	}
	return Taken;
}

bool FMantleBodyOffsetsBuilder::ComposeComponentPositions(const TConstArrayView<int32> Parents, const TConstArrayView<FTransform> Locals,
	TArray<FVector3f>& OutComponentPositions, FString& OutError)
{
	OutComponentPositions.Reset();
	if (Parents.Num() != Locals.Num())
	{
		OutError = FString::Printf(TEXT("%d parents for %d local transforms"), Parents.Num(), Locals.Num());
		return false;
	}
	TArray<FTransform> Component;
	Component.Reserve(Parents.Num());
	for (int32 Joint = 0; Joint < Parents.Num(); ++Joint)
	{
		const int32 Parent = Parents[Joint];
		if (Parent == INDEX_NONE)
		{
			Component.Add(Locals[Joint]);
		}
		else if (Parent < 0 || Parent >= Joint)
		{
			OutError = FString::Printf(TEXT("joint %d's parent %d does not come before it"), Joint, Parent);
			return false;
		}
		else
		{
			Component.Add(Locals[Joint] * Component[Parent]);
		}
	}
	OutComponentPositions.Reserve(Component.Num());
	for (const FTransform& Transform : Component)
	{
		OutComponentPositions.Add(FVector3f(Transform.GetTranslation()));
	}
	return true;
}

FName FMantleBodyOffsetsBuilder::ShapeConstraint(const FString& ShapeName)
{
	if (ShapeName.StartsWith(TEXT("weight")))
	{
		return FName(TEXT("Fat"));
	}
	if (ShapeName.StartsWith(TEXT("muscle")))
	{
		return FName(TEXT("Muscularity"));
	}
	if (ShapeName.StartsWith(TEXT("chest")))
	{
		return FName(TEXT("Chest"));
	}
	if (ShapeName.StartsWith(TEXT("hips")))
	{
		return FName(TEXT("Hip"));
	}
	return NAME_None;
}

float FMantleBodyOffsetsBuilder::ShapeTargetMeasurement(const FString& ShapeName, const float Current, const float Min, const float Max)
{
	const bool bPlus = ShapeName.EndsWith(TEXT("+"));
	float Target = Current;
	if (ShapeName.StartsWith(TEXT("weight")) || ShapeName.StartsWith(TEXT("muscle")))
	{
		// The slider's end is the constraint's own end: Creator's Fat and Muscularity are unitless scales, not centimetres.
		Target = bPlus ? Max : Min;
	}
	else if (ShapeName.StartsWith(TEXT("chest")))
	{
		Target = Current + (bPlus ? 24.0f : -24.0f);
	}
	else if (ShapeName.StartsWith(TEXT("hips")))
	{
		Target = Current + (bPlus ? 20.0f : -20.0f);
	}
	return FMath::Clamp(Target, Min, Max);
}

UMetaHumanCharacter* FMantleBodyOffsetsBuilder::FindCharacterForBuild(const USkeletalMesh* BuiltBody, FString& OutError)
{
	const FString* Built = BuiltBody ? BuiltBody->GetPackage()->GetMetaData().FindValue(BuiltBody, FMantleAvatarConverter::BuiltKey) : nullptr;
	if (!Built || Built->IsEmpty())
	{
		OutError = TEXT("the body carries no converter build stamp, so the MetaHuman Creator character it was built from is not known");
		return nullptr;
	}
	// The stamp is the build's assembly folder: <character folder>/Unpacked/<character name> (FMantleMetaHumanBuild::Build).
	static const FString Marker = TEXT("/Unpacked/");
	const int32 At = Built->Find(Marker, ESearchCase::CaseSensitive);
	if (At == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("the body's build stamp \"%s\" is not a MetaHuman Creator build folder"), **Built);
		return nullptr;
	}
	const FString CharacterFolder = Built->Left(At);
	FString Name = Built->Mid(At + Marker.Len());
	int32 Slash = INDEX_NONE;
	if (Name.FindChar(TEXT('/'), Slash))
	{
		Name.LeftInline(Slash);
	}
	const FString Path = FString::Printf(TEXT("%s/%s.%s"), *CharacterFolder, *Name, *Name);
	UMetaHumanCharacter* Character = LoadObject<UMetaHumanCharacter>(nullptr, *Path);
	if (!Character)
	{
		OutError = FString::Printf(TEXT("the MetaHuman Creator character %s it was built from is no longer there"), *Path);
	}
	return Character;
}

FMantleBodyOffsetsBuilder::EBuildResult FMantleBodyOffsetsBuilder::BuildForCharacter(UMetaHumanCharacter* Character, const USkeletalMesh* BuiltBody,
	const FTarget& Body, const FTarget* Face, TArray<FMantleBodyOffsetShape>& OutBody, TArray<FMantleBodyOffsetShape>& OutFace, FString& OutMessage)
{
	using namespace MantleBodyOffsetsBuilderPrivate;

	OutBody.Reset();
	OutFace.Reset();
	const auto Fail = [&OutBody, &OutFace, &OutMessage](const FString& Message)
	{
		OutBody.Reset();
		OutFace.Reset();
		OutMessage = Message;
		return EBuildResult::Failed;
	};
	const auto Valid = [](const FTarget& Target)
	{
		return Target.LODs.Num() > 0 && Target.LODs[0].Num() > 0 && Target.JointNames.Num() > 0 && Target.JointNames.Num() == Target.Parents.Num()
			&& Target.JointNames.Num() == Target.Locals.Num();
	};
	if (!Character)
	{
		return Fail(TEXT("there is no MetaHuman Creator character to read the body shapes from"));
	}
	if (!Valid(Body) || (Face && !Valid(*Face)))
	{
		return Fail(TEXT("the exported body or face has no vertices or no skin to put the shapes on"));
	}
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	if (!Subsystem)
	{
		return Fail(TEXT("MetaHuman Creator's editor subsystem is unavailable"));
	}
	const double StartSeconds = FPlatformTime::Seconds();

	// Opened for reading only when it is not open already; every state below is a copy, and nothing is committed or saved.
	const bool bWasEditing = Subsystem->IsObjectAddedForEditing(Character);
	if (!bWasEditing && !Subsystem->TryAddObjectToEdit(Character))
	{
		return Fail(TEXT("MetaHuman Creator could not open the character"));
	}
	ON_SCOPE_EXIT
	{
		if (!bWasEditing)
		{
			Subsystem->RemoveObjectToEdit(Character);
		}
	};
	if (Character->bFixedBodyType)
	{
		OutMessage = TEXT("this MetaHuman's body is a fixed type (imported from a DNA or a compatibility body), which has no weight, muscle, chest or hips settings");
		return EBuildResult::Skipped;
	}

	const TSharedRef<FMetaHumanCharacterBodyIdentity::FState> Normal = Subsystem->CopyBodyState(Character);
	const TArray<FMetaHumanCharacterBodyConstraint> Base = Normal->GetBodyConstraints();
	const FMetaHumanRigEvaluatedState NormalGeo = Normal->GetVerticesAndVertexNormals();
	const TArray<int32> StateLODs = Normal->GetNumVerticesPerLOD();
	const TArray<FVector3f> NormalJoints = Normal->GetJointTranslations();
	TArray<int32> LodStart;
	int32 TotalVertices = 0;
	for (const int32 Count : StateLODs)
	{
		LodStart.Add(TotalVertices);
		TotalVertices += Count;
	}
	if (StateLODs.Num() == 0 || TotalVertices > NormalGeo.Vertices.Num())
	{
		return Fail(FString::Printf(TEXT("Creator's body state has %d vertices for LODs that add up to %d"), NormalGeo.Vertices.Num(), TotalVertices));
	}
	FString StateLodText;
	for (const int32 Count : StateLODs)
	{
		StateLodText += (StateLodText.IsEmpty() ? TEXT("") : TEXT("/")) + FString::FromInt(Count);
	}

	// Body: built LOD k is the state's LOD k+1 (LOD0 of the state is the full-resolution body the build drops). That is
	// proven for built LOD0; the lower LODs are tried at the expected state LOD first and then at the others, and the
	// text says which one matched so a different layout shows up in the log rather than as a wrong body.
	TArray<TArray<int32>> BodyMaps;
	TArray<int32> BodyStateLod;
	FString BodyMapText;
	for (int32 Lod = 0; Lod < Body.LODs.Num(); ++Lod)
	{
		TArray<int32> Order;
		if (StateLODs.IsValidIndex(Lod + 1))
		{
			Order.Add(Lod + 1);
		}
		for (int32 Candidate = 0; Candidate < StateLODs.Num(); ++Candidate)
		{
			Order.AddUnique(Candidate);
		}
		TArray<int32> Map;
		FString LastError;
		int32 Matched = INDEX_NONE;
		for (const int32 Candidate : Order)
		{
			if (StateLODs[Candidate] < Body.LODs[Lod].Num())
			{
				continue;
			}
			if (LearnVertexMap(Body.LODs[Lod], Slice(NormalGeo.Vertices, LodStart[Candidate], StateLODs[Candidate]), Map, LastError))
			{
				Matched = Candidate;
				break;
			}
		}
		if (Matched == INDEX_NONE)
		{
			return Fail(FString::Printf(TEXT("the body's LOD%d (%d vertices) matches none of Creator's body LODs (%s vertices): %s"), Lod, Body.LODs[Lod].Num(),
				*StateLodText, *LastError));
		}
		BodyMaps.Add(MoveTemp(Map));
		BodyStateLod.Add(Matched);
		BodyMapText += FString::Printf(TEXT("%sLOD%d=state%d%s"), BodyMapText.IsEmpty() ? TEXT("") : TEXT(" "), Lod, Matched, Matched == Lod + 1 ? TEXT("") : TEXT("(unexpected)"));
	}

	// Face: the face owns the neck, which Creator refits to the body, so its map is to the nearest vertex of the committed
	// face state, and its shapes come from refitting that face to each reshaped body.
	const auto RefitFace = [Subsystem, Character](const FMetaHumanCharacterBodyIdentity::FState& BodyState, const FMetaHumanRigEvaluatedState& BodyGeo)
	{
		// What UMetaHumanCharacterEditorSubsystem::UpdateFaceFromBodyInternal does after a body change, on a copy.
		const TSharedRef<FMetaHumanCharacterIdentity::FState> FaceState = Subsystem->CopyFaceState(Character);
		FaceState->SetBodyJointsAndBodyFaceVertices(BodyState.CopyComponentPose(), BodyGeo.Vertices);
		FaceState->SetBodyVertexNormals(BodyGeo.VertexNormals, BodyState.GetNumVerticesPerLOD());
		return FaceState->Evaluate().Vertices;
	};
	TArray<TArray<int32>> FaceMaps;
	TArray<FVector3f> FaceNormal;
	FString FaceMapText;
	if (Face)
	{
		const TArray<FVector3f> Committed = Subsystem->CopyFaceState(Character)->Evaluate().Vertices;
		for (int32 Lod = 0; Lod < Face->LODs.Num(); ++Lod)
		{
			TArray<int32> Map;
			FString MapError;
			int32 Exact = 0;
			float Worst = 0.0f;
			// Up to 1 cm: the built face is Creator's face to about a millimetre, but on some presets a few vertices sit a few
			// millimetres off every state vertex (Aera: 0.39 cm). The shapes change smoothly over the face, so a vertex that
			// near takes the same move; the note reports the worst match.
			if (!LearnNearestVertexMap(Face->LODs[Lod], Committed, Map, MapError, 1.0f, &Exact, &Worst))
			{
				return Fail(FString::Printf(TEXT("the face's LOD%d (%d vertices) does not follow Creator's face (%d vertices): %s"), Lod, Face->LODs[Lod].Num(),
					Committed.Num(), *MapError));
			}
			FaceMaps.Add(MoveTemp(Map));
			FaceMapText += FString::Printf(TEXT("%sLOD%d %d/%d exact, worst %.3f cm"), FaceMapText.IsEmpty() ? TEXT("") : TEXT("; "), Lod, Exact, Face->LODs[Lod].Num(), Worst);
		}
		// The deltas below are refit(reshaped) - refit(normal), so both sides go through the same call; the committed face
		// is only used for the map. Their disagreement is logged: it should be nothing.
		FaceNormal = RefitFace(*Normal, NormalGeo);
		float Drift = 0.0f;
		for (int32 Index = 0; Index < FaceNormal.Num() && Index < Committed.Num(); ++Index)
		{
			Drift = FMath::Max(Drift, FVector3f::Distance(FaceNormal[Index], Committed[Index]));
		}
		FaceMapText += FString::Printf(TEXT("; refit vs committed face %.3f cm"), Drift);
	}

	// Joints: the state's joints are the rig's DNA joints in DNA order; their names (the body edit mesh carries the DNA)
	// are what ties them to the exported skins.
	TSharedPtr<IDNAReader> Dna = USkelMeshDNAUtils::GetDNAReader(const_cast<USkeletalMesh*>(static_cast<const USkeletalMesh*>(Subsystem->GetBodyEditMesh(Character))));
	if (!Dna.IsValid() && BuiltBody)
	{
		Dna = USkelMeshDNAUtils::GetDNAReader(const_cast<USkeletalMesh*>(BuiltBody));
	}
	if (!Dna.IsValid())
	{
		return Fail(TEXT("the body's rig (DNA) is not available to name Creator's joints"));
	}
	if (static_cast<int32>(Dna->GetJointCount()) != NormalJoints.Num())
	{
		return Fail(FString::Printf(TEXT("Creator's body state has %d joints and its DNA names %d"), NormalJoints.Num(), static_cast<int32>(Dna->GetJointCount())));
	}
	TArray<FName> DnaNames;
	for (int32 Joint = 0; Joint < NormalJoints.Num(); ++Joint)
	{
		DnaNames.Add(FName(*Dna->GetJointName(static_cast<uint16>(Joint))));
	}
	FString Error;
	TArray<FVector3f> BodyComponent;
	if (!ComposeComponentPositions(Body.Parents, Body.Locals, BodyComponent, Error))
	{
		return Fail(FString::Printf(TEXT("the body's skin: %s"), *Error));
	}
	// The frame check: a named joint of the exported rest pose is where Creator's normal state has it (axes swapped), or the
	// joint deltas below would be in the wrong frame.
	{
		TMap<FName, int32> BodyIndex;
		for (int32 Joint = 0; Joint < Body.JointNames.Num(); ++Joint)
		{
			BodyIndex.Add(Body.JointNames[Joint], Joint);
		}
		int32 Compared = 0;
		float Worst = 0.0f;
		FName WorstName;
		for (int32 Joint = 0; Joint < DnaNames.Num(); ++Joint)
		{
			if (const int32* Exported = BodyIndex.Find(DnaNames[Joint]))
			{
				++Compared;
				const float Distance = FVector3f::Distance(BodyComponent[*Exported], StateToBuilt(NormalJoints[Joint]));
				if (Distance > Worst)
				{
					Worst = Distance;
					WorstName = DnaNames[Joint];
				}
			}
		}
		if (Compared == 0)
		{
			return Fail(TEXT("no bone of the exported body skeleton is named like a joint of Creator's rig"));
		}
		if (Worst > 1.0f)
		{
			return Fail(FString::Printf(TEXT("Creator's joints are not where the exported rest pose has them (%s is %.2f cm off, of %d compared)"), *WorstName.ToString(), Worst, Compared));
		}
	}
	const auto Rotations = [](const FTarget& Target)
	{
		TArray<FQuat4f> Out;
		Out.Reserve(Target.Locals.Num());
		for (const FTransform& Local : Target.Locals)
		{
			Out.Add(FQuat4f(Local.GetRotation()));
		}
		return Out;
	};
	TArray<FQuat4f> BodyRotations;
	TArray<FQuat4f> FaceRotations;
	if (!ComposeComponentRotations(Body.Parents, Rotations(Body), BodyRotations, Error)
		|| (Face && !ComposeComponentRotations(Face->Parents, Rotations(*Face), FaceRotations, Error)))
	{
		return Fail(FString::Printf(TEXT("the skin's rest pose: %s"), *Error));
	}
	// A skin's LOCAL joint deltas for the named component-space ones; OutTaken is how many joints the rig named.
	const auto SkinJoints = [&Error](const FTarget& Target, const TArray<FQuat4f>& ComponentRotations, const TMap<FName, FVector3f>& Named,
		TArray<FVector3f>& OutLocal, int32& OutTaken)
	{
		TArray<FVector3f> Component;
		OutTaken = ComponentDeltasByName(Target.JointNames, Target.Parents, Named, Component);
		return OutTaken != INDEX_NONE && LocalJointDeltas(Target.Parents, ComponentRotations, Component, OutLocal, Error);
	};

	static const TCHAR* const ShapeNames[] = { TEXT("weight+"), TEXT("weight-"), TEXT("muscle+"), TEXT("muscle-"), TEXT("chest+"), TEXT("chest-"), TEXT("hips+"), TEXT("hips-") };
	for (const TCHAR* ShapeNameText : ShapeNames)
	{
		const FString ShapeName(ShapeNameText);
		const double ShapeStart = FPlatformTime::Seconds();
		const FName ConstraintName = ShapeConstraint(ShapeName);
		const int32 ConstraintIndex = Base.IndexOfByPredicate([ConstraintName](const FMetaHumanCharacterBodyConstraint& C) { return C.Name == ConstraintName; });
		if (ConstraintIndex == INDEX_NONE)
		{
			return Fail(FString::Printf(TEXT("Creator's body has no \"%s\" setting for the %s shape"), *ConstraintName.ToString(), *ShapeName));
		}
		const float Current = Normal->GetMeasurement(ConstraintIndex);
		const float Target = ShapeTargetMeasurement(ShapeName, Current, Base[ConstraintIndex].MinMeasurement, Base[ConstraintIndex].MaxMeasurement);

		// Every setting the preset saved is released except Height (held where it is): they would otherwise hold the body
		// against this one (Fat 2.8 grew a preset's waist 64.5 -> 71 cm, not to ~103), then this one is set.
		TArray<FMetaHumanCharacterBodyConstraint> Constraints = Base;
		for (int32 Index = 0; Index < Constraints.Num(); ++Index)
		{
			Constraints[Index].bIsActive = Constraints[Index].Name == FName(TEXT("Height"));
			Constraints[Index].TargetMeasurement = Normal->GetMeasurement(Index);
		}
		Constraints[ConstraintIndex].bIsActive = true;
		Constraints[ConstraintIndex].TargetMeasurement = Target;
		const TSharedRef<FMetaHumanCharacterBodyIdentity::FState> Shaped = MakeShared<FMetaHumanCharacterBodyIdentity::FState>(*Normal);
		Shaped->EvaluateBodyConstraints(Constraints);
		const FMetaHumanRigEvaluatedState ShapedGeo = Shaped->GetVerticesAndVertexNormals();
		const TArray<FVector3f> ShapedJoints = Shaped->GetJointTranslations();
		if (ShapedGeo.Vertices.Num() != NormalGeo.Vertices.Num() || ShapedJoints.Num() != NormalJoints.Num())
		{
			return Fail(FString::Printf(TEXT("the %s shape changed the body state's size (%d vertices, %d joints)"), *ShapeName, ShapedGeo.Vertices.Num(), ShapedJoints.Num()));
		}

		TMap<FName, FVector3f> Named;
		for (int32 Joint = 0; Joint < DnaNames.Num(); ++Joint)
		{
			Named.Add(DnaNames[Joint], StateToBuilt(ShapedJoints[Joint] - NormalJoints[Joint]));
		}

		FMantleBodyOffsetShape& BodyShape = OutBody.AddDefaulted_GetRef();
		BodyShape.Name = ShapeName;
		for (int32 Lod = 0; Lod < Body.LODs.Num(); ++Lod)
		{
			const int32 StateLod = BodyStateLod[Lod];
			BodyShape.LODs.Add(PositionDeltas(Slice(NormalGeo.Vertices, LodStart[StateLod], StateLODs[StateLod]),
				Slice(ShapedGeo.Vertices, LodStart[StateLod], StateLODs[StateLod]), BodyMaps[Lod]));
		}
		int32 BodyTaken = 0;
		if (!SkinJoints(Body, BodyRotations, Named, BodyShape.Joints, BodyTaken))
		{
			return Fail(FString::Printf(TEXT("the %s shape's body joints: %s"), *ShapeName, *Error));
		}

		FMantleBodyOffsetShape* FaceShape = nullptr;
		int32 FaceTaken = 0;
		if (Face)
		{
			const TArray<FVector3f> FaceShaped = RefitFace(*Shaped, ShapedGeo);
			if (FaceShaped.Num() != FaceNormal.Num())
			{
				return Fail(FString::Printf(TEXT("the %s shape changed the face state's size (%d vertices, was %d)"), *ShapeName, FaceShaped.Num(), FaceNormal.Num()));
			}
			FaceShape = &OutFace.AddDefaulted_GetRef();
			FaceShape->Name = ShapeName;
			for (int32 Lod = 0; Lod < Face->LODs.Num(); ++Lod)
			{
				FaceShape->LODs.Add(PositionDeltas(FaceNormal, FaceShaped, FaceMaps[Lod]));
			}
			// Bones the face shares with the body (spine_04, the neck, the head) take the body's move; its facial joints
			// follow the head, so they come out zero in local terms.
			if (!SkinJoints(*Face, FaceRotations, Named, FaceShape->Joints, FaceTaken))
			{
				return Fail(FString::Printf(TEXT("the %s shape's face joints: %s"), *ShapeName, *Error));
			}
		}

		// The body's rule keeps the feet on the floor; the face takes the same shift so the neck still meets the body.
		const float Shift = StripVerticalOffset(Body.LODs[0], BodyShape.LODs, Body.Parents, BodyShape.Joints);
		if (FaceShape)
		{
			ShiftVertical(Shift, FaceShape->LODs, Face->Parents, FaceShape->Joints);
		}
		const auto MaxMove = [](const TArray<FVector3f>& Deltas)
		{
			float Longest = 0.0f;
			for (const FVector3f& Delta : Deltas)
			{
				Longest = FMath::Max(Longest, Delta.Size());
			}
			return Longest;
		};
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_OFFSETS shape=%s %s %.2f -> %.2f; body max move %.2f cm (LOD0), face max move %.2f cm, floor shift %.2f cm removed, joints named body %d/%d face %d/%d, %.0f ms"),
			*ShapeName, *ConstraintName.ToString(), Current, Target, MaxMove(BodyShape.LODs[0]), FaceShape ? MaxMove(FaceShape->LODs[0]) : 0.0f, Shift,
			BodyTaken, Body.JointNames.Num(), FaceTaken, Face ? Face->JointNames.Num() : 0, (FPlatformTime::Seconds() - ShapeStart) * 1000.0);
	}

	OutMessage = FString::Printf(TEXT("Body shapes (weight, muscle, chest, hips): 8 on the body (%s)%s, %.1f s"), *BodyMapText,
		Face ? *FString::Printf(TEXT(" and the face (%s)"), *FaceMapText) : TEXT(""), FPlatformTime::Seconds() - StartSeconds);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_OFFSETS %s; Creator body LODs %s"), *OutMessage, *StateLodText);
	return EBuildResult::Built;
}
