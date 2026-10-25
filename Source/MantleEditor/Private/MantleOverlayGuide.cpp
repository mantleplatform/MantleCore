// Copyright Joshua Eger. All Rights Reserved.
#include "MantleOverlayGuide.h"

#include "MantleAvatarContract.h"
#include "MantleOverlayChecks.h"
#include "MantlePng.h"
#include "MantleReferenceBodies.h"

#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOverlayGuidePrivate
{
	using ERegion = FMantleOverlayGuide::ERegion;
	constexpr int32 RegionCount = static_cast<int32>(ERegion::Count);

	/**
	 * The bone-name table: the first row whose needle is in the (lower-case) bone name decides the region. Left and
	 * Right are for bones whose name ends _l / _r; an unsided bone takes Left.
	 */
	struct FBoneRule
	{
		const TCHAR* Needle;
		ERegion Left;
		ERegion Right;
	};

	const FBoneRule BoneRules[] = {
		{ TEXT("thumb"),    ERegion::HandL, ERegion::HandR },
		{ TEXT("index"),    ERegion::HandL, ERegion::HandR },
		{ TEXT("middle"),   ERegion::HandL, ERegion::HandR },
		{ TEXT("ring"),     ERegion::HandL, ERegion::HandR },
		{ TEXT("pinky"),    ERegion::HandL, ERegion::HandR },
		{ TEXT("hand"),     ERegion::HandL, ERegion::HandR },
		{ TEXT("lowerarm"), ERegion::ArmL,  ERegion::ArmR  },
		{ TEXT("upperarm"), ERegion::ArmL,  ERegion::ArmR  },
		{ TEXT("clavicle"), ERegion::ArmL,  ERegion::ArmR  },
		{ TEXT("shoulder"), ERegion::ArmL,  ERegion::ArmR  },
		{ TEXT("foot"),     ERegion::FootL, ERegion::FootR },
		{ TEXT("ball"),     ERegion::FootL, ERegion::FootR },
		{ TEXT("toe"),      ERegion::FootL, ERegion::FootR },
		{ TEXT("thigh"),    ERegion::LegL,  ERegion::LegR  },
		{ TEXT("calf"),     ERegion::LegL,  ERegion::LegR  },
		{ TEXT("knee"),     ERegion::LegL,  ERegion::LegR  },
		{ TEXT("neck"),     ERegion::Neck,  ERegion::Neck  },
		{ TEXT("head"),     ERegion::Head,  ERegion::Head  },
		{ TEXT("spine"),    ERegion::Chest, ERegion::Chest },
		{ TEXT("pelvis"),   ERegion::Chest, ERegion::Chest },
	};

	/** One font pixel grid per glyph: 5 wide, 7 tall, bit 4 is the left column. Index 0..25 = A..Z, 26 = '&'. */
	const uint8 Glyphs[27][7] = {
		{ 0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 }, // A
		{ 0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110 }, // B
		{ 0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110 }, // C
		{ 0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110 }, // D
		{ 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111 }, // E
		{ 0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000 }, // F
		{ 0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111 }, // G
		{ 0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001 }, // H
		{ 0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110 }, // I
		{ 0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100 }, // J
		{ 0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001 }, // K
		{ 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111 }, // L
		{ 0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001 }, // M
		{ 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001 }, // N
		{ 0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 }, // O
		{ 0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000 }, // P
		{ 0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101 }, // Q
		{ 0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001 }, // R
		{ 0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110 }, // S
		{ 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100 }, // T
		{ 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110 }, // U
		{ 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100 }, // V
		{ 0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010 }, // W
		{ 0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001 }, // X
		{ 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100 }, // Y
		{ 0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111 }, // Z
		{ 0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101 }, // &
	};

	constexpr int32 TextScale = 5; // A glyph is 25 x 35 px at 2048.

	FColor GuideBackground()
	{
		return FColor(38, 38, 40, 255);
	}

	FColor RegionColor(const ERegion Region)
	{
		switch (Region)
		{
		case ERegion::Head:  return FColor(232, 160, 160, 255);
		case ERegion::Neck:  return FColor(232, 196, 150, 255);
		case ERegion::Chest: return FColor(160, 205, 232, 255);
		case ERegion::Back:  return FColor(150, 170, 232, 255);
		case ERegion::ArmL:  return FColor(160, 225, 170, 255);
		case ERegion::ArmR:  return FColor(190, 232, 150, 255);
		case ERegion::HandL: return FColor(120, 200, 190, 255);
		case ERegion::HandR: return FColor(150, 215, 130, 255);
		case ERegion::LegL:  return FColor(215, 170, 232, 255);
		case ERegion::LegR:  return FColor(232, 170, 215, 255);
		case ERegion::FootL: return FColor(190, 150, 210, 255);
		case ERegion::FootR: return FColor(215, 140, 180, 255);
		case ERegion::Face:  return FColor(232, 180, 165, 255);
		case ERegion::Scalp: return FColor(200, 185, 150, 255);
		case ERegion::Ears:  return FColor(232, 215, 140, 255);
		default:             return FColor(185, 185, 185, 255);
		}
	}

	uint8 GuideLerpChannel(const uint8 From, const uint8 To, const float Alpha)
	{
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(From) + (static_cast<float>(To) - static_cast<float>(From)) * Alpha), 0, 255));
	}

	void GuideBlendPixel(TArray<FColor>& Pixels, const int32 X, const int32 Y, const FColor Over, const float Alpha)
	{
		if (X < 0 || Y < 0 || X >= FMantleOverlayGuide::Size || Y >= FMantleOverlayGuide::Size)
		{
			return;
		}
		FColor& Pixel = Pixels[Y * FMantleOverlayGuide::Size + X];
		Pixel.R = GuideLerpChannel(Pixel.R, Over.R, Alpha);
		Pixel.G = GuideLerpChannel(Pixel.G, Over.G, Alpha);
		Pixel.B = GuideLerpChannel(Pixel.B, Over.B, Alpha);
		Pixel.A = 255;
	}

	void GuideDrawLine(TArray<FColor>& Pixels, const FVector2f& A, const FVector2f& B, const FColor Color, const float Alpha)
	{
		const float Length = FMath::Max(FMath::Abs(B.X - A.X), FMath::Abs(B.Y - A.Y));
		const int32 Steps = FMath::Max(1, FMath::CeilToInt(Length));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const float T = static_cast<float>(Step) / static_cast<float>(Steps);
			GuideBlendPixel(Pixels, FMath::FloorToInt(A.X + (B.X - A.X) * T), FMath::FloorToInt(A.Y + (B.Y - A.Y) * T), Color, Alpha);
		}
	}

	void GuideFillRect(TArray<FColor>& Pixels, const int32 X0, const int32 Y0, const int32 W, const int32 H, const FColor Color)
	{
		for (int32 Y = Y0; Y < Y0 + H; ++Y)
		{
			for (int32 X = X0; X < X0 + W; ++X)
			{
				GuideBlendPixel(Pixels, X, Y, Color, 1.f);
			}
		}
	}

	int32 GuideGlyphIndex(const TCHAR Character)
	{
		if (Character >= TEXT('A') && Character <= TEXT('Z'))
		{
			return static_cast<int32>(Character - TEXT('A'));
		}
		return Character == TEXT('&') ? 26 : INDEX_NONE;
	}

	void GuideDrawGlyphs(TArray<FColor>& Pixels, const FString& Text, const int32 Left, const int32 Top, const int32 OffsetX, const int32 OffsetY, const FColor Color)
	{
		int32 PenX = Left;
		for (int32 Letter = 0; Letter < Text.Len(); ++Letter)
		{
			const int32 Glyph = GuideGlyphIndex(Text[Letter]);
			if (Glyph != INDEX_NONE)
			{
				for (int32 Row = 0; Row < 7; ++Row)
				{
					for (int32 Column = 0; Column < 5; ++Column)
					{
						if (Glyphs[Glyph][Row] & (1 << (4 - Column)))
						{
							GuideFillRect(Pixels, PenX + Column * TextScale + OffsetX, Top + Row * TextScale + OffsetY, TextScale, TextScale, Color);
						}
					}
				}
			}
			PenX += 6 * TextScale;
		}
	}

	/** White outline then black text, centred on Centre. */
	void GuideDrawLabel(TArray<FColor>& Pixels, const FString& Name, const FVector2f& Centre)
	{
		const FString Text = Name.ToUpper();
		const int32 Width = Text.Len() * 6 * TextScale - TextScale;
		const int32 Height = 7 * TextScale;
		const int32 Left = FMath::Clamp(FMath::RoundToInt(Centre.X) - Width / 2, 4, FMantleOverlayGuide::Size - Width - 4);
		const int32 Top = FMath::Clamp(FMath::RoundToInt(Centre.Y) - Height / 2, 4, FMantleOverlayGuide::Size - Height - 4);
		for (int32 OffsetY = -2; OffsetY <= 2; OffsetY += 2)
		{
			for (int32 OffsetX = -2; OffsetX <= 2; OffsetX += 2)
			{
				if (OffsetX != 0 || OffsetY != 0)
				{
					GuideDrawGlyphs(Pixels, Text, Left, Top, OffsetX, OffsetY, FColor(255, 255, 255, 255));
				}
			}
		}
		GuideDrawGlyphs(Pixels, Text, Left, Top, 0, 0, FColor(10, 10, 10, 255));
	}

	/** A pixel-space point at 1/16 px, so equal corners compare equal. */
	struct FQuantised
	{
		int32 X = 0;
		int32 Y = 0;
	};

	FQuantised GuideQuantise(const FVector2f& Pixel)
	{
		FQuantised Result;
		Result.X = FMath::RoundToInt(Pixel.X * 16.f);
		Result.Y = FMath::RoundToInt(Pixel.Y * 16.f);
		return Result;
	}

	/** An undirected edge's key: its two corners, ordered. */
	uint64 GuideEdgeKeyPart(const FQuantised& Point)
	{
		return (static_cast<uint64>(static_cast<uint32>(Point.X)) << 32) | static_cast<uint64>(static_cast<uint32>(Point.Y));
	}

	struct FGuideEdge
	{
		FVector2f A = FVector2f::ZeroVector;
		FVector2f B = FVector2f::ZeroVector;
		int32 Count = 0;
	};

	int32 GuideFindRoot(TArray<int32>& Parent, int32 Index)
	{
		while (Parent[Index] != Index)
		{
			Parent[Index] = Parent[Parent[Index]];
			Index = Parent[Index];
		}
		return Index;
	}
}

