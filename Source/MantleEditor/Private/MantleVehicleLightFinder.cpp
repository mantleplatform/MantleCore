// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleLightFinder.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "MantleGlowMaterial.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLightData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "MantleVehicleLights"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleLightsPrivate
{
	constexpr EMantleLightKind Kinds[] = { EMantleLightKind::Head, EMantleLightKind::Rear, EMantleLightKind::Reverse,
		EMantleLightKind::Left, EMantleLightKind::Right };

	/** How bright (0..1, the brightest channel) a glow texture must be under a triangle for the triangle to glow: the dark of a glow texture is near 0. */
	constexpr float GlowThreshold = 0.3f;

	/** How far toward the end (of the half length, 0 the middle, 1 the end) a white or red spot must be to be a headlight, rear or reverse light. */
	constexpr float EndShare = 0.35f;

	/** Which way a new beam points: forward and a little down, at the road ahead. */
	const FVector BeamDirection(1.0, 0.0, -0.05);

	/** The brightest of a color's red, green and blue (not its alpha, which FLinearColor::GetMax counts). */
	float Brightest(const FLinearColor& Color)
	{
		return FMath::Max3(Color.R, Color.G, Color.B);
	}

	bool IsSignal(const EMantleLightKind Kind)
	{
		return Kind == EMantleLightKind::Left || Kind == EMantleLightKind::Right;
	}

	/** Left is -Y (the vehicle faces +X). */
	EMantleLightKind SideOf(const float Y, const FBox3f& Bounds)
	{
		return Y < Bounds.GetCenter().Y ? EMantleLightKind::Left : EMantleLightKind::Right;
	}

	/** A glow texture's pixels, read once per texture. */
	struct FGlowImage
	{
		TArray<FColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;

		bool Read(UTexture* Texture)
		{
			UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
			if (!Texture2D || !Texture2D->Source.IsValid())
			{
				return false;
			}
			FImage Image;
			if (!Texture2D->Source.GetMipImage(Image, 0, 0, 0))
			{
				return false;
			}
			FImage Bgra;
			Image.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			const TArrayView64<FColor> View = Bgra.AsBGRA8();
			Pixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
			Width = Bgra.SizeX;
			Height = Bgra.SizeY;
			return Pixels.Num() == Width * Height && Pixels.Num() > 0;
		}

		/** The color at a UV, the texture repeating (a model may lay its UVs over 1..2, as the Fab Hatchback does). */
		FLinearColor At(const FVector2f& UV) const
		{
			const float U = UV.X - FMath::FloorToFloat(UV.X);
			const float V = UV.Y - FMath::FloorToFloat(UV.Y);
			const int32 X = FMath::Clamp(static_cast<int32>(U * Width), 0, Width - 1);
			const int32 Y = FMath::Clamp(static_cast<int32>(V * Height), 0, Height - 1);
			const FColor& Pixel = Pixels[Y * Width + X];
			return FLinearColor(Pixel.R / 255.f, Pixel.G / 255.f, Pixel.B / 255.f);
		}
	};

	/** A name's words, lower case: split at anything not a letter or digit and where lower case turns upper (HeadLight_L is head, light, l). */
	TArray<FString> Words(const FString& Name)
	{
		TArray<FString> Out;
		FString Word;
		TCHAR Previous = 0;
		for (const TCHAR Char : Name)
		{
			const bool bAlnum = FChar::IsAlnum(Char);
			if (!bAlnum || (FChar::IsUpper(Char) && FChar::IsLower(Previous)))
			{
				if (!Word.IsEmpty())
				{
					Out.Add(Word.ToLower());
					Word.Reset();
				}
			}
			if (bAlnum)
			{
				Word.AppendChar(Char);
			}
			Previous = Char;
		}
		if (!Word.IsEmpty())
		{
			Out.Add(Word.ToLower());
		}
		return Out;
	}

	bool AnyWord(const TArray<FString>& Words, std::initializer_list<const TCHAR*> Starts)
	{
		for (const FString& Word : Words)
		{
			for (const TCHAR* Start : Starts)
			{
				if (Word.StartsWith(Start))
				{
					return true;
				}
			}
		}
		return false;
	}

	/** The triangle's three corners. */
	void Corners(const FMantleSkinnedMeshLOD& LOD, const int32 Triangle, int32 (&Out)[3])
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Out[Corner] = static_cast<int32>(LOD.Indices[Triangle * 3 + Corner]);
		}
	}

	FVector3f Centroid(const FMantleSkinnedMeshLOD& LOD, const int32 Triangle)
	{
		int32 C[3];
		Corners(LOD, Triangle, C);
		return (LOD.Positions[C[0]] + LOD.Positions[C[1]] + LOD.Positions[C[2]]) / 3.f;
	}

	double Area(const FMantleSkinnedMeshLOD& LOD, const int32 Triangle)
	{
		int32 C[3];
		Corners(LOD, Triangle, C);
		return 0.5 * FVector3f::CrossProduct(LOD.Positions[C[1]] - LOD.Positions[C[0]], LOD.Positions[C[2]] - LOD.Positions[C[0]]).Size();
	}

	/** The brightest glow under a triangle (its middle and halfway to each corner), and the UV it was read at. */
	FLinearColor GlowUnder(const FMantleSkinnedMeshLOD& LOD, const int32 Triangle, const FGlowImage& Image, FVector2f& OutUV)
	{
		int32 C[3];
		Corners(LOD, Triangle, C);
		if (!LOD.UVs.IsValidIndex(C[0]) || !LOD.UVs.IsValidIndex(C[1]) || !LOD.UVs.IsValidIndex(C[2]))
		{
			return FLinearColor::Black;
		}
		const FVector2f Middle = (LOD.UVs[C[0]] + LOD.UVs[C[1]] + LOD.UVs[C[2]]) / 3.f;
		OutUV = Middle;
		FLinearColor Best = Image.At(Middle);
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector2f UV = (Middle + LOD.UVs[C[Corner]]) * 0.5f;
			const FLinearColor Here = Image.At(UV);
			if (Brightest(Here) > Brightest(Best))
			{
				Best = Here;
				OutUV = UV;
			}
		}
		return Best;
	}

	/** A color with little hue in it: its weakest channel at least SaturationFloor of its strongest. */
	constexpr float SaturationFloor = 0.6f;
	bool IsPale(const FLinearColor& Color)
	{
		const float Max = Brightest(Color);
		return Max <= KINDA_SMALL_NUMBER || FMath::Min3(Color.R, Color.G, Color.B) >= Max * SaturationFloor;
	}
}

