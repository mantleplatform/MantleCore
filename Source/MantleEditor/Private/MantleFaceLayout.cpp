// Copyright Joshua Eger. All Rights Reserved.
#include "MantleFaceLayout.h"

#include "MantleBodyCorrespondence.h"
#include "MantleBodyTopologyData.h"

#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleFaceLayout, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleFaceLayoutPrivate
{
	bool Fail(FString& Error, const FString& Reason)
	{
		Error = Reason;
		return false;
	}

	/** "head 3401v/6000t, teeth 300v/500t" */
	FString DescribeSections(const FMantleGlbMesh& Mesh)
	{
		FString Out;
		for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
		{
			Out += FString::Printf(TEXT("%s%s %dv/%dt"), Out.IsEmpty() ? TEXT("") : TEXT(", "),
				Mesh.MaterialNames.IsValidIndex(Primitive.MaterialIndex) ? *Mesh.MaterialNames[Primitive.MaterialIndex] : TEXT("?"), Primitive.NumVertices, Primitive.Indices.Num() / 3);
		}
		return Out;
	}

	FString DescribeSections(const FMantleFaceLayout& Layout)
	{
		FString Out;
		for (const FMantleFaceLayoutSection& Section : Layout.Sections)
		{
			Out += FString::Printf(TEXT("%s%s %dv/%dt"), Out.IsEmpty() ? TEXT("") : TEXT(", "), *Section.Material, Section.NumVertices, Section.NumTriangles);
		}
		return Out;
	}

	template <typename T>
	void ReorderArray(TArray<T>& Array, const TArray<int32>& InputOfReference, const int32 OldCount, const int32 Stride)
	{
		const int32 N = InputOfReference.Num();
		if (Array.Num() != OldCount * Stride)
		{
			return;
		}
		TArray<T> Out;
		Out.SetNumUninitialized(N * Stride);
		for (int32 Reference = 0; Reference < N; ++Reference)
		{
			for (int32 Slot = 0; Slot < Stride; ++Slot)
			{
				Out[Reference * Stride + Slot] = Array[InputOfReference[Reference] * Stride + Slot];
			}
		}
		Array = MoveTemp(Out);
	}

	FIntPoint Quantize(const FVector2f& UV)
	{
		return FIntPoint(FMath::RoundToInt32(UV.X * FMantleFaceLayout::Quantization), FMath::RoundToInt32(UV.Y * FMantleFaceLayout::Quantization));
	}

	/** A face section with its surplus vertices taken out: the kept vertices (indices local to the section), their UVs and the triangles over them. */
	struct FReducedSection
	{
		TArray<int32> Kept;
		TArray<FVector2f> UVs;
		TArray<uint32> Indices;
		int32 Skipped = 0;
	};

	/**
	 * Takes Primitive down to the layout section's vertex count. Surplus vertices are, in order of preference: unreferenced ones whose UV the layout
	 * does not have that often, referenced ones with such a UV (merged into the kept vertex of the same UV nearest to them, triangles remapped), any
	 * other unreferenced ones. Fails when that does not reach the count.
	 */
	bool ReduceSection(const FMantleGlbMesh& Mesh, const FMantleGlbPrimitive& Primitive, const FMantleFaceLayout& Layout, const FMantleFaceLayoutSection& Section,
		FReducedSection& Out, FString& OutError)
	{
		Out = FReducedSection();
		const int32 Count = Primitive.NumVertices;
		TArray<FIntPoint> InputUV;
		InputUV.SetNum(Count);
		for (int32 Vertex = 0; Vertex < Count; ++Vertex) { InputUV[Vertex] = Quantize(Mesh.UVs[Primitive.FirstVertex + Vertex]); }
		int32 Extra = Count - Section.NumVertices;
		TBitArray<> Dropped(false, Count);
		if (Extra > 0)
		{
			TBitArray<> Referenced(false, Count);
			for (const uint32 Index : Primitive.Indices) { if (static_cast<int32>(Index) < Count) { Referenced[Index] = true; } }
			TMap<FIntPoint, int32> Surplus;
			for (int32 Vertex = 0; Vertex < Count; ++Vertex) { ++Surplus.FindOrAdd(InputUV[Vertex]); }
			for (int32 Vertex = 0; Vertex < Section.NumVertices; ++Vertex) { --Surplus.FindOrAdd(Layout.QuantizedUVs[Section.FirstVertex + Vertex]); }
			const auto HasSurplus = [&Surplus](const FIntPoint& UV) { const int32* Found = Surplus.Find(UV); return Found && *Found > 0; };
			const auto Drop = [&](const int32 Vertex) { Dropped[Vertex] = true; --Surplus.FindOrAdd(InputUV[Vertex]); --Extra; };
			for (int32 Vertex = Count - 1; Vertex >= 0 && Extra > 0; --Vertex) { if (!Referenced[Vertex] && HasSurplus(InputUV[Vertex])) { Drop(Vertex); } }
			for (int32 Vertex = Count - 1; Vertex >= 0 && Extra > 0; --Vertex) { if (!Dropped[Vertex] && Referenced[Vertex] && HasSurplus(InputUV[Vertex])) { Drop(Vertex); } }
			for (int32 Vertex = Count - 1; Vertex >= 0 && Extra > 0; --Vertex) { if (!Dropped[Vertex] && !Referenced[Vertex]) { Drop(Vertex); } }
			if (Extra > 0)
			{
				return Fail(OutError, FString::Printf(TEXT("%d vertices more than the layout's %d and they cannot be told apart as spare (unused, or twins by UV)"), Count - Section.NumVertices, Section.NumVertices));
			}
		}
		TArray<int32> NewIndex;
		NewIndex.Init(INDEX_NONE, Count);
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			if (!Dropped[Vertex])
			{
				NewIndex[Vertex] = Out.Kept.Num();
				Out.Kept.Add(Vertex);
				Out.UVs.Add(Mesh.UVs[Primitive.FirstVertex + Vertex]);
			}
		}
		Out.Skipped = Count - Out.Kept.Num();
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			if (!Dropped[Vertex]) { continue; }
			int32 Twin = INDEX_NONE;
			float Nearest = TNumericLimits<float>::Max();
			for (const int32 Candidate : Out.Kept)
			{
				if (InputUV[Candidate] != InputUV[Vertex]) { continue; }
				const float Distance = FVector3f::DistSquared(Mesh.Positions[Primitive.FirstVertex + Candidate], Mesh.Positions[Primitive.FirstVertex + Vertex]);
				if (Distance < Nearest) { Nearest = Distance; Twin = Candidate; }
			}
			NewIndex[Vertex] = Twin == INDEX_NONE ? INDEX_NONE : NewIndex[Twin];
		}
		for (const uint32 Index : Primitive.Indices)
		{
			if (static_cast<int32>(Index) >= Count || NewIndex[Index] == INDEX_NONE)
			{
				return Fail(OutError, TEXT("a triangle uses a surplus vertex that has no twin with the same UV"));
			}
			Out.Indices.Add(static_cast<uint32>(NewIndex[Index]));
		}
		return true;
	}
}