FString FMantleOverlayGuide::RegionName(const ERegion Region)
{
	switch (Region)
	{
	case ERegion::Head:  return TEXT("Head");
	case ERegion::Neck:  return TEXT("Neck");
	case ERegion::Chest: return TEXT("Chest & belly");
	case ERegion::Back:  return TEXT("Back");
	case ERegion::ArmL:  return TEXT("Left arm");
	case ERegion::ArmR:  return TEXT("Right arm");
	case ERegion::HandL: return TEXT("Left hand");
	case ERegion::HandR: return TEXT("Right hand");
	case ERegion::LegL:  return TEXT("Left leg");
	case ERegion::LegR:  return TEXT("Right leg");
	case ERegion::FootL: return TEXT("Left foot");
	case ERegion::FootR: return TEXT("Right foot");
	case ERegion::Face:  return TEXT("Face");
	case ERegion::Scalp: return TEXT("Scalp");
	case ERegion::Ears:  return TEXT("Ears");
	default:             return TEXT("Other");
	}
}

FMantleOverlayGuide::ERegion FMantleOverlayGuide::ClassifyBone(const FString& BoneName, const bool bFace)
{
	using namespace MantleOverlayGuidePrivate;
	const FString Name = BoneName.ToLower();
	if (Name.StartsWith(TEXT("facial")))
	{
		if (bFace && Name.Contains(TEXT("ear")))
		{
			return ERegion::Ears;
		}
		return bFace ? ERegion::Face : ERegion::Head;
	}
	if (bFace && Name.Contains(TEXT("ear")))
	{
		return ERegion::Ears;
	}
	const bool bRight = Name.EndsWith(TEXT("_r")) || Name.Contains(TEXT("_r_"));
	for (const FBoneRule& Rule : BoneRules)
	{
		if (!Name.Contains(Rule.Needle))
		{
			continue;
		}
		const ERegion Region = bRight ? Rule.Right : Rule.Left;
		if (bFace)
		{
			// The face mesh has no limbs: its head bone carries the scalp, and the torso bones only reach the neck.
			if (Region == ERegion::Head)
			{
				return ERegion::Scalp;
			}
			if (Region == ERegion::Chest || Region == ERegion::Neck)
			{
				return ERegion::Neck;
			}
			return ERegion::Other;
		}
		return Region;
	}
	return ERegion::Other;
}

