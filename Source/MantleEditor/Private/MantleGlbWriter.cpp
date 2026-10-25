// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const TCHAR* FMantleGlbWriter::BodyOffsetsExtension = TEXT("MANTLE_body_offsets");
const TCHAR* FMantleGlbWriter::BodyShapeExtension = TEXT("MANTLE_body_shape");
const TCHAR* FMantleGlbWriter::HairRootUVAttribute = TEXT("_MANTLE_HAIR_ROOT_UV");
const TCHAR* FMantleGlbWriter::HairCoordUAttribute = TEXT("_MANTLE_HAIR_COORD_U");
const TCHAR* FMantleGlbWriter::LODNodePrefix = TEXT("LOD");
const TCHAR* FMantleGlbWriter::StaticMeshExtension = TEXT("MANTLE_static_mesh");

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGlbWriterPrivate
{
	constexpr uint32 GlbMagic = 0x46546C67u;      // "glTF"
	constexpr uint32 ChunkJson = 0x4E4F534Au;     // "JSON"
	constexpr uint32 ChunkBin = 0x004E4942u;      // "BIN\0"
	constexpr int32 ComponentUnsignedByte = 5121;
	constexpr int32 ComponentUnsignedShort = 5123;
	constexpr int32 ComponentUnsignedInt = 5125;
	constexpr int32 ComponentFloat = 5126;
	constexpr int32 TargetArrayBuffer = 34962;
	constexpr int32 TargetElementArrayBuffer = 34963;
	constexpr double MetresPerCentimetre = 0.01;

	/** Column-major 4x4, the glTF layout (element [Col * 4 + Row]). */
	struct FGlbMatrix
	{
		double M[16];

		static FGlbMatrix Identity()
		{
			FGlbMatrix Out;
			for (int32 Index = 0; Index < 16; ++Index)
			{
				Out.M[Index] = (Index % 5 == 0) ? 1.0 : 0.0;
			}
			return Out;
		}

		/** T * R * S, the glTF node transform. */
		static FGlbMatrix FromTRS(const FVector3d& T, const FQuat& Q, const FVector3d& S)
		{
			const double X = Q.X, Y = Q.Y, Z = Q.Z, W = Q.W;
			const double R[9] = {
				1 - 2 * (Y * Y + Z * Z), 2 * (X * Y + Z * W), 2 * (X * Z - Y * W),   // column 0
				2 * (X * Y - Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z + X * W),   // column 1
				2 * (X * Z + Y * W), 2 * (Y * Z - X * W), 1 - 2 * (X * X + Y * Y),   // column 2
			};
			FGlbMatrix Out = Identity();
			const double Scale[3] = { S.X, S.Y, S.Z };
			for (int32 Col = 0; Col < 3; ++Col)
			{
				for (int32 Row = 0; Row < 3; ++Row)
				{
					Out.M[Col * 4 + Row] = R[Col * 3 + Row] * Scale[Col];
				}
			}
			Out.M[12] = T.X;
			Out.M[13] = T.Y;
			Out.M[14] = T.Z;
			return Out;
		}

		FGlbMatrix operator*(const FGlbMatrix& B) const
		{
			FGlbMatrix Out;
			for (int32 Col = 0; Col < 4; ++Col)
			{
				for (int32 Row = 0; Row < 4; ++Row)
				{
					double Sum = 0.0;
					for (int32 K = 0; K < 4; ++K)
					{
						Sum += M[K * 4 + Row] * B.M[Col * 4 + K];
					}
					Out.M[Col * 4 + Row] = Sum;
				}
			}
			return Out;
		}

		/** Inverse of an affine matrix; false when its 3x3 part is singular. */
		bool InverseAffine(FGlbMatrix& Out) const
		{
			const double A = M[0], B = M[4], C = M[8];
			const double D = M[1], E = M[5], F = M[9];
			const double G = M[2], H = M[6], I = M[10];
			const double Det = A * (E * I - F * H) - B * (D * I - F * G) + C * (D * H - E * G);
			if (FMath::Abs(Det) < 1e-12)
			{
				return false;
			}
			const double InvDet = 1.0 / Det;
			const double Inv[9] = {
				(E * I - F * H) * InvDet, (C * H - B * I) * InvDet, (B * F - C * E) * InvDet,
				(F * G - D * I) * InvDet, (A * I - C * G) * InvDet, (C * D - A * F) * InvDet,
				(D * H - E * G) * InvDet, (B * G - A * H) * InvDet, (A * E - B * D) * InvDet,
			};
			// Inv is row-major here: Inv[Row * 3 + Col].
			Out = Identity();
			for (int32 Row = 0; Row < 3; ++Row)
			{
				for (int32 Col = 0; Col < 3; ++Col)
				{
					Out.M[Col * 4 + Row] = Inv[Row * 3 + Col];
				}
			}
			for (int32 Row = 0; Row < 3; ++Row)
			{
				Out.M[12 + Row] = -(Out.M[Row] * M[12] + Out.M[4 + Row] * M[13] + Out.M[8 + Row] * M[14]);
			}
			return true;
		}
	};

	/** The binary chunk being built: views appended at 4-byte boundaries. */
	struct FGlbBuffer
	{
		TArray<uint8> Bytes;
		TArray<TSharedPtr<FJsonValue>> Views;

		int32 AddView(const void* Data, int64 Size, int32 Target)
		{
			while (Bytes.Num() % 4 != 0)
			{
				Bytes.Add(0);
			}
			const int32 Offset = Bytes.Num();
			Bytes.Append(static_cast<const uint8*>(Data), static_cast<int32>(Size));
			const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
			View->SetNumberField(TEXT("buffer"), 0);
			View->SetNumberField(TEXT("byteOffset"), Offset);
			View->SetNumberField(TEXT("byteLength"), static_cast<double>(Size));
			if (Target != 0)
			{
				View->SetNumberField(TEXT("target"), Target);
			}
			Views.Add(MakeShared<FJsonValueObject>(View));
			return Views.Num() - 1;
		}
	};

	TSharedRef<FJsonValueArray> NumberArray(const TArray<double>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const double Value : Values)
		{
			Out.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return MakeShared<FJsonValueArray>(Out);
	}

	struct FAccessorList
	{
		TArray<TSharedPtr<FJsonValue>> Accessors;

		int32 Add(int32 View, int64 ByteOffset, int32 ComponentType, int32 Count, const TCHAR* Type,
			const TArray<double>* Min = nullptr, const TArray<double>* Max = nullptr)
		{
			const TSharedRef<FJsonObject> Accessor = MakeShared<FJsonObject>();
			Accessor->SetNumberField(TEXT("bufferView"), View);
			if (ByteOffset != 0)
			{
				Accessor->SetNumberField(TEXT("byteOffset"), static_cast<double>(ByteOffset));
			}
			Accessor->SetNumberField(TEXT("componentType"), ComponentType);
			Accessor->SetNumberField(TEXT("count"), Count);
			Accessor->SetStringField(TEXT("type"), Type);
			if (Min && Max)
			{
				Accessor->SetField(TEXT("min"), NumberArray(*Min));
				Accessor->SetField(TEXT("max"), NumberArray(*Max));
			}
			Accessors.Add(MakeShared<FJsonValueObject>(Accessor));
			return Accessors.Num() - 1;
		}
	};

	bool Fail(FString& OutError, const FString& Error)
	{
		OutError = Error;
		return false;
	}

	void AppendU32(TArray<uint8>& Out, uint32 Value)
	{
		Out.Add(static_cast<uint8>(Value));
		Out.Add(static_cast<uint8>(Value >> 8));
		Out.Add(static_cast<uint8>(Value >> 16));
		Out.Add(static_cast<uint8>(Value >> 24));
	}

	uint32 ReadU32(const uint8* Data)
	{
		return uint32(Data[0]) | (uint32(Data[1]) << 8) | (uint32(Data[2]) << 16) | (uint32(Data[3]) << 24);
	}
}