FString FMantleFaceLayout::DefaultPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("FaceLayout"), TEXT("face-layout-v1.json")) : FString();
}

const FMantleFaceLayout* FMantleFaceLayout::Get()
{
	static TUniquePtr<FMantleFaceLayout> Loaded;
	static bool bTried = false;
	if (!bTried)
	{
		bTried = true;
		const FString Path = DefaultPath();
		FString Json, Error;
		FMantleFaceLayout Layout;
		if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Json, *Path))
		{
			UE_LOG(LogMantleFaceLayout, Display, TEXT("MANTLE_FACE_LAYOUT no layout file at %s"), *Path);
		}
		else if (!Parse(Json, Layout, Error))
		{
			UE_LOG(LogMantleFaceLayout, Warning, TEXT("MANTLE_FACE_LAYOUT %s is not usable: %s"), *Path, *Error);
		}
		else
		{
			Loaded = MakeUnique<FMantleFaceLayout>(MoveTemp(Layout));
		}
	}
	return Loaded.Get();
}

int32 FMantleFaceLayout::FindFaceLOD(const USkeletalMesh* Face)
{
	const FSkeletalMeshRenderData* RenderData = Face ? Face->GetResourceForRendering() : nullptr;
	if (!RenderData)
	{
		return INDEX_NONE;
	}
	for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
	{
		if (RenderData->LODRenderData[LOD].GetTotalFaces() == ReferenceTriangles)
		{
			return LOD;
		}
	}
	return INDEX_NONE;
}

