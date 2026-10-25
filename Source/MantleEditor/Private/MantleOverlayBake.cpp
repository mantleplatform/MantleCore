// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOverlayBake.h"

#include "MantleCosmeticExport.h"
#include "MantleOverlayGuide.h"
#include "MantleReferenceBodies.h"

#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"

USkeletalMesh* FMantleOverlayBake::DefaultMesh(const bool bFace, const FMantleAvatarContract* Contract)
{
	if (USkeletalMesh* Template = LoadObject<USkeletalMesh>(nullptr, bFace ? FMantleOverlayGuide::TemplateFacePath : FMantleOverlayGuide::TemplateBodyPath))
	{
		return Template;
	}
	if (bFace)
	{
		return FMantleReferenceBodies::GetFace();
	}
	if (!Contract)
	{
		return nullptr;
	}
	const TArray<USkeletalMesh*> References = FMantleReferenceBodies::FindBodies(*Contract, nullptr);
	return References.IsEmpty() ? nullptr : References[0];
}

TArray<uint8> FMantleOverlayBake::PictureStrength(TConstArrayView<FColor> Pixels, const int32 Width, const int32 Height)
{
	TArray<uint8> Out;
	if (Width <= 0 || Height <= 0 || Pixels.Num() < Width * Height)
	{
		return Out;
	}
	bool bTransparent = false;
	for (const FColor& Pixel : Pixels)
	{
		if (Pixel.A != 255)
		{
			bTransparent = true;
			break;
		}
	}
	Out.SetNumUninitialized(Width * Height);
	for (int32 Index = 0; Index < Width * Height; ++Index)
	{
		const FColor& Pixel = Pixels[Index];
		Out[Index] = bTransparent ? Pixel.A : static_cast<uint8>((Pixel.R * 54 + Pixel.G * 183 + Pixel.B * 19) >> 8);
	}
	return Out;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOverlayBakePrivate
{
	bool ReadLod(const USkeletalMesh& Mesh, int32 LodIndex, TArray<FMantleOverlayBake::FTriangle>& OutTriangles, FString& OutError);
}

bool MantleOverlayBakePrivate::ReadLod(const USkeletalMesh& Mesh, const int32 LodIndex, TArray<FMantleOverlayBake::FTriangle>& OutTriangles, FString& OutError)
{
	OutTriangles.Reset();
	const FSkeletalMeshModel* Imported = Mesh.GetImportedModel();
	if (!Imported || !Imported->LODModels.IsValidIndex(LodIndex))
	{
		OutError = FString::Printf(TEXT("%s has no LOD%d to read."), *Mesh.GetName(), LodIndex);
		return false;
	}
	const FSkeletalMeshLODModel& Model = Imported->LODModels[LodIndex];
	// Only the skin sections: a face mesh also carries eyes, teeth and lashes on their own UV layouts. The role comes from the slot name as the
	// export and the worn preview read it (a built face names its skin slot something the guide's name match would not catch).
	TSet<FString> SkinSlots;
	{
		const TArray<FString> Names = FMantleCosmeticExport::SectionMaterialNames(&Mesh, LodIndex);
		const TArray<FString> Roles = FMantleCosmeticExport::DefaultSectionRoles(&Mesh, LodIndex);
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			if (Roles.IsValidIndex(Index) && Roles[Index] == TEXT("skin"))
			{
				SkinSlots.Add(Names[Index]);
			}
		}
	}
	const TArray<FSkeletalMaterial>& Slots = Mesh.GetMaterials();
	const auto IsSkin = [&Slots, &SkinSlots](const FSkelMeshSection& Section)
	{
		if (!Slots.IsValidIndex(Section.MaterialIndex)) return false;
		const FString Name = Slots[Section.MaterialIndex].MaterialSlotName.ToString();
		if (!SkinSlots.IsEmpty()) return SkinSlots.Contains(Name);
		const FString Lower = Name.ToLower();
		return Lower.Contains(TEXT("head")) || Lower.Contains(TEXT("body")) || Lower.Contains(TEXT("skin"));
	};
	const bool bAnySkin = Model.Sections.ContainsByPredicate(IsSkin);
	for (const FSkelMeshSection& Section : Model.Sections)
	{
		if (Section.bDisabled || (bAnySkin && !IsSkin(Section)))
		{
			continue;
		}
		for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
		{
			FMantleOverlayBake::FTriangle Out;
			bool bValid = true;
			for (int32 Corner = 0; Corner < 3 && bValid; ++Corner)
			{
				const int32 IndexSlot = static_cast<int32>(Section.BaseIndex + TriangleIndex * 3 + static_cast<uint32>(Corner));
				if (!Model.IndexBuffer.IsValidIndex(IndexSlot))
				{
					bValid = false;
					break;
				}
				const int32 Local = static_cast<int32>(Model.IndexBuffer[IndexSlot]) - static_cast<int32>(Section.BaseVertexIndex);
				if (!Section.SoftVertices.IsValidIndex(Local))
				{
					bValid = false;
					break;
				}
				const FSoftSkinVertex& Vertex = Section.SoftVertices[Local];
				Out.Position[Corner] = Vertex.Position;
				Out.Normal[Corner] = Vertex.TangentZ;
				Out.UV[Corner] = Vertex.UVs[0];
			}
			if (!bValid)
			{
				continue;
			}
			// A layout in a UDIM tile other than 0..1 (the MetaHuman body's is) samples the same texture: fold it back.
			const FVector2f Centre = (Out.UV[0] + Out.UV[1] + Out.UV[2]) / 3.f;
			const FVector2f Tile(FMath::FloorToFloat(Centre.X), FMath::FloorToFloat(Centre.Y));
			for (FVector2f& UV : Out.UV)
			{
				UV -= Tile;
			}
			OutTriangles.Add(Out);
		}
	}
	if (OutTriangles.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s has no skin triangles in LOD%d the editor can read."), *Mesh.GetName(), LodIndex);
		return false;
	}
	return true;
}