bool FMantleVehicleLightGuess::FoundAny() const
{
	for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
	{
		if (Triangles[Kind].Num() > 0 || WholeSlots[Kind].Num() > 0)
		{
			return true;
		}
	}
	return false;
}

const TCHAR* FMantleVehicleLights::Key(const EMantleLightKind Kind)
{
	switch (Kind)
	{
	case EMantleLightKind::Head: return TEXT("head");
	case EMantleLightKind::Rear: return TEXT("rear");
	case EMantleLightKind::Reverse: return TEXT("reverse");
	case EMantleLightKind::Left: return TEXT("left");
	case EMantleLightKind::Right: return TEXT("right");
	}
	return TEXT("head");
}

FText FMantleVehicleLights::Label(const EMantleLightKind Kind)
{
	switch (Kind)
	{
	case EMantleLightKind::Head: return LOCTEXT("Head", "Headlights");
	case EMantleLightKind::Rear: return LOCTEXT("Rear", "Rear lights");
	case EMantleLightKind::Reverse: return LOCTEXT("Reverse", "Reverse lights");
	case EMantleLightKind::Left: return LOCTEXT("Left", "Left signal");
	case EMantleLightKind::Right: return LOCTEXT("Right", "Right signal");
	}
	return FText::GetEmpty();
}

