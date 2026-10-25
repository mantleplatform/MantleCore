// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyCorrespondence.h"
#include "Animation/MorphTarget.h"
#include <algorithm>
#include "DNAReader.h"
#include "SkelMeshDNAUtils.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Math/Float16.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

#include "MantleAvatarContract.h"
#include "MantleBodyTopologyData.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MantleBodyCorrespondencePrivate
{
	bool Fail(FString& Error, const TCHAR* Reason)
	{
		Error = Reason;
		return false;
	}

	bool Signatures(TConstArrayView<FIntPoint> UVs, TConstArrayView<uint32> Indices, TArray<FString>& Out, FString& Error)
	{
		if (UVs.IsEmpty() || Indices.IsEmpty() || Indices.Num() % 3 != 0)
		{
			return Fail(Error, TEXT("the body layout has no complete triangles"));
		}
		TArray<TArray<FString>> Incident;
		Incident.SetNum(UVs.Num());
		for (int32 First = 0; First < Indices.Num(); First += 3)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Vertex = Indices[First + Corner];
				const uint32 Next = Indices[First + (Corner + 1) % 3];
				const uint32 Last = Indices[First + (Corner + 2) % 3];
				if (Vertex >= static_cast<uint32>(UVs.Num()) || Next >= static_cast<uint32>(UVs.Num()) || Last >= static_cast<uint32>(UVs.Num()))
				{
					return Fail(Error, TEXT("the body layout has an out-of-range triangle"));
				}
				Incident[Vertex].Add(FString::Printf(TEXT("%d,%d:%d,%d"), UVs[Next].X, UVs[Next].Y, UVs[Last].X, UVs[Last].Y));
			}
		}
		Out.SetNum(UVs.Num());
		for (int32 Vertex = 0; Vertex < UVs.Num(); ++Vertex)
		{
			Incident[Vertex].Sort();
			Out[Vertex] = FString::Printf(TEXT("%d,%d|%s"), UVs[Vertex].X, UVs[Vertex].Y, *FString::Join(Incident[Vertex], TEXT(";")));
		}
		return true;
	}

	FString TriangleKey(uint32 A, uint32 B, uint32 C)
	{
		if (B < A && B < C) { const uint32 T = A; A = B; B = C; C = T; }
		else if (C < A && C < B) { const uint32 T = A; A = C; C = B; B = T; }
		return FString::Printf(TEXT("%u,%u,%u"), A, B, C);
	}
}

bool FMantleBodyCorrespondence::Match(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> InputIndices,
	TArray<int32>& OutInputOfReference, FString& OutError) const
{
	using namespace MantleBodyCorrespondencePrivate;
	OutInputOfReference.Reset();
	if (UVs.Num() != QuantizedUVs.Num() || InputIndices.Num() != Indices.Num())
	{
		return Fail(OutError, TEXT("the body has a different vertex or triangle count from the complete reference layout"));
	}
	TArray<FIntPoint> InputUVs;
	InputUVs.Reserve(UVs.Num());
	for (const FVector2f& UV : UVs)
	{
		if (UV.ContainsNaN() || FMath::Abs(UV.X) > 16.0f || FMath::Abs(UV.Y) > 16.0f)
		{
			return Fail(OutError, TEXT("the body has invalid or out-of-range UVs"));
		}
		InputUVs.Add(FIntPoint(FMath::RoundToInt32(UV.X * 2048.0f), FMath::RoundToInt32(UV.Y * 2048.0f)));
	}
	TArray<FString> ReferenceSignatures, InputSignatures;
	if (!Signatures(QuantizedUVs, Indices, ReferenceSignatures, OutError) || !Signatures(InputUVs, InputIndices, InputSignatures, OutError))
	{
		return false;
	}
	TMap<FString, int32> InputBySignature;
	for (int32 Vertex = 0; Vertex < InputSignatures.Num(); ++Vertex)
	{
		if (InputBySignature.Contains(InputSignatures[Vertex]))
		{
			return Fail(OutError, TEXT("the body's UV/connectivity correspondence is ambiguous"));
		}
		InputBySignature.Add(InputSignatures[Vertex], Vertex);
	}
	TArray<int32> InputOfReference, ReferenceOfInput;
	ReferenceOfInput.Init(INDEX_NONE, UVs.Num());
	for (int32 Reference = 0; Reference < ReferenceSignatures.Num(); ++Reference)
	{
		const int32* Input = InputBySignature.Find(ReferenceSignatures[Reference]);
		if (!Input || ReferenceOfInput[*Input] != INDEX_NONE)
		{
			return Fail(OutError, TEXT("the body does not have the reference UV layout and triangle connectivity"));
		}
		InputOfReference.Add(*Input);
		ReferenceOfInput[*Input] = Reference;
	}
	TArray<FString> ReferenceTriangles, InputTriangles;
	for (int32 First = 0; First < Indices.Num(); First += 3)
	{
		ReferenceTriangles.Add(TriangleKey(Indices[First], Indices[First + 1], Indices[First + 2]));
		InputTriangles.Add(TriangleKey(ReferenceOfInput[InputIndices[First]], ReferenceOfInput[InputIndices[First + 1]], ReferenceOfInput[InputIndices[First + 2]]));
	}
	ReferenceTriangles.Sort();
	InputTriangles.Sort();
	if (ReferenceTriangles != InputTriangles)
	{
		return Fail(OutError, TEXT("the mapped body triangles differ from the reference layout"));
	}
	OutInputOfReference = MoveTemp(InputOfReference);
	OutError.Reset();
	return true;
}

