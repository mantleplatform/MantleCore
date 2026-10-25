// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyShapeReader.h"

#include "MantleBodyTopologyData.h"
#include "MantleCookedCursor.h"
#include "MantleRuntime.h"
#include "MantleNeckSamples.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleBodyShapeReaderPrivate
{
	constexpr uint32 FlagWarpWeights = 1 << 0;

	FMantleBodyShapeReadResult FailShape(const FString& Error)
	{
		FMantleBodyShapeReadResult Result;
		Result.Error = Error;
		return Result;
	}

	FMantleBodyShapeReadResult TruncatedShape(const TCHAR* Part)
	{
		return FailShape(FString::Printf(TEXT("That body shape is incomplete or corrupt (%s)."), Part));
	}

	/** The NECK block's method values (backend cook.NeckMethodCorrespondence / NeckMethodRays). */
	constexpr uint32 NeckMethodCorrespondence = 1;
	constexpr uint32 NeckMethodRays = 2;

	/** Reads Count finite floats into Out, or only checks them when bCandidate. */
	bool ReadFiniteWeights(MantleCookedFormat::FCursor& Cursor, const int64 Count, TArray<float>& Out, const bool bCandidate)
	{
		if (!Cursor.Has(Count * 4)) { return false; }
		if (!bCandidate)
		{
			if (!Cursor.Array(Out, Count)) { return false; }
			for (const float Weight : Out) { if (!FMath::IsFinite(Weight)) { return false; } }
			return true;
		}
		for (int64 WeightIndex = 0; WeightIndex < Count; ++WeightIndex)
		{
			if (!FMath::IsFinite(Cursor.F32())) { return false; }
		}
		return !Cursor.Failed();
	}

	/**
	 * The NECK extension block (backend/internal/cook/bodyshape.go) into Shape.Neck: method, K ids, K
	 * positions, S sample indices into the shape's positions followed by the K, (S + 4)^2 weights. Counts are
	 * checked against the block before anything is sized; the whole neck against the shape by ValidateNeck.
	 */
	bool ReadNeckBlock(const TConstArrayView<uint8> Block, FMantleBodyShape& Shape, FString& OutError, const bool bCandidate)
	{
		using MantleCookedFormat::FCursor;
		FCursor Cursor(Block);
		const uint32 Method = Cursor.U32();
		const uint32 Count = Cursor.U32();
		if (Cursor.Failed())
		{
			OutError = TEXT("its neck block is truncated");
			return false;
		}
		if (Method != NeckMethodCorrespondence && Method != NeckMethodRays)
		{
			OutError = FString::Printf(TEXT("its neck names method %u"), Method);
			return false;
		}
		if (Count < 1 || Count > static_cast<uint32>(FMantleBodyShapeReader::MaxNeckSamples) || !Cursor.Has(static_cast<int64>(Count) * 16 + 4))
		{
			OutError = FString::Printf(TEXT("its neck claims %u samples (1 to %d)"), Count, FMantleBodyShapeReader::MaxNeckSamples);
			return false;
		}
		TArray<uint32> Ids;
		Cursor.Array(Ids, Count);
		FMantleBodyShapeNeck& Neck = Shape.Neck;
		Cursor.Array(Neck.Positions, Count);
		const uint32 SampleCount = Cursor.U32();
		const uint32 Total = static_cast<uint32>(Shape.Positions.Num()) + Count;
		if (Cursor.Failed() || SampleCount < 1 || SampleCount > static_cast<uint32>(FMantleBodyShapeReader::MaxWarpSamples) || SampleCount > Total)
		{
			OutError = FString::Printf(TEXT("its neck claims %u warp samples (1 to %d, at most one per position)"), SampleCount, FMantleBodyShapeReader::MaxWarpSamples);
			return false;
		}
		const int64 S = SampleCount;
		const int64 NumWeights = (S + 4) * (S + 4);
		if (!Cursor.Has(S * 4 + NumWeights * 4))
		{
			OutError = TEXT("its neck's warp weights are truncated");
			return false;
		}
		TArray<uint32> Indices;
		Cursor.Array(Indices, S);
		if (!ReadFiniteWeights(Cursor, NumWeights, Neck.Weights.InterpolationWeights, bCandidate))
		{
			OutError = TEXT("its neck's warp weights are truncated or not finite"); return false;
		}
		if (Cursor.Failed() || Cursor.Remaining() != 0)
		{
			OutError = TEXT("its neck block does not end where its counts say");
			return false;
		}
		Neck.Method = Method == NeckMethodCorrespondence ? EMantleNeckMethod::Correspondence : EMantleNeckMethod::Rays;
		Neck.Ids.Reserve(Ids.Num());
		for (const uint32 Id : Ids)
		{
			if (Id > static_cast<uint32>(MAX_int32) || (Neck.Ids.Num() > 0 && Id <= static_cast<uint32>(Neck.Ids.Last()))
				|| (Method == NeckMethodRays && Id >= static_cast<uint32>(FMantleNeckSamples::NumRays())))
			{
				OutError = TEXT("its neck names an id out of range");
				return false;
			}
			Neck.Ids.Add(static_cast<int32>(Id));
		}
		TBitArray<> Seen(false, static_cast<int32>(Total));
		Neck.Weights.SampleIndices.Reserve(static_cast<int32>(S));
		Neck.Weights.SampleRestPositions.Reserve(static_cast<int32>(S));
		for (const uint32 Index : Indices)
		{
			if (Index >= Total || Seen[static_cast<int32>(Index)])
			{
				OutError = TEXT("a neck warp sample index is out of range or repeated");
				return false;
			}
			Seen[static_cast<int32>(Index)] = true;
			Neck.Weights.SampleIndices.Add(static_cast<int32>(Index));
			const int32 Position = static_cast<int32>(Index);
			Neck.Weights.SampleRestPositions.Add(Position < Shape.Positions.Num() ? Shape.Positions[Position] : Neck.Positions[Position - Shape.Positions.Num()]);
		}
		if (!MantleCookedFormat::AllFinite(Neck.Positions)) { OutError = TEXT("a neck position is not finite"); return false; }
		return bCandidate || Shape.ValidateNeck(OutError);
	}

	/**
	 * The extension table, as MantleCookedFormat::SkipExtensions reads it, except that the NECK block is read
	 * into Shape (once) and the rest are skipped with their tags in OutSkipped.
	 */
	bool ReadShapeExtensions(MantleCookedFormat::FCursor& Cursor, const TConstArrayView<uint8> Data, FMantleBodyShape& Shape, TArray<FString>& OutSkipped, FString& OutError, const bool bCandidate)
	{
		OutSkipped.Reset();
		const uint32 Count = Cursor.U32();
		if (Cursor.Failed())
		{
			OutError = TEXT("the extension table is missing");
			return false;
		}
		if (Count > static_cast<uint32>(MantleCookedFormat::MaxExtensions))
		{
			OutError = FString::Printf(TEXT("the file claims %u extension blocks (at most %d)"), Count, MantleCookedFormat::MaxExtensions);
			return false;
		}
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			uint8 Tag[4] = {};
			Tag[0] = Cursor.U8();
			Tag[1] = Cursor.U8();
			Tag[2] = Cursor.U8();
			Tag[3] = Cursor.U8();
			const uint32 Length = Cursor.U32();
			const bool bNeck = Tag[0] == 'N' && Tag[1] == 'E' && Tag[2] == 'C' && Tag[3] == 'K';
			if (!bNeck)
			{
				if (Cursor.Failed() || !Cursor.Skip(static_cast<int64>(Length)))
				{
					OutError = FString::Printf(TEXT("extension block %u is truncated"), Index);
					return false;
				}
				OutSkipped.Add(MantleCookedFormat::DescribeTag(Tag));
				continue;
			}
			if (Cursor.Failed() || !Cursor.Has(Length))
			{
				OutError = FString::Printf(TEXT("extension block %u is truncated"), Index);
				return false;
			}
			const TConstArrayView<uint8> Block(Data.GetData() + Cursor.Offset(), static_cast<int32>(Length));
			Cursor.Skip(Length);
			if (Shape.Neck.Method != EMantleNeckMethod::None)
			{
				OutError = TEXT("it has two neck blocks");
				return false;
			}
			if (!ReadNeckBlock(Block, Shape, OutError, bCandidate))
			{
				Shape.Neck = FMantleBodyShapeNeck();
				return false;
			}
		}
		return true;
	}
}