FString FMantleVehicleLights::SectionName(const EMantleLightKind Kind)
{
	switch (Kind)
	{
	case EMantleLightKind::Head: return TEXT("Headlights");
	case EMantleLightKind::Rear: return TEXT("RearLights");
	case EMantleLightKind::Reverse: return TEXT("ReverseLights");
	case EMantleLightKind::Left: return TEXT("LeftSignal");
	case EMantleLightKind::Right: return TEXT("RightSignal");
	}
	return TEXT("Light");
}

FLinearColor FMantleVehicleLights::Color(const EMantleLightKind Kind)
{
	switch (Kind)
	{
	case EMantleLightKind::Head: return FLinearColor(1.0f, 0.95f, 0.85f);
	case EMantleLightKind::Rear: return FLinearColor(1.0f, 0.02f, 0.01f);
	case EMantleLightKind::Reverse: return FLinearColor(1.0f, 1.0f, 1.0f);
	case EMantleLightKind::Left:
	case EMantleLightKind::Right: return FLinearColor(1.0f, 0.35f, 0.0f);
	}
	return FLinearColor::White;
}

TArray<FString>& FMantleVehicleLights::Sections(FMantleVehicleSourceLights& Lights, const EMantleLightKind Kind)
{
	switch (Kind)
	{
	case EMantleLightKind::Rear: return Lights.Rear;
	case EMantleLightKind::Reverse: return Lights.Reverse;
	case EMantleLightKind::Left: return Lights.Left;
	case EMantleLightKind::Right: return Lights.Right;
	default: return Lights.Head;
	}
}

const TArray<FString>& FMantleVehicleLights::Sections(const FMantleVehicleSourceLights& Lights, const EMantleLightKind Kind)
{
	return Sections(const_cast<FMantleVehicleSourceLights&>(Lights), Kind);
}

TOptional<EMantleLightKind> FMantleVehicleLights::Classify(const FVector3f& Center, const FLinearColor& Glow, const FBox3f& Bounds)
{
	using namespace MantleVehicleLightsPrivate;
	const float Max = Brightest(Glow);
	if (!Bounds.IsValid || Max <= KINDA_SMALL_NUMBER)
	{
		return {};
	}
	const float G = Glow.G / Max;
	const float B = Glow.B / Max;
	const bool bRedMost = Glow.R >= Max;
	const float HalfLength = FMath::Max(Bounds.GetExtent().X, 1.f);
	// -1 at the back, 1 at the front.
	const float Along = (Center.X - Bounds.GetCenter().X) / HalfLength;
	// Amber (red with some green, little blue): a signal, wherever it sits (mirrors and fenders carry them too).
	if (bRedMost && G >= 0.25f && G <= 0.8f && B <= 0.35f)
	{
		return SideOf(Center.Y, Bounds);
	}
	// Red: a rear light, at the back.
	if (bRedMost && G < 0.25f && B < 0.4f)
	{
		return Along <= -EndShare ? TOptional<EMantleLightKind>(EMantleLightKind::Rear) : TOptional<EMantleLightKind>();
	}
	// White or warm white: a headlight at the front, a reverse light at the back.
	if (G > 0.8f && (B > 0.45f || bRedMost))
	{
		if (Along >= EndShare)
		{
			return EMantleLightKind::Head;
		}
		if (Along <= -EndShare)
		{
			return EMantleLightKind::Reverse;
		}
	}
	return {};
}