bool FMantleOverlayBake::ReadMesh(const USkeletalMesh& Mesh, TArray<FTriangle>& OutTriangles, FString& OutError)
{
	// The LOD with the most skin: LOD0 for a body. A built face can carry its fine skin on a later LOD (its LOD0 holds only a low-detail strip),
	// and every LOD shares the one UV layout.
	OutTriangles.Reset();
	const FSkeletalMeshModel* Imported = Mesh.GetImportedModel();
	const int32 LodCount = Imported ? Imported->LODModels.Num() : 0;
	if (LodCount == 0)
	{
		OutError = FString::Printf(TEXT("%s has no LOD0 to read."), *Mesh.GetName());
		return false;
	}
	FString FirstError;
	for (int32 LodIndex = 0; LodIndex < LodCount; ++LodIndex)
	{
		TArray<FTriangle> Lod;
		FString Error;
		if (!MantleOverlayBakePrivate::ReadLod(Mesh, LodIndex, Lod, Error))
		{
			if (FirstError.IsEmpty()) FirstError = Error;
			continue;
		}
		if (Lod.Num() > OutTriangles.Num())
		{
			OutTriangles = MoveTemp(Lod);
		}
	}
	if (OutTriangles.IsEmpty())
	{
		OutError = FirstError;
		return false;
	}
	return true;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOverlayBakePrivate
{
	float SampleStrength(TConstArrayView<uint8> Strength, const int32 Width, const int32 Height, const float U, const float V)
	{
		const float X = FMath::Clamp(U * Width - 0.5f, 0.f, static_cast<float>(Width - 1));
		const float Y = FMath::Clamp(V * Height - 0.5f, 0.f, static_cast<float>(Height - 1));
		const int32 X0 = FMath::FloorToInt(X), Y0 = FMath::FloorToInt(Y);
		const int32 X1 = FMath::Min(X0 + 1, Width - 1), Y1 = FMath::Min(Y0 + 1, Height - 1);
		const float FX = X - X0, FY = Y - Y0;
		const float Top = FMath::Lerp<float>(Strength[Y0 * Width + X0], Strength[Y0 * Width + X1], FX);
		const float Bottom = FMath::Lerp<float>(Strength[Y1 * Width + X0], Strength[Y1 * Width + X1], FX);
		return FMath::Lerp(Top, Bottom, FY) / 255.f;
	}
}

int32 FMantleOverlayBake::Bake(TConstArrayView<FTriangle> Triangles, TConstArrayView<uint8> Strength, const int32 Width, const int32 Height,
	const FMantleOverlayPlacement& Placement, TArray<uint8>& OutMask, TArray<FColor>* OutNormal)
{
	using namespace MantleOverlayBakePrivate;
	OutMask.Init(0, Size * Size);
	if (Width <= 0 || Height <= 0 || Strength.Num() < Width * Height)
	{
		return 0;
	}
	TArray<bool> Covered;
	Covered.Init(false, Size * Size);

	const FTransform ToProjector = Placement.Transform.Inverse();
	const double WidthCm = FMath::Max(0.01, static_cast<double>(Placement.WidthCm));
	const double HeightCm = WidthCm * Height / Width;
	const double Depth = FMath::Max(0.01, static_cast<double>(Placement.DepthCm));
	const double Solid = Depth * (1.0 - FMath::Clamp(static_cast<double>(Placement.Falloff), 0.0, 1.0));
	const FVector Look = Placement.Transform.GetRotation().GetForwardVector();

	for (const FTriangle& Triangle : Triangles)
	{
		FVector2f P[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			P[Corner] = Triangle.UV[Corner] * static_cast<float>(Size);
		}
		const float Area = (P[1].X - P[0].X) * (P[2].Y - P[0].Y) - (P[2].X - P[0].X) * (P[1].Y - P[0].Y);
		if (FMath::Abs(Area) < 1e-6f)
		{
			continue;
		}
		const int32 MinX = FMath::Max(0, FMath::FloorToInt(FMath::Min3(P[0].X, P[1].X, P[2].X)));
		const int32 MaxX = FMath::Min(Size - 1, FMath::CeilToInt(FMath::Max3(P[0].X, P[1].X, P[2].X)));
		const int32 MinY = FMath::Max(0, FMath::FloorToInt(FMath::Min3(P[0].Y, P[1].Y, P[2].Y)));
		const int32 MaxY = FMath::Min(Size - 1, FMath::CeilToInt(FMath::Max3(P[0].Y, P[1].Y, P[2].Y)));
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FVector2f C(X + 0.5f, Y + 0.5f);
				const float W0 = ((P[1].X - C.X) * (P[2].Y - C.Y) - (P[2].X - C.X) * (P[1].Y - C.Y)) / Area;
				const float W1 = ((P[2].X - C.X) * (P[0].Y - C.Y) - (P[0].X - C.X) * (P[2].Y - C.Y)) / Area;
				const float W2 = 1.f - W0 - W1;
				constexpr float Eps = -0.001f;
				if (W0 < Eps || W1 < Eps || W2 < Eps)
				{
					continue;
				}
				const int32 Texel = Y * Size + X;
				Covered[Texel] = true;
				const FVector Position(Triangle.Position[0] * W0 + Triangle.Position[1] * W1 + Triangle.Position[2] * W2);
				const FVector Normal = FVector(Triangle.Normal[0] * W0 + Triangle.Normal[1] * W1 + Triangle.Normal[2] * W2).GetSafeNormal();
				const FVector Local = ToProjector.TransformPosition(Position);
				const double U = Local.Y / WidthCm + 0.5;
				const double V = 0.5 - Local.Z / HeightCm;
				if (U < 0.0 || U > 1.0 || V < 0.0 || V > 1.0)
				{
					continue;
				}
				// Depth: solid within Solid, fading to nothing at Depth. Facing: nothing edge-on or away, full from about 60 degrees.
				const double Along = FMath::Abs(Local.X);
				if (Along >= Depth)
				{
					continue;
				}
				const double DepthFade = Along <= Solid ? 1.0 : 1.0 - (Along - Solid) / (Depth - Solid);
				const double Facing = FMath::SmoothStep(0.05, 0.5, -Normal.Dot(Look));
				const float Value = SampleStrength(Strength, Width, Height, static_cast<float>(U), static_cast<float>(V))
					* static_cast<float>(DepthFade * Facing);
				const uint8 Byte = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 255.f), 0, 255));
				OutMask[Texel] = FMath::Max(OutMask[Texel], Byte);
			}
		}
	}

	// Pad every island's border outward so bilinear sampling across a seam shows no line.
	for (int32 Pass = 0; Pass < Padding; ++Pass)
	{
		TArray<uint8> NextMask = OutMask;
		TArray<bool> NextCovered = Covered;
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const int32 Texel = Y * Size + X;
				if (Covered[Texel])
				{
					continue;
				}
				uint8 Best = 0;
				bool bAny = false;
				for (int32 DY = -1; DY <= 1; ++DY)
				{
					for (int32 DX = -1; DX <= 1; ++DX)
					{
						const int32 NX = X + DX, NY = Y + DY;
						if ((DX || DY) && NX >= 0 && NX < Size && NY >= 0 && NY < Size && Covered[NY * Size + NX])
						{
							Best = FMath::Max(Best, OutMask[NY * Size + NX]);
							bAny = true;
						}
					}
				}
				if (bAny)
				{
					NextMask[Texel] = Best;
					NextCovered[Texel] = true;
				}
			}
		}
		OutMask = MoveTemp(NextMask);
		Covered = MoveTemp(NextCovered);
	}

	int32 Painted = 0;
	for (const uint8 Value : OutMask)
	{
		Painted += Value > 0 ? 1 : 0;
	}

	if (OutNormal)
	{
		OutNormal->SetNumUninitialized(Size * Size);
		const auto Height01 = [&OutMask](const int32 X, const int32 Y)
		{
			return OutMask[FMath::Clamp(Y, 0, Size - 1) * Size + FMath::Clamp(X, 0, Size - 1)] / 255.f;
		};
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const float DX = (Height01(X + 1, Y) - Height01(X - 1, Y)) * 0.5f;
				// Unreal reads green as +V, which runs down the image: a raised edge tilts its normal toward the lower height, so green falls where the height rises downward.
				const float DY = (Height01(X, Y + 1) - Height01(X, Y - 1)) * 0.5f;
				const FVector3f N = FVector3f(-DX * Placement.Relief * 4.f, -DY * Placement.Relief * 4.f, 1.f).GetSafeNormal();
				(*OutNormal)[Y * Size + X] = FColor(
					static_cast<uint8>(FMath::RoundToInt((N.X * 0.5f + 0.5f) * 255.f)),
					static_cast<uint8>(FMath::RoundToInt((N.Y * 0.5f + 0.5f) * 255.f)),
					static_cast<uint8>(FMath::RoundToInt((N.Z * 0.5f + 0.5f) * 255.f)), 255);
			}
		}
	}
	return Painted;
}

TArray<FColor> FMantleOverlayBake::MaskToPixels(TConstArrayView<uint8> Mask)
{
	TArray<FColor> Out;
	Out.SetNumUninitialized(Mask.Num());
	for (int32 Index = 0; Index < Mask.Num(); ++Index)
	{
		Out[Index] = FColor(Mask[Index], Mask[Index], Mask[Index], 255);
	}
	return Out;
}