bool FMantleFaceLayout::ReadRender(const USkeletalMesh* Face, const int32 LODIndex, FMantleGlbMesh& Out, FString& OutError)
{
	using namespace MantleFaceLayoutPrivate;
	Out = FMantleGlbMesh();
	const FSkeletalMeshRenderData* RenderData = Face ? Face->GetResourceForRendering() : nullptr;
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return Fail(OutError, TEXT("the face has no render data for that LOD"));
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
	const FPositionVertexBuffer& Positions = LOD.StaticVertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Vertices = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	const int32 N = static_cast<int32>(Positions.GetNumVertices());
	if (N == 0 || !Positions.GetVertexData() || Vertices.GetNumVertices() != static_cast<uint32>(N) || Vertices.GetNumTexCoords() == 0
		|| !Vertices.GetTexCoordData() || !LOD.MultiSizeIndexContainer.IsIndexBufferValid())
	{
		return Fail(OutError, TEXT("the face has no CPU positions, UVs or indices"));
	}
	TArray<uint32> Indices;
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	Out.Positions.SetNumUninitialized(N);
	Out.UVs.SetNumUninitialized(N);
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		Out.Positions[Vertex] = Positions.VertexPosition(Vertex);
		Out.UVs[Vertex] = Vertices.GetVertexUV(Vertex, 0);
	}
	int32 Covered = 0;
	for (int32 Section = 0; Section < LOD.RenderSections.Num(); ++Section)
	{
		const FSkelMeshRenderSection& Render = LOD.RenderSections[Section];
		if (static_cast<int32>(Render.BaseVertexIndex) != Covered)
		{
			return Fail(OutError, TEXT("the face's render sections do not tile its vertices in order"));
		}
		Covered += static_cast<int32>(Render.NumVertices);
		const TArray<FSkeletalMaterial>& Materials = Face->GetMaterials();
		FString Material = Materials.IsValidIndex(Render.MaterialIndex) ? Materials[Render.MaterialIndex].MaterialSlotName.ToString() : FString::Printf(TEXT("section_%d"), Section);
		if (Out.MaterialNames.Contains(Material))
		{
			Material = FString::Printf(TEXT("%s_%d"), *Material, Section);
		}
		Out.MaterialNames.Add(Material);
		FMantleGlbPrimitive& Primitive = Out.Primitives.AddDefaulted_GetRef();
		Primitive.MaterialIndex = Out.MaterialNames.Num() - 1;
		Primitive.FirstVertex = static_cast<int32>(Render.BaseVertexIndex);
		Primitive.NumVertices = static_cast<int32>(Render.NumVertices);
		const int32 Begin = static_cast<int32>(Render.BaseIndex);
		const int32 End = Begin + static_cast<int32>(Render.NumTriangles) * 3;
		if (End > Indices.Num())
		{
			return Fail(OutError, TEXT("a face section runs past its index buffer"));
		}
		for (int32 Index = Begin; Index < End; ++Index)
		{
			const int64 Local = static_cast<int64>(Indices[Index]) - Render.BaseVertexIndex;
			if (Local < 0 || Local >= Render.NumVertices)
			{
				return Fail(OutError, TEXT("a face triangle uses another section's vertices"));
			}
			Primitive.Indices.Add(static_cast<uint32>(Local));
		}
	}
	if (Covered != N)
	{
		return Fail(OutError, TEXT("the face has vertices outside its sections"));
	}
	return true;
}

bool FMantleFaceLayout::Record(const USkeletalMesh* Face, FMantleFaceLayout& Out, FString& OutError)
{
	Out = FMantleFaceLayout();
	const int32 LOD = FindFaceLOD(Face);
	if (LOD == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("the face has no LOD with %d triangles"), ReferenceTriangles);
		return false;
	}
	FMantleGlbMesh Mesh;
	if (!ReadRender(Face, LOD, Mesh, OutError))
	{
		return false;
	}
	for (const FVector2f& UV : Mesh.UVs)
	{
		Out.QuantizedUVs.Add(FIntPoint(FMath::RoundToInt32(UV.X * Quantization), FMath::RoundToInt32(UV.Y * Quantization)));
	}
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		FMantleFaceLayoutSection& Section = Out.Sections.AddDefaulted_GetRef();
		Section.Material = Mesh.MaterialNames[Primitive.MaterialIndex];
		Section.FirstVertex = Primitive.FirstVertex;
		Section.NumVertices = Primitive.NumVertices;
		Section.FirstIndex = Out.Indices.Num();
		Section.NumTriangles = Primitive.Indices.Num() / 3;
		for (const uint32 Index : Primitive.Indices)
		{
			Out.Indices.Add(Index + static_cast<uint32>(Primitive.FirstVertex));
		}
	}
	return true;
}

