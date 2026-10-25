// Copyright Joshua Eger. All Rights Reserved.
#include "MantleStaticMeshReader.h"

#include "MantleCookedCursor.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleStaticMeshReaderPrivate
{
	constexpr uint32 FlagVertexColors = 1 << 0;
	constexpr uint32 KnownFlags = FlagVertexColors;

	/** A lower LOD's flags in the LODS block. */
	constexpr uint32 LODFlagColors = 1 << 0;
	constexpr uint32 LODFlagUV1 = 1 << 1;
	constexpr uint32 LODKnownFlags = LODFlagColors | LODFlagUV1;

	/** The COLL block's flags. */
	constexpr uint32 CollisionFlagSimpleAsComplex = 1 << 0;
	constexpr uint32 CollisionKnownFlags = CollisionFlagSimpleAsComplex;

	/** Position, normal, tangent and UV float32s of one vertex; colours add 4 bytes, UV1 8. */
	constexpr int64 VertexBytes = 12 + 12 + 16 + 8;

	/** How far a hull's points must spread in each of three directions (cook.minHullSpanCm). */
	constexpr double MinHullSpanCm = 1.0e-3;

	static_assert(sizeof(FVector3f) == 12 && sizeof(FVector4f) == 16 && sizeof(FVector2f) == 8,
		"MRM2 arrays are read straight into these types");

	bool FailStatic(FMantleStaticMeshData& Out, FString* OutError, const FString& Error)
	{
		Out = FMantleStaticMeshData();
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("MRM2: %s"), *Error);
		}
		return false;
	}

	bool TagIs(const uint8 (&Tag)[4], const char (&Expected)[5])
	{
		return FMemory::Memcmp(Tag, Expected, 4) == 0;
	}

	/**
	 * MRM2's section rule on top of the shared one: the table tiles the index buffer in draw order with
	 * no empty section and every name present. LOD0's slots are 0..count-1, each once; a lower LOD's
	 * are some of LOD0's (Lod0 non-null), each under LOD0's name for it, none twice.
	 */
	bool CheckStaticSections(const TArray<FMantleSkinnedMeshSection>& Sections, const uint32 IndexCount,
		const TArray<FMantleSkinnedMeshSection>* Lod0, FString& OutError)
	{
		const int32 SectionCount = Sections.Num();
		TSet<int32> SlotsTaken;
		int64 Next = 0;
		for (int32 Index = 0; Index < SectionCount; ++Index)
		{
			const FMantleSkinnedMeshSection& Section = Sections[Index];
			if (Section.FirstIndex != Next || Section.IndexCount <= 0)
			{
				OutError = FString::Printf(TEXT("section %d does not start where the previous one ended (%lld), or is empty"), Index, Next);
				return false;
			}
			Next = static_cast<int64>(Section.FirstIndex) + Section.IndexCount;
			if (Section.MaterialName.IsNone())
			{
				OutError = FString::Printf(TEXT("section %d has no material name"), Index);
				return false;
			}
			bool bTaken = false;
			SlotsTaken.Add(Section.MaterialIndex, &bTaken);
			if (Lod0)
			{
				const FMantleSkinnedMeshSection* Slot = Lod0->FindByPredicate([&Section](const FMantleSkinnedMeshSection& Candidate)
				{
					return Candidate.MaterialIndex == Section.MaterialIndex;
				});
				// FName compares case-insensitively; LOD0's names are the slot's, so the text must match exactly.
				if (bTaken || !Slot || !Slot->MaterialName.ToString().Equals(Section.MaterialName.ToString(), ESearchCase::CaseSensitive))
				{
					OutError = FString::Printf(TEXT("section %d (slot %d, '%s') is not one of LOD0's material slots, or repeats one"),
						Index, Section.MaterialIndex, *Section.MaterialName.ToString());
					return false;
				}
			}
			else if (bTaken || Section.MaterialIndex < 0 || Section.MaterialIndex >= SectionCount)
			{
				OutError = FString::Printf(TEXT("section %d uses material slot %d, out of range for %d sections or repeated"),
					Index, Section.MaterialIndex, SectionCount);
				return false;
			}
		}
		if (Next != static_cast<int64>(IndexCount))
		{
			OutError = FString::Printf(TEXT("the sections cover %lld of %u indices"), Next, IndexCount);
			return false;
		}
		return true;
	}

	/**
	 * One LOD's streams after its section table, into Out: every array checked against the bytes left
	 * before any is sized, then the content rules (finite, binormal signs, indices in range).
	 */
	bool ReadStaticStreams(MantleCookedFormat::FCursor& Cursor, const uint32 VertexCount, const uint32 IndexCount, const bool bColors,
		const bool bUV1, FMantleStaticMeshLOD& Out, FString& OutError)
	{
		const int64 PerVertex = VertexBytes + (bColors ? 4 : 0) + (bUV1 ? 8 : 0);
		if (!Cursor.Has(static_cast<int64>(VertexCount) * PerVertex + static_cast<int64>(IndexCount) * 4))
		{
			OutError = TEXT("the vertex and index arrays are truncated");
			return false;
		}
		TArray<uint8> ColorBytes;
		Cursor.Array(Out.Positions, VertexCount);
		Cursor.Array(Out.Normals, VertexCount);
		Cursor.Array(Out.Tangents, VertexCount);
		Cursor.Array(Out.UVs, VertexCount);
		if (bColors)
		{
			Cursor.Array(ColorBytes, static_cast<int64>(VertexCount) * 4);
		}
		if (bUV1)
		{
			Cursor.Array(Out.UV1s, VertexCount);
		}
		Cursor.Array(Out.Indices, IndexCount);
		if (Cursor.Failed())
		{
			OutError = TEXT("the vertex and index arrays are truncated");
			return false;
		}

		if (!MantleCookedFormat::AllFinite(Out.Positions) || !MantleCookedFormat::AllFinite(Out.Normals)
			|| !MantleCookedFormat::AllFinite(Out.Tangents) || !MantleCookedFormat::AllFinite(Out.UVs) || !MantleCookedFormat::AllFinite(Out.UV1s))
		{
			OutError = TEXT("a vertex attribute is not finite");
			return false;
		}
		for (int32 Vertex = 0; Vertex < Out.Tangents.Num(); ++Vertex)
		{
			const float Sign = Out.Tangents[Vertex].W;
			if (Sign != 1.0f && Sign != -1.0f)
			{
				OutError = FString::Printf(TEXT("vertex %d's binormal sign is %g, not +1 or -1"), Vertex, Sign);
				return false;
			}
		}
		for (const uint32 Index : Out.Indices)
		{
			if (Index >= VertexCount)
			{
				OutError = FString::Printf(TEXT("index %u is out of range for %u vertices"), Index, VertexCount);
				return false;
			}
		}
		if (bColors)
		{
			// The file's bytes are R, G, B, A; FColor's memory order is not, so each is built by value.
			Out.Colors.SetNumUninitialized(static_cast<int32>(VertexCount));
			for (int32 Vertex = 0; Vertex < Out.Colors.Num(); ++Vertex)
			{
				const uint8* Bytes = ColorBytes.GetData() + Vertex * 4;
				Out.Colors[Vertex] = FColor(Bytes[0], Bytes[1], Bytes[2], Bytes[3]);
			}
		}
		return true;
	}

	/** TEX1: LOD0's UV1, exactly one per vertex. */
	bool ReadUV1Block(const TConstArrayView<uint8> Block, FMantleStaticMeshData& Out, FString& OutError)
	{
		MantleCookedFormat::FCursor Cursor(Block);
		if (Block.Num() != static_cast<int64>(Out.VertexCount()) * 8 || !Cursor.Array(Out.UV1s, Out.VertexCount()) || !MantleCookedFormat::AllFinite(Out.UV1s))
		{
			OutError = FString::Printf(TEXT("the TEX1 block is %d bytes for %d vertices, or not finite"), Block.Num(), Out.VertexCount());
			return false;
		}
		return true;
	}

	/** LODS: the lower LODs and every LOD's screen size (backend staticfidelity.go). */
	bool ReadLODsBlock(const TConstArrayView<uint8> Block, FMantleStaticMeshData& Out, FString& OutError)
	{
		MantleCookedFormat::FCursor Cursor(Block);
		const uint32 Count = Cursor.U32();
		if (Cursor.Failed() || Count < 1 || Count > static_cast<uint32>(FMantleStaticMeshReader::MaxLODs - 1))
		{
			OutError = FString::Printf(TEXT("the LODS block claims %u lower LODs (1 to %d)"), Count, FMantleStaticMeshReader::MaxLODs - 1);
			return false;
		}
		if (!Cursor.Array(Out.ScreenSizes, static_cast<int64>(Count) + 1))
		{
			OutError = TEXT("the LODS block's screen sizes are truncated");
			return false;
		}
		for (int32 Index = 0; Index < Out.ScreenSizes.Num(); ++Index)
		{
			const float Size = Out.ScreenSizes[Index];
			if (!FMath::IsFinite(Size) || !(Size > 0.0f) || Size > FMantleStaticMeshReader::MaxScreenSize || (Index > 0 && !(Size < Out.ScreenSizes[Index - 1])))
			{
				OutError = FString::Printf(TEXT("LOD%d's screen size %g is not in (0, %g] and below the one before"), Index, Size, FMantleStaticMeshReader::MaxScreenSize);
				return false;
			}
		}
		Out.LowerLODs.SetNum(static_cast<int32>(Count));
		for (int32 Level = 1; Level <= static_cast<int32>(Count); ++Level)
		{
			FMantleStaticMeshLOD& LOD = Out.LowerLODs[Level - 1];
			const uint32 Flags = Cursor.U32();
			const uint32 VertexCount = Cursor.U32();
			const uint32 IndexCount = Cursor.U32();
			if (Cursor.Failed())
			{
				OutError = FString::Printf(TEXT("LOD%d's header is truncated"), Level);
				return false;
			}
			if ((Flags & ~LODKnownFlags) != 0)
			{
				OutError = FString::Printf(TEXT("LOD%d has unknown flags 0x%x"), Level, Flags);
				return false;
			}
			if (VertexCount < 1 || VertexCount > FMantleStaticMeshReader::MaxVertices || IndexCount < 3 || IndexCount % 3 != 0
				|| IndexCount > FMantleStaticMeshReader::MaxIndices)
			{
				OutError = FString::Printf(TEXT("LOD%d claims %u vertices and %u indices"), Level, VertexCount, IndexCount);
				return false;
			}
			FString Error;
			if (!MantleCookedFormat::ReadSectionTable(Cursor, IndexCount, LOD.Sections, Error)
				|| !CheckStaticSections(LOD.Sections, IndexCount, &Out.Sections, Error)
				|| !ReadStaticStreams(Cursor, VertexCount, IndexCount, (Flags & LODFlagColors) != 0, (Flags & LODFlagUV1) != 0, LOD, Error))
			{
				OutError = FString::Printf(TEXT("LOD%d: %s"), Level, *Error);
				return false;
			}
		}
		if (Cursor.Remaining() != 0)
		{
			OutError = FString::Printf(TEXT("the LODS block has %lld trailing bytes"), Cursor.Remaining());
			return false;
		}
		return true;
	}

	bool WithinExtent(const float Value)
	{
		return FMath::IsFinite(Value) && FMath::Abs(Value) <= FMantleStaticMeshReader::MaxCollisionExtent;
	}

	bool WithinExtent(const FVector3f& Value)
	{
		return WithinExtent(Value.X) && WithinExtent(Value.Y) && WithinExtent(Value.Z);
	}

	FVector3f ReadVector(MantleCookedFormat::FCursor& Cursor)
	{
		const float X = Cursor.F32();
		const float Y = Cursor.F32();
		const float Z = Cursor.F32();
		return FVector3f(X, Y, Z);
	}

	/** A stored rotation: finite, of length 1 within 1e-3 (the cook normalised it), returned normalised. */
	bool ReadRotation(MantleCookedFormat::FCursor& Cursor, FQuat4f& Out)
	{
		const float X = Cursor.F32();
		const float Y = Cursor.F32();
		const float Z = Cursor.F32();
		const float W = Cursor.F32();
		if (!WithinExtent(X) || !WithinExtent(Y) || !WithinExtent(Z) || !WithinExtent(W))
		{
			return false;
		}
		const double Length = FMath::Sqrt(static_cast<double>(X) * X + static_cast<double>(Y) * Y + static_cast<double>(Z) * Z + static_cast<double>(W) * W);
		if (FMath::Abs(Length - 1.0) > 1.0e-3)
		{
			return false;
		}
		Out = FQuat4f(X, Y, Z, W).GetNormalized();
		return true;
	}

	/** The point of Points farthest by Distance, and that distance (-1 for no points). */
	double FarthestHullPoint(const TConstArrayView<FVector3f> Points, TFunctionRef<double(const FVector3d&)> Distance, FVector3d& OutPoint)
	{
		double Best = -1.0;
		for (const FVector3f& Point : Points)
		{
			const FVector3d Candidate(Point);
			const double D = Distance(Candidate);
			if (D > Best)
			{
				Best = D;
				OutPoint = Candidate;
			}
		}
		return Best;
	}

	/** A shape count of the COLL block, with the running total of boxes, spheres and capsules held to the cap and the bytes checked. */
	bool ReadCollisionCount(MantleCookedFormat::FCursor& Cursor, const int64 BytesEach, int64& InOutPrimitives, uint32& OutCount)
	{
		OutCount = Cursor.U32();
		InOutPrimitives += OutCount;
		return !Cursor.Failed() && InOutPrimitives <= FMantleStaticMeshReader::MaxCollisionPrimitives && Cursor.Has(static_cast<int64>(OutCount) * BytesEach);
	}

	/** COLL: simple collision (backend staticfidelity.go), every count checked against the bytes left first. */
	bool ReadCollisionBlock(const TConstArrayView<uint8> Block, FMantleStaticMeshCollision& Out, FString& OutError)
	{
		MantleCookedFormat::FCursor Cursor(Block);
		const uint32 Flags = Cursor.U32();
		if (Cursor.Failed() || (Flags & ~CollisionKnownFlags) != 0)
		{
			OutError = FString::Printf(TEXT("the COLL block is empty or has unknown flags 0x%x"), Flags);
			return false;
		}
		Out.bSimpleAsComplex = (Flags & CollisionFlagSimpleAsComplex) != 0;
		int64 Primitives = 0;

		uint32 Boxes = 0;
		if (!ReadCollisionCount(Cursor, 40, Primitives, Boxes))
		{
			OutError = TEXT("the COLL block's boxes are truncated or too many");
			return false;
		}
		for (uint32 Index = 0; Index < Boxes; ++Index)
		{
			FMantleStaticMeshBox& Box = Out.Boxes.AddDefaulted_GetRef();
			Box.Center = ReadVector(Cursor);
			const bool bRotation = ReadRotation(Cursor, Box.Rotation);
			Box.Size = ReadVector(Cursor);
			if (!WithinExtent(Box.Center) || !bRotation || !WithinExtent(Box.Size) || !(Box.Size.X > 0.0f && Box.Size.Y > 0.0f && Box.Size.Z > 0.0f))
			{
				OutError = FString::Printf(TEXT("box %u is not a finite box with a unit rotation and a positive size"), Index);
				return false;
			}
		}

		uint32 Spheres = 0;
		if (!ReadCollisionCount(Cursor, 16, Primitives, Spheres))
		{
			OutError = TEXT("the COLL block's spheres are truncated or too many");
			return false;
		}
		for (uint32 Index = 0; Index < Spheres; ++Index)
		{
			FMantleStaticMeshSphere& Sphere = Out.Spheres.AddDefaulted_GetRef();
			Sphere.Center = ReadVector(Cursor);
			Sphere.Radius = Cursor.F32();
			if (!WithinExtent(Sphere.Center) || !WithinExtent(Sphere.Radius) || !(Sphere.Radius > 0.0f))
			{
				OutError = FString::Printf(TEXT("sphere %u is not a finite sphere with a positive radius"), Index);
				return false;
			}
		}

		uint32 Capsules = 0;
		if (!ReadCollisionCount(Cursor, 36, Primitives, Capsules))
		{
			OutError = TEXT("the COLL block's capsules are truncated or too many");
			return false;
		}
		for (uint32 Index = 0; Index < Capsules; ++Index)
		{
			FMantleStaticMeshCapsule& Capsule = Out.Capsules.AddDefaulted_GetRef();
			Capsule.Center = ReadVector(Cursor);
			const bool bRotation = ReadRotation(Cursor, Capsule.Rotation);
			Capsule.Radius = Cursor.F32();
			Capsule.Length = Cursor.F32();
			if (!WithinExtent(Capsule.Center) || !bRotation || !WithinExtent(Capsule.Radius) || !WithinExtent(Capsule.Length)
				|| !(Capsule.Radius > 0.0f) || !(Capsule.Length >= 0.0f))
			{
				OutError = FString::Printf(TEXT("capsule %u is not a finite capsule with a unit rotation, a positive radius and a length of 0 or more"), Index);
				return false;
			}
		}

		const uint32 Hulls = Cursor.U32();
		if (Cursor.Failed() || Hulls > static_cast<uint32>(FMantleStaticMeshReader::MaxCollisionHulls))
		{
			OutError = FString::Printf(TEXT("the COLL block claims %u hulls (at most %d)"), Hulls, FMantleStaticMeshReader::MaxCollisionHulls);
			return false;
		}
		int64 HullPoints = 0;
		for (uint32 Index = 0; Index < Hulls; ++Index)
		{
			const uint32 Points = Cursor.U32();
			HullPoints += Points;
			FMantleStaticMeshHull& Hull = Out.Hulls.AddDefaulted_GetRef();
			if (Cursor.Failed() || Points < 4 || Points > static_cast<uint32>(FMantleStaticMeshReader::MaxHullVertices)
				|| HullPoints > FMantleStaticMeshReader::MaxCollisionHullVertices || !Cursor.Array(Hull.Vertices, Points))
			{
				OutError = FString::Printf(TEXT("hull %u claims %u points (4 to %d, at most %d in all hulls), or is truncated"),
					Index, Points, FMantleStaticMeshReader::MaxHullVertices, FMantleStaticMeshReader::MaxCollisionHullVertices);
				return false;
			}
			for (const FVector3f& Point : Hull.Vertices)
			{
				if (!WithinExtent(Point))
				{
					OutError = FString::Printf(TEXT("hull %u has a point that is not finite or is past %g cm"), Index, FMantleStaticMeshReader::MaxCollisionExtent);
					return false;
				}
			}
			if (!FMantleStaticMeshReader::HullSpansVolume(Hull.Vertices))
			{
				OutError = FString::Printf(TEXT("hull %u is flat: its points do not span a volume"), Index);
				return false;
			}
		}
		if (Cursor.Remaining() != 0)
		{
			OutError = FString::Printf(TEXT("the COLL block has %lld trailing bytes"), Cursor.Remaining());
			return false;
		}
		if (!Out.HasShapes())
		{
			OutError = TEXT("the COLL block has no shapes");
			return false;
		}
		return true;
	}

	/**
	 * The extension table: the extension blocks read into Out (each at most once), every other tag skipped and
	 * recorded. Out's LOD0 is already read, which the TEX1 and LODS blocks are checked against.
	 */
	bool ReadStaticExtensions(MantleCookedFormat::FCursor& Cursor, const TConstArrayView<uint8> Data, FMantleStaticMeshData& Out, FString& OutError)
	{
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
		bool bUV1 = false;
		bool bLODs = false;
		bool bCollision = false;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			uint8 Tag[4] = {};
			Tag[0] = Cursor.U8();
			Tag[1] = Cursor.U8();
			Tag[2] = Cursor.U8();
			Tag[3] = Cursor.U8();
			const uint32 Length = Cursor.U32();
			if (Cursor.Failed() || !Cursor.Has(static_cast<int64>(Length)))
			{
				OutError = FString::Printf(TEXT("extension block %u is truncated"), Index);
				return false;
			}
			const TConstArrayView<uint8> Block = Data.Slice(static_cast<int32>(Cursor.Offset()), static_cast<int32>(Length));
			Cursor.Skip(static_cast<int64>(Length));

			bool* Seen = TagIs(Tag, "TEX1") ? &bUV1 : TagIs(Tag, "LODS") ? &bLODs : TagIs(Tag, "COLL") ? &bCollision : nullptr;
			if (!Seen)
			{
				Out.SkippedExtensions.Add(MantleCookedFormat::DescribeTag(Tag));
				continue;
			}
			if (*Seen)
			{
				OutError = FString::Printf(TEXT("the file has two %s blocks"), *MantleCookedFormat::DescribeTag(Tag));
				return false;
			}
			*Seen = true;
			const bool bRead = Seen == &bUV1 ? ReadUV1Block(Block, Out, OutError)
				: Seen == &bLODs ? ReadLODsBlock(Block, Out, OutError)
				: ReadCollisionBlock(Block, Out.Collision, OutError);
			if (!bRead)
			{
				return false;
			}
		}
		return true;
	}
}