bool FMantleOverlayGuide::ReadMesh(const USkeletalMesh& Mesh, const bool bFace, TArray<FTriangle>& OutTriangles, FString& OutError)
{
	using namespace MantleOverlayGuidePrivate;
	OutTriangles.Reset();
	const FSkeletalMeshModel* Imported = Mesh.GetImportedModel();
	if (!Imported || !Imported->LODModels.IsValidIndex(0))
	{
		OutError = FString::Printf(TEXT("%s has no LOD0 to read."), *Mesh.GetName());
		return false;
	}
	const FSkeletalMeshLODModel& Model = Imported->LODModels[0];
	const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();

	TArray<ERegion> BoneRegion;
	for (int32 BoneIndex = 0; BoneIndex < Reference.GetRawBoneNum(); ++BoneIndex)
	{
		BoneRegion.Add(ClassifyBone(Reference.GetBoneName(BoneIndex).ToString(), bFace));
	}

	// The body's forward: the direction from the foot to the ball of the foot in the reference pose, snapped to an axis.
	FVector Forward(0.0, 1.0, 0.0);
	{
		const TArray<FTransform>& Pose = Reference.GetRawRefBonePose();
		const auto ComponentLocation = [&Reference, &Pose](const int32 StartBone)
		{
			FTransform Result = FTransform::Identity;
			for (int32 Walk = StartBone; Walk != INDEX_NONE && Pose.IsValidIndex(Walk); Walk = Reference.GetParentIndex(Walk))
			{
				Result = Result * Pose[Walk];
			}
			return Result.GetLocation();
		};
		const int32 Foot = Reference.FindBoneIndex(FName(TEXT("foot_l")));
		const int32 Ball = Reference.FindBoneIndex(FName(TEXT("ball_l")));
		if (Foot != INDEX_NONE && Ball != INDEX_NONE)
		{
			const FVector Direction = ComponentLocation(Ball) - ComponentLocation(Foot);
			if (FMath::Abs(Direction.X) > UE_KINDA_SMALL_NUMBER || FMath::Abs(Direction.Y) > UE_KINDA_SMALL_NUMBER)
			{
				Forward = FMath::Abs(Direction.X) > FMath::Abs(Direction.Y)
					? FVector(Direction.X >= 0.0 ? 1.0 : -1.0, 0.0, 0.0)
					: FVector(0.0, Direction.Y >= 0.0 ? 1.0 : -1.0, 0.0);
			}
		}
	}

	// Only the skin sections: the face mesh also carries eyes, teeth, lashes and the like on their own UV layouts.
	const TArray<FSkeletalMaterial>& Slots = Mesh.GetMaterials();
	const auto IsSkin = [&Slots](const FSkelMeshSection& Section)
	{
		if (!Slots.IsValidIndex(Section.MaterialIndex)) return false;
		const FString Name = Slots[Section.MaterialIndex].MaterialSlotName.ToString().ToLower();
		return Name.Contains(TEXT("head")) || Name.Contains(TEXT("body")) || Name.Contains(TEXT("skin"));
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
			FTriangle Out;
			float Weights[RegionCount] = {};
			FVector3f Normal = FVector3f::ZeroVector;
			bool bValid = true;
			for (int32 Corner = 0; Corner < 3; ++Corner)
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
				Out.UV[Corner] = Vertex.UVs[0];
				Normal += Vertex.TangentZ;
				for (int32 Influence = 0; Influence < MAX_TOTAL_INFLUENCES; ++Influence)
				{
					const uint32 Weight = Vertex.InfluenceWeights[Influence];
					if (Weight == 0)
					{
						continue;
					}
					const int32 MapIndex = static_cast<int32>(Vertex.InfluenceBones[Influence]);
					if (!Section.BoneMap.IsValidIndex(MapIndex))
					{
						continue;
					}
					const int32 MeshBone = static_cast<int32>(Section.BoneMap[MapIndex]);
					if (BoneRegion.IsValidIndex(MeshBone))
					{
						Weights[static_cast<int32>(BoneRegion[MeshBone])] += static_cast<float>(Weight);
					}
				}
			}
			if (!bValid)
			{
				continue;
			}
			// A layout laid out in a UDIM tile other than 0..1 (MetaHuman's body is) samples the same texture: fold the
			// triangle back by its tile.
			const FVector2f Centre = (Out.UV[0] + Out.UV[1] + Out.UV[2]) / 3.f;
			const FVector2f Tile(FMath::FloorToFloat(Centre.X), FMath::FloorToFloat(Centre.Y));
			for (FVector2f& UV : Out.UV)
			{
				UV -= Tile;
			}
			// The region with the most skin weight; no weight falls to Other.
			int32 Best = static_cast<int32>(ERegion::Other);
			float BestWeight = 0.f;
			for (int32 Region = 0; Region < RegionCount; ++Region)
			{
				if (Region != static_cast<int32>(ERegion::Other) && Weights[Region] > BestWeight)
				{
					BestWeight = Weights[Region];
					Best = Region;
				}
			}
			Out.Region = static_cast<ERegion>(Best);
			if (Out.Region == ERegion::Chest && !bFace && FVector(Normal).Dot(Forward) < 0.0)
			{
				Out.Region = ERegion::Back;
			}
			OutTriangles.Add(Out);
		}
	}
	if (OutTriangles.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s has no triangles in LOD0 the editor can read."), *Mesh.GetName());
		return false;
	}
	return true;
}

