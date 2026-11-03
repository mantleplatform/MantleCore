// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleLiveryTools.h"

#include "MantleSkinnedMesh.h"
#include "MantleVehicleConverter.h"

#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleLiveryToolsPrivate
{
	/** The colors the painted sections are outlined in, one each in turn. */
	const FColor SectionColors[] = {
		FColor(255, 96, 64), FColor(64, 200, 255), FColor(120, 230, 90), FColor(255, 210, 60),
		FColor(220, 110, 255), FColor(255, 140, 200), FColor(90, 255, 200), FColor(255, 255, 255),
	};

	void Plot(TArray<FColor>& Pixels, const int32 Size, const int32 X, const int32 Y, const FColor Color)
	{
		if (X >= 0 && Y >= 0 && X < Size && Y < Size)
		{
			Pixels[Y * Size + X] = Color;
		}
	}

	void Line(TArray<FColor>& Pixels, const int32 Size, FIntPoint A, const FIntPoint B, const FColor Color)
	{
		const int32 Dx = FMath::Abs(B.X - A.X);
		const int32 Dy = -FMath::Abs(B.Y - A.Y);
		const int32 Sx = A.X < B.X ? 1 : -1;
		const int32 Sy = A.Y < B.Y ? 1 : -1;
		int32 Error = Dx + Dy;
		// A stray UV far outside the picture is cut short, not walked.
		for (int32 Steps = 0; Steps < Size * 4; ++Steps)
		{
			Plot(Pixels, Size, A.X, A.Y, Color);
			if (A == B)
			{
				return;
			}
			const int32 Twice = 2 * Error;
			if (Twice >= Dy)
			{
				Error += Dy;
				A.X += Sx;
			}
			if (Twice <= Dx)
			{
				Error += Dx;
				A.Y += Sy;
			}
		}
	}

	/** Fills the triangle's pixels with Color, blended over what is there by Alpha (0 to 1). */
	void Fill(TArray<FColor>& Pixels, const int32 Size, const FVector2f& A, const FVector2f& B, const FVector2f& C, const FColor Color, const float Alpha)
	{
		const int32 MinX = FMath::Max(0, FMath::FloorToInt(FMath::Min3(A.X, B.X, C.X)));
		const int32 MaxX = FMath::Min(Size - 1, FMath::CeilToInt(FMath::Max3(A.X, B.X, C.X)));
		const int32 MinY = FMath::Max(0, FMath::FloorToInt(FMath::Min3(A.Y, B.Y, C.Y)));
		const int32 MaxY = FMath::Min(Size - 1, FMath::CeilToInt(FMath::Max3(A.Y, B.Y, C.Y)));
		const float Area = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
		if (FMath::Abs(Area) < UE_SMALL_NUMBER)
		{
			return;
		}
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FVector2f P(X + 0.5f, Y + 0.5f);
				const float W0 = ((B.X - P.X) * (C.Y - P.Y) - (B.Y - P.Y) * (C.X - P.X)) / Area;
				const float W1 = ((C.X - P.X) * (A.Y - P.Y) - (C.Y - P.Y) * (A.X - P.X)) / Area;
				const float W2 = 1.f - W0 - W1;
				if (W0 >= 0.f && W1 >= 0.f && W2 >= 0.f)
				{
					FColor& Pixel = Pixels[Y * Size + X];
					Pixel.R = static_cast<uint8>(FMath::Lerp<float>(Pixel.R, Color.R, Alpha));
					Pixel.G = static_cast<uint8>(FMath::Lerp<float>(Pixel.G, Color.G, Alpha));
					Pixel.B = static_cast<uint8>(FMath::Lerp<float>(Pixel.B, Color.B, Alpha));
				}
			}
		}
	}
}

UMaterialInterface* FMantleVehicleLiveryTools::MakePreviewMaterial(UTexture2D* Livery, const FLinearColor& Paint)
{
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	Material->SetShadingModel(MSM_DefaultLit);
	Material->SetUsageByFlag(MATUSAGE_SkeletalMesh, true);
	UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();

	UMaterialExpressionVectorParameter* Color = NewObject<UMaterialExpressionVectorParameter>(Material);
	Color->ParameterName = TEXT("Paint");
	Color->DefaultValue = Paint;
	Material->GetExpressionCollection().AddExpression(Color);

	UMaterialExpression* Base = Color;
	if (Livery)
	{
		UMaterialExpressionTextureSampleParameter2D* Sample = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
		Sample->ParameterName = TEXT("Livery");
		Sample->Texture = Livery;
		Sample->SamplerType = SAMPLERTYPE_Color;
		Material->GetExpressionCollection().AddExpression(Sample);
		// Paint where the livery is clear, the livery's color where it is not: the game lays it over the paint the same way.
		UMaterialExpressionLinearInterpolate* Mix = NewObject<UMaterialExpressionLinearInterpolate>(Material);
		Mix->A.Connect(0, Color);
		Mix->B.Connect(0, Sample);
		Mix->Alpha.Connect(4, Sample);
		Material->GetExpressionCollection().AddExpression(Mix);
		Base = Mix;
	}
	Inputs->BaseColor.Connect(0, Base);

	UMaterialExpressionConstant* Metallic = NewObject<UMaterialExpressionConstant>(Material);
	Metallic->R = 0.5f;
	Material->GetExpressionCollection().AddExpression(Metallic);
	Inputs->Metallic.Connect(0, Metallic);
	UMaterialExpressionConstant* Roughness = NewObject<UMaterialExpressionConstant>(Material);
	Roughness->R = 0.3f;
	Material->GetExpressionCollection().AddExpression(Roughness);
	Inputs->Roughness.Connect(0, Roughness);

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	return Material;
}