FVector3d FMantleGlbWriter::ToGltfPosition(const FVector3d& Unreal)
{
	using namespace MantleGlbWriterPrivate;
	return FVector3d(Unreal.X * MetresPerCentimetre, Unreal.Z * MetresPerCentimetre, Unreal.Y * MetresPerCentimetre);
}

FVector3d FMantleGlbWriter::ToGltfDirection(const FVector3d& Unreal)
{
	return FVector3d(Unreal.X, Unreal.Z, Unreal.Y);
}

FQuat FMantleGlbWriter::ToGltfRotation(const FQuat& Unreal)
{
	FQuat Out(-Unreal.X, -Unreal.Z, -Unreal.Y, Unreal.W);
	Out.Normalize();
	return Out;
}

FVector3d FMantleGlbWriter::ToGltfScale(const FVector3d& Unreal)
{
	return FVector3d(Unreal.X, Unreal.Z, Unreal.Y);
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGlbWriterPrivate
{
	/** Every rule one mesh's arrays must keep; What names it in the message ("the mesh", "LOD1"). */
	bool CheckGlbMesh(const FMantleGlbMesh& Mesh, const TCHAR* What, FString& OutError)
	{
		const int32 NumVertices = Mesh.Positions.Num();
		if (NumVertices == 0)
		{
			return Fail(OutError, FString::Printf(TEXT("%s has no vertices"), What));
		}
		if (Mesh.Normals.Num() != NumVertices || Mesh.UVs.Num() != NumVertices)
		{
			return Fail(OutError, FString::Printf(TEXT("%s's normals or texture coordinates do not match its vertices"), What));
		}
		if (Mesh.Tangents.Num() != 0 && Mesh.Tangents.Num() != NumVertices)
		{
			return Fail(OutError, FString::Printf(TEXT("%s's tangents do not match its vertices"), What));
		}
		if ((Mesh.UV1s.Num() != 0 && Mesh.UV1s.Num() != NumVertices) || (Mesh.Colors.Num() != 0 && Mesh.Colors.Num() != NumVertices))
		{
			return Fail(OutError, FString::Printf(TEXT("%s's second UVs or vertex colours do not match its vertices"), What));
		}
		if ((Mesh.HairRootUV.Num() > 0) != (Mesh.HairCoordU.Num() > 0)
			|| (Mesh.HairRootUV.Num() > 0 && (Mesh.HairRootUV.Num() != NumVertices || Mesh.HairCoordU.Num() != NumVertices)))
		{
			return Fail(OutError, TEXT("hair card data must cover every vertex, root UV and CoordU together"));
		}
		if (Mesh.Primitives.Num() == 0)
		{
			return Fail(OutError, FString::Printf(TEXT("%s has no primitives"), What));
		}
		if (Mesh.MaterialNames.Num() == 0)
		{
			return Fail(OutError, FString::Printf(TEXT("%s has no materials"), What));
		}
		{
			TSet<FString> Seen;
			for (const FString& Material : Mesh.MaterialNames)
			{
				bool bDuplicate = false;
				Seen.Add(Material, &bDuplicate);
				if (bDuplicate || Material.IsEmpty())
				{
					return Fail(OutError, FString::Printf(TEXT("two materials are named \"%s\" (or one has no name); sections are identified by material name"), *Material));
				}
			}
		}
		int32 Covered = 0;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			if (Primitive.FirstVertex != Covered || Primitive.NumVertices <= 0 || !Mesh.MaterialNames.IsValidIndex(Primitive.MaterialIndex)
				|| Primitive.Indices.Num() == 0 || Primitive.Indices.Num() % 3 != 0)
			{
				return Fail(OutError, TEXT("the primitives must tile the vertices in order, name a material and hold whole triangles"));
			}
			for (const uint32 Index : Primitive.Indices)
			{
				if (Index >= static_cast<uint32>(Primitive.NumVertices))
				{
					return Fail(OutError, TEXT("a primitive's index points past its vertices"));
				}
			}
			Covered += Primitive.NumVertices;
		}
		if (Covered != NumVertices)
		{
			return Fail(OutError, TEXT("the primitives do not cover every vertex"));
		}
		if (Mesh.IsSkinned())
		{
			if (Mesh.NumInfluences < 1 || Mesh.NumInfluences > 8
				|| Mesh.InfluenceJoints.Num() != NumVertices * Mesh.NumInfluences
				|| Mesh.InfluenceWeights.Num() != NumVertices * Mesh.NumInfluences)
			{
				return Fail(OutError, TEXT("the skin weights do not match the vertices (1 to 8 influences each)"));
			}
			for (int32 Joint = 0; Joint < Mesh.Joints.Num(); ++Joint)
			{
				if (Mesh.Joints[Joint].Parent >= Joint || Mesh.Joints[Joint].Name.IsNone())
				{
					return Fail(OutError, TEXT("the joints must be named and parent-first"));
				}
			}
			for (const uint16 Joint : Mesh.InfluenceJoints)
			{
				if (Joint >= Mesh.Joints.Num())
				{
					return Fail(OutError, TEXT("a skin weight names a joint the skin does not have"));
				}
			}
		}
		if ((Mesh.WarpSampleIndices.Num() > 0) != (Mesh.WarpInterpolationWeights.Num() > 0))
		{
			return Fail(OutError, TEXT("warp weights need both sample indices and interpolation weights"));
		}
		if (Mesh.HasNeck())
		{
			if (Mesh.WarpSampleIndices.Num() == 0)
			{
				return Fail(OutError, TEXT("a neck rides beside the body's warp weights, which are missing"));
			}
			const bool bKnownMethod = Mesh.NeckMethod.Equals(TEXT("correspondence"), ESearchCase::CaseSensitive)
				|| Mesh.NeckMethod.Equals(TEXT("rays"), ESearchCase::CaseSensitive);
			if (!bKnownMethod || Mesh.NeckIds.Num() != Mesh.NeckPositions.Num()
				|| Mesh.NeckSampleIndices.Num() == 0 || Mesh.NeckInterpolationWeights.Num() == 0)
			{
				return Fail(OutError, TEXT("a neck needs its method, one id per sample, sample indices and interpolation weights"));
			}
		}
		return true;
	}

	/**
	 * One mesh's vertex streams (each one view over the whole mesh, converted to glTF's axes) and its
	 * primitives (accessors into those views at each primitive's first vertex). MaterialOf maps the mesh's
	 * material index to the file's.
	 */
	TArray<TSharedPtr<FJsonValue>> AppendGlbMeshPrimitives(const FMantleGlbMesh& Mesh, const TArray<int32>& MaterialOf, FGlbBuffer& Buffer, FAccessorList& Accessors)
	{
		const int32 NumVertices = Mesh.Positions.Num();
		const bool bSkinned = Mesh.IsSkinned();

		// Vertex streams, converted, each one view over the whole mesh.
		TArray<float> Positions, Normals, Tangents, UVs;
		Positions.Reserve(NumVertices * 3);
		Normals.Reserve(NumVertices * 3);
		UVs.Reserve(NumVertices * 2);
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			const FVector3d P = FMantleGlbWriter::ToGltfPosition(FVector3d(Mesh.Positions[Vertex]));
			const FVector3d N = FMantleGlbWriter::ToGltfDirection(FVector3d(Mesh.Normals[Vertex]));
			Positions.Append({ static_cast<float>(P.X), static_cast<float>(P.Y), static_cast<float>(P.Z) });
			Normals.Append({ static_cast<float>(N.X), static_cast<float>(N.Y), static_cast<float>(N.Z) });
			UVs.Append({ Mesh.UVs[Vertex].X, Mesh.UVs[Vertex].Y });
			if (Mesh.Tangents.Num() > 0)
			{
				const FVector4f& T = Mesh.Tangents[Vertex];
				const FVector3d D = FMantleGlbWriter::ToGltfDirection(FVector3d(T.X, T.Y, T.Z));
				Tangents.Append({ static_cast<float>(D.X), static_cast<float>(D.Y), static_cast<float>(D.Z), T.W < 0.0f ? -1.0f : 1.0f });
			}
		}
		const int32 PositionView = Buffer.AddView(Positions.GetData(), Positions.Num() * sizeof(float), TargetArrayBuffer);
		const int32 NormalView = Buffer.AddView(Normals.GetData(), Normals.Num() * sizeof(float), TargetArrayBuffer);
		const int32 UVView = Buffer.AddView(UVs.GetData(), UVs.Num() * sizeof(float), TargetArrayBuffer);
		const int32 TangentView = Tangents.Num() > 0
			? Buffer.AddView(Tangents.GetData(), Tangents.Num() * sizeof(float), TargetArrayBuffer) : INDEX_NONE;

		// #470: the second UV set and the vertex colours, when the mesh has them. Colours are written R, G, B, A
		// (FColor's memory order is not), as the normalized unsigned bytes GLTFExporter writes.
		int32 UV1View = INDEX_NONE;
		if (Mesh.UV1s.Num() > 0)
		{
			TArray<float> UV1s;
			UV1s.Reserve(NumVertices * 2);
			for (const FVector2f& UV : Mesh.UV1s)
			{
				UV1s.Append({ UV.X, UV.Y });
			}
			UV1View = Buffer.AddView(UV1s.GetData(), UV1s.Num() * sizeof(float), TargetArrayBuffer);
		}
		int32 ColorView = INDEX_NONE;
		if (Mesh.Colors.Num() > 0)
		{
			TArray<uint8> ColorBytes;
			ColorBytes.Reserve(NumVertices * 4);
			for (const FColor& Color : Mesh.Colors)
			{
				ColorBytes.Append({ Color.R, Color.G, Color.B, Color.A });
			}
			ColorView = Buffer.AddView(ColorBytes.GetData(), ColorBytes.Num(), TargetArrayBuffer);
		}

		// Skin streams: JOINTS_0/WEIGHTS_0, and JOINTS_1/WEIGHTS_1 past four influences.
		const int32 SkinSets = bSkinned ? (Mesh.NumInfluences > 4 ? 2 : 1) : 0;
		int32 JointViews[2] = { INDEX_NONE, INDEX_NONE };
		int32 WeightViews[2] = { INDEX_NONE, INDEX_NONE };
		for (int32 Set = 0; Set < SkinSets; ++Set)
		{
			TArray<uint16> Joints;
			TArray<float> Weights;
			Joints.SetNumZeroed(NumVertices * 4);
			Weights.SetNumZeroed(NumVertices * 4);
			for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
			{
				float Total = 0.0f;
				for (int32 Slot = 0; Slot < Mesh.NumInfluences; ++Slot)
				{
					Total += FMath::Max(0.0f, Mesh.InfluenceWeights[Vertex * Mesh.NumInfluences + Slot]);
				}
				for (int32 Slot = 0; Slot < 4; ++Slot)
				{
					const int32 Source = Set * 4 + Slot;
					if (Source >= Mesh.NumInfluences)
					{
						continue;
					}
					const float Weight = FMath::Max(0.0f, Mesh.InfluenceWeights[Vertex * Mesh.NumInfluences + Source]);
					if (Weight <= 0.0f)
					{
						continue;
					}
					Joints[Vertex * 4 + Slot] = Mesh.InfluenceJoints[Vertex * Mesh.NumInfluences + Source];
					Weights[Vertex * 4 + Slot] = Total > 0.0f ? Weight / Total : 0.0f;
				}
			}
			JointViews[Set] = Buffer.AddView(Joints.GetData(), Joints.Num() * sizeof(uint16), TargetArrayBuffer);
			WeightViews[Set] = Buffer.AddView(Weights.GetData(), Weights.Num() * sizeof(float), TargetArrayBuffer);
		}

		int32 RootUVView = INDEX_NONE;
		int32 CoordUView = INDEX_NONE;
		if (Mesh.HairRootUV.Num() > 0)
		{
			TArray<float> Root;
			Root.Reserve(NumVertices * 2);
			for (const FVector2f& UV : Mesh.HairRootUV)
			{
				Root.Append({ UV.X, UV.Y });
			}
			RootUVView = Buffer.AddView(Root.GetData(), Root.Num() * sizeof(float), TargetArrayBuffer);
			CoordUView = Buffer.AddView(Mesh.HairCoordU.GetData(), Mesh.HairCoordU.Num() * sizeof(float), TargetArrayBuffer);
		}

		TArray<uint32> AllIndices;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			AllIndices.Append(Primitive.Indices);
		}
		const int32 IndexView = Buffer.AddView(AllIndices.GetData(), AllIndices.Num() * sizeof(uint32), TargetElementArrayBuffer);

		// Primitives: accessors into the shared views at each primitive's first vertex.
		TArray<TSharedPtr<FJsonValue>> Primitives;
		int64 IndexOffset = 0;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			const int64 First = Primitive.FirstVertex;
			const int32 Count = Primitive.NumVertices;

			TArray<double> Min = { TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Max() };
			TArray<double> Max = { TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest() };
			for (int32 Vertex = 0; Vertex < Count; ++Vertex)
			{
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Value = Positions[static_cast<int32>((First + Vertex) * 3 + Axis)];
					Min[Axis] = FMath::Min(Min[Axis], Value);
					Max[Axis] = FMath::Max(Max[Axis], Value);
				}
			}

			const TSharedRef<FJsonObject> Attributes = MakeShared<FJsonObject>();
			Attributes->SetNumberField(TEXT("POSITION"), Accessors.Add(PositionView, First * 12, ComponentFloat, Count, TEXT("VEC3"), &Min, &Max));
			Attributes->SetNumberField(TEXT("NORMAL"), Accessors.Add(NormalView, First * 12, ComponentFloat, Count, TEXT("VEC3")));
			Attributes->SetNumberField(TEXT("TEXCOORD_0"), Accessors.Add(UVView, First * 8, ComponentFloat, Count, TEXT("VEC2")));
			if (TangentView != INDEX_NONE)
			{
				Attributes->SetNumberField(TEXT("TANGENT"), Accessors.Add(TangentView, First * 16, ComponentFloat, Count, TEXT("VEC4")));
			}
			if (UV1View != INDEX_NONE)
			{
				Attributes->SetNumberField(TEXT("TEXCOORD_1"), Accessors.Add(UV1View, First * 8, ComponentFloat, Count, TEXT("VEC2")));
			}
			if (ColorView != INDEX_NONE)
			{
				const int32 ColorAccessor = Accessors.Add(ColorView, First * 4, ComponentUnsignedByte, Count, TEXT("VEC4"));
				Accessors.Accessors[ColorAccessor]->AsObject()->SetBoolField(TEXT("normalized"), true);
				Attributes->SetNumberField(TEXT("COLOR_0"), ColorAccessor);
			}
			for (int32 Set = 0; Set < SkinSets; ++Set)
			{
				Attributes->SetNumberField(FString::Printf(TEXT("JOINTS_%d"), Set),
					Accessors.Add(JointViews[Set], First * 8, ComponentUnsignedShort, Count, TEXT("VEC4")));
				Attributes->SetNumberField(FString::Printf(TEXT("WEIGHTS_%d"), Set),
					Accessors.Add(WeightViews[Set], First * 16, ComponentFloat, Count, TEXT("VEC4")));
			}
			if (RootUVView != INDEX_NONE)
			{
				Attributes->SetNumberField(FMantleGlbWriter::HairRootUVAttribute, Accessors.Add(RootUVView, First * 8, ComponentFloat, Count, TEXT("VEC2")));
				Attributes->SetNumberField(FMantleGlbWriter::HairCoordUAttribute, Accessors.Add(CoordUView, First * 4, ComponentFloat, Count, TEXT("SCALAR")));
			}

			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetObjectField(TEXT("attributes"), Attributes);
			Json->SetNumberField(TEXT("indices"), Accessors.Add(IndexView, IndexOffset * 4, ComponentUnsignedInt, Primitive.Indices.Num(), TEXT("SCALAR")));
			Json->SetNumberField(TEXT("material"), MaterialOf[Primitive.MaterialIndex]);
			Json->SetNumberField(TEXT("mode"), 4);
			Primitives.Add(MakeShared<FJsonValueObject>(Json));
			IndexOffset += Primitive.Indices.Num();
		}
		return Primitives;
	}
}