bool FMantleBodyCorrespondence::MatchCorners(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> InputIndices,
	TArray<int32>& OutReferenceOfInput, FString& OutError) const
{
	using namespace MantleBodyCorrespondencePrivate;
	OutReferenceOfInput.Reset();
	if (QuantizedUVs.IsEmpty() || Indices.IsEmpty() || Indices.Num() % 3 != 0 || UVs.IsEmpty()
		|| UVs.Num() > 100000 || InputIndices.Num() != Indices.Num())
	{
		return Fail(OutError, TEXT("the lower body LOD does not contain the complete declared face set"));
	}
	TArray<FIntPoint> InputUVs;
	InputUVs.Reserve(UVs.Num());
	for (const FIntPoint UV : QuantizedUVs)
	{
		if (UV.X < -32768 || UV.X > 32768 || UV.Y < -32768 || UV.Y > 32768)
		{
			return Fail(OutError, TEXT("the lower reference has invalid quantized UVs"));
		}
	}
	for (const FVector2f& UV : UVs)
	{
		if (UV.ContainsNaN() || FMath::Abs(UV.X) > 16.0f || FMath::Abs(UV.Y) > 16.0f)
		{
			return Fail(OutError, TEXT("the lower body LOD has invalid UVs"));
		}
		InputUVs.Add(FIntPoint(FMath::RoundToInt32(UV.X * 2048.0f), FMath::RoundToInt32(UV.Y * 2048.0f)));
	}
	struct FFace { uint32 Vertices[3]; };
	const auto Key = [](TConstArrayView<FIntPoint> Points, TConstArrayView<uint32> Triangles, int32 First,
		FFace& Face, FString& OutKey) -> bool
	{
		const uint32 A = Triangles[First], B = Triangles[First + 1], C = Triangles[First + 2];
		if (A >= static_cast<uint32>(Points.Num()) || B >= static_cast<uint32>(Points.Num()) || C >= static_cast<uint32>(Points.Num())
			|| A == B || B == C || C == A || Points[A] == Points[B] || Points[B] == Points[C] || Points[C] == Points[A]) return false;
		const auto Less = [](FIntPoint X, FIntPoint Y) { return X.X < Y.X || (X.X == Y.X && X.Y < Y.Y); };
		int32 Corner = 0;
		if (Less(Points[B], Points[A]) && Less(Points[B], Points[C])) Corner = 1;
		else if (Less(Points[C], Points[A]) && Less(Points[C], Points[B])) Corner = 2;
		for (int32 I = 0; I < 3; ++I) Face.Vertices[I] = Triangles[First + (Corner + I) % 3];
		const FIntPoint X = Points[Face.Vertices[0]], Y = Points[Face.Vertices[1]], Z = Points[Face.Vertices[2]];
		OutKey = FString::Printf(TEXT("%d,%d|%d,%d|%d,%d"), X.X, X.Y, Y.X, Y.Y, Z.X, Z.Y);
		return true;
	};
	TMap<FString, FFace> ReferenceFaces;
	for (int32 First = 0; First < Indices.Num(); First += 3)
	{
		FFace Face; FString FaceKey;
		if (!Key(QuantizedUVs, Indices, First, Face, FaceKey) || ReferenceFaces.Contains(FaceKey))
		{
			return Fail(OutError, TEXT("the lower reference has ambiguous UV face/corner identity"));
		}
		ReferenceFaces.Add(FaceKey, Face);
	}
	TSet<FString> SeenFaces;
	TArray<int32> ReferenceOfInput; ReferenceOfInput.Init(INDEX_NONE, UVs.Num());
	TBitArray<> SeenReference(false, QuantizedUVs.Num());
	for (int32 First = 0; First < InputIndices.Num(); First += 3)
	{
		FFace Face; FString FaceKey;
		if (!Key(InputUVs, InputIndices, First, Face, FaceKey))
		{
			return Fail(OutError, TEXT("the lower body has invalid or ambiguous triangle corners"));
		}
		const FFace* Reference = ReferenceFaces.Find(FaceKey);
		if (!Reference || SeenFaces.Contains(FaceKey))
		{
			return Fail(OutError, TEXT("the lower body's complete oriented UV faces differ from the reference"));
		}
		SeenFaces.Add(FaceKey);
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			int32& Assigned = ReferenceOfInput[Face.Vertices[Corner]];
			const int32 Expected = static_cast<int32>(Reference->Vertices[Corner]);
			if (Assigned != INDEX_NONE && Assigned != Expected)
			{
				return Fail(OutError, TEXT("a lower body vertex maps ambiguously to different reference corners"));
			}
			Assigned = Expected; SeenReference[Expected] = true;
		}
	}
	if (ReferenceOfInput.Contains(INDEX_NONE) || SeenReference.CountSetBits() != QuantizedUVs.Num())
	{
		return Fail(OutError, TEXT("the lower body has unused vertices or missing reference corners"));
	}
	OutReferenceOfInput = MoveTemp(ReferenceOfInput);
	return true;
}

bool FMantleBodyCorrespondence::ValidateCornerCopies(TConstArrayView<int32> ReferenceOfInput,
	TConstArrayView<FVector3f> Positions, TConstArrayView<uint16> GlobalBones, TConstArrayView<float> Weights,
	int32 NumInfluences, FString& OutError, const bool bCookQuantization) const
{
	using namespace MantleBodyCorrespondencePrivate;
	if (ReferenceOfInput.IsEmpty() || Positions.Num() != ReferenceOfInput.Num() || NumInfluences < 1 || NumInfluences > 32
		|| GlobalBones.Num() != static_cast<int64>(Positions.Num()) * NumInfluences || Weights.Num() != GlobalBones.Num())
	{
		return Fail(OutError, TEXT("the native corner position/skin streams are malformed"));
	}
	TArray<int32> FirstOfReference; FirstOfReference.Init(INDEX_NONE, QuantizedUVs.Num());
	TArray<TArray<TPair<uint16, double>>> Bindings; Bindings.SetNum(Positions.Num());
	for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
	{
		const int32 Reference = ReferenceOfInput[Vertex];
		if (!FirstOfReference.IsValidIndex(Reference) || Positions[Vertex].ContainsNaN())
		{
			return Fail(OutError, TEXT("the native corner has an invalid reference or position"));
		}
		TArray<TPair<uint16, double>> Slots;
		double Total = 0;
		for (int32 I = 0; I < NumInfluences; ++I)
		{
			const int32 Index = Vertex * NumInfluences + I;
			if (!FMath::IsFinite(Weights[Index]) || Weights[Index] < 0)
			{
				return Fail(OutError, TEXT("the native corner has invalid skin weights"));
			}
			if (Weights[Index] > 0)
			{
				Total += Weights[Index];
				auto* Existing = Slots.FindByPredicate([&](const auto& Slot) { return Slot.Key == GlobalBones[Index]; });
				if (Existing) Existing->Value += Weights[Index];
				else Slots.Emplace(GlobalBones[Index], static_cast<double>(Weights[Index]));
			}
		}
		if (!FMath::IsFinite(Total) || Total <= 0) return Fail(OutError, TEXT("the native corner has no valid skin binding"));
		TArray<TPair<uint16, double>>& Effective = Bindings[Vertex];
		if (bCookQuantization)
		{
			// Match backend quantizeSkin: merge global bones in first-occurrence order;
			// floor normalized weights, then distribute residual by stable largest remainder.
			TArray<int32> Quantized, RemainderOrder; TArray<double> Remainders; int32 Sum = 0;
			for (int32 I = 0; I < Slots.Num(); ++I)
			{
				const double Exact = Slots[I].Value / Total * 255.0;
				const int32 Q = FMath::FloorToInt32(Exact);
				Quantized.Add(Q); Sum += Q; Remainders.Add(Exact - Q); RemainderOrder.Add(I);
			}
			if (Slots.Num() > 8 || Sum > 255) return Fail(OutError, TEXT("the exported native corner exceeds the cooked skin contract"));
			RemainderOrder.Sort([&](int32 A, int32 B) { return Remainders[A] > Remainders[B] || (Remainders[A] == Remainders[B] && A < B); });
			for (int32 I = 0; Sum < 255; ++I, ++Sum) ++Quantized[RemainderOrder[I % RemainderOrder.Num()]];
			for (int32 I = 0; I < Slots.Num(); ++I) if (Quantized[I] > 0) Effective.Emplace(Slots[I].Key, Quantized[I] / 255.0);
		}
		else for (const auto& Slot : Slots) Effective.Emplace(Slot.Key, Slot.Value / Total);
		Effective.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
		int32& First = FirstOfReference[Reference];
		if (First != INDEX_NONE && (Positions[Vertex] != Positions[First] || Effective != Bindings[First]))
		{
			return Fail(OutError, TEXT("packed copies of a lower reference corner disagree about position or global skin binding"));
		}
		if (First == INDEX_NONE) First = Vertex;
	}
	return true;
}

