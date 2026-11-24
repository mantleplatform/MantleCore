// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairHatSource.h"

#include "MantleAccessoryPlacement.h"
#include "MantleHairHeadFit.h"
#include "MantleRuntimeMesh.h"
#include "MantleStaticMeshReader.h"
#include "Misc/Crc.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleHairHatSourcePrivate
{
	void HashBytes(uint32& Crc, const void* Data, const int32 Length)
	{
		Crc = FCrc::MemCrc32(Data, Length, Crc);
	}

	void HashDouble(uint32& Crc, const double Value)
	{
		// -0 and 0 are the same placement.
		const double Clean = Value == 0.0 ? 0.0 : Value;
		HashBytes(Crc, &Clean, sizeof(Clean));
	}

	void HashVector(uint32& Crc, const FVector& Value)
	{
		HashDouble(Crc, Value.X);
		HashDouble(Crc, Value.Y);
		HashDouble(Crc, Value.Z);
	}

	/** Which parts are set, then the set ones. */
	void HashPlacement(uint32& Crc, const FMantleCosmeticTransform& Value)
	{
		const uint8 Flags = (Value.Location.IsSet() ? 1 : 0) | (Value.Rotation.IsSet() ? 2 : 0) | (Value.Scale.IsSet() ? 4 : 0);
		HashBytes(Crc, &Flags, sizeof(Flags));
		if (Value.Location.IsSet())
		{
			HashVector(Crc, Value.Location.GetValue());
		}
		if (Value.Rotation.IsSet())
		{
			HashDouble(Crc, Value.Rotation.GetValue().Pitch);
			HashDouble(Crc, Value.Rotation.GetValue().Yaw);
			HashDouble(Crc, Value.Rotation.GetValue().Roll);
		}
		if (Value.Scale.IsSet())
		{
			HashVector(Crc, Value.Scale.GetValue());
		}
	}

	const TCHAR* RuleName(const EMantleHairHatRule Rule)
	{
		switch (Rule)
		{
		case EMantleHairHatRule::HideAll: return TEXT("hideAll");
		case EMantleHairHatRule::LeaveAlone: return TEXT("leaveAlone");
		default: return TEXT("underIt");
		}
	}

	bool IsFiniteVector(const FVector3f& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}

	/**
	 * A LOD with no triangle at all is not a mesh (FMantleSkinnedMesh::Validate), and a hat that takes every triangle of a hair
	 * should leave nothing to see. So one triangle stays, its three corners made tiny and put at the head centre, inside the head.
	 * LOD is the edited LOD with no indices left; the originals say which triangle and section that was.
	 */
	void KeepOneHiddenTriangle(FMantleSkinnedMeshLOD& LOD, const TArray<uint32>& OriginalIndices, const TArray<FMantleSkinnedMeshSection>& OriginalSections,
		const FVector3f& Centre)
	{
		if (OriginalIndices.Num() < 3 || LOD.Sections.IsEmpty())
		{
			return;
		}
		const uint32 A = OriginalIndices[0];
		const uint32 B = OriginalIndices[1];
		const uint32 C = OriginalIndices[2];
		const uint32 NumVertices = static_cast<uint32>(LOD.Positions.Num());
		if (A >= NumVertices || B >= NumVertices || C >= NumVertices)
		{
			return;
		}
		LOD.Indices.Append(OriginalIndices.GetData(), 3);
		// The section that drew that triangle holds it; the ones after it start past it.
		int32 Owner = 0;
		for (int32 SectionIndex = 0; SectionIndex < OriginalSections.Num(); ++SectionIndex)
		{
			if (OriginalSections[SectionIndex].FirstIndex == 0 && OriginalSections[SectionIndex].IndexCount > 0)
			{
				Owner = SectionIndex;
				break;
			}
		}
		Owner = FMath::Min(Owner, LOD.Sections.Num() - 1);
		for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
		{
			LOD.Sections[SectionIndex].FirstIndex = SectionIndex > Owner ? 3 : 0;
			LOD.Sections[SectionIndex].IndexCount = SectionIndex == Owner ? 3 : 0;
		}
		LOD.Positions[A] = Centre;
		LOD.Positions[B] = Centre + FVector3f(0.01f, 0.0f, 0.0f);
		LOD.Positions[C] = Centre + FVector3f(0.0f, 0.01f, 0.0f);
	}
}

FName FMantleHairHood::HeadBoneName()
{
	return FName(TEXT("head"));
}