TOptional<EMantleLightKind> FMantleVehicleLights::FromName(const FString& Name, const FVector3f& Center, const FBox3f& Bounds)
{
	using namespace MantleVehicleLightsPrivate;
	const TArray<FString> Said = Words(Name);
	if (AnyWord(Said, { TEXT("indicator"), TEXT("blinker"), TEXT("signal"), TEXT("turnsignal") }))
	{
		return SideOf(Center.Y, Bounds);
	}
	// A light word, whole ("light", "lights", "lamp", "headlight", "taillight"), never "lightning".
	bool bLight = false;
	for (const FString& Word : Said)
	{
		bLight = bLight || (Word.Contains(TEXT("light")) && !Word.Contains(TEXT("lightning"))) || Word.Contains(TEXT("lamp"));
	}
	if (AnyWord(Said, { TEXT("reverse"), TEXT("backup") }) && bLight)
	{
		return EMantleLightKind::Reverse;
	}
	if (AnyWord(Said, { TEXT("taillight"), TEXT("taillamp"), TEXT("brakelight") }) || (bLight && AnyWord(Said, { TEXT("tail"), TEXT("brake"), TEXT("rear"), TEXT("stop") })))
	{
		return EMantleLightKind::Rear;
	}
	if (AnyWord(Said, { TEXT("headlight"), TEXT("headlamp") }) || (bLight && AnyWord(Said, { TEXT("head"), TEXT("front") })))
	{
		return EMantleLightKind::Head;
	}
	if (bLight && Bounds.IsValid)
	{
		// Only "light" or "lamp": which end it sits at says which.
		const float Along = (Center.X - Bounds.GetCenter().X) / FMath::Max(Bounds.GetExtent().X, 1.f);
		if (Along >= EndShare)
		{
			return EMantleLightKind::Head;
		}
		if (Along <= -EndShare)
		{
			return EMantleLightKind::Rear;
		}
	}
	return {};
}