bool FMantleBodyCorrespondence::MatchNativeSplits(TConstArrayView<FVector2f> UVs,
	TConstArrayView<uint32> InputIndices, TConstArrayView<FVector3f> Positions,
	TConstArrayView<uint16> GlobalBones, TConstArrayView<float> Weights, const int32 NumInfluences,
	TArray<int32>& OutReferenceOfInput, TArray<int32>& OutInputOfReference,
	FString& OutError) const
{
	using namespace MantleBodyCorrespondencePrivate;
	OutReferenceOfInput.Reset(); OutInputOfReference.Reset();
	if (UVs.Num() <= QuantizedUVs.Num() || UVs.Num() > 100000 || Positions.Num() != UVs.Num()
		|| InputIndices.Num() != Indices.Num() || NumInfluences < 1 || NumInfluences > 32
		|| GlobalBones.Num() != static_cast<int64>(UVs.Num()) * NumInfluences || Weights.Num() != GlobalBones.Num())
		return Fail(OutError, TEXT("native split correspondence has invalid or non-split streams"));
	const auto Bits = [](const float Value) { uint32 Result; FMemory::Memcpy(&Result, &Value, sizeof(Result)); return Result; };
	TMap<FString, int32> LogicalByExactCopy;
	TArray<FVector2f> LogicalUVs;
	TArray<int32> LogicalOfInput, FirstOfLogical;
	for (int32 Input = 0; Input < UVs.Num(); ++Input)
	{
		const FVector3f P = Positions[Input]; const FVector2f UV = UVs[Input];
		if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z)
			|| !FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y))
			return Fail(OutError, TEXT("native split correspondence has non-finite position or UV"));
		const FString Key = FString::Printf(TEXT("%08x,%08x,%08x,%08x,%08x"), Bits(P.X), Bits(P.Y), Bits(P.Z), Bits(UV.X), Bits(UV.Y));
		if (const int32* ExistingLogical = LogicalByExactCopy.Find(Key)) LogicalOfInput.Add(*ExistingLogical);
		else
		{
			const int32 Logical = LogicalUVs.Add(UV);
			LogicalByExactCopy.Add(Key, Logical); FirstOfLogical.Add(Input); LogicalOfInput.Add(Logical);
		}
	}
	if (LogicalUVs.Num() != QuantizedUVs.Num())
		return Fail(OutError, TEXT("exact native copies do not form the complete indexed reference vertex set"));
	TArray<uint32> LogicalIndices; LogicalIndices.Reserve(InputIndices.Num());
	TBitArray<> SeenInput(false, UVs.Num());
	for (const uint32 Input : InputIndices)
	{
		if (Input >= static_cast<uint32>(UVs.Num())) return Fail(OutError, TEXT("native split triangle has an out-of-range vertex"));
		SeenInput[Input] = true; LogicalIndices.Add(static_cast<uint32>(LogicalOfInput[Input]));
	}
	if (SeenInput.CountSetBits() != UVs.Num()) return Fail(OutError, TEXT("native split correspondence contains unused physical vertices"));
	TArray<int32> LogicalOfReference;
	if (!Match(LogicalUVs, LogicalIndices, LogicalOfReference, OutError)) return false;
	TArray<int32> ReferenceOfLogical; ReferenceOfLogical.Init(INDEX_NONE, LogicalUVs.Num());
	TArray<int32> InputOfReference;
	for (int32 Reference = 0; Reference < LogicalOfReference.Num(); ++Reference)
	{
		ReferenceOfLogical[LogicalOfReference[Reference]] = Reference;
		InputOfReference.Add(FirstOfLogical[LogicalOfReference[Reference]]);
	}
	TArray<int32> ReferenceOfInput; ReferenceOfInput.Reserve(UVs.Num());
	for (const int32 Logical : LogicalOfInput) ReferenceOfInput.Add(ReferenceOfLogical[Logical]);
	if (!ValidateCornerCopies(ReferenceOfInput, Positions, GlobalBones, Weights, NumInfluences, OutError)) return false;
	OutReferenceOfInput = MoveTemp(ReferenceOfInput); OutInputOfReference = MoveTemp(InputOfReference);
	OutError.Reset();
	return true;
}

bool FMantleBodyCorrespondence::MatchTangentSplits(TConstArrayView<FVector2f> UVs, TConstArrayView<FVector2f> UV1s,
	TConstArrayView<uint32> InputIndices, TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> Normals,
	TConstArrayView<FColor> Colors, TConstArrayView<uint16> GlobalBones, TConstArrayView<float> Weights,
	const int32 NumInfluences, TArray<int32>& OutReferenceOfInput, TArray<int32>& OutInputOfReference, FString& OutError) const
{
	using namespace MantleBodyCorrespondencePrivate;
	OutReferenceOfInput.Reset(); OutInputOfReference.Reset();
	const int32 Count = UVs.Num();
	if (Count < QuantizedUVs.Num() || Count > 100000 || Positions.Num() != Count || Normals.Num() != Count
		|| (!UV1s.IsEmpty() && UV1s.Num() != Count) || (!Colors.IsEmpty() && Colors.Num() != Count)
		|| InputIndices.Num() != Indices.Num() || NumInfluences < 1 || NumInfluences > 32
		|| GlobalBones.Num() != static_cast<int64>(Count) * NumInfluences || Weights.Num() != GlobalBones.Num())
		return Fail(OutError, TEXT("the body's vertex streams are malformed, so tangent-only vertex splits cannot be checked"));
	constexpr float PositionTolerance = 1.0e-4f, UVTolerance = 1.0e-5f, NormalTolerance = 1.0e-4f;
	const auto Near3 = [](const FVector3f& A, const FVector3f& B, const float Tolerance)
	{
		return FMath::Abs(A.X - B.X) <= Tolerance && FMath::Abs(A.Y - B.Y) <= Tolerance && FMath::Abs(A.Z - B.Z) <= Tolerance;
	};
	const auto Near2 = [](const FVector2f& A, const FVector2f& B) { return FMath::Abs(A.X - B.X) <= UVTolerance && FMath::Abs(A.Y - B.Y) <= UVTolerance; };
	// Candidates come from the 27 neighbouring 0.01-unit position cells; everything else is compared by tolerance.
	TMap<FIntVector, TArray<int32>> ByCell;
	TArray<int32> LogicalOfInput, FirstOfLogical;
	TArray<FVector2f> LogicalUVs;
	for (int32 Input = 0; Input < Count; ++Input)
	{
		const FVector3f P = Positions[Input];
		if (P.ContainsNaN() || Normals[Input].ContainsNaN() || UVs[Input].ContainsNaN()
			|| (!UV1s.IsEmpty() && UV1s[Input].ContainsNaN()))
			return Fail(OutError, TEXT("the body has non-finite vertex data"));
		const FIntVector Cell(FMath::RoundToInt32(P.X * 100.0f), FMath::RoundToInt32(P.Y * 100.0f), FMath::RoundToInt32(P.Z * 100.0f));
		int32 Found = INDEX_NONE;
		for (int32 DX = -1; DX <= 1 && Found == INDEX_NONE; ++DX)
		for (int32 DY = -1; DY <= 1 && Found == INDEX_NONE; ++DY)
		for (int32 DZ = -1; DZ <= 1 && Found == INDEX_NONE; ++DZ)
		{
			const TArray<int32>* Candidates = ByCell.Find(Cell + FIntVector(DX, DY, DZ));
			if (!Candidates) continue;
			for (const int32 Other : *Candidates)
			{
				if (!Near3(Positions[Other], P, PositionTolerance) || !Near2(UVs[Other], UVs[Input])
					|| !Near3(Normals[Other], Normals[Input], NormalTolerance)
					|| (!UV1s.IsEmpty() && !Near2(UV1s[Other], UV1s[Input]))
					|| (!Colors.IsEmpty() && Colors[Other] != Colors[Input])) continue;
				bool bSameSkin = true;
				for (int32 S = 0; S < NumInfluences && bSameSkin; ++S)
					bSameSkin = Weights[Other * NumInfluences + S] == Weights[Input * NumInfluences + S]
						&& (Weights[Input * NumInfluences + S] == 0 || GlobalBones[Other * NumInfluences + S] == GlobalBones[Input * NumInfluences + S]);
				if (bSameSkin) { Found = LogicalOfInput[Other]; break; }
			}
		}
		if (Found == INDEX_NONE)
		{
			Found = LogicalUVs.Add(UVs[Input]); FirstOfLogical.Add(Input);
		}
		LogicalOfInput.Add(Found);
		ByCell.FindOrAdd(Cell).Add(Input);
	}
	if (LogicalUVs.Num() != QuantizedUVs.Num())
		return Fail(OutError, TEXT("after merging vertices that differ only in tangent the body still has a different vertex count from the reference, so it differs by more than a tangent split"));
	TArray<uint32> LogicalIndices; LogicalIndices.Reserve(InputIndices.Num());
	TBitArray<> SeenInput(false, Count);
	for (const uint32 Input : InputIndices)
	{
		if (Input >= static_cast<uint32>(Count)) return Fail(OutError, TEXT("the body has an out-of-range triangle vertex"));
		SeenInput[Input] = true; LogicalIndices.Add(static_cast<uint32>(LogicalOfInput[Input]));
	}
	if (SeenInput.CountSetBits() != Count) return Fail(OutError, TEXT("the body has vertices no triangle uses"));
	TArray<int32> LogicalOfReference;
	if (!Match(LogicalUVs, LogicalIndices, LogicalOfReference, OutError)) return false;
	TArray<int32> ReferenceOfLogical; ReferenceOfLogical.Init(INDEX_NONE, LogicalUVs.Num());
	TArray<int32> InputOfReference;
	for (int32 Reference = 0; Reference < LogicalOfReference.Num(); ++Reference)
	{
		ReferenceOfLogical[LogicalOfReference[Reference]] = Reference;
		InputOfReference.Add(FirstOfLogical[LogicalOfReference[Reference]]);
	}
	OutReferenceOfInput.Reserve(Count);
	for (const int32 Logical : LogicalOfInput) OutReferenceOfInput.Add(ReferenceOfLogical[Logical]);
	OutInputOfReference = MoveTemp(InputOfReference);
	OutError.Reset();
	return true;
}