bool FMantleHairHood::Extract(const FMantleSkinnedMesh& Fitted, FMantleHairHood& Out)
{
	Out.Positions.Reset();
	Out.Indices.Reset();
	if (!Fitted.LODs.IsValidIndex(0))
	{
		return false;
	}
	const FName HeadBone = HeadBoneName();
	const int32 Head = Fitted.Bones.IndexOfByPredicate([HeadBone](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == HeadBone; });
	if (Head == INDEX_NONE)
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Fitted.LODs[0];
	const int32 NumVertices = LOD.NumVertices();
	const int32 Slots = LOD.NumInfluences;
	// A mesh whose skin data does not agree with its vertices is not read (Validate keeps this true for every fitted mesh).
	if (Slots < 1 || LOD.InfluenceBones.Num() != NumVertices * Slots || LOD.InfluenceWeights.Num() != NumVertices * Slots)
	{
		return false;
	}
	// A vertex is head when its weights on the head bone, over all its slots, are MinHeadWeight of the 255 or more.
	TArray<uint8> IsHead;
	IsHead.Init(0, NumVertices);
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		uint32 HeadWeight = 0;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			if (LOD.InfluenceBones[Vertex * Slots + Slot] == Head)
			{
				HeadWeight += LOD.InfluenceWeights[Vertex * Slots + Slot];
			}
		}
		IsHead[Vertex] = static_cast<float>(HeadWeight) / 255.0f >= MinHeadWeight ? 1 : 0;
	}
	// A triangle is the hood's when all three corners are head; the vertices it uses are renumbered in order of first use.
	TArray<int32> Remap;
	Remap.Init(INDEX_NONE, NumVertices);
	for (int32 Corner = 0; Corner + 2 < LOD.Indices.Num(); Corner += 3)
	{
		const uint32 A = LOD.Indices[Corner];
		const uint32 B = LOD.Indices[Corner + 1];
		const uint32 C = LOD.Indices[Corner + 2];
		if (A >= static_cast<uint32>(NumVertices) || B >= static_cast<uint32>(NumVertices) || C >= static_cast<uint32>(NumVertices)
			|| IsHead[A] == 0 || IsHead[B] == 0 || IsHead[C] == 0)
		{
			continue;
		}
		for (const uint32 Source : { A, B, C })
		{
			if (Remap[Source] == INDEX_NONE)
			{
				Remap[Source] = Out.Positions.Add(LOD.Positions[Source]);
			}
			Out.Indices.Add(static_cast<uint32>(Remap[Source]));
		}
	}
	return !Out.Indices.IsEmpty();
}

TSharedPtr<const FMantleHairHood, ESPMode::ThreadSafe> FMantleHairHood::Make(const FMantleSkinnedMesh& Fitted)
{
	TSharedRef<FMantleHairHood, ESPMode::ThreadSafe> Hood = MakeShared<FMantleHairHood, ESPMode::ThreadSafe>();
	if (!Extract(Fitted, *Hood))
	{
		return nullptr;
	}
	return Hood;
}

EMantleHairHatRule FMantleHairHatSource::RuleFromText(const FString& Text)
{
	if (Text.Equals(TEXT("hideAll"), ESearchCase::CaseSensitive))
	{
		return EMantleHairHatRule::HideAll;
	}
	if (Text.Equals(TEXT("leaveAlone"), ESearchCase::CaseSensitive))
	{
		return EMantleHairHatRule::LeaveAlone;
	}
	return EMantleHairHatRule::UnderIt;
}

uint32 FMantleHairHatSource::PlacementHash() const
{
	using namespace MantleHairHatSourcePrivate;
	uint32 Crc = 0;
	HashBytes(Crc, *SlotId, SlotId.Len() * static_cast<int32>(sizeof(TCHAR)));
	HashPlacement(Crc, Attach);
	const uint8 HasOverride = SocketOverride.IsSet() ? 1 : 0;
	HashBytes(Crc, &HasOverride, sizeof(HasOverride));
	if (SocketOverride.IsSet())
	{
		HashPlacement(Crc, SocketOverride.GetValue());
	}
	HashVector(Crc, SocketLocal.GetTranslation());
	const FQuat Rotation = SocketLocal.GetRotation();
	HashDouble(Crc, Rotation.X);
	HashDouble(Crc, Rotation.Y);
	HashDouble(Crc, Rotation.Z);
	HashDouble(Crc, Rotation.W);
	HashVector(Crc, SocketLocal.GetScale3D());
	const FString Bone = BoneName.ToString();
	HashBytes(Crc, *Bone, Bone.Len() * static_cast<int32>(sizeof(TCHAR)));
	return Crc;
}