FMantleVehicleLightGuess FMantleVehicleLights::Find(const FMantleSkinnedMesh& Body, const TArray<FSkeletalMaterial>& Materials, const bool bCenterline)
{
	using namespace MantleVehicleLightsPrivate;
	FMantleVehicleLightGuess Out;
	if (Body.LODs.Num() == 0)
	{
		return Out;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	const FBox3f Bounds = Body.ComputeBounds(0);
	if (!Bounds.IsValid)
	{
		return Out;
	}

	// Each slot's triangles (a slot may be drawn by more than one section).
	TMap<int32, TArray<int32>> SlotTriangles;
	for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		if (Section.IndexCount < 3 || Section.FirstIndex < 0 || Section.FirstIndex + Section.IndexCount > LOD.Indices.Num())
		{
			continue;
		}
		TArray<int32>& Triangles = SlotTriangles.FindOrAdd(Section.MaterialIndex);
		for (int32 Triangle = Section.FirstIndex / 3; Triangle < (Section.FirstIndex + Section.IndexCount) / 3; ++Triangle)
		{
			Triangles.Add(Triangle);
		}
	}
	TArray<int32> Slots;
	SlotTriangles.GetKeys(Slots);
	Slots.Sort();

	TMap<UTexture*, TSharedPtr<FGlowImage>> Images;
	for (const int32 Slot : Slots)
	{
		const TArray<int32>& Triangles = SlotTriangles[Slot];
		UMaterialInterface* Material = Materials.IsValidIndex(Slot) ? Materials[Slot].MaterialInterface.Get() : nullptr;
		TArray<TOptional<EMantleLightKind>> Kind;
		Kind.SetNum(Triangles.Num());

		FVector3f Middle = FVector3f::ZeroVector;
		for (const int32 Triangle : Triangles)
		{
			Middle += Centroid(LOD, Triangle);
		}
		Middle /= static_cast<float>(FMath::Max(Triangles.Num(), 1));
		TOptional<EMantleLightKind> Named = Materials.IsValidIndex(Slot) ? FromName(Materials[Slot].MaterialSlotName.ToString(), Middle, Bounds) : TOptional<EMantleLightKind>();
		if (!Named && Material)
		{
			Named = FromName(Material->GetName(), Middle, Bounds);
		}
		if (Named)
		{
			// A slot named for a light is that light; one named for a signal is split by side, for a model with one material for both.
			for (int32 Index = 0; Index < Triangles.Num(); ++Index)
			{
				Kind[Index] = IsSignal(*Named) ? SideOf(Centroid(LOD, Triangles[Index]).Y, Bounds) : *Named;
			}
		}
		else
		{
			FMantleMaterialGlow Glow;
			if (!FMantleMaterialGlow::Read(Material, Glow))
			{
				continue;
			}
			const FLinearColor Tint = Brightest(Glow.Color) > KINDA_SMALL_NUMBER ? Glow.Color : FLinearColor::White;
			const FGlowImage* Image = nullptr;
			if (Glow.Texture)
			{
				TSharedPtr<FGlowImage>& Cached = Images.FindOrAdd(Glow.Texture);
				if (!Cached.IsValid())
				{
					Cached = MakeShared<FGlowImage>();
					Cached->Read(Glow.Texture);
				}
				Image = Cached->Pixels.Num() > 0 ? Cached.Get() : nullptr;
			}
			if (Glow.Texture && !Image)
			{
				// A glow texture Mantle cannot read (no source data): nothing is guessed from it.
				continue;
			}
			// A glow texture that is white everywhere (a mask) leaves the color to the lens under it: the base color, read where the glow is.
			const FGlowImage* Base = nullptr;
			if (Glow.BaseTexture && Glow.BaseTexture != Glow.Texture)
			{
				TSharedPtr<FGlowImage>& Cached = Images.FindOrAdd(Glow.BaseTexture);
				if (!Cached.IsValid())
				{
					Cached = MakeShared<FGlowImage>();
					Cached->Read(Glow.BaseTexture);
				}
				Base = Cached->Pixels.Num() > 0 ? Cached.Get() : nullptr;
			}
			for (int32 Index = 0; Index < Triangles.Num(); ++Index)
			{
				FLinearColor Here = Tint;
				if (Image)
				{
					FVector2f UV;
					const FLinearColor Pixel = GlowUnder(LOD, Triangles[Index], *Image, UV);
					if (Brightest(Pixel) < GlowThreshold)
					{
						continue;
					}
					Here = Pixel * Tint;
					if (Base && IsPale(Here))
					{
						const FLinearColor Lens = Base->At(UV);
						if (!IsPale(Lens))
						{
							Here = Lens;
						}
					}
				}
				Kind[Index] = Classify(Centroid(LOD, Triangles[Index]), Here, Bounds);
			}
		}

		// A slot that is all one light stays whole; otherwise its light triangles are cut out.
		bool bWhole = Kind.Num() > 0 && Kind[0].IsSet();
		for (int32 Index = 1; bWhole && Index < Kind.Num(); ++Index)
		{
			bWhole = Kind[Index] == Kind[0];
		}
		if (bWhole)
		{
			Out.WholeSlots[static_cast<int32>(*Kind[0])].Add(Slot);
			continue;
		}
		for (int32 Index = 0; Index < Kind.Num(); ++Index)
		{
			if (Kind[Index])
			{
				Out.Triangles[static_cast<int32>(*Kind[Index])].Add(Triangles[Index]);
			}
		}
	}

	// The beams: one at the middle of each side's headlights, at their front; with no headlights, at the front corners.
	const int32 Head = static_cast<int32>(EMantleLightKind::Head);
	TArray<int32> HeadTriangles = Out.Triangles[Head];
	for (const int32 Slot : Out.WholeSlots[Head])
	{
		HeadTriangles.Append(SlotTriangles.FindRef(Slot));
	}
	const auto AddBeam = [&Out](const FVector& Position)
	{
		FMantleVehicleSourceLightBeam& Beam = Out.Beams.AddDefaulted_GetRef();
		Beam.Position = Position;
		Beam.Direction = BeamDirection;
	};
	if (HeadTriangles.Num() > 0)
	{
		const int32 Sides = bCenterline ? 1 : 2;
		for (int32 Side = 0; Side < Sides; ++Side)
		{
			FVector Sum = FVector::ZeroVector;
			double Weight = 0.0;
			double Front = -UE_BIG_NUMBER;
			for (const int32 Triangle : HeadTriangles)
			{
				const FVector3f Center = Centroid(LOD, Triangle);
				if (!bCenterline && (Side == 0) != (Center.Y < Bounds.GetCenter().Y))
				{
					continue;
				}
				const double TriangleArea = FMath::Max(Area(LOD, Triangle), UE_SMALL_NUMBER);
				Sum += FVector(Center) * TriangleArea;
				Weight += TriangleArea;
				Front = FMath::Max(Front, static_cast<double>(Center.X));
			}
			if (Weight > 0.0)
			{
				FVector Position = Sum / Weight;
				Position.X = Front;
				AddBeam(Position);
			}
		}
	}
	else
	{
		const FVector3f Center = Bounds.GetCenter();
		const FVector3f Extent = Bounds.GetExtent();
		const double Z = Bounds.Min.Z + Extent.Z * 2.0 * 0.4;
		if (bCenterline)
		{
			AddBeam(FVector(Bounds.Max.X, Center.Y, Z));
		}
		else
		{
			AddBeam(FVector(Bounds.Max.X, Center.Y - Extent.Y * 0.7, Z));
			AddBeam(FVector(Bounds.Max.X, Center.Y + Extent.Y * 0.7, Z));
		}
	}
	return Out;
}