bool FMantleBodyCorrespondence::ValidateDNABodyTarget(uint32 PositionCount, uint32 DeltaCount,
	TConstArrayView<float> Xs, TConstArrayView<float> Ys, TConstArrayView<float> Zs,
	TConstArrayView<uint32> PositionIDs, FString& OutError)
{
	using namespace MantleBodyCorrespondencePrivate;
	if (!PositionCount || PositionCount > 100000 || DeltaCount > PositionCount
		|| static_cast<uint32>(Xs.Num()) != DeltaCount || static_cast<uint32>(Ys.Num()) != DeltaCount
		|| static_cast<uint32>(Zs.Num()) != DeltaCount || static_cast<uint32>(PositionIDs.Num()) != DeltaCount)
		return Fail(OutError, TEXT("attached DNA blendshape target has unreadable or inconsistent offset arrays"));
	TSet<uint32> Seen;
	for (int32 I = 0; I < PositionIDs.Num(); ++I)
	{
		if (PositionIDs[I] >= PositionCount || Seen.Contains(PositionIDs[I])
			|| !FMath::IsFinite(Xs[I]) || !FMath::IsFinite(Ys[I]) || !FMath::IsFinite(Zs[I]))
			return Fail(OutError, TEXT("attached DNA blendshape target has duplicate/invalid position IDs or non-finite offsets"));
		Seen.Add(PositionIDs[I]);
	}
	OutError.Reset(); return true;
}

bool FMantleBodyCorrespondence::MatchDNABodyPolygons(TConstArrayView<FVector2f> RawUVs,
	TConstArrayView<TArray<uint32>> Faces, TArray<int32>& OutLayoutOfReference, FString& OutError) const
{
	using namespace MantleBodyCorrespondencePrivate;
	OutLayoutOfReference.Reset();
	if (RawUVs.Num() != QuantizedUVs.Num() || Faces.IsEmpty() || Faces.Num() > Indices.Num() / 3)
		return Fail(OutError, TEXT("DNA body polygon layout is absent or has a different canonical vertex count"));
	TArray<FVector2f> UVs; TArray<FIntPoint> Quantized;
	for (const FVector2f& Raw : RawUVs)
	{
		const FVector2f UV(Raw.X, 1.0f - Raw.Y);
		if (UV.ContainsNaN() || FMath::Abs(UV.X) > 16.0f || FMath::Abs(UV.Y) > 16.0f)
			return Fail(OutError, TEXT("DNA body polygon UV is not readable in the canonical UV basis"));
		UVs.Add(UV); Quantized.Add(FIntPoint(FMath::RoundToInt32(UV.X * 2048.0f), FMath::RoundToInt32(UV.Y * 2048.0f)));
	}
	const auto Key = [](const TConstArrayView<FIntPoint> Points, uint32 A, uint32 B, uint32 C)
	{
		const FString X = FString::Printf(TEXT("%d,%d"), Points[A].X, Points[A].Y);
		const FString Y = FString::Printf(TEXT("%d,%d"), Points[B].X, Points[B].Y);
		const FString Z = FString::Printf(TEXT("%d,%d"), Points[C].X, Points[C].Y);
		const FString K0 = X + TEXT(":") + Y + TEXT(":") + Z;
		const FString K1 = Y + TEXT(":") + Z + TEXT(":") + X;
		const FString K2 = Z + TEXT(":") + X + TEXT(":") + Y;
		return K0 < K1 ? (K0 < K2 ? K0 : K2) : (K1 < K2 ? K1 : K2);
	};
	TMap<FString, int32> ReferenceFaces;
	for (int32 I = 0; I < Indices.Num(); I += 3) ++ReferenceFaces.FindOrAdd(Key(QuantizedUVs, Indices[I], Indices[I+1], Indices[I+2]));
	const auto Supported = [&ReferenceFaces, &Quantized, &Key](const TArray<uint32>& Candidate)
	{
		TMap<FString, int32> Needed;
		for (int32 I = 0; I < Candidate.Num(); I += 3)
		{
			const FString K = Key(Quantized, Candidate[I], Candidate[I+1], Candidate[I+2]);
			if (++Needed.FindOrAdd(K) > ReferenceFaces.FindRef(K)) return false;
		}
		return true;
	};
	TArray<uint32> Triangles;
	for (const TArray<uint32>& Face : Faces)
	{
		if (Face.Num() != 3 && Face.Num() != 4) return Fail(OutError, TEXT("DNA body has unsupported polygon corners"));
		for (uint32 I : Face) if (I >= static_cast<uint32>(UVs.Num())) return Fail(OutError, TEXT("DNA body polygon index is out of range"));
		if (Face.Num() == 3) { Triangles.Append(Face); continue; }
		const TArray<uint32> A = {Face[0], Face[1], Face[2], Face[0], Face[2], Face[3]};
		const TArray<uint32> B = {Face[0], Face[1], Face[3], Face[1], Face[2], Face[3]};
		const bool bA = Supported(A), bB = Supported(B);
		if (bA == bB) return Fail(OutError, TEXT("DNA body polygon has no unique reference-proved triangulation"));
		Triangles.Append(bA ? A : B);
	}
	if (!Match(UVs, Triangles, OutLayoutOfReference, OutError)) return false;
	OutError.Reset(); return true;
}