bool FMantleGlbWriter::Write(const FMantleGlbMesh& Mesh, TArray<uint8>& OutBytes, FString& OutError)
{
	return WriteLODs(TConstArrayView<FMantleGlbMesh>(&Mesh, 1), OutBytes, OutError);
}

bool FMantleGlbWriter::WriteLODs(const TConstArrayView<FMantleGlbMesh> LODs, TArray<uint8>& OutBytes, FString& OutError)
{
	return WriteStatic(LODs, FMantleGlbStaticExtras(), OutBytes, OutError);
}

bool FMantleGlbWriter::WriteGeometryOnly(const FMantleGlbMesh& Mesh, TArray<uint8>& OutBytes, FString& OutError)
{
	return WriteGeometryOnlyLODs(TConstArrayView<FMantleGlbMesh>(&Mesh, 1), OutBytes, OutError);
}

bool FMantleGlbWriter::WriteGeometryOnlyLODs(const TConstArrayView<FMantleGlbMesh> LODs, TArray<uint8>& OutBytes, FString& OutError)
{
	TArray<FMantleGlbMesh> GeometryOnly;
	GeometryOnly.Reserve(LODs.Num());
	for (const FMantleGlbMesh& LOD : LODs)
	{
		FMantleGlbMesh& Sanitized = GeometryOnly.Add_GetRef(LOD);
		// COLOR_* is appearance data and is rejected by the converted-avatar publication contract.
		Sanitized.Colors.Reset();
	}
	return WriteLODs(GeometryOnly, OutBytes, OutError);
}