bool FMantleFaceLayout::Reorder(FMantleGlbMesh& Mesh, FString& OutNote, FString& OutError) const
{
	using namespace MantleFaceLayoutPrivate;
	OutNote.Reset();
	const int32 N = QuantizedUVs.Num();
	const int32 OldCount = Mesh.Positions.Num();
	if (N == 0 || OldCount < N || Mesh.UVs.Num() != OldCount)
	{
		return Fail(OutError, FString::Printf(TEXT("the face has %d vertices and the built face layout %d (face sections: %s; layout sections: %s)"), OldCount, N,
			*DescribeSections(Mesh), *DescribeSections(*this)));
	}
	if (Mesh.Primitives.Num() != Sections.Num())
	{
		return Fail(OutError, FString::Printf(TEXT("the face has %d sections and the built face layout %d (face: %s; layout: %s)"), Mesh.Primitives.Num(), Sections.Num(),
			*DescribeSections(Mesh), *DescribeSections(*this)));
	}
	// Pair each layout section with a face section of the same triangle count and the same vertex count (or a few more: spare vertices are taken out);
	// the names are not trusted, the UV match decides. Several candidates are tried best-fitting first.
	TArray<int32> PrimitiveOfSection;
	PrimitiveOfSection.Init(INDEX_NONE, Sections.Num());
	TBitArray<> Used(false, Mesh.Primitives.Num());
	TArray<int32> InputOfReference;
	InputOfReference.Init(INDEX_NONE, N);
	int32 Exact = 0, FlippedSections = 0, FlippedPairs = 0, Skipped = 0;
	for (int32 Section = 0; Section < Sections.Num(); ++Section)
	{
		const FMantleFaceLayoutSection& Layout = Sections[Section];
		TArray<TPair<int32, int32>> Candidates; // (score, primitive)
		for (int32 Primitive = 0; Primitive < Mesh.Primitives.Num(); ++Primitive)
		{
			const FMantleGlbPrimitive& Candidate = Mesh.Primitives[Primitive];
			const int32 Spare = Candidate.NumVertices - Layout.NumVertices;
			if (Used[Primitive] || Spare < 0 || Spare > 8 || Candidate.Indices.Num() != Layout.NumTriangles * 3) { continue; }
			const bool bSameName = Mesh.MaterialNames.IsValidIndex(Candidate.MaterialIndex) && Mesh.MaterialNames[Candidate.MaterialIndex].Equals(Layout.Material, ESearchCase::IgnoreCase);
			Candidates.Add(TPair<int32, int32>((Spare == 0 ? 100 : 50 - Spare) + (bSameName ? 1 : 0), Primitive));
		}
		Candidates.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Key > B.Key; });
		if (Candidates.IsEmpty())
		{
			return Fail(OutError, FString::Printf(TEXT("no face section matches the layout's section %s (%d vertices, %d triangles); face: %s; layout: %s"), *Layout.Material,
				Layout.NumVertices, Layout.NumTriangles, *DescribeSections(Mesh), *DescribeSections(*this)));
		}
		FMantleBodyCorrespondence Reference;
		for (int32 Vertex = 0; Vertex < Layout.NumVertices; ++Vertex) { Reference.QuantizedUVs.Add(QuantizedUVs[Layout.FirstVertex + Vertex]); }
		for (int32 Index = 0; Index < Layout.NumTriangles * 3; ++Index) { Reference.Indices.Add(Indices[Layout.FirstIndex + Index] - static_cast<uint32>(Layout.FirstVertex)); }
		FString Tried;
		bool bMatched = false;
		for (const TPair<int32, int32>& Candidate : Candidates)
		{
			const FMantleGlbPrimitive& Primitive = Mesh.Primitives[Candidate.Value];
			FReducedSection Reduced;
			FString Error;
			if (!ReduceSection(Mesh, Primitive, *this, Layout, Reduced, Error))
			{
				Tried += FString::Printf(TEXT("%s[face section %s]: %s. "), *Layout.Material, Mesh.MaterialNames.IsValidIndex(Primitive.MaterialIndex) ? *Mesh.MaterialNames[Primitive.MaterialIndex] : TEXT("?"), *Error);
				continue;
			}
			TArray<int32> Local;
			bool bFlipped = false;
			int32 Pairs = 0;
			if (!Reference.Match(Reduced.UVs, Reduced.Indices, Local, Error))
			{
				FString Note;
				FString FlipError;
				if (!Reference.MatchFlippedDiagonals(Reduced.UVs, Reduced.Indices, Local, Note, FlipError, MAX_int32, &Pairs))
				{
					Tried += FString::Printf(TEXT("layout section %s vs face section %s: exact match: %s; flipped-diagonal match: %s. "), *Layout.Material,
						Mesh.MaterialNames.IsValidIndex(Primitive.MaterialIndex) ? *Mesh.MaterialNames[Primitive.MaterialIndex] : TEXT("?"), *Error, *FlipError);
					continue;
				}
				bFlipped = true;
			}
			bMatched = true;
			Used[Candidate.Value] = true;
			PrimitiveOfSection[Section] = Candidate.Value;
			Skipped += Reduced.Skipped;
			if (bFlipped) { ++FlippedSections; FlippedPairs += Pairs; } else { ++Exact; }
			for (int32 Vertex = 0; Vertex < Layout.NumVertices; ++Vertex)
			{
				InputOfReference[Layout.FirstVertex + Vertex] = Primitive.FirstVertex + Reduced.Kept[Local[Vertex]];
			}
			break;
		}
		if (!bMatched)
		{
			return Fail(OutError, Tried);
		}
	}
	// Everything checked: now change the mesh.
	ReorderArray(Mesh.Positions, InputOfReference, OldCount, 1);
	ReorderArray(Mesh.Normals, InputOfReference, OldCount, 1);
	ReorderArray(Mesh.Tangents, InputOfReference, OldCount, 1);
	ReorderArray(Mesh.UVs, InputOfReference, OldCount, 1);
	ReorderArray(Mesh.UV1s, InputOfReference, OldCount, 1);
	ReorderArray(Mesh.Colors, InputOfReference, OldCount, 1);
	ReorderArray(Mesh.HairRootUV, InputOfReference, OldCount, 1);
	ReorderArray(Mesh.HairCoordU, InputOfReference, OldCount, 1);
	if (Mesh.NumInfluences > 0)
	{
		ReorderArray(Mesh.InfluenceJoints, InputOfReference, OldCount, Mesh.NumInfluences);
		ReorderArray(Mesh.InfluenceWeights, InputOfReference, OldCount, Mesh.NumInfluences);
	}
	TArray<FMantleGlbPrimitive> Rebuilt;
	for (int32 Section = 0; Section < Sections.Num(); ++Section)
	{
		const FMantleFaceLayoutSection& Layout = Sections[Section];
		FMantleGlbPrimitive& Primitive = Rebuilt.AddDefaulted_GetRef();
		Primitive.MaterialIndex = Mesh.Primitives[PrimitiveOfSection[Section]].MaterialIndex;
		Primitive.FirstVertex = Layout.FirstVertex;
		Primitive.NumVertices = Layout.NumVertices;
		for (int32 Index = 0; Index < Layout.NumTriangles * 3; ++Index) { Primitive.Indices.Add(Indices[Layout.FirstIndex + Index] - static_cast<uint32>(Layout.FirstVertex)); }
	}
	Mesh.Primitives = MoveTemp(Rebuilt);
	OutNote = FString::Printf(TEXT("face put in the built face layout (%s): %d sections, %d matched exactly, %d with %d flipped quad diagonals, %d surplus vertices skipped"), *Source, Sections.Num(), Exact, FlippedSections, FlippedPairs, Skipped);
	OutError.Reset();
	return true;
}