FString FMantleHairHatSource::KeyPiece() const
{
	if (Hood.IsValid())
	{
		// A hood is placed by its own fit, which MeshHash names; the wearer is in the hair's key already.
		return FString::Printf(TEXT("%s=%s/%s/hood"), *SlotId, *MeshHash, MantleHairHatSourcePrivate::RuleName(Rule));
	}
	return FString::Printf(TEXT("%s=%s/%s/%08x"), *SlotId, *MeshHash, MantleHairHatSourcePrivate::RuleName(Rule), PlacementHash());
}

FString FMantleHairHatSource::KeyFor(const TConstArrayView<FMantleHairHatSource> Sources, const EMantleHairHatAim Aim)
{
	if (Sources.IsEmpty())
	{
		return FString();
	}
	TArray<FString> Pieces;
	for (const FMantleHairHatSource& Source : Sources)
	{
		Pieces.Add(Source.KeyPiece());
	}
	// The default aim's key is the one fits have always had; the dev comparison's rule b is a fit of its own.
	const TCHAR* AimSuffix = Aim == EMantleHairHatAim::UnderHatCards ? TEXT("-cards") : TEXT("");
	return FString::Printf(TEXT(":hathair-v%d%s[%s]"), RuleVersion, AimSuffix, *FString::Join(Pieces, TEXT(";")));
}

bool FMantleHairHatSource::BoneComponentTransform(const FMantleSkinnedMesh& Body, const FName Bone, FTransform& Out)
{
	if (Bone.IsNone() || Body.ReferencePose.Num() != Body.Bones.Num())
	{
		return false;
	}
	const int32 Index = Body.Bones.IndexOfByPredicate([Bone](const FMantleSkinnedMeshBone& Candidate) { return Candidate.Name == Bone; });
	if (Index == INDEX_NONE)
	{
		return false;
	}
	// Parents come before their children (Validate), so the walk up always ends; the bound is only a guard.
	FTransform Chain = Body.ReferencePose[Index];
	int32 Parent = Body.Bones[Index].ParentIndex;
	int32 Steps = 0;
	while (Parent != INDEX_NONE && Steps++ < Body.Bones.Num())
	{
		if (!Body.ReferencePose.IsValidIndex(Parent))
		{
			return false;
		}
		Chain = Chain * Body.ReferencePose[Parent];
		Parent = Body.Bones[Parent].ParentIndex;
	}
	Out = Chain;
	return true;
}

bool FMantleHairHatSource::HatToHair(const FMantleSkinnedMesh& Body, const FBox3f& MeshBounds, FTransform& Out, FString& Error) const
{
	FTransform Bone;
	if (!BoneComponentTransform(Body, BoneName, Bone))
	{
		Error = FString::Printf(TEXT("the body has no reference pose for the bone %s the %s socket is on"), *BoneName.ToString(), *SlotId);
		return false;
	}
	// What the assembler puts on the socket (FMantleAvatarAccessories::Sync), then the socket on its bone, then the bone in the reference pose.
	float Factor = 1.0f;
	const FTransform Relative = MantleAccessoryPlacement::PlacedRelative(Attach, MeshBounds, SlotId, SocketLocal,
		SocketOverride.IsSet() ? &SocketOverride.GetValue() : nullptr, Factor);
	Out = Relative * SocketLocal * Bone;
	return true;
}

bool FMantleHairHatSource::ReadMesh(TArray<FVector3f>& OutPositions, TArray<uint32>& OutIndices, FBox3f& OutBounds, FString& Error) const
{
	OutPositions.Reset();
	OutIndices.Reset();
	if (!MeshBytes.IsValid())
	{
		Error = TEXT("the hat's mesh is not loaded");
		return false;
	}
	const TArray<uint8>& Bytes = *MeshBytes;
	const TConstArrayView<uint8> View(Bytes);
	if (FMantleStaticMeshReader::HasMagic(View))
	{
		FMantleStaticMeshData Data;
		if (!FMantleStaticMeshReader::Read(View, Data, &Error))
		{
			return false;
		}
		OutPositions = MoveTemp(Data.Positions);
		OutIndices = MoveTemp(Data.Indices);
		OutBounds = Data.Bounds;
		return true;
	}
	if (Bytes.Num() >= 4 && FMemory::Memcmp(Bytes.GetData(), "MRM1", 4) == 0)
	{
		// Accessories cooked before F2: MRM1's own axis convention, the way the assembler reads them.
		FMantleRuntimeMeshResult Old = FMantleRuntimeMeshReader::Read(Bytes);
		if (!Old.bSuccess)
		{
			Error = Old.Error;
			return false;
		}
		OutPositions = MoveTemp(Old.Mesh.Positions);
		OutIndices = MoveTemp(Old.Mesh.Indices);
		// MRM1's header bounds are not checked against its positions; measure the positions themselves.
		OutBounds = FBox3f(OutPositions.GetData(), OutPositions.Num());
		return true;
	}
	Error = TEXT("the hat's mesh is not a cooked Mantle mesh (MRM4 or MRM1)");
	return false;
}