bool FMantleGlbWriter::WriteStatic(const TConstArrayView<FMantleGlbMesh> LODs, const FMantleGlbStaticExtras& Extras, TArray<uint8>& OutBytes, FString& OutError)
{
	using namespace MantleGlbWriterPrivate;

	OutBytes.Reset();
	if (LODs.Num() == 0 || LODs.Num() > MaxLODs)
	{
		return Fail(OutError, FString::Printf(TEXT("%d LODs; an upload carries 1 to %d"), LODs.Num(), MaxLODs));
	}
	const FMantleGlbMesh& Mesh = LODs[0];
	if (!Mesh.AuthoringSourceHash.IsEmpty())
	{
		bool bValidHash = Mesh.AuthoringSourceHash.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive) && Mesh.AuthoringSourceHash.Len() == 71;
		for (int32 CharacterIndex = 7; bValidHash && CharacterIndex < Mesh.AuthoringSourceHash.Len(); ++CharacterIndex)
		{
			const TCHAR Character = Mesh.AuthoringSourceHash[CharacterIndex];
			bValidHash = (Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'));
		}
		if (!bValidHash) { return Fail(OutError, TEXT("the authoring source needs a complete lowercase SHA256 content hash")); }
	}
	if (!CheckGlbMesh(Mesh, LODs.Num() > 1 ? TEXT("LOD0") : TEXT("the mesh"), OutError))
	{
		return false;
	}
	const bool bSkinned = Mesh.IsSkinned();
	if (!Extras.IsEmpty())
	{
		// The cook's rules for MANTLE_static_mesh (backend cook.checkScreenSizes; the shapes are its Validate's).
		if (bSkinned)
		{
			return Fail(OutError, TEXT("screen sizes and simple collision are for static meshes; a skinned upload carries neither"));
		}
		if (Extras.ScreenSizes.Num() > 0 && Extras.ScreenSizes.Num() != LODs.Num())
		{
			return Fail(OutError, FString::Printf(TEXT("%d screen sizes for %d LODs"), Extras.ScreenSizes.Num(), LODs.Num()));
		}
		for (int32 Index = 0; Index < Extras.ScreenSizes.Num(); ++Index)
		{
			const float Size = Extras.ScreenSizes[Index];
			if (!FMath::IsFinite(Size) || !(Size > 0.0f) || Size > FMantleStaticMeshReader::MaxScreenSize || (Index > 0 && !(Size < Extras.ScreenSizes[Index - 1])))
			{
				return Fail(OutError, FString::Printf(TEXT("LOD%d's screen size %g is not in (0, %g] and below the one before"),
					Index, Size, FMantleStaticMeshReader::MaxScreenSize));
			}
		}
	}

	// Lower LODs: the one skin, LOD0's materials, the same optional streams.
	TArray<TArray<int32>> MaterialOf;
	MaterialOf.SetNum(LODs.Num());
	for (int32 Material = 0; Material < Mesh.MaterialNames.Num(); ++Material)
	{
		MaterialOf[0].Add(Material);
	}
	for (int32 LODIndex = 1; LODIndex < LODs.Num(); ++LODIndex)
	{
		const FMantleGlbMesh& LOD = LODs[LODIndex];
		const FString What = FString::Printf(TEXT("%s%d"), LODNodePrefix, LODIndex);
		if (!LOD.AuthoringSourceHash.IsEmpty() && LOD.AuthoringSourceHash != Mesh.AuthoringSourceHash)
		{
			return Fail(OutError, TEXT("every LOD must share LOD0's authoring source"));
		}
		if (!CheckGlbMesh(LOD, *What, OutError))
		{
			return false;
		}
		bool bSameJoints = LOD.IsSkinned() == bSkinned && LOD.Joints.Num() == Mesh.Joints.Num();
		for (int32 Joint = 0; bSameJoints && Joint < Mesh.Joints.Num(); ++Joint)
		{
			bSameJoints = LOD.Joints[Joint].Name == Mesh.Joints[Joint].Name && LOD.Joints[Joint].Parent == Mesh.Joints[Joint].Parent;
		}
		if (!bSameJoints)
		{
			return Fail(OutError, FString::Printf(TEXT("%s is not skinned to LOD0's joints; every LOD shares the one skin"), *What));
		}
		if ((LOD.Tangents.Num() > 0) != (Mesh.Tangents.Num() > 0) || (LOD.HairRootUV.Num() > 0) != (Mesh.HairRootUV.Num() > 0))
		{
			return Fail(OutError, FString::Printf(TEXT("%s and LOD0 disagree about tangents or hair card data; every LOD carries the same streams"), *What));
		}
		for (const FString& Name : LOD.MaterialNames)
		{
			const int32 InLOD0 = Mesh.MaterialNames.IndexOfByPredicate([&Name](const FString& Candidate) { return Candidate.Equals(Name, ESearchCase::CaseSensitive); });
			if (InLOD0 == INDEX_NONE)
			{
				return Fail(OutError, FString::Printf(TEXT("%s draws with material \"%s\", which LOD0 does not have"), *What, *Name));
			}
			MaterialOf[LODIndex].Add(InLOD0);
		}
	}

	if (Mesh.BodyOffsets.Num() > 0)
	{
		// The cook's rules for MANTLE_body_offsets (backend cook/bodyoffsets.go): known names once each, a delta per vertex of
		// every LOD written and one per skin joint, all finite.
		static const TCHAR* const ShapeNames[] = { TEXT("weight+"), TEXT("weight-"), TEXT("muscle+"), TEXT("muscle-"), TEXT("chest+"), TEXT("chest-"), TEXT("hips+"), TEXT("hips-") };
		if (!bSkinned)
		{
			return Fail(OutError, TEXT("body offsets move a skin's joints; a rigid mesh has none"));
		}
		TSet<FString> Seen;
		for (const FMantleBodyOffsetShape& Shape : Mesh.BodyOffsets)
		{
			bool bKnown = false;
			for (const TCHAR* Name : ShapeNames)
			{
				bKnown |= Shape.Name.Equals(Name, ESearchCase::CaseSensitive);
			}
			if (!bKnown || Seen.Contains(Shape.Name))
			{
				return Fail(OutError, FString::Printf(TEXT("body offset \"%s\" is not one of the eight shapes, or is repeated"), *Shape.Name));
			}
			Seen.Add(Shape.Name);
			if (Shape.LODs.Num() != LODs.Num())
			{
				return Fail(OutError, FString::Printf(TEXT("body offset \"%s\" has %d LODs; the file has %d"), *Shape.Name, Shape.LODs.Num(), LODs.Num()));
			}
			for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
			{
				if (Shape.LODs[LODIndex].Num() != LODs[LODIndex].Positions.Num())
				{
					return Fail(OutError, FString::Printf(TEXT("body offset \"%s\" has %d deltas for LOD%d's %d vertices"),
						*Shape.Name, Shape.LODs[LODIndex].Num(), LODIndex, LODs[LODIndex].Positions.Num()));
				}
			}
			if (Shape.Joints.Num() != Mesh.Joints.Num())
			{
				return Fail(OutError, FString::Printf(TEXT("body offset \"%s\" has %d joint deltas for %d skin joints"), *Shape.Name, Shape.Joints.Num(), Mesh.Joints.Num()));
			}
			auto AllFinite = [](const TArray<FVector3f>& Deltas)
			{
				for (const FVector3f& Delta : Deltas)
				{
					if (!FMath::IsFinite(Delta.X) || !FMath::IsFinite(Delta.Y) || !FMath::IsFinite(Delta.Z)) { return false; }
				}
				return true;
			};
			bool bFinite = AllFinite(Shape.Joints);
			for (const TArray<FVector3f>& Lod : Shape.LODs)
			{
				bFinite = bFinite && AllFinite(Lod);
			}
			if (!bFinite)
			{
				return Fail(OutError, FString::Printf(TEXT("body offset \"%s\" has a value that is not a number"), *Shape.Name));
			}
		}
	}

	FGlbBuffer Buffer;
	FAccessorList Accessors;
	TArray<TArray<TSharedPtr<FJsonValue>>> Primitives;
	for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
	{
		Primitives.Add(AppendGlbMeshPrimitives(LODs[LODIndex], MaterialOf[LODIndex], Buffer, Accessors));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	{
		const TSharedRef<FJsonObject> Asset = MakeShared<FJsonObject>();
		Asset->SetStringField(TEXT("version"), TEXT("2.0"));
		Asset->SetStringField(TEXT("generator"), TEXT("Mantle plugin (MantleEditor FMantleGlbWriter)"));
		if (!Mesh.AuthoringSourceHash.IsEmpty())
		{
			const TSharedRef<FJsonObject> Provenance = MakeShared<FJsonObject>();
			Provenance->SetNumberField(TEXT("version"), 1);
			Provenance->SetStringField(TEXT("contentHash"), Mesh.AuthoringSourceHash);
			const TSharedRef<FJsonObject> AssetExtras = MakeShared<FJsonObject>();
			AssetExtras->SetObjectField(TEXT("mantleAuthoringSource"), Provenance);
			Asset->SetObjectField(TEXT("extras"), AssetExtras);
		}
		Root->SetObjectField(TEXT("asset"), Asset);
	}

	// Nodes: the joints first (their index is their joint index), then the mesh node (one per LOD).
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<TSharedPtr<FJsonValue>> SceneNodes;
	TArray<FGlbMatrix> World;
	for (int32 Joint = 0; Joint < Mesh.Joints.Num(); ++Joint)
	{
		const FMantleGlbJoint& Source = Mesh.Joints[Joint];
		const FVector3d T = ToGltfPosition(Source.Local.GetTranslation());
		const FQuat R = ToGltfRotation(Source.Local.GetRotation());
		const FVector3d S = ToGltfScale(Source.Local.GetScale3D());

		const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("name"), Source.Name.ToString());
		Node->SetField(TEXT("translation"), NumberArray(TArray<double>{ T.X, T.Y, T.Z }));
		Node->SetField(TEXT("rotation"), NumberArray(TArray<double>{ R.X, R.Y, R.Z, R.W }));
		Node->SetField(TEXT("scale"), NumberArray(TArray<double>{ S.X, S.Y, S.Z }));
		Nodes.Add(MakeShared<FJsonValueObject>(Node));

		const FGlbMatrix Local = FGlbMatrix::FromTRS(T, R, S);
		World.Add(Source.Parent == INDEX_NONE ? Local : World[Source.Parent] * Local);
		if (Source.Parent == INDEX_NONE)
		{
			SceneNodes.Add(MakeShared<FJsonValueNumber>(Joint));
		}
	}
	for (int32 Joint = 0; Joint < Mesh.Joints.Num(); ++Joint)
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (int32 Child = Joint + 1; Child < Mesh.Joints.Num(); ++Child)
		{
			if (Mesh.Joints[Child].Parent == Joint)
			{
				Children.Add(MakeShared<FJsonValueNumber>(Child));
			}
		}
		if (Children.Num() > 0)
		{
			Nodes[Joint]->AsObject()->SetArrayField(TEXT("children"), Children);
		}
	}
	const FString BaseName = Mesh.Name.IsEmpty() ? FString(TEXT("mesh")) : Mesh.Name;
	for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
	{
		// One LOD: the node is named after the mesh, as it always was. Several: LOD0, LOD1, ... (the cook's convention).
		const TSharedRef<FJsonObject> MeshNode = MakeShared<FJsonObject>();
		MeshNode->SetStringField(TEXT("name"), LODs.Num() > 1 ? FString::Printf(TEXT("%s%d"), LODNodePrefix, LODIndex) : BaseName);
		MeshNode->SetNumberField(TEXT("mesh"), LODIndex);
		if (bSkinned)
		{
			MeshNode->SetNumberField(TEXT("skin"), 0);
		}
		SceneNodes.Add(MakeShared<FJsonValueNumber>(Nodes.Num()));
		Nodes.Add(MakeShared<FJsonValueObject>(MeshNode));
	}
	Root->SetArrayField(TEXT("nodes"), Nodes);

	{
		const TSharedRef<FJsonObject> Scene = MakeShared<FJsonObject>();
		Scene->SetArrayField(TEXT("nodes"), SceneNodes);
		TArray<TSharedPtr<FJsonValue>> Scenes;
		Scenes.Add(MakeShared<FJsonValueObject>(Scene));
		Root->SetArrayField(TEXT("scenes"), Scenes);
		Root->SetNumberField(TEXT("scene"), 0);
	}

	if (bSkinned)
	{
		TArray<float> InverseBind;
		InverseBind.Reserve(Mesh.Joints.Num() * 16);
		for (int32 Joint = 0; Joint < Mesh.Joints.Num(); ++Joint)
		{
			FGlbMatrix Inverse;
			if (!World[Joint].InverseAffine(Inverse))
			{
				return Fail(OutError, FString::Printf(TEXT("bone \"%s\" has a degenerate rest transform"), *Mesh.Joints[Joint].Name.ToString()));
			}
			for (int32 Index = 0; Index < 16; ++Index)
			{
				InverseBind.Add(static_cast<float>(Inverse.M[Index]));
			}
		}
		const int32 InverseBindView = Buffer.AddView(InverseBind.GetData(), InverseBind.Num() * sizeof(float), 0);
		const TSharedRef<FJsonObject> Skin = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Joints;
		int32 SkeletonRoot = INDEX_NONE;
		for (int32 Joint = 0; Joint < Mesh.Joints.Num(); ++Joint)
		{
			Joints.Add(MakeShared<FJsonValueNumber>(Joint));
			if (Mesh.Joints[Joint].Parent == INDEX_NONE && SkeletonRoot == INDEX_NONE)
			{
				SkeletonRoot = Joint;
			}
		}
		Skin->SetArrayField(TEXT("joints"), Joints);
		Skin->SetNumberField(TEXT("inverseBindMatrices"),
			Accessors.Add(InverseBindView, 0, ComponentFloat, Mesh.Joints.Num(), TEXT("MAT4")));
		if (SkeletonRoot != INDEX_NONE)
		{
			Skin->SetNumberField(TEXT("skeleton"), SkeletonRoot);
		}
		TArray<TSharedPtr<FJsonValue>> Skins;
		Skins.Add(MakeShared<FJsonValueObject>(Skin));
		Root->SetArrayField(TEXT("skins"), Skins);
	}

	{
		TArray<TSharedPtr<FJsonValue>> Meshes;
		for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
		{
			const TSharedRef<FJsonObject> MeshJson = MakeShared<FJsonObject>();
			MeshJson->SetStringField(TEXT("name"), LODIndex == 0 ? BaseName : FString::Printf(TEXT("%s_%s%d"), *BaseName, LODNodePrefix, LODIndex));
			MeshJson->SetArrayField(TEXT("primitives"), Primitives[LODIndex]);
			Meshes.Add(MakeShared<FJsonValueObject>(MeshJson));
		}
		Root->SetArrayField(TEXT("meshes"), Meshes);
	}

	{
		TArray<TSharedPtr<FJsonValue>> Materials;
		for (const FString& Name : Mesh.MaterialNames)
		{
			const TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
			Material->SetStringField(TEXT("name"), Name);
			Materials.Add(MakeShared<FJsonValueObject>(Material));
		}
		Root->SetArrayField(TEXT("materials"), Materials);
	}

	const TSharedRef<FJsonObject> RootExtensions = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> UsedExtensions;
	if (Mesh.WarpSampleIndices.Num() > 0)
	{
		const int32 SampleView = Buffer.AddView(Mesh.WarpSampleIndices.GetData(), Mesh.WarpSampleIndices.Num() * sizeof(uint32), 0);
		const int32 WeightView = Buffer.AddView(Mesh.WarpInterpolationWeights.GetData(), Mesh.WarpInterpolationWeights.Num() * sizeof(float), 0);
		const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
		Shape->SetNumberField(TEXT("sampleIndices"),
			Accessors.Add(SampleView, 0, ComponentUnsignedInt, Mesh.WarpSampleIndices.Num(), TEXT("SCALAR")));
		Shape->SetNumberField(TEXT("interpolationWeights"),
			Accessors.Add(WeightView, 0, ComponentFloat, Mesh.WarpInterpolationWeights.Num(), TEXT("SCALAR")));
		if (Mesh.HasNeck())
		{
			// #430 (backend cook.readNeck): the neck samples in glTF's axes and metres like every position, their ids,
			// and the RBF weights over the positions followed by them.
			TArray<float> NeckPositions;
			NeckPositions.Reserve(Mesh.NeckPositions.Num() * 3);
			for (const FVector3f& Position : Mesh.NeckPositions)
			{
				const FVector3d P = FMantleGlbWriter::ToGltfPosition(FVector3d(Position));
				NeckPositions.Add(static_cast<float>(P.X));
				NeckPositions.Add(static_cast<float>(P.Y));
				NeckPositions.Add(static_cast<float>(P.Z));
			}
			const int32 IdView = Buffer.AddView(Mesh.NeckIds.GetData(), Mesh.NeckIds.Num() * sizeof(uint32), 0);
			const int32 PositionView = Buffer.AddView(NeckPositions.GetData(), NeckPositions.Num() * sizeof(float), 0);
			const int32 NeckSampleView = Buffer.AddView(Mesh.NeckSampleIndices.GetData(), Mesh.NeckSampleIndices.Num() * sizeof(uint32), 0);
			const int32 NeckWeightView = Buffer.AddView(Mesh.NeckInterpolationWeights.GetData(), Mesh.NeckInterpolationWeights.Num() * sizeof(float), 0);
			const TSharedRef<FJsonObject> Neck = MakeShared<FJsonObject>();
			Neck->SetStringField(TEXT("method"), Mesh.NeckMethod);
			Neck->SetNumberField(TEXT("ids"), Accessors.Add(IdView, 0, ComponentUnsignedInt, Mesh.NeckIds.Num(), TEXT("SCALAR")));
			Neck->SetNumberField(TEXT("positions"), Accessors.Add(PositionView, 0, ComponentFloat, Mesh.NeckPositions.Num(), TEXT("VEC3")));
			Neck->SetNumberField(TEXT("sampleIndices"), Accessors.Add(NeckSampleView, 0, ComponentUnsignedInt, Mesh.NeckSampleIndices.Num(), TEXT("SCALAR")));
			Neck->SetNumberField(TEXT("interpolationWeights"),
				Accessors.Add(NeckWeightView, 0, ComponentFloat, Mesh.NeckInterpolationWeights.Num(), TEXT("SCALAR")));
			Shape->SetObjectField(TEXT("neck"), Neck);
		}
		RootExtensions->SetObjectField(BodyShapeExtension, Shape);
		UsedExtensions.Add(MakeShared<FJsonValueString>(FString(BodyShapeExtension)));
	}

	if (Mesh.BodyOffsets.Num() > 0)
	{
		// #849: deltas convert like POSITION and the joints' translations (centimetres to metres, y and z swapped), without a
		// translation: they are differences. One accessor a LOD, one for the joints, in skin.joints order (= Mesh.Joints).
		auto AddDeltas = [&Buffer, &Accessors](const TArray<FVector3f>& Deltas)
		{
			TArray<float> Converted;
			Converted.Reserve(Deltas.Num() * 3);
			for (const FVector3f& Delta : Deltas)
			{
				const FVector3d D = FMantleGlbWriter::ToGltfPosition(FVector3d(Delta));
				Converted.Add(static_cast<float>(D.X));
				Converted.Add(static_cast<float>(D.Y));
				Converted.Add(static_cast<float>(D.Z));
			}
			const int32 View = Buffer.AddView(Converted.GetData(), Converted.Num() * sizeof(float), 0);
			return Accessors.Add(View, 0, ComponentFloat, Deltas.Num(), TEXT("VEC3"));
		};
		TArray<TSharedPtr<FJsonValue>> ShapeJsons;
		for (const FMantleBodyOffsetShape& Offset : Mesh.BodyOffsets)
		{
			const TSharedRef<FJsonObject> ShapeJson = MakeShared<FJsonObject>();
			ShapeJson->SetStringField(TEXT("name"), Offset.Name);
			TArray<TSharedPtr<FJsonValue>> LodAccessors;
			for (const TArray<FVector3f>& Lod : Offset.LODs)
			{
				LodAccessors.Add(MakeShared<FJsonValueNumber>(AddDeltas(Lod)));
			}
			ShapeJson->SetArrayField(TEXT("lods"), LodAccessors);
			ShapeJson->SetNumberField(TEXT("joints"), AddDeltas(Offset.Joints));
			ShapeJsons.Add(MakeShared<FJsonValueObject>(ShapeJson));
		}
		const TSharedRef<FJsonObject> OffsetsJson = MakeShared<FJsonObject>();
		OffsetsJson->SetNumberField(TEXT("version"), 1);
		OffsetsJson->SetArrayField(TEXT("shapes"), ShapeJsons);
		RootExtensions->SetObjectField(BodyOffsetsExtension, OffsetsJson);
		UsedExtensions.Add(MakeShared<FJsonValueString>(FString(BodyOffsetsExtension)));
	}

	if (!Extras.IsEmpty())
	{
		// #470 (backend cook.readStaticMeshExtension): screen sizes, and the simple collision in Unreal's frame and
		// centimetres as FKAggregateGeom has it, each hull's points an accessor.
		const TSharedRef<FJsonObject> Static = MakeShared<FJsonObject>();
		if (Extras.ScreenSizes.Num() > 0)
		{
			TArray<double> Sizes;
			for (const float Size : Extras.ScreenSizes)
			{
				Sizes.Add(Size);
			}
			Static->SetField(TEXT("screenSizes"), NumberArray(Sizes));
		}
		const FMantleStaticMeshCollision& Collision = Extras.Collision;
		if (Collision.HasShapes())
		{
			auto Vector = [](const FVector3f& Value) { return NumberArray(TArray<double>{ Value.X, Value.Y, Value.Z }); };
			auto Rotation = [](const FQuat4f& Value) { return NumberArray(TArray<double>{ Value.X, Value.Y, Value.Z, Value.W }); };
			const TSharedRef<FJsonObject> Shapes = MakeShared<FJsonObject>();
			Shapes->SetBoolField(TEXT("simpleAsComplex"), Collision.bSimpleAsComplex);
			TArray<TSharedPtr<FJsonValue>> Boxes;
			for (const FMantleStaticMeshBox& Box : Collision.Boxes)
			{
				const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetField(TEXT("center"), Vector(Box.Center));
				Json->SetField(TEXT("rotation"), Rotation(Box.Rotation));
				Json->SetField(TEXT("size"), Vector(Box.Size));
				Boxes.Add(MakeShared<FJsonValueObject>(Json));
			}
			TArray<TSharedPtr<FJsonValue>> Spheres;
			for (const FMantleStaticMeshSphere& Sphere : Collision.Spheres)
			{
				const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetField(TEXT("center"), Vector(Sphere.Center));
				Json->SetNumberField(TEXT("radius"), Sphere.Radius);
				Spheres.Add(MakeShared<FJsonValueObject>(Json));
			}
			TArray<TSharedPtr<FJsonValue>> Capsules;
			for (const FMantleStaticMeshCapsule& Capsule : Collision.Capsules)
			{
				const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetField(TEXT("center"), Vector(Capsule.Center));
				Json->SetField(TEXT("rotation"), Rotation(Capsule.Rotation));
				Json->SetNumberField(TEXT("radius"), Capsule.Radius);
				Json->SetNumberField(TEXT("length"), Capsule.Length);
				Capsules.Add(MakeShared<FJsonValueObject>(Json));
			}
			TArray<TSharedPtr<FJsonValue>> Hulls;
			for (const FMantleStaticMeshHull& Hull : Collision.Hulls)
			{
				const int32 HullView = Buffer.AddView(Hull.Vertices.GetData(), Hull.Vertices.Num() * sizeof(FVector3f), 0);
				const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetNumberField(TEXT("vertices"), Accessors.Add(HullView, 0, ComponentFloat, Hull.Vertices.Num(), TEXT("VEC3")));
				Hulls.Add(MakeShared<FJsonValueObject>(Json));
			}
			Shapes->SetArrayField(TEXT("boxes"), Boxes);
			Shapes->SetArrayField(TEXT("spheres"), Spheres);
			Shapes->SetArrayField(TEXT("capsules"), Capsules);
			Shapes->SetArrayField(TEXT("convexHulls"), Hulls);
			Static->SetObjectField(TEXT("collision"), Shapes);
		}
		RootExtensions->SetObjectField(StaticMeshExtension, Static);
		UsedExtensions.Add(MakeShared<FJsonValueString>(FString(StaticMeshExtension)));
	}

	if (UsedExtensions.Num() > 0)
	{
		Root->SetObjectField(TEXT("extensions"), RootExtensions);
		Root->SetArrayField(TEXT("extensionsUsed"), UsedExtensions);
	}

	// #435: a vehicle's document rides in the root's extras (backend cook.VehicleExtrasKey).
	if (Mesh.Extras.IsValid())
	{
		Root->SetObjectField(TEXT("extras"), Mesh.Extras);
	}

	Root->SetArrayField(TEXT("accessors"), Accessors.Accessors);
	Root->SetArrayField(TEXT("bufferViews"), Buffer.Views);
	while (Buffer.Bytes.Num() % 4 != 0)
	{
		Buffer.Bytes.Add(0);
	}
	{
		const TSharedRef<FJsonObject> BufferJson = MakeShared<FJsonObject>();
		BufferJson->SetNumberField(TEXT("byteLength"), Buffer.Bytes.Num());
		TArray<TSharedPtr<FJsonValue>> Buffers;
		Buffers.Add(MakeShared<FJsonValueObject>(BufferJson));
		Root->SetArrayField(TEXT("buffers"), Buffers);
	}

	FString JsonText;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		return Fail(OutError, TEXT("the glTF JSON could not be written"));
	}
	FTCHARToUTF8 Utf8(*JsonText);
	TArray<uint8> JsonBytes;
	JsonBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	while (JsonBytes.Num() % 4 != 0)
	{
		JsonBytes.Add(' ');
	}

	const uint32 TotalLength = 12u + 8u + static_cast<uint32>(JsonBytes.Num()) + 8u + static_cast<uint32>(Buffer.Bytes.Num());
	OutBytes.Reserve(static_cast<int32>(TotalLength));
	AppendU32(OutBytes, GlbMagic);
	AppendU32(OutBytes, 2);
	AppendU32(OutBytes, TotalLength);
	AppendU32(OutBytes, static_cast<uint32>(JsonBytes.Num()));
	AppendU32(OutBytes, ChunkJson);
	OutBytes.Append(JsonBytes);
	AppendU32(OutBytes, static_cast<uint32>(Buffer.Bytes.Num()));
	AppendU32(OutBytes, ChunkBin);
	OutBytes.Append(Buffer.Bytes);
	return true;
}