bool FMantleFaceLayout::IsNumberedLikeThis(const FMantleGlbMesh& Mesh, FString& OutWhy, const float* NeckBaseZ, FString* OutNote) const
{
	int32 Triangles = 0;
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives) { Triangles += Primitive.Indices.Num() / 3; }
	if (Triangles != Indices.Num() / 3 || Mesh.UVs.IsEmpty())
	{
		OutWhy = FString::Printf(TEXT("the face has %d triangles and the built face layout %d"), Triangles, Indices.Num() / 3);
		return false;
	}
	const float Scale = 1.0f / static_cast<float>(Quantization);
	int32 Triangle = 0, Differing = 0, FirstDiffering = INDEX_NONE, InRegion = 0, DifferingInRegion = 0;
	for (const FMantleGlbPrimitive& Primitive : Mesh.Primitives)
	{
		for (int32 First = 0; First + 2 < Primitive.Indices.Num(); First += 3, ++Triangle)
		{
			FVector2f Centroid = FVector2f::ZeroVector;
			bool bValid = true;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = Primitive.FirstVertex + static_cast<int32>(Primitive.Indices[First + Corner]);
				if (!Mesh.UVs.IsValidIndex(Vertex)) { bValid = false; break; }
				Centroid += Mesh.UVs[Vertex] / 3.0f;
			}
			FVector2f Wanted = FVector2f::ZeroVector;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const FIntPoint& UV = QuantizedUVs[Indices[Triangle * 3 + Corner]];
				Wanted += FVector2f(UV.X * Scale, UV.Y * Scale) / 3.0f;
			}
			bool bRegion = false;
			if (NeckBaseZ && bValid)
			{
				FVector3f Where = FVector3f::ZeroVector;
				for (int32 Corner = 0; Corner < 3; ++Corner) { Where += Mesh.Positions[Primitive.FirstVertex + static_cast<int32>(Primitive.Indices[First + Corner])] / 3.0f; }
				bRegion = Where.Z >= *NeckBaseZ - 6.0f && Where.Z < *NeckBaseZ + 12.0f && FMath::Sqrt(Where.X * Where.X + Where.Y * Where.Y) <= 16.0f;
			}
			InRegion += bRegion ? 1 : 0;
			if (!bValid || FMath::Abs(Centroid.X - Wanted.X) > 2.0f * Scale || FMath::Abs(Centroid.Y - Wanted.Y) > 2.0f * Scale)
			{
				++Differing;
				DifferingInRegion += bRegion ? 1 : 0;
				if (FirstDiffering == INDEX_NONE) { FirstDiffering = Triangle; }
			}
		}
	}
	if (NeckBaseZ)
	{
		if (DifferingInRegion > 0 || InRegion == 0)
		{
			OutWhy = FString::Printf(TEXT("%d of the %d collar-region triangles are not where the built face layout has them (%d of %d triangles overall)"), DifferingInRegion, InRegion, Differing, Triangles);
			return false;
		}
		if (OutNote) { *OutNote = FString::Printf(TEXT("face numbered like Epic's built faces in the collar region (%d triangles); %d of %d triangles outside it differ"), InRegion, Differing, Triangles); }
		UE_LOG(LogMantleFaceLayout, Display, TEXT("MANTLE_FACE_LAYOUT numbering: %d collar-region triangles all match; %d of %d triangles differ, all outside the region"), InRegion, Differing, Triangles);
		return true;
	}
	if (Differing > 0)
	{
		OutWhy = FString::Printf(TEXT("%d of %d triangles are not where the built face layout has them (the first is triangle %d)"), Differing, Triangles, FirstDiffering);
		return false;
	}
	if (OutNote) { *OutNote = TEXT("face numbered like Epic's built faces"); }
	return true;
}