bool FMantleHairHatSource::ApplyTriangles(const TConstArrayView<FVector3f> HatPositions, const TConstArrayView<uint32> HatIndices,
	const TConstArrayView<FVector3f> ScalpPositions, const TConstArrayView<uint32> ScalpIndices,
	const bool bFacialHair, FMantleSkinnedMesh& Hair, FMantleHairHatStats& Stats, FString& Error, const EMantleHairHatAim Aim)
{
	if (HatIndices.Num() < 3 || ScalpIndices.Num() < 3)
	{
		return true;
	}
	FMantleHairHatFitParams Params;
	Params.Rule = EMantleHairHatRule::UnderIt;
	Params.Aim = Aim;
	Params.bFacialHair = bFacialHair;
	// Every ray starts from the same point inside the head, whatever the LOD.
	Params.HeadCentre = FMantleHairHatFit::HeadCentreFromSkin(ScalpPositions, ScalpIndices);

	TArray<FMantleHairHatFitResult> Results;
	Results.SetNum(Hair.LODs.Num());
	bool bAnyChange = false;
	for (int32 LODIndex = 0; LODIndex < Hair.LODs.Num(); ++LODIndex)
	{
		FMantleHairHatFit::Fit(Hair.LODs[LODIndex], HatPositions, HatIndices, ScalpPositions, ScalpIndices, Params, Results[LODIndex]);
		Stats.FitMs += Results[LODIndex].Ms;
		Stats.PointsPulled += Results[LODIndex].PointsPulled;
		Stats.TrianglesDropped += Results[LODIndex].TrianglesDropped;
		bAnyChange |= Results[LODIndex].PointsPulled > 0 || Results[LODIndex].TrianglesDropped > 0;
	}
	if (!bAnyChange)
	{
		return true;
	}

	// Edit the fitted CPU mesh itself, so the component build and the merged look both read the result. What changes is
	// kept so a result the mesh refuses (it should never) leaves the hair as the head fit made it.
	struct FSaved
	{
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		TArray<FMantleSkinnedMeshSection> Sections;
	};
	TArray<FSaved> Saved;
	Saved.SetNum(Hair.LODs.Num());
	for (int32 LODIndex = 0; LODIndex < Hair.LODs.Num(); ++LODIndex)
	{
		FMantleSkinnedMeshLOD& LOD = Hair.LODs[LODIndex];
		const FMantleHairHatFitResult& Result = Results[LODIndex];
		if (Result.PointsPulled == 0 && Result.TrianglesDropped == 0)
		{
			continue;
		}
		Saved[LODIndex].Positions = LOD.Positions;
		Saved[LODIndex].Indices = LOD.Indices;
		Saved[LODIndex].Sections = LOD.Sections;
		LOD.Positions = Result.Positions;
		FMantleHairHatFit::ApplyDrops(LOD, Result.DroppedTriangles);
		if (LOD.Indices.IsEmpty())
		{
			MantleHairHatSourcePrivate::KeepOneHiddenTriangle(LOD, Saved[LODIndex].Indices, Saved[LODIndex].Sections, Params.HeadCentre);
		}
	}
	FString ValidateError;
	if (!Hair.Validate(ValidateError))
	{
		for (int32 LODIndex = 0; LODIndex < Hair.LODs.Num(); ++LODIndex)
		{
			if (Saved[LODIndex].Positions.Num() > 0)
			{
				FMantleSkinnedMeshLOD& LOD = Hair.LODs[LODIndex];
				LOD.Positions = MoveTemp(Saved[LODIndex].Positions);
				LOD.Indices = MoveTemp(Saved[LODIndex].Indices);
				LOD.Sections = MoveTemp(Saved[LODIndex].Sections);
			}
		}
		Stats.PointsPulled = 0;
		Stats.TrianglesDropped = 0;
		Error = TEXT("the hair fitted under the hat is not a valid mesh: ") + ValidateError;
		return false;
	}
	return true;
}