bool FMantleGlbDocument::Parse(TConstArrayView<uint8> Bytes, FMantleGlbDocument& Out, FString& OutError)
{
	using namespace MantleGlbWriterPrivate;

	Out = FMantleGlbDocument();
	if (Bytes.Num() < 20 || ReadU32(Bytes.GetData()) != GlbMagic || ReadU32(Bytes.GetData() + 4) != 2)
	{
		OutError = TEXT("not a glTF 2.0 binary file");
		return false;
	}
	const uint32 Length = ReadU32(Bytes.GetData() + 8);
	if (Length > static_cast<uint32>(Bytes.Num()))
	{
		OutError = TEXT("the file is shorter than its header says");
		return false;
	}
	int64 Offset = 12;
	bool bJson = false;
	while (Offset + 8 <= static_cast<int64>(Length))
	{
		const uint32 ChunkLength = ReadU32(Bytes.GetData() + Offset);
		const uint32 ChunkType = ReadU32(Bytes.GetData() + Offset + 4);
		Offset += 8;
		if (Offset + static_cast<int64>(ChunkLength) > static_cast<int64>(Length))
		{
			OutError = TEXT("a chunk runs past the end of the file");
			return false;
		}
		if (ChunkType == ChunkJson && !bJson)
		{
			const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset), static_cast<int32>(ChunkLength));
			const FString JsonText(Text.Length(), Text.Get());
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			if (!FJsonSerializer::Deserialize(Reader, Out.Json) || !Out.Json.IsValid())
			{
				OutError = TEXT("the JSON chunk is not valid JSON");
				return false;
			}
			bJson = true;
		}
		else if (ChunkType == ChunkBin && Out.Bin.Num() == 0)
		{
			Out.Bin.Append(Bytes.GetData() + Offset, static_cast<int32>(ChunkLength));
		}
		Offset += ChunkLength;
	}
	if (!bJson)
	{
		OutError = TEXT("the file has no JSON chunk");
		return false;
	}
	return true;
}