bool FMantleBodyCorrespondence::MatchFlippedDiagonals(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> InputIndices,
	TArray<int32>& OutInputOfReference, FString& OutNote, FString& OutError, const int32 MaxFlippedPairs, int32* OutFlippedPairs) const
{
	using namespace MantleBodyCorrespondencePrivate;
	OutInputOfReference.Reset();
	OutNote.Reset();
	if (OutFlippedPairs) { *OutFlippedPairs = 0; }
	const int32 Count = QuantizedUVs.Num();
	if (Count == 0 || UVs.Num() != Count || InputIndices.Num() != Indices.Num() || Indices.Num() % 3 != 0)
	{
		return Fail(OutError, TEXT("the body has a different vertex or triangle count from the reference layout"));
	}
	TArray<FIntPoint> InputUVs;
	InputUVs.Reserve(Count);
	for (const FVector2f& UV : UVs)
	{
		if (UV.ContainsNaN() || FMath::Abs(UV.X) > 16.0f || FMath::Abs(UV.Y) > 16.0f)
		{
			return Fail(OutError, TEXT("the body has invalid or out-of-range UVs"));
		}
		InputUVs.Add(FIntPoint(FMath::RoundToInt32(UV.X * 2048.0f), FMath::RoundToInt32(UV.Y * 2048.0f)));
	}
	// The UVs of each vertex's neighbours, which tell apart vertices that share a UV.
	const auto Neighbours = [](TConstArrayView<FIntPoint> Points, TConstArrayView<uint32> Triangles, TArray<TSet<FIntPoint>>& Out) -> bool
	{
		Out.Reset();
		Out.SetNum(Points.Num());
		for (int32 First = 0; First + 2 < Triangles.Num(); First += 3)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Vertex = Triangles[First + Corner];
				const uint32 Next = Triangles[First + (Corner + 1) % 3];
				if (Vertex >= static_cast<uint32>(Points.Num()) || Next >= static_cast<uint32>(Points.Num())) { return false; }
				Out[Vertex].Add(Points[Next]);
				Out[Next].Add(Points[Vertex]);
			}
		}
		return true;
	};
	TArray<TSet<FIntPoint>> ReferenceNeighbours, InputNeighbours;
	if (!Neighbours(QuantizedUVs, Indices, ReferenceNeighbours) || !Neighbours(InputUVs, InputIndices, InputNeighbours))
	{
		return Fail(OutError, TEXT("the body has an out-of-range triangle"));
	}
	TMap<FIntPoint, TArray<int32>> ReferenceByUV, InputByUV;
	for (int32 Vertex = 0; Vertex < Count; ++Vertex)
	{
		ReferenceByUV.FindOrAdd(QuantizedUVs[Vertex]).Add(Vertex);
		InputByUV.FindOrAdd(InputUVs[Vertex]).Add(Vertex);
	}
	TArray<int32> InputOfReference;
	InputOfReference.Init(INDEX_NONE, Count);
	int32 Repeated = 0;
	for (const TPair<FIntPoint, TArray<int32>>& Group : ReferenceByUV)
	{
		const TArray<int32>* Input = InputByUV.Find(Group.Key);
		if (!Input || Input->Num() != Group.Value.Num())
		{
			return Fail(OutError, TEXT("the body does not have the reference UV layout"));
		}
		const int32 N = Group.Value.Num();
		if (N == 1)
		{
			InputOfReference[Group.Value[0]] = (*Input)[0];
			continue;
		}
		if (N > 6)
		{
			return Fail(OutError, TEXT("the reference repeats a UV too often to tell the vertices apart"));
		}
		Repeated += N;
		TArray<int32> Order;
		for (int32 Index = 0; Index < N; ++Index) { Order.Add(Index); }
		int32 Best = -1, BestCount = 0;
		TArray<int32> BestOrder;
		do
		{
			int32 Sum = 0;
			for (int32 A = 0; A < N; ++A) { Sum += ReferenceNeighbours[Group.Value[A]].Intersect(InputNeighbours[(*Input)[Order[A]]]).Num(); }
			if (Sum > Best) { Best = Sum; BestCount = 1; BestOrder = Order; }
			else if (Sum == Best) { ++BestCount; }
		}
		while (std::next_permutation(Order.GetData(), Order.GetData() + N));
		if (BestCount != 1)
		{
			return Fail(OutError, TEXT("vertices that share a UV cannot be told apart by their neighbours"));
		}
		for (int32 A = 0; A < N; ++A) { InputOfReference[Group.Value[A]] = (*Input)[BestOrder[A]]; }
	}
	TArray<int32> ReferenceOfInput;
	ReferenceOfInput.Init(INDEX_NONE, Count);
	for (int32 Reference = 0; Reference < Count; ++Reference)
	{
		if (InputOfReference[Reference] == INDEX_NONE || ReferenceOfInput[InputOfReference[Reference]] != INDEX_NONE)
		{
			return Fail(OutError, TEXT("the body's vertices are not one-to-one with the reference's"));
		}
		ReferenceOfInput[InputOfReference[Reference]] = Reference;
	}
	// Triangles: each reference triangle must be an input triangle, but for flipped-diagonal pairs.
	TMap<FString, int32> ReferenceCount, InputCount;
	for (int32 First = 0; First < Indices.Num(); First += 3)
	{
		++ReferenceCount.FindOrAdd(TriangleKey(Indices[First], Indices[First + 1], Indices[First + 2]));
		++InputCount.FindOrAdd(TriangleKey(ReferenceOfInput[InputIndices[First]], ReferenceOfInput[InputIndices[First + 1]], ReferenceOfInput[InputIndices[First + 2]]));
	}
	const auto Sorted = [](uint32 A, uint32 B, uint32 C)
	{
		TArray<uint32> Out = { A, B, C };
		Out.Sort();
		return Out;
	};
	TArray<TArray<uint32>> OnlyReference, OnlyInput;
	{
		TMap<FString, int32> Budget = InputCount;
		for (int32 First = 0; First < Indices.Num(); First += 3)
		{
			int32* Left = Budget.Find(TriangleKey(Indices[First], Indices[First + 1], Indices[First + 2]));
			if (Left && *Left > 0) { --*Left; }
			else { OnlyReference.Add(Sorted(Indices[First], Indices[First + 1], Indices[First + 2])); }
		}
		Budget = ReferenceCount;
		for (int32 First = 0; First < Indices.Num(); First += 3)
		{
			const uint32 A = ReferenceOfInput[InputIndices[First]], B = ReferenceOfInput[InputIndices[First + 1]], C = ReferenceOfInput[InputIndices[First + 2]];
			int32* Left = Budget.Find(TriangleKey(A, B, C));
			if (Left && *Left > 0) { --*Left; }
			else { OnlyInput.Add(Sorted(A, B, C)); }
		}
	}
	if (OnlyReference.Num() != OnlyInput.Num() || static_cast<int64>(OnlyReference.Num()) > 2ll * MaxFlippedPairs || OnlyReference.Num() % 2 != 0)
	{
		return Fail(OutError, TEXT("the body's triangles differ from the reference layout by more than flipped quad diagonals"));
	}
	int32 Flipped = 0;
	TBitArray<> InputUsed(false, OnlyInput.Num()), ReferenceUsed(false, OnlyReference.Num());
	for (int32 I = 0; I < OnlyReference.Num(); ++I)
	{
		if (ReferenceUsed[I]) { continue; }
		for (int32 J = I + 1; J < OnlyReference.Num() && !ReferenceUsed[I]; ++J)
		{
			if (ReferenceUsed[J]) { continue; }
			TArray<uint32> Shared, OnlyI, OnlyJ;
			for (const uint32 Vertex : OnlyReference[I]) { (OnlyReference[J].Contains(Vertex) ? Shared : OnlyI).Add(Vertex); }
			for (const uint32 Vertex : OnlyReference[J]) { if (!Shared.Contains(Vertex)) { OnlyJ.Add(Vertex); } }
			if (Shared.Num() != 2 || OnlyI.Num() != 1 || OnlyJ.Num() != 1) { continue; }
			// The input has the two triangles on the other diagonal of the same quad.
			const TArray<uint32> First = Sorted(OnlyI[0], OnlyJ[0], Shared[0]);
			const TArray<uint32> Second = Sorted(OnlyI[0], OnlyJ[0], Shared[1]);
			int32 FoundFirst = INDEX_NONE, FoundSecond = INDEX_NONE;
			for (int32 K = 0; K < OnlyInput.Num(); ++K)
			{
				if (InputUsed[K]) { continue; }
				if (FoundFirst == INDEX_NONE && OnlyInput[K] == First) { FoundFirst = K; }
				else if (FoundSecond == INDEX_NONE && OnlyInput[K] == Second) { FoundSecond = K; }
			}
			if (FoundFirst == INDEX_NONE || FoundSecond == INDEX_NONE) { continue; }
			ReferenceUsed[I] = true; ReferenceUsed[J] = true; InputUsed[FoundFirst] = true; InputUsed[FoundSecond] = true;
			++Flipped;
		}
	}
	if (ReferenceUsed.CountSetBits() != OnlyReference.Num())
	{
		return Fail(OutError, TEXT("some of the body's triangles differ from the reference layout in more than a flipped quad diagonal"));
	}
	if (OutFlippedPairs) { *OutFlippedPairs = Flipped; }
	OutInputOfReference = MoveTemp(InputOfReference);
	OutNote = FString::Printf(TEXT("UV match, %d repeated-UV vertices told apart by their neighbours, %d diagonal%s flipped"), Repeated, Flipped, Flipped == 1 ? TEXT("") : TEXT("s"));
	OutError.Reset();
	return true;
}