TArray<FColor> FMantleVehicleLiveryTools::DrawUvGuide(const FMantleSkinnedMesh& Body, const TSet<int32>& PaintedMaterials, const int32 Size)
{
	using namespace MantleVehicleLiveryToolsPrivate;
	TArray<FColor> Pixels;
	if (Size <= 0 || Body.LODs.IsEmpty() || Body.LODs[0].UVs.IsEmpty() || Body.LODs[0].UVs.Num() != Body.LODs[0].Positions.Num())
	{
		return Pixels;
	}
	const FMantleSkinnedMeshLOD& LOD = Body.LODs[0];
	Pixels.Init(FColor(24, 26, 30, 255), Size * Size);
	// A faint grid every tenth of the picture, so an artist can place things by eye.
	for (int32 Tenth = 1; Tenth < 10; ++Tenth)
	{
		const int32 At = Tenth * Size / 10;
		for (int32 Along = 0; Along < Size; ++Along)
		{
			Plot(Pixels, Size, At, Along, FColor(40, 43, 50, 255));
			Plot(Pixels, Size, Along, At, FColor(40, 43, 50, 255));
		}
	}
	auto Corner = [&](const int32 Index) { const FVector2f& UV = LOD.UVs[LOD.Indices[Index]]; return FVector2f(UV.X * Size, UV.Y * Size); };
	auto Edges = [&](const int32 First, const FColor Color)
	{
		for (int32 Side = 0; Side < 3; ++Side)
		{
			const FVector2f A = Corner(First + Side);
			const FVector2f B = Corner(First + (Side + 1) % 3);
			Line(Pixels, Size, FIntPoint(FMath::RoundToInt(A.X), FMath::RoundToInt(A.Y)), FIntPoint(FMath::RoundToInt(B.X), FMath::RoundToInt(B.Y)), Color);
		}
	};

	// Unpainted sections first, dim, then the painted ones over them.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		int32 Painted = 0;
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			const bool bPainted = PaintedMaterials.Contains(Section.MaterialIndex);
			if (bPainted)
			{
				++Painted;
			}
			if (bPainted != (Pass == 1))
			{
				continue;
			}
			const FColor Color = SectionColors[(Painted - 1 + 8) % 8];
			const int32 End = FMath::Min(Section.FirstIndex + Section.IndexCount, LOD.Indices.Num());
			for (int32 First = Section.FirstIndex; First + 2 < End; First += 3)
			{
				if (bPainted)
				{
					Fill(Pixels, Size, Corner(First), Corner(First + 1), Corner(First + 2), Color, 0.22f);
				}
				Edges(First, bPainted ? Color : FColor(70, 76, 88, 255));
			}
		}
	}
	// The picture's own border.
	const FColor Border(110, 118, 132, 255);
	for (int32 Along = 0; Along < Size; ++Along)
	{
		Plot(Pixels, Size, 0, Along, Border);
		Plot(Pixels, Size, Size - 1, Along, Border);
		Plot(Pixels, Size, Along, 0, Border);
		Plot(Pixels, Size, Along, Size - 1, Border);
	}
	return Pixels;
}

bool FMantleVehicleLiveryTools::WriteUvGuide(const FMantleSkinnedMesh& Body, const TSet<int32>& PaintedMaterials, const int32 Size, const FString& File, FString& OutError)
{
	const TArray<FColor> Pixels = DrawUvGuide(Body, PaintedMaterials, Size);
	if (Pixels.IsEmpty())
	{
		OutError = TEXT("Your model has no UV layout to draw. A livery is laid out on the model's first UV set.");
		return false;
	}
	IImageWrapperModule& ImageWrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Png = ImageWrappers.CreateImageWrapper(EImageFormat::PNG);
	if (!Png.IsValid() || !Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size, Size, ERGBFormat::BGRA, 8))
	{
		OutError = TEXT("The guide picture could not be made.");
		return false;
	}
	const TArray64<uint8> Bytes = Png->GetCompressed();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree*/ true);
	if (!FFileHelper::SaveArrayToFile(Bytes, *File))
	{
		OutError = FString::Printf(TEXT("Mantle could not write %s. Pick a folder you can save to."), *File);
		return false;
	}
	return true;
}

FString FMantleVehicleLiveryTools::DisplayName(const FString& Name, const int32 Index)
{
	const FString Trimmed = Name.TrimStartAndEnd();
	return Trimmed.IsEmpty() ? FString::Printf(TEXT("Livery %d"), Index + 1) : Trimmed;
}

FString FMantleVehicleLiveryTools::NameFromImage(const FString& ImageName)
{
	FString Name = ImageName;
	Name.ReplaceInline(TEXT("_"), TEXT(" "));
	Name.ReplaceInline(TEXT("-"), TEXT(" "));
	return Name.TrimStartAndEnd().Left(FMantleVehicleConverter::MaxLiveryNameLength);
}