bool FMantleFaceLayout::Parse(const FString& Json, FMantleFaceLayout& Out, FString& OutError)
{
	Out = FMantleFaceLayout();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("not valid JSON");
		return false;
	}
	int32 Version = 0, Count = 0, Triangles = 0;
	const TArray<TSharedPtr<FJsonValue>>* UVs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* IndexValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* SectionValues = nullptr;
	if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 1 || !Root->TryGetNumberField(TEXT("vertexCount"), Count) || Count <= 0
		|| !Root->TryGetNumberField(TEXT("triangleCount"), Triangles) || Triangles <= 0 || !Root->TryGetArrayField(TEXT("quantizedUVs"), UVs)
		|| !Root->TryGetArrayField(TEXT("indices"), IndexValues) || !Root->TryGetArrayField(TEXT("sections"), SectionValues)
		|| UVs->Num() != Count || IndexValues->Num() != Triangles * 3)
	{
		OutError = TEXT("not a version 1 face layout with matching counts");
		return false;
	}
	Root->TryGetStringField(TEXT("source"), Out.Source);
	Root->TryGetStringField(TEXT("generated"), Out.Generated);
	for (const TSharedPtr<FJsonValue>& Value : *UVs)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Pair) || Pair->Num() != 2)
		{
			OutError = TEXT("a quantised UV is not a pair");
			return false;
		}
		Out.QuantizedUVs.Add(FIntPoint(static_cast<int32>((*Pair)[0]->AsNumber()), static_cast<int32>((*Pair)[1]->AsNumber())));
	}
	for (const TSharedPtr<FJsonValue>& Value : *IndexValues)
	{
		double Number = 0.0;
		if (!Value.IsValid() || !Value->TryGetNumber(Number) || Number < 0.0 || Number >= Count)
		{
			OutError = TEXT("a triangle index is out of range");
			return false;
		}
		Out.Indices.Add(static_cast<uint32>(Number));
	}
	int32 NextVertex = 0, NextIndex = 0;
	for (const TSharedPtr<FJsonValue>& Value : *SectionValues)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		FMantleFaceLayoutSection Section;
		if (!Object.IsValid() || !Object->TryGetStringField(TEXT("material"), Section.Material) || !Object->TryGetNumberField(TEXT("firstVertex"), Section.FirstVertex)
			|| !Object->TryGetNumberField(TEXT("numVertices"), Section.NumVertices) || !Object->TryGetNumberField(TEXT("firstIndex"), Section.FirstIndex)
			|| !Object->TryGetNumberField(TEXT("numTriangles"), Section.NumTriangles) || Section.FirstVertex != NextVertex || Section.FirstIndex != NextIndex
			|| Section.NumVertices <= 0 || Section.NumTriangles <= 0)
		{
			OutError = TEXT("a section is missing a field or does not follow the one before");
			return false;
		}
		NextVertex += Section.NumVertices;
		NextIndex += Section.NumTriangles * 3;
		Out.Sections.Add(MoveTemp(Section));
	}
	if (NextVertex != Count || NextIndex != Out.Indices.Num())
	{
		OutError = TEXT("the sections do not cover the vertices and triangles");
		return false;
	}
	return true;
}