bool FMantleGlbDocument::ReadBufferView(int32 Index, TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Views = nullptr;
	if (!Json.IsValid() || !Json->TryGetArrayField(TEXT("bufferViews"), Views) || !Views->IsValidIndex(Index))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> View = (*Views)[Index]->AsObject();
	double Offset = 0.0, Length = 0.0;
	if (!View.IsValid() || !View->TryGetNumberField(TEXT("byteLength"), Length))
	{
		return false;
	}
	View->TryGetNumberField(TEXT("byteOffset"), Offset);
	if (Offset < 0.0 || Length < 0.0 || Offset + Length > Bin.Num())
	{
		return false;
	}
	OutBytes.Append(Bin.GetData() + static_cast<int64>(Offset), static_cast<int32>(Length));
	return true;
}

bool FMantleGlbDocument::ReadAccessor(int32 Index, TArray<double>& OutValues, int32* OutComponents, int32* OutCount) const
{
	using namespace MantleGlbWriterPrivate;

	OutValues.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Accessors = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Views = nullptr;
	if (!Json.IsValid() || !Json->TryGetArrayField(TEXT("accessors"), Accessors) || !Accessors->IsValidIndex(Index)
		|| !Json->TryGetArrayField(TEXT("bufferViews"), Views))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Accessor = (*Accessors)[Index]->AsObject();
	double ViewIndex = 0.0, ComponentType = 0.0, Count = 0.0, AccessorOffset = 0.0;
	FString Type;
	bool bNormalized = false;
	if (!Accessor.IsValid() || !Accessor->TryGetNumberField(TEXT("bufferView"), ViewIndex) || !Accessor->TryGetNumberField(TEXT("componentType"), ComponentType)
		|| !Accessor->TryGetNumberField(TEXT("count"), Count) || !Accessor->TryGetStringField(TEXT("type"), Type))
	{
		return false;
	}
	Accessor->TryGetNumberField(TEXT("byteOffset"), AccessorOffset);
	Accessor->TryGetBoolField(TEXT("normalized"), bNormalized);
	if (!Views->IsValidIndex(static_cast<int32>(ViewIndex)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> View = (*Views)[static_cast<int32>(ViewIndex)]->AsObject();
	double ViewOffset = 0.0, ViewLength = 0.0, Stride = 0.0;
	if (!View.IsValid() || !View->TryGetNumberField(TEXT("byteLength"), ViewLength))
	{
		return false;
	}
	View->TryGetNumberField(TEXT("byteOffset"), ViewOffset);
	View->TryGetNumberField(TEXT("byteStride"), Stride);

	const int32 Components = Type == TEXT("SCALAR") ? 1 : Type == TEXT("VEC2") ? 2 : Type == TEXT("VEC3") ? 3
		: Type == TEXT("VEC4") ? 4 : Type == TEXT("MAT4") ? 16 : 0;
	const int32 Kind = static_cast<int32>(ComponentType);
	const int32 Size = Kind == ComponentFloat || Kind == ComponentUnsignedInt ? 4 : Kind == ComponentUnsignedShort ? 2
		: Kind == ComponentUnsignedByte ? 1 : 0;
	if (Components == 0 || Size == 0)
	{
		return false;
	}
	const int64 Element = static_cast<int64>(Components) * Size;
	const int64 Step = Stride > 0.0 ? static_cast<int64>(Stride) : Element;
	const int64 Start = static_cast<int64>(ViewOffset) + static_cast<int64>(AccessorOffset);
	const int64 Elements = static_cast<int64>(Count);
	if (Elements < 0 || (Elements > 0 && (Start + (Elements - 1) * Step + Element > static_cast<int64>(ViewOffset + ViewLength)
		|| Start + (Elements - 1) * Step + Element > Bin.Num())))
	{
		return false;
	}
	OutValues.Reserve(static_cast<int32>(Elements * Components));
	for (int64 Item = 0; Item < Elements; ++Item)
	{
		const uint8* At = Bin.GetData() + Start + Item * Step;
		for (int32 Component = 0; Component < Components; ++Component)
		{
			const uint8* Value = At + Component * Size;
			double Number = 0.0;
			if (Kind == ComponentFloat)
			{
				float Float = 0.0f;
				FMemory::Memcpy(&Float, Value, 4);
				Number = Float;
			}
			else if (Kind == ComponentUnsignedInt)
			{
				Number = ReadU32(Value);
			}
			else if (Kind == ComponentUnsignedShort)
			{
				Number = uint16(Value[0] | (Value[1] << 8));
				Number = bNormalized ? Number / 65535.0 : Number;
			}
			else
			{
				Number = Value[0];
				Number = bNormalized ? Number / 255.0 : Number;
			}
			OutValues.Add(Number);
		}
	}
	if (OutComponents)
	{
		*OutComponents = Components;
	}
	if (OutCount)
	{
		*OutCount = static_cast<int32>(Elements);
	}
	return true;
}