bool FMantleBodyCorrespondence::MatchMergedBodyExport(const USkeletalMesh* Body,
	TArray<int32>& OutReferenceOfInput, TArray<int32>& OutInputOfReference,
	TArray<FString>& OutMergedSplits, FString& OutError) const
{
	using namespace MantleBodyCorrespondencePrivate;
	OutReferenceOfInput.Reset(); OutInputOfReference.Reset(); OutMergedSplits.Reset();
	const FSkeletalMeshRenderData* Render = Body ? Body->GetResourceForRendering() : nullptr;
	if (bLowerCornerTopology || !Render || !Render->LODRenderData.IsValidIndex(0))
		return Fail(OutError, TEXT("body export merge requires readable native LOD0"));
	const FSkeletalMeshLODRenderData& LOD = Render->LODRenderData[0];
	const auto& Vertex = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	const auto& Position = LOD.StaticVertexBuffers.PositionVertexBuffer;
	const auto& Color = LOD.StaticVertexBuffers.ColorVertexBuffer;
	const FSkinWeightVertexBuffer* Skin = LOD.GetSkinWeightVertexBuffer();
	const int32 Count = static_cast<int32>(Position.GetNumVertices());
	if (Count <= QuantizedUVs.Num() || Count > 100000 || !Position.GetVertexData()
		|| !Vertex.GetTexCoordData() || Vertex.GetNumVertices() != static_cast<uint32>(Count)
		|| Vertex.GetNumTexCoords() == 0 || !Skin || Skin->GetNumVertices() != static_cast<uint32>(Count)
		|| Skin->GetMaxBoneInfluences() == 0 || Skin->GetMaxBoneInfluences() > 32
		|| !LOD.MultiSizeIndexContainer.IsIndexBufferValid() || LOD.RenderSections.Num() != 1
		|| LOD.RenderSections[0].BaseVertexIndex != 0 || LOD.RenderSections[0].NumVertices != static_cast<uint32>(Count)
		|| (Color.GetNumVertices() != 0 && (Color.GetNumVertices() != static_cast<uint32>(Count) || !Color.GetVertexData())))
		return Fail(OutError, TEXT("body export merge has unreadable or unsupported native streams/sections"));
	const auto& BoneMap = LOD.RenderSections[0].BoneMap;
	const int32 Slots = static_cast<int32>(Skin->GetMaxBoneInfluences());
	TArray<FVector3f> Positions; TArray<FVector2f> UVs; TArray<uint16> Bones; TArray<float> Weights;
	for (int32 I = 0; I < Count; ++I)
	{
		Positions.Add(Position.VertexPosition(I)); UVs.Add(Vertex.GetVertexUV(I, 0));
		uint32 Offset = 0, Influences = 0; Skin->GetVertexInfluenceOffsetCount(I, Offset, Influences);
		if (Influences > static_cast<uint32>(Slots)) return Fail(OutError, TEXT("body export merge has malformed native skin"));
		for (int32 S = 0; S < Slots; ++S)
		{
			const uint16 Weight = static_cast<uint32>(S) < Influences ? Skin->GetBoneWeight(I, S) : 0;
			const uint32 Local = Weight ? Skin->GetBoneIndex(I, S) : 0;
			if (Weight && (Local >= static_cast<uint32>(BoneMap.Num()) || BoneMap[Local] >= Body->GetRefSkeleton().GetRawBoneNum()))
				return Fail(OutError, TEXT("body export merge has invalid native global skin bone"));
			Bones.Add(Weight ? BoneMap[Local] : 0); Weights.Add(static_cast<float>(Weight));
		}
	}
	TArray<uint32> NativeIndices; LOD.MultiSizeIndexContainer.GetIndexBuffer(NativeIndices);
	TArray<int32> ReferenceOfInput, InputOfReference;
	if (!MatchNativeSplits(UVs, NativeIndices, Positions, Bones, Weights, Slots, ReferenceOfInput, InputOfReference, OutError)) return false;
	TArray<TPair<int32, int32>> Twins;
	for (int32 I = 0; I < Count; ++I)
	{
		const int32 First = InputOfReference[ReferenceOfInput[I]];
		if (First == I) continue;
		for (uint32 Channel = 0; Channel < Vertex.GetNumTexCoords(); ++Channel)
			if (Vertex.GetVertexUV(I, Channel) != Vertex.GetVertexUV(First, Channel))
				return Fail(OutError, TEXT("body export shading twins differ in a UV channel"));
		if (Color.GetNumVertices() && Color.VertexColor(I) != Color.VertexColor(First))
			return Fail(OutError, TEXT("body export shading twins differ in vertex colour"));
		for (int32 S = 0; S < Slots; ++S)
			if (Bones[I * Slots + S] != Bones[First * Slots + S] || Weights[I * Slots + S] != Weights[First * Slots + S])
				return Fail(OutError, TEXT("body export shading twins differ in exact native global skin weights"));
		Twins.Emplace(I, First);
	}
	TMap<int32, FString> DNAPointProof;
	const TSharedPtr<IDNAReader> DNA = USkelMeshDNAUtils::GetDNAReader(const_cast<USkeletalMesh*>(Body));
	const bool bAttachedDNA = DNA.IsValid() || USkelMeshDNAUtils::GetMeshDNAAsset(const_cast<USkeletalMesh*>(Body))
		|| USkelMeshDNAUtils::GetMeshDNA(const_cast<USkeletalMesh*>(Body));
	if (bAttachedDNA)
	{
		// Morph SourceIdx applies to the physical render domain. Prove the indexed source
		// bridge independently; native skin packing is deliberately not compared across domains.
		const FSkeletalMeshModel* Imported = Body->GetImportedModel();
		if (!DNA || DNA->GetLODCount() == 0 || !Imported || !Imported->LODModels.IsValidIndex(0))
			return Fail(OutError, TEXT("body export merge cannot read the attached DNA/imported LOD0"));
		int64 CheckedDNATargets = 0;
		// Read every original attached target, including lower DNA meshes. Zero targets
		// is the reader's observed count, never inferred from the UE morph inventory.
		for (uint16 Mesh = 0; Mesh < DNA->GetMeshCount(); ++Mesh)
		{
			for (uint16 Target = 0; Target < DNA->GetBlendShapeTargetCount(Mesh); ++Target)
			{
				if (!ValidateDNABodyTarget(DNA->GetVertexPositionCount(Mesh), DNA->GetBlendShapeTargetDeltaCount(Mesh, Target),
					DNA->GetBlendShapeTargetDeltaXs(Mesh, Target), DNA->GetBlendShapeTargetDeltaYs(Mesh, Target),
					DNA->GetBlendShapeTargetDeltaZs(Mesh, Target), DNA->GetBlendShapeTargetVertexIndices(Mesh, Target), OutError))
				{
					OutError = FString::Printf(TEXT("DNA mesh %u target %u: %s"), Mesh, Target, *OutError); return false;
				}
				++CheckedDNATargets;
			}
		}
		const auto& Model = Imported->LODModels[0]; TArray<FSoftSkinVertex> Source;
		Model.GetVertices(Source);
		if (Source.Num() != Count || Vertex.GetNumTexCoords() > UE_ARRAY_COUNT(Source[0].UVs)
			|| Model.MeshToImportVertexMap.Num() != Count || Model.IndexBuffer != NativeIndices
			|| Model.Sections.Num() != 1 || Model.Sections[0].BaseVertexIndex != 0
			|| Model.Sections[0].NumVertices != Count || Model.Sections[0].BaseIndex != LOD.RenderSections[0].BaseIndex
			|| Model.Sections[0].NumTriangles != LOD.RenderSections[0].NumTriangles)
			return Fail(OutError, TEXT("body export merge cannot prove indexed imported/render source identity"));
		for (int32 I = 0; I < Count; ++I)
		{
			if (Source[I].Position != Positions[I] || Model.MeshToImportVertexMap[I] < 0)
				return Fail(OutError, TEXT("body export merge imported/render source positions or source IDs differ"));
			for (uint32 Channel = 0; Channel < Vertex.GetNumTexCoords(); ++Channel)
			{
				const FVector2f Raw = Source[I].UVs[Channel];
				FVector2f Packed = Raw;
				if (!Vertex.GetUseFullPrecisionUVs())
				{
					const FSkeletalMeshLODInfo* Info = Body->GetLODInfo(0);
					if (!Info) return Fail(OutError, TEXT("body export merge cannot read declared native UV packing"));
					FFloat16 X(Raw.X), Y(Raw.Y);
					if (Info->BuildSettings.bUseBackwardsCompatibleF16TruncUVs) { X.SetTruncate(Raw.X); Y.SetTruncate(Raw.Y); }
					Packed = FVector2f(X.GetFloat(), Y.GetFloat());
				}
				if (Packed != Vertex.GetVertexUV(I, Channel))
					return Fail(OutError, TEXT("body export merge imported/render UV domain differs after declared UV packing"));
			}
		}
		for (const auto& Twin : Twins)
		{
			if (Model.MeshToImportVertexMap[Twin.Key] != Model.MeshToImportVertexMap[Twin.Value])
				return Fail(OutError, TEXT("body export shading twins have different original source FVertexIDs"));
			for (uint32 Channel = 0; Channel < UE_ARRAY_COUNT(Source[Twin.Key].UVs); ++Channel)
				if (Source[Twin.Key].UVs[Channel] != Source[Twin.Value].UVs[Channel])
					return Fail(OutError, TEXT("body export shading twins differ in imported UV channels"));
			if (Source[Twin.Key].Color != Source[Twin.Value].Color)
				return Fail(OutError, TEXT("body export shading twins differ in imported vertex colour"));
			for (int32 S = 0; S < MAX_TOTAL_INFLUENCES; ++S)
				if (Source[Twin.Key].InfluenceWeights[S] != Source[Twin.Value].InfluenceWeights[S]
					|| (Source[Twin.Key].InfluenceWeights[S] && Source[Twin.Key].InfluenceBones[S] != Source[Twin.Value].InfluenceBones[S]))
					return Fail(OutError, TEXT("body export shading twins differ in exact imported skin"));
		}
		int32 MatchingMeshes = 0;
		for (uint16 Mesh : DNA->GetMeshIndicesForLOD(0))
		{
			if (Mesh >= DNA->GetMeshCount()) return Fail(OutError, TEXT("attached body DNA LOD0 mesh index is invalid"));
			if (DNA->GetVertexLayoutCount(Mesh) != static_cast<uint32>(QuantizedUVs.Num())) continue;
			if (++MatchingMeshes != 1 || !DNA->GetVertexPositionCount(Mesh)
				|| static_cast<uint32>(DNA->GetVertexPositionXs(Mesh).Num()) != DNA->GetVertexPositionCount(Mesh)
				|| static_cast<uint32>(DNA->GetVertexPositionYs(Mesh).Num()) != DNA->GetVertexPositionCount(Mesh)
				|| static_cast<uint32>(DNA->GetVertexPositionZs(Mesh).Num()) != DNA->GetVertexPositionCount(Mesh))
				return Fail(OutError, TEXT("attached body DNA has ambiguous or unreadable canonical geometry"));
			TArray<FVector2f> RawUVs; TArray<uint32> DNAOfLayout; TArray<TArray<uint32>> Faces;
			for (uint32 I = 0; I < DNA->GetVertexLayoutCount(Mesh); ++I)
			{
				const auto L = DNA->GetVertexLayout(Mesh, I);
				if (L.Position < 0 || L.TextureCoordinate < 0 || static_cast<uint32>(L.Position) >= DNA->GetVertexPositionCount(Mesh)
					|| static_cast<uint32>(L.TextureCoordinate) >= DNA->GetVertexTextureCoordinateCount(Mesh))
					return Fail(OutError, TEXT("attached body DNA vertex layout is malformed"));
				const auto UV = DNA->GetVertexTextureCoordinate(Mesh, L.TextureCoordinate);
				RawUVs.Add(FVector2f(UV.U, UV.V)); DNAOfLayout.Add(L.Position);
			}
			for (uint32 I = 0; I < DNA->GetFaceCount(Mesh); ++I)
				Faces.Add(TArray<uint32>(DNA->GetFaceVertexLayoutIndices(Mesh, I)));
			TArray<int32> LayoutOfReference;
			if (!MatchDNABodyPolygons(RawUVs, Faces, LayoutOfReference, OutError)) return false;
			// This DNA point is topology-derived. Independent equal original source FVertexID,
			// physical CPU morph checks and readable original DNA targets are also required.
			for (const auto& Twin : Twins)
			{
				const uint32 Point = DNAOfLayout[LayoutOfReference[ReferenceOfInput[Twin.Key]]];
				DNAPointProof.Add(Twin.Key, FString::Printf(TEXT(" (equal source FVertexID %d; topology-derived DNA mesh %u position %u; %lld original attached DNA targets read/validated; %d registered physical morphs checked)"),
					Model.MeshToImportVertexMap[Twin.Key], Mesh, Point, CheckedDNATargets, Body->GetMorphTargets().Num()));
			}
		}
		if (MatchingMeshes != 1) return Fail(OutError, TEXT("attached body DNA has no complete canonical LOD0 geometry"));
	}
	if (LOD.MorphTargetVertexInfoBuffers.GetNumMorphs() > static_cast<uint32>(Body->GetMorphTargets().Num()))
		return Fail(OutError, TEXT("body export merge has GPU morphs without readable native target objects"));
	// Sparse absent entries mean zero, never an unreadable/implicit success. Check both delta fields.
	for (const UMorphTarget* Morph : Body->GetMorphTargets())
	{
		if (!Morph) return Fail(OutError, TEXT("body export merge has an unreadable morph target"));
		const auto Deltas = Morph->GetMorphTargetDeltas(0);
		if (Deltas.Num() != Morph->GetNumDeltasForLOD(0)) return Fail(OutError, TEXT("body export merge cannot read every morph delta"));
		if (Deltas.Num() > 0 && (!Morph->GetMorphLODModels().IsValidIndex(0)
			|| Morph->GetMorphLODModels()[0].NumBaseMeshVerts != Count))
			return Fail(OutError, TEXT("body export merge morph offsets are not bound to the native LOD0 vertex domain"));
		TMap<uint32, const FMorphTargetDelta*> ByVertex;
		for (const FMorphTargetDelta& Delta : Deltas)
		{
			if (Delta.SourceIdx >= static_cast<uint32>(Count) || ByVertex.Contains(Delta.SourceIdx)
				|| Delta.PositionDelta.ContainsNaN() || Delta.TangentZDelta.ContainsNaN())
				return Fail(OutError, TEXT("body export merge has malformed or non-finite morph offsets"));
			ByVertex.Add(Delta.SourceIdx, &Delta);
		}
		for (const auto& Twin : Twins)
		{
			const FMorphTargetDelta* A = ByVertex.FindRef(Twin.Key), *B = ByVertex.FindRef(Twin.Value);
			const FVector3f AP = A ? A->PositionDelta : FVector3f::ZeroVector, BP = B ? B->PositionDelta : FVector3f::ZeroVector;
			const FVector3f AN = A ? A->TangentZDelta : FVector3f::ZeroVector, BN = B ? B->TangentZDelta : FVector3f::ZeroVector;
			if (AP != BP || AN != BN)
			{
				OutError = FString::Printf(TEXT("body export shading twins %d/%d differ in morph %s offsets"), Twin.Value, Twin.Key, *Morph->GetName());
				return false;
			}
		}
	}
	for (const auto& Twin : Twins) OutMergedSplits.Add(FString::Printf(TEXT("%d->%d%s"), Twin.Key, Twin.Value, *DNAPointProof.FindRef(Twin.Key)));
	OutReferenceOfInput = MoveTemp(ReferenceOfInput); OutInputOfReference = MoveTemp(InputOfReference);
	OutError.Reset();
	return true;
}