void FMantleOverlayGuide::Render(TConstArrayView<FTriangle> Triangles, TArray<FColor>& OutPixels, TArray<FString>* OutRegionsDrawn)
{
	using namespace MantleOverlayGuidePrivate;
	OutPixels.Init(GuideBackground(), Size * Size);
	if (OutRegionsDrawn)
	{
		OutRegionsDrawn->Reset();
	}

	// Triangles in pixel space.
	TArray<FVector2f> Corners;
	Corners.SetNumUninitialized(Triangles.Num() * 3);
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Corners[Index * 3 + Corner] = Triangles[Index].UV[Corner] * static_cast<float>(Size);
		}
	}

	// 1. Fill.
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		const FVector2f P0 = Corners[Index * 3];
		const FVector2f P1 = Corners[Index * 3 + 1];
		const FVector2f P2 = Corners[Index * 3 + 2];
		const float Det = (P1.X - P0.X) * (P2.Y - P0.Y) - (P1.Y - P0.Y) * (P2.X - P0.X);
		if (FMath::Abs(Det) < 1e-6f)
		{
			continue;
		}
		const int32 MinX = FMath::Max(0, FMath::FloorToInt(FMath::Min3(P0.X, P1.X, P2.X)));
		const int32 MaxX = FMath::Min(Size - 1, FMath::CeilToInt(FMath::Max3(P0.X, P1.X, P2.X)));
		const int32 MinY = FMath::Max(0, FMath::FloorToInt(FMath::Min3(P0.Y, P1.Y, P2.Y)));
		const int32 MaxY = FMath::Min(Size - 1, FMath::CeilToInt(FMath::Max3(P0.Y, P1.Y, P2.Y)));
		const FColor Color = RegionColor(Triangles[Index].Region);
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const float PX = static_cast<float>(X) + 0.5f - P0.X;
				const float PY = static_cast<float>(Y) + 0.5f - P0.Y;
				const float L1 = (PX * (P2.Y - P0.Y) - PY * (P2.X - P0.X)) / Det;
				const float L2 = ((P1.X - P0.X) * PY - (P1.Y - P0.Y) * PX) / Det;
				const float L0 = 1.f - L1 - L2;
				constexpr float Slack = -1e-4f;
				if (L0 >= Slack && L1 >= Slack && L2 >= Slack)
				{
					OutPixels[Y * Size + X] = Color;
				}
			}
		}
	}

	// 2. Edges: every distinct edge once; an edge only one triangle uses is an island border.
	TMap<TPair<uint64, uint64>, FGuideEdge> Edges;
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		for (int32 Side = 0; Side < 3; ++Side)
		{
			const FVector2f A = Corners[Index * 3 + Side];
			const FVector2f B = Corners[Index * 3 + (Side + 1) % 3];
			const FQuantised QA = GuideQuantise(A);
			const FQuantised QB = GuideQuantise(B);
			const uint64 KeyA = GuideEdgeKeyPart(QA);
			const uint64 KeyB = GuideEdgeKeyPart(QB);
			FGuideEdge& Edge = Edges.FindOrAdd(KeyA <= KeyB ? TPair<uint64, uint64>(KeyA, KeyB) : TPair<uint64, uint64>(KeyB, KeyA));
			if (Edge.Count == 0)
			{
				Edge.A = A;
				Edge.B = B;
			}
			++Edge.Count;
		}
	}
	for (const TPair<TPair<uint64, uint64>, FGuideEdge>& Entry : Edges)
	{
		if (Entry.Value.Count > 1)
		{
			GuideDrawLine(OutPixels, Entry.Value.A, Entry.Value.B, FColor(255, 255, 255, 255), 0.16f);
		}
	}
	for (const TPair<TPair<uint64, uint64>, FGuideEdge>& Entry : Edges)
	{
		if (Entry.Value.Count == 1)
		{
			GuideDrawLine(OutPixels, Entry.Value.A, Entry.Value.B, FColor(250, 250, 250, 255), 0.9f);
		}
	}

	// 3. Islands per region (triangles of one region that share a corner); the largest by UV area gets the label.
	TArray<int32> Parent;
	Parent.SetNumUninitialized(Triangles.Num());
	TArray<float> Area;
	Area.SetNumZeroed(Triangles.Num());
	TArray<FVector2f> Centre;
	Centre.SetNumZeroed(Triangles.Num());
	TMap<TPair<uint64, int32>, int32> CornerOwner;
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		Parent[Index] = Index;
		const FVector2f P0 = Corners[Index * 3];
		const FVector2f P1 = Corners[Index * 3 + 1];
		const FVector2f P2 = Corners[Index * 3 + 2];
		Area[Index] = 0.5f * FMath::Abs((P1.X - P0.X) * (P2.Y - P0.Y) - (P1.Y - P0.Y) * (P2.X - P0.X));
		Centre[Index] = (P0 + P1 + P2) / 3.f;
	}
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const TPair<uint64, int32> Key(GuideEdgeKeyPart(GuideQuantise(Corners[Index * 3 + Corner])), static_cast<int32>(Triangles[Index].Region));
			if (const int32* Owner = CornerOwner.Find(Key))
			{
				const int32 RootA = GuideFindRoot(Parent, Index);
				const int32 RootB = GuideFindRoot(Parent, *Owner);
				if (RootA != RootB)
				{
					Parent[RootA] = RootB;
				}
			}
			else
			{
				CornerOwner.Add(Key, Index);
			}
		}
	}
	TMap<int32, float> IslandArea;
	TMap<int32, FVector2f> IslandSum;
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		const int32 Root = GuideFindRoot(Parent, Index);
		IslandArea.FindOrAdd(Root) += Area[Index];
		IslandSum.FindOrAdd(Root, FVector2f::ZeroVector) += Centre[Index] * Area[Index];
	}
	bool bRegionSeen[RegionCount] = {};
	int32 BestRoot[RegionCount];
	float BestArea[RegionCount];
	for (int32 Region = 0; Region < RegionCount; ++Region)
	{
		BestRoot[Region] = INDEX_NONE;
		BestArea[Region] = -1.f;
	}
	for (const TPair<int32, float>& Island : IslandArea)
	{
		const int32 Region = static_cast<int32>(Triangles[Island.Key].Region);
		bRegionSeen[Region] = true;
		// Larger area wins; equal areas go to the lower root so the choice does not depend on map order.
		if (Island.Value > BestArea[Region] || (Island.Value == BestArea[Region] && Island.Key < BestRoot[Region]))
		{
			BestArea[Region] = Island.Value;
			BestRoot[Region] = Island.Key;
		}
	}
	TArray<FBox2f> Placed;
	for (int32 Region = 0; Region < RegionCount; ++Region)
	{
		if (!bRegionSeen[Region])
		{
			continue;
		}
		if (OutRegionsDrawn)
		{
			OutRegionsDrawn->Add(RegionName(static_cast<ERegion>(Region)));
		}
		// The area-weighted centroid, moved to the nearest triangle of the island so the label sits on the island.
		const float TotalArea = FMath::Max(IslandArea.FindChecked(BestRoot[Region]), 1e-6f);
		const FVector2f Mean = IslandSum.FindChecked(BestRoot[Region]) / TotalArea;
		FVector2f At = Mean;
		float BestDistance = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < Triangles.Num(); ++Index)
		{
			if (static_cast<int32>(Triangles[Index].Region) == Region && GuideFindRoot(Parent, Index) == BestRoot[Region])
			{
				const float Distance = FVector2f::DistSquared(Centre[Index], Mean);
				if (Distance < BestDistance)
				{
					BestDistance = Distance;
					At = Centre[Index];
				}
			}
		}
		// Labels never overlap: a label that would sit on an earlier one steps down, then up, a line at a time.
		const FString Label = RegionName(static_cast<ERegion>(Region));
		// Pixel space, like GuideDrawLabel, with a margin for the outline.
		const FVector2f Half(Label.Len() * 3.f * TextScale + 8.f, 3.5f * TextScale + 8.f);
		const FVector2f Origin = At;
		for (int32 Step = 1; Step < 12; ++Step)
		{
			const bool bClear = !Placed.ContainsByPredicate([&](const FBox2f& Other)
			{
				return Other.Intersect(FBox2f(At - Half, At + Half));
			});
			if (bClear) break;
			At = Origin + FVector2f(0.f, (Step % 2 ? 1.f : -1.f) * ((Step + 1) / 2) * 2.f * Half.Y);
		}
		Placed.Add(FBox2f(At - Half, At + Half));
		GuideDrawLabel(OutPixels, Label, At);
	}
}