TArray<FString> FMantleVehicleLights::SectionNames(const TArray<FSkeletalMaterial>& Materials)
{
	TArray<FString> Names;
	for (const FSkeletalMaterial& Material : Materials)
	{
		Names.Add(Material.MaterialSlotName.ToString());
	}
	return FMantleVehicleConverter::UniqueSectionNames(Names);
}

void FMantleVehicleLights::Cut(FMantleSkinnedMesh& Body, TArray<FSkeletalMaterial>& Materials, const FMantleVehicleLightGuess& Guess, FMantleVehicleSourceLights& OutLights)
{
	using namespace MantleVehicleLightsPrivate;
	OutLights = FMantleVehicleSourceLights();
	OutLights.Beams = Guess.Beams;
	TArray<int32> NewSlots[NumKinds];
	if (Body.LODs.Num() > 0)
	{
		FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
		TMap<int32, int32> KindOfTriangle;
		for (int32 Kind = 0; Kind < NumKinds; ++Kind)
		{
			for (const int32 Triangle : Guess.Triangles[Kind])
			{
				KindOfTriangle.Add(Triangle, Kind);
			}
		}
		if (KindOfTriangle.Num() > 0)
		{
			// The triangles each kind takes from each slot, in the order they were drawn.
			TMap<TPair<int32, int32>, TArray<int32>> Moved;
			TArray<TPair<int32, int32>> MovedOrder;
			TArray<uint32> Indices;
			Indices.Reserve(LOD.Indices.Num());
			TArray<FMantleSkinnedMeshSection> Sections;
			for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
			{
				FMantleSkinnedMeshSection& Kept = Sections.Add_GetRef(Section);
				Kept.FirstIndex = Indices.Num();
				Kept.IndexCount = 0;
				if (Section.FirstIndex < 0 || Section.FirstIndex + Section.IndexCount > LOD.Indices.Num())
				{
					continue;
				}
				for (int32 Triangle = Section.FirstIndex / 3; Triangle < (Section.FirstIndex + Section.IndexCount) / 3; ++Triangle)
				{
					if (const int32* Kind = KindOfTriangle.Find(Triangle))
					{
						const TPair<int32, int32> Key(*Kind, Section.MaterialIndex);
						if (!Moved.Contains(Key))
						{
							MovedOrder.Add(Key);
						}
						Moved.FindOrAdd(Key).Add(Triangle);
						continue;
					}
					Indices.Append(&LOD.Indices[Triangle * 3], 3);
					Kept.IndexCount += 3;
				}
			}
			MovedOrder.Sort([](const TPair<int32, int32>& A, const TPair<int32, int32>& B) { return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value; });
			TSet<FString> Taken;
			for (const FSkeletalMaterial& Material : Materials)
			{
				Taken.Add(Material.MaterialSlotName.ToString().ToLower());
			}
			for (const TPair<int32, int32>& Key : MovedOrder)
			{
				const EMantleLightKind Kind = static_cast<EMantleLightKind>(Key.Key);
				FString Name = SectionName(Kind);
				for (int32 Number = 2; Taken.Contains(Name.ToLower()); ++Number)
				{
					Name = FString::Printf(TEXT("%s%d"), *SectionName(Kind), Number);
				}
				Taken.Add(Name.ToLower());
				FSkeletalMaterial Slot = Materials.IsValidIndex(Key.Value) ? Materials[Key.Value] : FSkeletalMaterial();
				Slot.MaterialSlotName = FName(*Name);
				Slot.ImportedMaterialSlotName = Slot.MaterialSlotName;
				const int32 NewSlot = Materials.Add(Slot);
				NewSlots[Key.Key].Add(NewSlot);
				FMantleSkinnedMeshSection& Added = Sections.AddDefaulted_GetRef();
				Added.MaterialIndex = NewSlot;
				Added.MaterialName = Slot.MaterialSlotName;
				Added.FirstIndex = Indices.Num();
				for (const int32 Triangle : Moved[Key])
				{
					Indices.Append(&LOD.Indices[Triangle * 3], 3);
				}
				Added.IndexCount = Indices.Num() - Added.FirstIndex;
			}
			LOD.Indices = MoveTemp(Indices);
			LOD.Sections = MoveTemp(Sections);
		}
	}
	const TArray<FString> Names = SectionNames(Materials);
	for (int32 Kind = 0; Kind < NumKinds; ++Kind)
	{
		TArray<FString>& List = Sections(OutLights, static_cast<EMantleLightKind>(Kind));
		for (const int32 Slot : Guess.WholeSlots[Kind])
		{
			if (Names.IsValidIndex(Slot))
			{
				List.AddUnique(Names[Slot]);
			}
		}
		for (const int32 Slot : NewSlots[Kind])
		{
			if (Names.IsValidIndex(Slot))
			{
				List.AddUnique(Names[Slot]);
			}
		}
	}
}