FMantleBodyShapeReadResult FMantleBodyShapeReader::ReadInternal(const TConstArrayView<uint8> Data, const bool bCandidate, const int32 ExpectedVertices)
{
	using namespace MantleBodyShapeReaderPrivate;
	using MantleCookedFormat::FCursor;

	FCursor Cursor(Data);
	if (!Cursor.Magic("MBS1"))
	{
		return FailShape(TEXT("That asset is not a Mantle body shape."));
	}
	const uint32 Version = Cursor.U32();
	if (Cursor.Failed())
	{
		return TruncatedShape(TEXT("header"));
	}
	if (Version != SupportedVersion)
	{
		return FailShape(FString::Printf(
			TEXT("That body shape was cooked for a newer version of Mantle (format %u, this build reads %u)."), Version, SupportedVersion));
	}

	FMantleBodyShapeReadResult Result;
	FMantleBodyShape& Shape = Result.Shape;
	FMantleBodyShapeHeader& Header = Result.Header;

	const uint32 Flags = Cursor.U32();
	const uint32 VertexCount = Cursor.U32();
	const uint32 BoneCount = Cursor.U32();
	Header.UVHash = Cursor.U32();
	const uint32 PositionsHash = Cursor.U32();
	FVector3f Min, Max;
	Min.X = Cursor.F32();
	Min.Y = Cursor.F32();
	Min.Z = Cursor.F32();
	Max.X = Cursor.F32();
	Max.Y = Cursor.F32();
	Max.Z = Cursor.F32();
	if (!Cursor.String(Header.Skeleton))
	{
		return TruncatedShape(TEXT("header"));
	}
	if ((Flags & ~FlagWarpWeights) != 0)
	{
		return FailShape(FString::Printf(TEXT("That body shape uses flags this build does not know (0x%x)."), Flags));
	}
	if (VertexCount < 1 || VertexCount > static_cast<uint32>(MaxVertices))
	{
		return FailShape(FString::Printf(TEXT("That body shape claims %u vertices (1 to %d)."), VertexCount, MaxVertices));
	}
	if (bCandidate && ExpectedVertices > 0 && VertexCount != static_cast<uint32>(ExpectedVertices))
	{
		return FailShape(TEXT("That clothing source does not share the wearer's complete body vertex count."));
	}
	if (BoneCount < 1 || BoneCount > static_cast<uint32>(MaxBones))
	{
		return FailShape(FString::Printf(TEXT("That body shape claims %u bones (1 to %d)."), BoneCount, MaxBones));
	}
	if (Min.ContainsNaN() || Max.ContainsNaN() || Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
	{
		return FailShape(TEXT("That body shape has invalid bounds."));
	}
	if (Header.Skeleton.IsEmpty())
	{
		return FailShape(TEXT("That body shape names no skeleton."));
	}
	Header.Bounds = FBox3f(Min, Max);
	Header.bWarpWeights = (Flags & FlagWarpWeights) != 0;

	// Bone table: each entry is at least a str16 length and a parent.
	if (!Cursor.Has(static_cast<int64>(BoneCount) * 6))
	{
		return TruncatedShape(TEXT("bone table"));
	}
	FMantleReferencePose& Pose = Shape.ReferencePose;
	Pose.Bones.SetNum(static_cast<int32>(BoneCount));
	for (FMantleSkinnedMeshBone& Bone : Pose.Bones)
	{
		FString Name;
		Cursor.String(Name);
		Bone.ParentIndex = Cursor.I32();
		if (Cursor.Failed())
		{
			return TruncatedShape(TEXT("bone table"));
		}
		if (Name.IsEmpty())
		{
			return FailShape(TEXT("That body shape has a bone without a name."));
		}
		Bone.Name = FName(*Name);
	}

	// The pose and the positions, checked together before either is sized.
	if (!Cursor.Has(static_cast<int64>(BoneCount) * 40 + static_cast<int64>(VertexCount) * 12))
	{
		return TruncatedShape(TEXT("pose and positions"));
	}
	Pose.LocalTransforms.Reserve(static_cast<int32>(BoneCount));
	for (uint32 Bone = 0; Bone < BoneCount; ++Bone)
	{
		float Values[10];
		for (float& Value : Values)
		{
			Value = Cursor.F32();
		}
		FTransform& Local = Pose.LocalTransforms.AddDefaulted_GetRef();
		Local.SetTranslation(FVector(Values[0], Values[1], Values[2]));
		Local.SetRotation(FQuat(Values[3], Values[4], Values[5], Values[6]));
		Local.SetScale3D(FVector(Values[7], Values[8], Values[9]));
	}
	Cursor.Array(Shape.Positions, VertexCount);
	if (Cursor.Failed())
	{
		return TruncatedShape(TEXT("pose and positions"));
	}
	if (!MantleCookedFormat::AllFinite(Shape.Positions))
	{
		return FailShape(TEXT("That body shape has a position that is not a finite number."));
	}

	if (Header.bWarpWeights)
	{
		const uint32 SampleCount = Cursor.U32();
		if (Cursor.Failed())
		{
			return TruncatedShape(TEXT("warp weights"));
		}
		if (SampleCount < 1 || SampleCount > static_cast<uint32>(MaxWarpSamples) || SampleCount > VertexCount)
		{
			return FailShape(FString::Printf(TEXT("That body shape claims %u warp samples (1 to %d, at most one per vertex)."), SampleCount, MaxWarpSamples));
		}
		const int64 S = SampleCount;
		const int64 NumWeights = (S + 4) * (S + 4);
		if (!Cursor.Has(S * 4 + NumWeights * 4))
		{
			return TruncatedShape(TEXT("warp weights"));
		}
		TArray<uint32> Indices;
		Cursor.Array(Indices, S);
		if (!ReadFiniteWeights(Cursor, NumWeights, Shape.Weights.InterpolationWeights, bCandidate))
		{
			return FailShape(TEXT("That body shape has warp weights that are truncated or not finite."));
		}
		if (Cursor.Failed())
		{
			return TruncatedShape(TEXT("warp weights"));
		}
		TBitArray<> Seen(false, static_cast<int32>(VertexCount));
		Shape.Weights.SampleIndices.Reserve(static_cast<int32>(S));
		Shape.Weights.SampleRestPositions.Reserve(static_cast<int32>(S));
		for (const uint32 Index : Indices)
		{
			if (Index >= VertexCount || Seen[static_cast<int32>(Index)])
			{
				return FailShape(TEXT("That body shape is corrupt: a warp sample index is out of range or repeated."));
			}
			Seen[static_cast<int32>(Index)] = true;
			Shape.Weights.SampleIndices.Add(static_cast<int32>(Index));
			Shape.Weights.SampleRestPositions.Add(Shape.Positions[static_cast<int32>(Index)]);
		}
	}

	FString ExtensionError;
	if (!ReadShapeExtensions(Cursor, Data, Shape, Header.SkippedExtensions, ExtensionError, bCandidate))
	{
		return FailShape(FString::Printf(TEXT("That body shape is corrupt: %s."), *ExtensionError));
	}
	Header.NeckSamples = Shape.Neck.Positions.Num();
	if (Cursor.Remaining() != 0)
	{
		return FailShape(FString::Printf(TEXT("That body shape is corrupt: %lld bytes follow its end."), Cursor.Remaining()));
	}

	// The hash is the fitter's cache key: it must be the positions' own.
	Shape.PositionsHash = UMantleBodyTopologyData::ComputePositionsHash(Shape.Positions);
	if (Shape.PositionsHash != PositionsHash)
	{
		return FailShape(FString::Printf(TEXT("That body shape is corrupt: its positions hash to %08x, the file says %08x."), Shape.PositionsHash, PositionsHash));
	}

	FString Invalid;
	if (!Pose.Validate(Invalid))
	{
		return FailShape(FString::Printf(TEXT("That body shape is corrupt: %s."), *Invalid));
	}
	if (!bCandidate && Shape.HasWeights() && !Shape.ValidateWeights(Invalid))
	{
		return FailShape(FString::Printf(TEXT("That body shape is corrupt: %s."), *Invalid));
	}

	UE_LOG(LogMantle, Verbose, TEXT("Read a body shape: %d vertices, %d bones, %d warp samples, positions %08x"),
		Shape.Positions.Num(), Pose.Num(), Shape.Weights.SampleIndices.Num(), Shape.PositionsHash);

	Result.bSuccess = true;
	return Result;
}

FMantleBodyShapeReadResult FMantleBodyShapeReader::Read(const TConstArrayView<uint8> Data)
{
	return ReadInternal(Data, false);
}

FMantleBodyShapeCandidateReadResult FMantleBodyShapeReader::ReadCandidate(const TConstArrayView<uint8> Data, const int32 ExpectedVertices)
{
	FMantleBodyShapeReadResult Read = ReadInternal(Data, true, ExpectedVertices);
	FMantleBodyShapeCandidateReadResult Result;
	Result.bSuccess = Read.bSuccess;
	Result.Error = MoveTemp(Read.Error);
	if (Read.bSuccess) { Result.Positions = MoveTemp(Read.Shape.Positions); Result.Header = MoveTemp(Read.Header); }
	return Result;
}