bool FMantleStaticMeshReader::HasMagic(const TConstArrayView<uint8> Data)
{
	return Data.Num() >= 4 && FMemory::Memcmp(Data.GetData(), "MRM2", 4) == 0;
}

bool FMantleStaticMeshReader::Read(const TConstArrayView<uint8> Data, FMantleStaticMeshData& Out, FString* OutError)
{
	using namespace MantleStaticMeshReaderPrivate;
	using MantleCookedFormat::FCursor;

	Out = FMantleStaticMeshData();
	FCursor Cursor(Data);
	if (!Cursor.Magic("MRM2"))
	{
		return FailStatic(Out, OutError, TEXT("that asset is not a Mantle static mesh"));
	}
	const uint32 Version = Cursor.U32();
	const uint32 Flags = Cursor.U32();
	const uint32 VertexCount = Cursor.U32();
	const uint32 IndexCount = Cursor.U32();
	float BoundsValues[6];
	for (float& Value : BoundsValues)
	{
		Value = Cursor.F32();
	}
	if (Cursor.Failed())
	{
		return FailStatic(Out, OutError, TEXT("the header is truncated"));
	}
	if (Version != SupportedVersion)
	{
		// Specific on purpose: an old client meeting a newer asset should say "update Mantle".
		return FailStatic(Out, OutError, FString::Printf(
			TEXT("the mesh was cooked for a newer version of Mantle (format %u, this build reads %u)"), Version, SupportedVersion));
	}
	if ((Flags & ~KnownFlags) != 0)
	{
		return FailStatic(Out, OutError, FString::Printf(TEXT("unknown flags 0x%x"), Flags));
	}
	if (VertexCount < 1 || VertexCount > MaxVertices || IndexCount < 3 || IndexCount % 3 != 0 || IndexCount > MaxIndices)
	{
		return FailStatic(Out, OutError, FString::Printf(
			TEXT("the header claims %u vertices and %u indices (1 to %u vertices, 3 to %u indices in whole triangles)"),
			VertexCount, IndexCount, MaxVertices, MaxIndices));
	}
	for (const float Value : BoundsValues)
	{
		if (!FMath::IsFinite(Value))
		{
			return FailStatic(Out, OutError, TEXT("the bounds are not finite"));
		}
	}

	FString Error;
	if (!MantleCookedFormat::ReadSectionTable(Cursor, IndexCount, Out.Sections, Error)
		|| !CheckStaticSections(Out.Sections, IndexCount, nullptr, Error)
		|| !ReadStaticStreams(Cursor, VertexCount, IndexCount, (Flags & FlagVertexColors) != 0, false, Out, Error))
	{
		return FailStatic(Out, OutError, Error);
	}

	FVector3f Min = Out.Positions[0];
	FVector3f Max = Out.Positions[0];
	for (const FVector3f& Position : Out.Positions)
	{
		Min = FVector3f(FMath::Min(Min.X, Position.X), FMath::Min(Min.Y, Position.Y), FMath::Min(Min.Z, Position.Z));
		Max = FVector3f(FMath::Max(Max.X, Position.X), FMath::Max(Max.Y, Position.Y), FMath::Max(Max.Z, Position.Z));
	}
	const FVector3f FileMin(BoundsValues[0], BoundsValues[1], BoundsValues[2]);
	const FVector3f FileMax(BoundsValues[3], BoundsValues[4], BoundsValues[5]);
	if (Min != FileMin || Max != FileMax)
	{
		return FailStatic(Out, OutError, FString::Printf(TEXT("the bounds %s..%s are not the positions' extent %s..%s"),
			*FileMin.ToString(), *FileMax.ToString(), *Min.ToString(), *Max.ToString()));
	}
	Out.Bounds = FBox3f(FileMin, FileMax);

	if (!ReadStaticExtensions(Cursor, Data, Out, Error))
	{
		return FailStatic(Out, OutError, Error);
	}
	if (Cursor.Remaining() != 0)
	{
		return FailStatic(Out, OutError, FString::Printf(TEXT("%lld trailing bytes"), Cursor.Remaining()));
	}
	return true;
}

bool FMantleStaticMeshReader::HullSpansVolume(const TConstArrayView<FVector3f> Points)
{
	using namespace MantleStaticMeshReaderPrivate;

	if (Points.Num() < 4)
	{
		return false;
	}
	const FVector3d P0(Points[0]);
	FVector3d P1 = P0;
	if (!(FarthestHullPoint(Points, [&P0](const FVector3d& P) { return (P - P0).Length(); }, P1) > MinHullSpanCm))
	{
		return false;
	}
	const FVector3d Axis = (P1 - P0).GetSafeNormal();
	FVector3d P2 = P0;
	const double FromLine = FarthestHullPoint(Points, [&P0, &Axis](const FVector3d& P)
	{
		const FVector3d Along = P - P0;
		return (Along - Axis * FVector3d::DotProduct(Along, Axis)).Length();
	}, P2);
	if (!(FromLine > MinHullSpanCm))
	{
		return false;
	}
	const FVector3d Normal = FVector3d::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		return false;
	}
	FVector3d P3 = P0;
	return FarthestHullPoint(Points, [&P0, &Normal](const FVector3d& P) { return FMath::Abs(FVector3d::DotProduct(P - P0, Normal)); }, P3) > MinHullSpanCm;
}