FString FMantleOverlayGuide::ShippedPath(const bool bFace)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	return Plugin.IsValid()
		? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("OverlayGuides"), bFace ? TEXT("MantleGuide_Face.png") : TEXT("MantleGuide_Body.png"))
		: FString();
}

bool FMantleOverlayGuide::Build(const bool bFace, TArray<FColor>& OutPixels, FString& OutError, TArray<FString>* OutRegionsDrawn)
{
	OutPixels.Reset();
	// MetaHuman Creator's own template meshes carry exactly the UV layout every converted avatar keeps; Mantle's
	// reference body and face (built once per project with Epic's cloud) are the fallback.
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, bFace ? TemplateFacePath : TemplateBodyPath);
	if (!Mesh && bFace)
	{
		FString Note;
		Mesh = FMantleReferenceBodies::GetFace(&Note);
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("Neither MetaHuman Creator's template face nor Mantle's reference face is available. %s"), *Note);
			return false;
		}
	}
	else if (!Mesh)
	{
		FString ContractError;
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
		if (!Contract)
		{
			OutError = ContractError;
			return false;
		}
		FString WhyNot;
		const TArray<USkeletalMesh*> Bodies = FMantleReferenceBodies::FindBodies(*Contract, &WhyNot);
		Mesh = Bodies.IsEmpty() ? nullptr : Bodies[0];
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("Neither MetaHuman Creator's template body nor Mantle's reference body is available. %s"), *WhyNot);
			return false;
		}
	}
	TArray<FTriangle> Triangles;
	if (!ReadMesh(*Mesh, bFace, Triangles, OutError))
	{
		return false;
	}
	Render(Triangles, OutPixels, OutRegionsDrawn);
	return true;
}