FString FMantleFaceLayout::ToJson() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetStringField(TEXT("comment"), TEXT("Maintainer-recorded by Mantle.Dev.RecordFaceLayout. Plugin-only; not part of the synced avatar contract. The layout of a built MetaHuman face (Epic's UE Optimized / Medium LOD0): quantised UVs (x2048) and triangles in built order, and the render sections. No geometry."));
	Root->SetStringField(TEXT("source"), Source);
	Root->SetStringField(TEXT("generated"), Generated);
	Root->SetNumberField(TEXT("vertexCount"), QuantizedUVs.Num());
	Root->SetNumberField(TEXT("triangleCount"), Indices.Num() / 3);
	Root->SetNumberField(TEXT("quantization"), Quantization);
	TArray<TSharedPtr<FJsonValue>> UVs;
	for (const FIntPoint& UV : QuantizedUVs)
	{
		TArray<TSharedPtr<FJsonValue>> Pair;
		Pair.Add(MakeShared<FJsonValueNumber>(UV.X));
		Pair.Add(MakeShared<FJsonValueNumber>(UV.Y));
		UVs.Add(MakeShared<FJsonValueArray>(Pair));
	}
	Root->SetArrayField(TEXT("quantizedUVs"), UVs);
	TArray<TSharedPtr<FJsonValue>> IndexValues;
	for (const uint32 Index : Indices) { IndexValues.Add(MakeShared<FJsonValueNumber>(Index)); }
	Root->SetArrayField(TEXT("indices"), IndexValues);
	TArray<TSharedPtr<FJsonValue>> SectionValues;
	for (const FMantleFaceLayoutSection& Section : Sections)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("material"), Section.Material);
		Object->SetNumberField(TEXT("firstVertex"), Section.FirstVertex);
		Object->SetNumberField(TEXT("numVertices"), Section.NumVertices);
		Object->SetNumberField(TEXT("firstIndex"), Section.FirstIndex);
		Object->SetNumberField(TEXT("numTriangles"), Section.NumTriangles);
		SectionValues.Add(MakeShared<FJsonValueObject>(Object));
	}
	Root->SetArrayField(TEXT("sections"), SectionValues);
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}