bool FMantleBodyCorrespondence::ReadLowerLODPolicy(const FJsonObject& Layout, const FJsonObject& Topology,
	bool& OutEnabled, FString& OutError)
{
	OutEnabled = false;
	const auto Policy = [](const FJsonObject& Object, bool& bEnabled) -> bool
	{
		bEnabled = false;
		if (!Object.HasField(TEXT("lowerLODTopology"))) return true;
		const TSharedPtr<FJsonObject>* Value = nullptr; double PolicyVersion = 0; FString Name;
		if (!Object.TryGetObjectField(TEXT("lowerLODTopology"), Value) || !Value || !Value->IsValid()
			|| (*Value)->Values.Num() != 2
			|| !(*Value)->TryGetNumberField(TEXT("version"), PolicyVersion) || PolicyVersion != 1
			|| !(*Value)->TryGetStringField(TEXT("policy"), Name) || Name != TEXT("oriented-uv-corners-v1")) return false;
		bEnabled = true; return true;
	};
	bool bLayout = false, bTopology = false;
	if (!Policy(Layout, bLayout) || !Policy(Topology, bTopology) || bLayout != bTopology)
	{
		OutError = TEXT("the complete layout and topology must agree on a supported lower-LOD policy");
		return false;
	}
	OutEnabled = bLayout;
	return true;
}