bool FMantleOverlayGuide::Write(const bool bFace, const FString& Path, FString& OutError, const bool bRebuild)
{
	if (Path.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("Choose where to save the guide.");
		return false;
	}
	// The plugin ships both guides (generated by Build), so a creator needs no MetaHuman content to save one.
	TArray<uint8> Png;
	const FString Shipped = ShippedPath(bFace);
	if (bRebuild || Shipped.IsEmpty() || !FFileHelper::LoadFileToArray(Png, *Shipped))
	{
		TArray<FColor> Pixels;
		if (!Build(bFace, Pixels, OutError))
		{
			return false;
		}
		Png = FMantlePng::EncodeRGBA8(Size, Size, Pixels);
	}
	if (Png.IsEmpty())
	{
		OutError = TEXT("The guide image could not be encoded.");
		return false;
	}
	FString File = FPaths::ConvertRelativePathToFull(Path);
	if (FPaths::GetExtension(File).IsEmpty())
	{
		File += TEXT(".png");
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	if (!FFileHelper::SaveArrayToFile(Png, *File))
	{
		OutError = FString::Printf(TEXT("The guide could not be written to %s."), *File);
		return false;
	}
	return true;
}

FString FMantleOverlayGuide::DefaultFileName(const FString& Slot)
{
	return FMantleOverlayChecks::IsFaceSlot(Slot) ? TEXT("MantleGuide_Face.png") : TEXT("MantleGuide_Body.png");
}