TOptional<EMantleLightKind> FMantleVehicleLights::KindOf(const UMaterialInterface* Material)
{
	using namespace MantleVehicleLightsPrivate;
	UMaterialInstanceConstant* Instance = const_cast<UMaterialInstanceConstant*>(Cast<UMaterialInstanceConstant>(Material));
	if (const UMantleVehicleLightData* Light = Instance ? Cast<UMantleVehicleLightData>(Instance->GetAssetUserDataOfClass(UMantleVehicleLightData::StaticClass())) : nullptr)
	{
		for (const EMantleLightKind Kind : Kinds)
		{
			if (Light->Kind == Key(Kind))
			{
				return Kind;
			}
		}
	}
	return {};
}

UMaterialInterface* FMantleVehicleLights::OriginalOf(UMaterialInterface* Material)
{
	const UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Material);
	return Instance && KindOf(Instance) ? Instance->Parent.Get() : Material;
}

UMaterialInterface* FMantleVehicleLights::MakeLightMaterial(UMaterialInterface* Original, const EMantleLightKind Kind, const FString& PackagePath, const FString& AssetName,
	FString& OutError)
{
	UMaterialInterface* Parent = OriginalOf(Original);
	if (!Parent)
	{
		OutError = TEXT("the section has no material to glow with");
		return nullptr;
	}
	const bool bPersist = !FMantleVehicleChassisMesh::IsScratchPath(PackagePath);
	const FString PackageName = PackagePath / AssetName;
	if (!PackagePath.StartsWith(TEXT("/")) || AssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackageName, !bPersist))
	{
		OutError = FString::Printf(TEXT("'%s' is not a folder a light can be saved in"), *PackagePath);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("%s could not be created"), *PackageName);
		return nullptr;
	}
	UMaterialInstanceConstant* Instance = FindObject<UMaterialInstanceConstant>(Package, *AssetName);
	if (!Instance && FPackageName::DoesPackageExist(PackageName))
	{
		Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(PackageName + TEXT(".") + AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	const bool bNew = Instance == nullptr;
	if (bNew)
	{
		Instance = NewObject<UMaterialInstanceConstant>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	}
	if (!Instance)
	{
		OutError = FString::Printf(TEXT("%s could not be created"), *PackageName);
		return nullptr;
	}
	if (!bNew && Instance->Parent == Parent && KindOf(Instance) == Kind)
	{
		// Already this light's copy: nothing to change or save.
		return Instance;
	}
	Instance->Modify();
	Instance->SetParentEditorOnly(Parent);
	// The glow the model ships switched off (an Emissive Intensity of 0) is turned on: the game turns it up and down from there.
	FMantleMaterialGlow Glow;
	FMantleMaterialGlow::Read(Parent, Glow);
	for (const FName& Scalar : Glow.OffScalars)
	{
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(Scalar), 1.f);
	}
	UMantleVehicleLightData* Data = Cast<UMantleVehicleLightData>(Instance->GetAssetUserDataOfClass(UMantleVehicleLightData::StaticClass()));
	if (!Data)
	{
		Data = NewObject<UMantleVehicleLightData>(Instance, NAME_None, RF_Transactional);
		Instance->AddAssetUserData(Data);
	}
	Data->Kind = Key(Kind);
	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	if (bPersist)
	{
		if (bNew)
		{
			FAssetRegistryModule::AssetCreated(Instance);
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Instance, *Filename, Args))
		{
			OutError = FString::Printf(TEXT("%s could not be saved (check that your project's files are not read-only)"), *PackageName);
			return nullptr;
		}
	}
	return Instance;
}

