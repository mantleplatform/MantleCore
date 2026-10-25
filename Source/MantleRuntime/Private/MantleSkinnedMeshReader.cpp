// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMeshReader.h"

#include "MantleCookedCursor.h"
#include "MantleRuntime.h"
#include "MantleSkeletonHash.h"
#include "Math/Float16.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSkinnedMeshReaderPrivate
{
	constexpr uint32 FlagReferencePose = 1 << 0;
	constexpr uint32 FlagGarmentCompatible = 1 << 1;
	constexpr uint32 FlagTangents = 1 << 2;
	constexpr uint32 FlagLongGarment = 1 << 3; // reserved: accepted when set, otherwise unused
	constexpr uint32 FlagHairCards = 1 << 4;
	constexpr uint32 KnownFlags = FlagReferencePose | FlagGarmentCompatible | FlagTangents | FlagLongGarment | FlagHairCards;

	/** Bytes of one reference-pose transform: translation 3, rotation 4, scale 3 float32. */
	constexpr int64 TransformBytes = 10 * 4;

	FMantleSkinnedMeshReadResult FailSkinned(const FString& Error)
	{
		FMantleSkinnedMeshReadResult Result;
		Result.Error = Error;
		return Result;
	}

	FMantleSkinnedMeshReadResult Truncated(const TCHAR* Part)
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh is incomplete or corrupt (%s)."), Part));
	}

	/** Ten float32 as an FTransform, kept exactly as written (FMantleSkinnedMesh::Validate checks it). */
	FTransform ReadPoseTransform(MantleCookedFormat::FCursor& Cursor)
	{
		float Values[10];
		for (float& Value : Values)
		{
			Value = Cursor.F32();
		}
		FTransform Transform;
		Transform.SetTranslation(FVector(Values[0], Values[1], Values[2]));
		Transform.SetRotation(FQuat(Values[3], Values[4], Values[5], Values[6]));
		Transform.SetScale3D(FVector(Values[7], Values[8], Values[9]));
		return Transform;
	}

	/**
	 * Decodes a BOFS block (backend/internal/cook/bodyoffsets.go) against the mesh it rides in. The exact
	 * byte length is worked out from the counts first, so nothing is allocated from a count the bytes cannot back.
	 */
	bool ReadBodyOffsets(const TConstArrayView<uint8> Block, const FMantleSkinnedMesh& Mesh, FMantleBodyOffsets& Out, FString& OutError)
	{
		MantleCookedFormat::FCursor Cursor(Block);
		const uint32 Version = Cursor.U32();
		const uint32 LODCount = Cursor.U32();
		const uint32 BoneCount = Cursor.U32();
		const uint32 ShapeCount = Cursor.U32();
		if (Cursor.Failed())
		{
			OutError = TEXT("its body offsets block is truncated");
			return false;
		}
		if (Version != 1)
		{
			OutError = FString::Printf(TEXT("its body offsets are version %u, this build reads 1"), Version);
			return false;
		}
		if (LODCount != static_cast<uint32>(Mesh.LODs.Num()) || BoneCount != static_cast<uint32>(Mesh.Bones.Num()))
		{
			OutError = TEXT("its body offsets do not match its LOD or bone count");
			return false;
		}
		if (ShapeCount > static_cast<uint32>(FMantleBodyOffsets::NumShapes))
		{
			OutError = FString::Printf(TEXT("its body offsets claim %u shapes (at most %d)"), ShapeCount, FMantleBodyOffsets::NumShapes);
			return false;
		}
		int64 Vectors = BoneCount;
		for (const FMantleSkinnedMeshLOD& LOD : Mesh.LODs)
		{
			Vectors += LOD.NumVertices();
		}
		const int64 Expected = 16 + static_cast<int64>(ShapeCount) * (1 + Vectors * 12);
		if (Expected != Block.Num())
		{
			OutError = FString::Printf(TEXT("its body offsets are %d bytes where their counts need %lld"), Block.Num(), Expected);
			return false;
		}

		Out = FMantleBodyOffsets();
		Out.Shapes.SetNum(FMantleBodyOffsets::NumShapes);
		int32 Previous = -1;
		for (uint32 Index = 0; Index < ShapeCount; ++Index)
		{
			const int32 Id = Cursor.U8();
			if (Id >= FMantleBodyOffsets::NumShapes || Id <= Previous)
			{
				OutError = TEXT("its body offsets name a shape id out of range, repeated or out of order");
				return false;
			}
			Previous = Id;
			FMantleBodyOffsets::FShape& Shape = Out.Shapes[Id];
			Shape.bPresent = true;
			Shape.LODs.SetNum(Mesh.LODs.Num());
			for (int32 LODIndex = 0; LODIndex < Mesh.LODs.Num(); ++LODIndex)
			{
				Cursor.Array(Shape.LODs[LODIndex], Mesh.LODs[LODIndex].NumVertices());
				if (!MantleCookedFormat::AllFinite(Shape.LODs[LODIndex]))
				{
					OutError = TEXT("its body offsets hold a value that is not finite");
					return false;
				}
			}
			Cursor.Array(Shape.Bones, BoneCount);
			if (Cursor.Failed() || !MantleCookedFormat::AllFinite(Shape.Bones))
			{
				OutError = TEXT("its body offsets are truncated or hold a value that is not finite");
				return false;
			}
		}
		return true;
	}

	/**
	 * The extension table as MantleCookedFormat::SkipExtensions reads it, except that the BOFS block is decoded
	 * into Mesh (once, avatar bodies only) and the rest are skipped with their tags in OutSkipped.
	 */
	bool ReadMeshExtensions(MantleCookedFormat::FCursor& Cursor, const TConstArrayView<uint8> Data, const EMantleSkinnedMeshRole Role,
		FMantleSkinnedMesh& Mesh, TArray<FString>& OutSkipped, FString& OutError)
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
			const bool bBodyOffsets = Tag[0] == 'B' && Tag[1] == 'O' && Tag[2] == 'F' && Tag[3] == 'S';
			if (!bBodyOffsets)
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
			// The face carries them too: it owns the neck, which follows a reshaped body.
			if (Role != EMantleSkinnedMeshRole::AvatarBody && Role != EMantleSkinnedMeshRole::AvatarFace)
			{
				OutError = TEXT("only an avatar body or face carries body offsets");
				return false;
			}
			if (!Mesh.BodyOffsets.IsEmpty())
			{
				OutError = TEXT("it has two body offsets blocks");
				return false;
			}
			if (!ReadBodyOffsets(Block, Mesh, Mesh.BodyOffsets, OutError))
			{
				Mesh.BodyOffsets = FMantleBodyOffsets();
				return false;
			}
		}
		return true;
	}
}