const FMantleBodyCorrespondence* FMantleBodyCorrespondence::Get()
{
	return GetLOD(0);
}

const FMantleBodyCorrespondence* FMantleBodyCorrespondence::GetLOD(const int32 LODIndex)
{
	static const TOptional<TArray<FMantleBodyCorrespondence>> Cached = []() -> TOptional<TArray<FMantleBodyCorrespondence>>
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!Plugin || !Contract || !FFileHelper::LoadFileToString(Text, *(Plugin->GetBaseDir() / TEXT("Resources/AvatarContract/body-layout-v1.json")))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root) { return {}; }
		double Version = 0, Quantization = 0, LODCount = 0;
		const TArray<TSharedPtr<FJsonValue>>* LowerLODs = nullptr;
		if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 1
			|| !Root->TryGetNumberField(TEXT("quantization"), Quantization) || Quantization != 2048
			|| !Root->TryGetNumberField(TEXT("lodCount"), LODCount) || LODCount != ExpectedNativeLODs
			|| !Root->TryGetArrayField(TEXT("lods"), LowerLODs) || LowerLODs->Num() != ExpectedNativeLODs - 1) { return {}; }
		FString TopologyText;
		TSharedPtr<FJsonObject> Topology;
		const TArray<TSharedPtr<FJsonValue>>* Specifications = nullptr;
		if (!FFileHelper::LoadFileToString(TopologyText, *(Plugin->GetBaseDir() / TEXT("Resources/AvatarContract/body-topology-v1.json")))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TopologyText), Topology) || !Topology
			|| !Topology->TryGetArrayField(TEXT("lods"), Specifications) || Specifications->Num() != ExpectedNativeLODs) { return {}; }
		bool bLayoutCorners = false; FString PolicyError;
		if (!ReadLowerLODPolicy(*Root, *Topology, bLayoutCorners, PolicyError)) return {};
		TArray<FMantleBodyCorrespondence> Loaded;
		for (int32 Level = 0; Level < ExpectedNativeLODs; ++Level)
		{
			const TSharedPtr<FJsonObject> Object = Level == 0 ? Root : (*LowerLODs)[Level - 1]->AsObject();
			double DeclaredLOD = -1, Vertices = 0, TriangleCount = 0;
			const TArray<TSharedPtr<FJsonValue>>* UVs = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Triangles = nullptr;
			FString Hash;
			if (!Object || !Object->TryGetNumberField(TEXT("lod"), DeclaredLOD) || DeclaredLOD != Level
				|| !Object->TryGetNumberField(TEXT("vertexCount"), Vertices) || Vertices <= 0 || Vertices > 100000
				|| !Object->TryGetNumberField(TEXT("triangleCount"), TriangleCount) || TriangleCount <= 0 || TriangleCount > 1000000
				|| !Object->TryGetArrayField(TEXT("quantizedUVs"), UVs) || UVs->Num() != Vertices
				|| !Object->TryGetArrayField(TEXT("indices"), Triangles) || Triangles->Num() != TriangleCount * 3
				|| !Object->TryGetStringField(TEXT("uvHash"), Hash) || Hash.Len() != 8
				|| (Level == 0 && (Vertices != Contract->TopologyVertexCount || Hash != UMantleBodyTopologyData::FormatHash(Contract->TopologyUVHash)))) { return {}; }
			const TSharedPtr<FJsonObject> Specification = (*Specifications)[Level]->AsObject();
			double SpecLOD = -1, SpecVertices = 0, SpecTriangles = 0;
			FString SpecHash;
			if (!Specification || !Specification->TryGetNumberField(TEXT("lod"), SpecLOD) || SpecLOD != Level
				|| !Specification->TryGetNumberField(TEXT("vertexCount"), SpecVertices) || SpecVertices != Vertices
				|| !Specification->TryGetNumberField(TEXT("triangleCount"), SpecTriangles) || SpecTriangles != TriangleCount
				|| !Specification->TryGetStringField(TEXT("uvHash"), SpecHash) || SpecHash != Hash) { return {}; }
			FMantleBodyCorrespondence Result;
			Result.bLowerCornerTopology = Level > 0 && bLayoutCorners;
			TArray<FVector2f> CheckUVs;
			for (const TSharedPtr<FJsonValue>& Value : *UVs)
			{
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				int32 U = 0, V = 0;
				if (!Value->TryGetArray(Pair) || Pair->Num() != 2 || !(*Pair)[0]->TryGetNumber(U) || !(*Pair)[1]->TryGetNumber(V)) { return {}; }
				Result.QuantizedUVs.Add(FIntPoint(U, V));
				CheckUVs.Add(FVector2f(U / 2048.0f, V / 2048.0f));
			}
			for (const TSharedPtr<FJsonValue>& Value : *Triangles)
			{
				int32 Index = INDEX_NONE;
				if (!Value->TryGetNumber(Index) || Index < 0 || Index >= CheckUVs.Num()) { return {}; }
				Result.Indices.Add(static_cast<uint32>(Index));
			}
			Result.UVHash = UMantleBodyTopologyData::ComputeUVHash(CheckUVs);
			TArray<int32> Identity;
			FString Error;
			if (UMantleBodyTopologyData::FormatHash(Result.UVHash) != Hash || !Result.Match(CheckUVs, Result.Indices, Identity, Error)) { return {}; }
			if (Result.bLowerCornerTopology && !Result.MatchCorners(CheckUVs, Result.Indices, Identity, Error)) { return {}; }
			Loaded.Add(MoveTemp(Result));
		}
		return Loaded;
	}();
	return Cached.IsSet() && Cached.GetValue().IsValidIndex(LODIndex) ? &Cached.GetValue()[LODIndex] : nullptr;
}