void FMantleVehicleLights::ApplyMaterials(UMantleVehicleSource& Source, TArray<FString>& OutNotes)
{
	using namespace MantleVehicleLightsPrivate;
	USkeletalMesh* Mesh = Source.Body.LoadSynchronous();
	if (!Mesh)
	{
		return;
	}
	TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	const TArray<FString> Names = SectionNames(Materials);
	const FString Folder = FPackageName::GetLongPackagePath(Mesh->GetOutermost()->GetName());
	bool bChanged = false;
	for (int32 Slot = 0; Slot < Materials.Num() && Slot < Names.Num(); ++Slot)
	{
		TOptional<EMantleLightKind> Kind;
		for (const EMantleLightKind Each : Kinds)
		{
			if (Sections(Source.Lights, Each).Contains(Names[Slot]))
			{
				Kind = Each;
				break;
			}
		}
		UMaterialInterface* Current = Materials[Slot].MaterialInterface;
		UMaterialInterface* Wanted = OriginalOf(Current);
		if (Kind && Wanted)
		{
			FString Error;
			const FString AssetName = FString::Printf(TEXT("MI_%s_%s_%s"), *Mesh->GetName(), *Names[Slot], Key(*Kind));
			UMaterialInterface* Glowing = MakeLightMaterial(Wanted, *Kind, Folder, AssetName, Error);
			if (!Glowing)
			{
				OutNotes.Add(FString::Printf(TEXT("The %s section %s can't glow: %s."), *Label(*Kind).ToString().ToLower(), *Names[Slot], *Error));
				continue;
			}
			Wanted = Glowing;
		}
		if (Wanted != Current)
		{
			if (!bChanged)
			{
				Mesh->Modify();
				bChanged = true;
			}
			Materials[Slot].MaterialInterface = Wanted;
		}
	}
	// A light section is not painted: it glows in its own color.
	for (const EMantleLightKind Kind : Kinds)
	{
		for (const FString& Section : Sections(Source.Lights, Kind))
		{
			if (Source.Paint.Sections.Remove(Section) > 0)
			{
				Source.Modify();
			}
		}
	}
	if (!bChanged)
	{
		return;
	}
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	if (!FMantleVehicleChassisMesh::IsScratchPath(Folder))
	{
		UPackage* Package = Mesh->GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, Mesh, *Filename, Args))
		{
			OutNotes.Add(FString::Printf(TEXT("The lights were set on %s, but it could not be saved (check that your project's files are not read-only)."), *Mesh->GetName()));
		}
	}
}

#undef LOCTEXT_NAMESPACE