FMantleSkinnedMeshReadResult FMantleSkinnedMeshReader::Read(const TConstArrayView<uint8> Data)
{
	using namespace MantleSkinnedMeshReaderPrivate;
	using MantleCookedFormat::FCursor;

	FCursor Cursor(Data);
	if (!Cursor.Magic("MSK1"))
	{
		return FailSkinned(TEXT("That asset is not a Mantle skinned mesh."));
	}
	const uint32 Version = Cursor.U32();
	if (Cursor.Failed())
	{
		return Truncated(TEXT("header"));
	}
	if (Version != SupportedVersion)
	{
		// Specific on purpose: an old client meeting a newer asset should say "update Mantle".
		return FailSkinned(FString::Printf(
			TEXT("That skinned mesh was cooked for a newer version of Mantle (format %u, this build reads %u)."), Version, SupportedVersion));
	}

	FMantleSkinnedMeshReadResult Result;
	FMantleSkinnedMesh& Mesh = Result.Mesh;
	FMantleSkinnedMeshHeader& Header = Result.Header;

	const uint32 Flags = Cursor.U32();
	const uint32 Role = Cursor.U32();
	const uint32 NumInfluences = Cursor.U32();
	const uint32 BoneCount = Cursor.U32();
	const uint32 LODCount = Cursor.U32();
	Header.UVHash = Cursor.U32();
	Header.SourceShapePositionsHash = Cursor.U32();
	FVector3f Min, Max;
	Min.X = Cursor.F32();
	Min.Y = Cursor.F32();
	Min.Z = Cursor.F32();
	Max.X = Cursor.F32();
	Max.Y = Cursor.F32();
	Max.Z = Cursor.F32();
	if (!Cursor.String(Header.Skeleton) || !Cursor.String(Header.SourceShape))
	{
		return Truncated(TEXT("header"));
	}

	if ((Flags & ~KnownFlags) != 0)
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh uses flags this build does not know (0x%x)."), Flags));
	}
	// Roles 0 to 4: avatar body, face, garment, place skeletal mesh, vehicle body.
	if (Role > static_cast<uint32>(EMantleSkinnedMeshRole::VehicleBody))
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh has an unknown role (%u)."), Role));
	}
	if (NumInfluences < 1 || NumInfluences > static_cast<uint32>(MaxInfluences) || NumInfluences > static_cast<uint32>(MAX_TOTAL_INFLUENCES))
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh claims %u influences per vertex (1 to %d)."), NumInfluences, MaxInfluences));
	}
	if (BoneCount < 1 || BoneCount > static_cast<uint32>(MaxBones))
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh claims %u bones (1 to %d)."), BoneCount, MaxBones));
	}
	if (LODCount < 1 || LODCount > static_cast<uint32>(MaxLODs))
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh claims %u LODs (1 to %d)."), LODCount, MaxLODs));
	}
	if (Min.ContainsNaN() || Max.ContainsNaN() || Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
	{
		return FailSkinned(TEXT("That skinned mesh has invalid bounds."));
	}
	if (Header.Skeleton.IsEmpty())
	{
		return FailSkinned(TEXT("That skinned mesh names no skeleton."));
	}
	// A vehicle body, and only a vehicle body, is on its own bone table, named "vehicle".
	if ((Role == static_cast<uint32>(EMantleSkinnedMeshRole::VehicleBody))
		!= Header.Skeleton.Equals(VehicleSkeleton, ESearchCase::CaseSensitive))
	{
		return FailSkinned(TEXT("That skinned mesh is corrupt: only a vehicle body is on the vehicle skeleton."));
	}

	Header.Role = static_cast<EMantleSkinnedMeshRole>(Role);
	Header.bGarmentCompatible = (Flags & FlagGarmentCompatible) != 0;
	Header.bHairCards = (Flags & FlagHairCards) != 0;
	Header.Bounds = FBox3f(Min, Max);
	const bool bGarment = Header.Role == EMantleSkinnedMeshRole::Garment;
	const bool bReferencePose = (Flags & FlagReferencePose) != 0;
	const bool bTangents = (Flags & FlagTangents) != 0;

	// The worker's own rules (SkinnedMesh.Validate): a garment names its body/head provenance and carries no
	// pose; avatars and faces carry their pose and name no shape.
	if (bGarment == Header.SourceShape.IsEmpty())
	{
		return FailSkinned(TEXT("That skinned mesh is corrupt: only a garment or hair names its authoring body or head provenance."));
	}
	if (bGarment == bReferencePose)
	{
		return FailSkinned(TEXT("That skinned mesh is corrupt: avatars, faces, place skeletal meshes and vehicle bodies carry a reference pose and garments do not."));
	}
	// A place skeletal mesh names its own skeleton by hash, and nothing else does.
	const bool bPlaceMesh = Header.Role == EMantleSkinnedMeshRole::PlaceMesh;
	if (bPlaceMesh != FMantleSkeletonHash::IsHash(Header.Skeleton))
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh is corrupt: skeleton '%s' does not fit its role (only a place skeletal mesh names its skeleton by hash)."),
			*Header.Skeleton));
	}

	// Bone table: each entry is at least a str16 length and a parent.
	if (!Cursor.Has(static_cast<int64>(BoneCount) * 6))
	{
		return Truncated(TEXT("bone table"));
	}
	Mesh.Bones.SetNum(static_cast<int32>(BoneCount));
	for (FMantleSkinnedMeshBone& Bone : Mesh.Bones)
	{
		FString Name;
		Cursor.String(Name);
		Bone.ParentIndex = Cursor.I32();
		if (Cursor.Failed())
		{
			return Truncated(TEXT("bone table"));
		}
		if (Name.IsEmpty())
		{
			return FailSkinned(TEXT("That skinned mesh has a bone without a name."));
		}
		Bone.Name = FName(*Name);
	}

	if (bReferencePose)
	{
		if (!Cursor.Has(static_cast<int64>(BoneCount) * TransformBytes))
		{
			return Truncated(TEXT("reference pose"));
		}
		Mesh.ReferencePose.Reserve(static_cast<int32>(BoneCount));
		for (uint32 Bone = 0; Bone < BoneCount; ++Bone)
		{
			Mesh.ReferencePose.Add(ReadPoseTransform(Cursor));
		}
	}

	Mesh.LODs.SetNum(static_cast<int32>(LODCount));
	for (int32 LODIndex = 0; LODIndex < Mesh.LODs.Num(); ++LODIndex)
	{
		FMantleSkinnedMeshLOD& LOD = Mesh.LODs[LODIndex];
		LOD.NumInfluences = static_cast<int32>(NumInfluences);
		LOD.bFullPrecisionUVs = true;

		const uint32 VertexCount = Cursor.U32();
		const uint32 IndexCount = Cursor.U32();
		if (Cursor.Failed())
		{
			return Truncated(TEXT("LOD header"));
		}
		if (VertexCount < 1 || VertexCount > static_cast<uint32>(MaxVertices))
		{
			return FailSkinned(FString::Printf(TEXT("That skinned mesh's LOD%d claims %u vertices (1 to %d)."), LODIndex, VertexCount, MaxVertices));
		}
		if (IndexCount < 3 || IndexCount > static_cast<uint32>(MaxIndices) || IndexCount % 3 != 0)
		{
			return FailSkinned(FString::Printf(TEXT("That skinned mesh's LOD%d claims %u indices, which cannot form triangles."), LODIndex, IndexCount));
		}

		// The section table MRM2 shares: whole triangles inside the indices, material names unique
		// (sections are bound to materials by name).
		FString SectionError;
		if (!MantleCookedFormat::ReadSectionTable(Cursor, IndexCount, LOD.Sections, SectionError))
		{
			return FailSkinned(FString::Printf(TEXT("That skinned mesh's LOD%d is corrupt: %s."), LODIndex, *SectionError));
		}

		// Everything the LOD's counts promise is checked against the bytes left before any array is sized.
		int64 PerVertex = 12 + 12 + 8 + static_cast<int64>(NumInfluences) * (2 + 1);
		PerVertex += bTangents ? 16 : 0;
		PerVertex += Header.bHairCards ? 6 : 0;
		const int64 Promised = static_cast<int64>(VertexCount) * PerVertex + static_cast<int64>(IndexCount) * 4;
		if (!Cursor.Has(Promised))
		{
			return FailSkinned(FString::Printf(TEXT("That skinned mesh is incomplete: LOD%d promises %lld more bytes and the file holds %lld."),
				LODIndex, Promised, Cursor.Remaining()));
		}

		const int64 V = VertexCount;
		const int64 Slots = V * NumInfluences;
		Cursor.Array(LOD.Positions, V);
		Cursor.Array(LOD.Normals, V);
		if (bTangents)
		{
			Cursor.Array(LOD.Tangents, V);
		}
		Cursor.Array(LOD.UVs, V);
		Cursor.Array(LOD.Indices, IndexCount);
		Cursor.Array(LOD.InfluenceBones, Slots);
		Cursor.Array(LOD.InfluenceWeights, Slots);
		if (Header.bHairCards)
		{
			TArray<uint16> Halves;
			Cursor.Array(Halves, V * 3);
			if (!Cursor.Failed())
			{
				LOD.HairCards.SetNumUninitialized(static_cast<int32>(V));
				for (int32 Vertex = 0; Vertex < LOD.HairCards.Num(); ++Vertex)
				{
					FVector3f& Card = LOD.HairCards[Vertex];
					for (int32 Component = 0; Component < 3; ++Component)
					{
						FFloat16 Half;
						Half.Encoded = Halves[Vertex * 3 + Component];
						if ((Half.Encoded & 0x7c00) == 0x7c00)
						{
							return FailSkinned(FString::Printf(TEXT("That skinned mesh's LOD%d has non-finite hair card data."), LODIndex));
						}
						Card[Component] = Half.GetFloat();
					}
				}
			}
		}
		if (Cursor.Failed())
		{
			return Truncated(TEXT("vertex data"));
		}

		// Validate checks positions; the builder feeds the rest to the GPU, so they must be finite too.
		if (!MantleCookedFormat::AllFinite(LOD.Normals) || !MantleCookedFormat::AllFinite(LOD.Tangents) || !MantleCookedFormat::AllFinite(LOD.UVs))
		{
			return FailSkinned(FString::Printf(TEXT("That skinned mesh's LOD%d has non-finite normals, tangents or UVs."), LODIndex));
		}
	}

	FString ExtensionError;
	if (!ReadMeshExtensions(Cursor, Data, Header.Role, Mesh, Header.SkippedExtensions, ExtensionError))
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh is corrupt: %s."), *ExtensionError));
	}
	if (Cursor.Remaining() != 0)
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh is corrupt: %lld bytes follow its end."), Cursor.Remaining()));
	}

	// The worker's rule (SkinnedMesh.Validate): a lower LOD draws only LOD0's materials, each in LOD0's
	// slot, so one material list serves every LOD of the built mesh.
	for (int32 LODIndex = 1; LODIndex < Mesh.LODs.Num(); ++LODIndex)
	{
		for (const FMantleSkinnedMeshSection& Section : Mesh.LODs[LODIndex].Sections)
		{
			const FMantleSkinnedMeshSection* InLOD0 = Mesh.LODs[0].Sections.FindByPredicate(
				[&Section](const FMantleSkinnedMeshSection& Candidate) { return Candidate.MaterialName == Section.MaterialName; });
			if (!InLOD0 || InLOD0->MaterialIndex != Section.MaterialIndex)
			{
				return FailSkinned(FString::Printf(TEXT("That skinned mesh is corrupt: its LOD%d draws material %s, which LOD0 does not draw in that slot."),
					LODIndex, *Section.MaterialName.ToString()));
			}
		}
	}

	// The structural rules the builder and the fitter rely on: indices and sections in range, influences
	// naming real bones, weights summing to 255, a parent-first bone table with one root and unique
	// names, a finite unit-rotation pose.
	FString Invalid;
	if (!Mesh.Validate(Invalid))
	{
		return FailSkinned(FString::Printf(TEXT("That skinned mesh is corrupt: %s."), *Invalid));
	}
	if (bPlaceMesh)
	{
		// The hash its clips carry must be the one its own bone table has (the worker wrote it so).
		const FString Computed = FMantleSkeletonHash::Compute(Mesh.Bones);
		if (!Computed.Equals(Header.Skeleton, ESearchCase::CaseSensitive))
		{
			return FailSkinned(FString::Printf(TEXT("That skinned mesh is corrupt: it names skeleton %s, but its bone table is %s."),
				*Header.Skeleton, *Computed));
		}
	}

	if (Header.SkippedExtensions.Num() > 0)
	{
		UE_LOG(LogMantle, Verbose, TEXT("Skinned mesh: skipped %d unknown extension block(s): %s"),
			Header.SkippedExtensions.Num(), *FString::Join(Header.SkippedExtensions, TEXT(", ")));
	}
	const TCHAR* RoleName = bGarment ? TEXT("garment")
		: bPlaceMesh ? TEXT("place skeletal mesh")
		: Header.Role == EMantleSkinnedMeshRole::VehicleBody ? TEXT("vehicle body")
		: (Header.Role == EMantleSkinnedMeshRole::AvatarFace ? TEXT("avatar face") : TEXT("avatar body"));
	UE_LOG(LogMantle, Verbose, TEXT("Read a skinned mesh: %s on %s, %d bones, %d LOD(s), LOD0 %d vertices, %d triangles"),
		RoleName, *Header.Skeleton, Mesh.Bones.Num(), Mesh.LODs.Num(), Mesh.LODs[0].NumVertices(), Mesh.LODs[0].NumTriangles());

	Result.bSuccess = true;
	return Result;
}