bool FMantleHairHatSource::Apply(const TConstArrayView<FMantleHairHatSource> Sources, const FMantleSkinnedMesh& Body, const FMantleSkinnedMesh& Face,
	const bool bFacialHair, FMantleSkinnedMesh& Hair, FMantleHairHatStats& Stats, FString& Error, const EMantleHairHatAim Aim)
{
	using namespace MantleHairHatSourcePrivate;
	Stats = FMantleHairHatStats();
	const double StartSeconds = FPlatformTime::Seconds();
	const auto Finish = [&Stats, StartSeconds](const bool bResult, const FString& Why)
	{
		Stats.Ms = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		Stats.Error = bResult ? FString() : Why;
		return bResult;
	};

	// The hats, in the hair's space.
	TArray<FVector3f> HatPositions;
	TArray<uint32> HatIndices;
	for (const FMantleHairHatSource& Source : Sources)
	{
		if (Source.Rule == EMantleHairHatRule::LeaveAlone)
		{
			continue;
		}
		if (Source.Hood.IsValid())
		{
			// A hood is fitted outerwear, already in the hair's space: its triangles join the hats' as they are.
			const FMantleHairHood& Hood = *Source.Hood;
			const uint32 HoodBase = static_cast<uint32>(HatPositions.Num());
			for (const FVector3f& Position : Hood.Positions)
			{
				HatPositions.Add(IsFiniteVector(Position) ? Position : FVector3f::ZeroVector);
			}
			for (int32 Corner = 0; Corner + 2 < Hood.Indices.Num(); Corner += 3)
			{
				const uint32 NumHoodPositions = static_cast<uint32>(Hood.Positions.Num());
				if (Hood.Indices[Corner] < NumHoodPositions && Hood.Indices[Corner + 1] < NumHoodPositions && Hood.Indices[Corner + 2] < NumHoodPositions)
				{
					HatIndices.Add(HoodBase + Hood.Indices[Corner]);
					HatIndices.Add(HoodBase + Hood.Indices[Corner + 1]);
					HatIndices.Add(HoodBase + Hood.Indices[Corner + 2]);
				}
			}
			++Stats.NumHoods;
			continue;
		}
		TArray<FVector3f> Positions;
		TArray<uint32> Indices;
		FBox3f Bounds(ForceInit);
		FTransform ToHair;
		if (!Source.ReadMesh(Positions, Indices, Bounds, Error) || !Source.HatToHair(Body, Bounds, ToHair, Error))
		{
			Error = FString::Printf(TEXT("%s: %s"), *Source.SlotId, *Error);
			return Finish(false, Error);
		}
		const uint32 Base = static_cast<uint32>(HatPositions.Num());
		for (const FVector3f& Position : Positions)
		{
			// A point that is not finite would be a ray the fit cannot use; it is put at the origin, where no triangle uses it.
			const FVector3f Placed = FVector3f(ToHair.TransformPosition(FVector(Position)));
			HatPositions.Add(IsFiniteVector(Placed) ? Placed : FVector3f::ZeroVector);
		}
		for (int32 Corner = 0; Corner + 2 < Indices.Num(); Corner += 3)
		{
			if (Indices[Corner] < static_cast<uint32>(Positions.Num()) && Indices[Corner + 1] < static_cast<uint32>(Positions.Num())
				&& Indices[Corner + 2] < static_cast<uint32>(Positions.Num()))
			{
				HatIndices.Add(Base + Indices[Corner]);
				HatIndices.Add(Base + Indices[Corner + 1]);
				HatIndices.Add(Base + Indices[Corner + 2]);
			}
		}
		++Stats.NumHats;
	}
	Stats.HatTriangles = HatIndices.Num() / 3;
	if (HatIndices.IsEmpty())
	{
		return Finish(true, FString());
	}

	// The wearer's head skin: the head centre, and the scalp the pull-in stops above.
	TArray<FVector3f> ScalpPositions;
	TArray<uint32> ScalpIndices;
	if (!FMantleHairHeadFit::HeadSkin(Face, ScalpPositions, ScalpIndices))
	{
		Error = TEXT("the wearer's face has no head skin to fit the hair under a hat against");
		return Finish(false, Error);
	}
	const bool bResult = ApplyTriangles(HatPositions, HatIndices, ScalpPositions, ScalpIndices, bFacialHair, Hair, Stats, Error, Aim);
	Stats.bApplied = bResult;
	return Finish(bResult, Error);
}
