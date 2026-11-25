// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMaterialEval.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "MaterialExpressionIO.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInterface.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMaterialEvalPrivate
{
	/** A node's value: up to four numbers, as the shader's float1..float4. */
	struct FValue
	{
		float C[4] = { 0.f, 0.f, 0.f, 0.f };
		int32 N = 1;

		static FValue Scalar(const float X)
		{
			FValue Out;
			Out.C[0] = X;
			Out.N = 1;
			return Out;
		}

		/** Component I, a scalar standing for every component (the shader's broadcast). */
		float Get(const int32 I) const { return N == 1 ? C[0] : (I < N ? C[I] : 0.f); }
	};

	/** Two values combined per component, a scalar broadcast against the other's width. */
	template <typename FOp>
	FValue Each(const FValue& A, const FValue& B, FOp Op)
	{
		FValue Out;
		Out.N = FMath::Max(A.N, B.N);
		for (int32 I = 0; I < Out.N; ++I)
		{
			Out.C[I] = Op(A.Get(I), B.Get(I));
		}
		return Out;
	}

	template <typename FOp>
	FValue Each(const FValue& A, FOp Op)
	{
		FValue Out = A;
		for (int32 I = 0; I < Out.N; ++I)
		{
			Out.C[I] = Op(A.C[I]);
		}
		return Out;
	}

	/** A texture's pixels as the sampler reads them: linear floats (an sRGB texture decoded, a mask's values as stored). */
	struct FImage32
	{
		TArray<FLinearColor> Pixels;
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
			FImage Linear;
			Image.CopyTo(Linear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
			const TArrayView64<FLinearColor> View = Linear.AsRGBA32F();
			Pixels = TArray<FLinearColor>(View.GetData(), static_cast<int32>(View.Num()));
			Width = Linear.SizeX;
			Height = Linear.SizeY;
			return Width > 0 && Height > 0 && Pixels.Num() == Width * Height;
		}

		/** The pixel at a UV, the texture repeating (wrap, as a material's default sampler does). */
		const FLinearColor& At(const FVector2f& UV) const
		{
			const float U = UV.X - FMath::FloorToFloat(UV.X);
			const float V = UV.Y - FMath::FloorToFloat(UV.Y);
			const int32 X = FMath::Clamp(static_cast<int32>(U * Width), 0, Width - 1);
			const int32 Y = FMath::Clamp(static_cast<int32>(V * Height), 0, Height - 1);
			return Pixels[Y * Width + X];
		}
	};
}

struct FMantleEmissiveEval::FImpl
{
	using FValue = MantleMaterialEvalPrivate::FValue;

	const FExpressionInput* Emissive = nullptr;
	/** Parameters as the instance sets them, read once. */
	TMap<const UMaterialExpression*, FValue> Parameters;
	/** Each texture sample's pixels (the instance's texture for a parameter). */
	TMap<const UMaterialExpression*, TSharedPtr<MantleMaterialEvalPrivate::FImage32>> Samples;
	TSet<int32> UVSets;

	/** Walks the graph once: every node known, parameters and textures read. False with Why (and Unsupported set) at the first node it cannot evaluate. */
	bool Visit(const FExpressionInput* Input, const UMaterialInterface* Material, const TArray<FName>& SwitchedOn, TSet<const UMaterialExpression*>& Seen,
		TMap<UTexture*, TSharedPtr<MantleMaterialEvalPrivate::FImage32>>* Images, FString& Why, FString& Unsupported, const int32 Depth)
	{
		using namespace MantleMaterialEvalPrivate;
		if (!Input || !Input->Expression)
		{
			return true;
		}
		const UMaterialExpression* Node = Input->Expression;
		if (Depth > 256)
		{
			Why = TEXT("its emissive graph is too deep to read");
			return false;
		}
		if (Seen.Contains(Node))
		{
			return true;
		}
		Seen.Add(Node);
		if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Node))
		{
			float Value = Scalar->DefaultValue;
			float Set = 0.f;
			if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Scalar->ParameterName)), Set))
			{
				Value = Set;
			}
			// A glow switch (the lamp's on/off) at 0: the lamp is read lit.
			if (FMath::IsNearlyZero(Value) && SwitchedOn.Contains(Scalar->ParameterName))
			{
				Value = 1.f;
			}
			Parameters.Add(Node, FValue::Scalar(Value));
			return true;
		}
		if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Node))
		{
			FLinearColor Value = Vector->DefaultValue;
			FLinearColor Set;
			if (Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Vector->ParameterName)), Set))
			{
				Value = Set;
			}
			FValue Out;
			Out.C[0] = Value.R;
			Out.C[1] = Value.G;
			Out.C[2] = Value.B;
			Out.C[3] = Value.A;
			Out.N = 4;
			Parameters.Add(Node, Out);
			return true;
		}
		if (Node->IsA<UMaterialExpressionConstant>() || Node->IsA<UMaterialExpressionConstant2Vector>() || Node->IsA<UMaterialExpressionConstant3Vector>()
			|| Node->IsA<UMaterialExpressionConstant4Vector>())
		{
			return true;
		}
		if (const UMaterialExpressionTextureCoordinate* Coordinate = Cast<UMaterialExpressionTextureCoordinate>(Node))
		{
			UVSets.Add(Coordinate->CoordinateIndex);
			return true;
		}
		if (const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Node))
		{
			if (Sample->TextureObject.Expression)
			{
				Unsupported = TEXT("a texture sample fed by a texture object");
				Why = TEXT("its emissive samples a texture passed in as an object");
				return false;
			}
			UTexture* Texture = Sample->Texture;
			if (const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Node))
			{
				UTexture* Set = nullptr;
				if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Set) && Set)
				{
					Texture = Set;
				}
			}
			if (!Sample->Coordinates.Expression)
			{
				UVSets.Add(Sample->ConstCoordinate);
			}
			// Images null: only looking (which UV sets the graph reads), no pixels read.
			if (Images)
			{
				TSharedPtr<FImage32>& Image = Images->FindOrAdd(Texture);
				if (!Image.IsValid())
				{
					Image = MakeShared<FImage32>();
					if (!Image->Read(Texture))
					{
						Why = FString::Printf(TEXT("its texture %s has no pixels Mantle can read"), Texture ? *Texture->GetName() : TEXT("(none)"));
						return false;
					}
				}
				Samples.Add(Node, Image);
			}
			return Visit(&Sample->Coordinates, Material, SwitchedOn, Seen, Images, Why, Unsupported, Depth + 1);
		}
		const bool bKnown = Node->IsA<UMaterialExpressionReroute>() || Node->IsA<UMaterialExpressionMultiply>() || Node->IsA<UMaterialExpressionAdd>()
			|| Node->IsA<UMaterialExpressionSubtract>() || Node->IsA<UMaterialExpressionDivide>() || Node->IsA<UMaterialExpressionMax>() || Node->IsA<UMaterialExpressionMin>()
			|| Node->IsA<UMaterialExpressionLinearInterpolate>() || Node->IsA<UMaterialExpressionIf>() || Node->IsA<UMaterialExpressionOneMinus>()
			|| Node->IsA<UMaterialExpressionSaturate>() || Node->IsA<UMaterialExpressionClamp>() || Node->IsA<UMaterialExpressionComponentMask>()
			|| Node->IsA<UMaterialExpressionAppendVector>() || Node->IsA<UMaterialExpressionPower>();
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Node);
		if (!bKnown)
		{
			Unsupported = Node->GetClass()->GetName();
			Why = FString::Printf(TEXT("its emissive uses a %s node, which Mantle does not work out"), *Unsupported);
			if (Images)
			{
				return false;
			}
			// Only looking (ReadsUV): on through it, so a second UV set read behind it is still seen and the caller can say why it was not worked out.
			for (FExpressionInputIterator It{ Mutable }; It; ++It)
			{
				Visit(It.Input, Material, SwitchedOn, Seen, Images, Why, Unsupported, Depth + 1);
			}
			return true;
		}
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			if (!Visit(It.Input, Material, SwitchedOn, Seen, Images, Why, Unsupported, Depth + 1))
			{
				return false;
			}
		}
		return true;
	}

	/** An input's value, or Fallback when nothing is connected; the input's own channel mask applied. */
	FValue In(const FExpressionInput& Input, const float Fallback, TConstArrayView<FVector2f> UVs) const
	{
		if (!Input.Expression)
		{
			return FValue::Scalar(Fallback);
		}
		FValue Value = Node(Input.Expression, Input.OutputIndex, UVs);
		if (Input.Mask)
		{
			FValue Masked;
			Masked.N = 0;
			const int32 Flags[4] = { Input.MaskR, Input.MaskG, Input.MaskB, Input.MaskA };
			for (int32 I = 0; I < 4; ++I)
			{
				if (Flags[I])
				{
					Masked.C[Masked.N++] = Value.Get(I);
				}
			}
			Value = Masked.N > 0 ? Masked : Value;
		}
		return Value;
	}

	/** A node's output OutputIndex (that output's channel mask applied: a texture sample's R, a vector parameter's RGB). */
	FValue Node(const UMaterialExpression* Expression, const int32 OutputIndex, TConstArrayView<FVector2f> UVs) const
	{
		using namespace MantleMaterialEvalPrivate;
		FValue Out = Whole(Expression, UVs);
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		const TArray<FExpressionOutput>& Outputs = Mutable->GetOutputs();
		if (Outputs.IsValidIndex(OutputIndex) && Outputs[OutputIndex].Mask)
		{
			const FExpressionOutput& Output = Outputs[OutputIndex];
			FValue Masked;
			Masked.N = 0;
			const int32 Flags[4] = { Output.MaskR, Output.MaskG, Output.MaskB, Output.MaskA };
			for (int32 I = 0; I < 4; ++I)
			{
				if (Flags[I])
				{
					Masked.C[Masked.N++] = Out.Get(I);
				}
			}
			if (Masked.N > 0)
			{
				Out = Masked;
			}
		}
		return Out;
	}

	/** A node's full value, before its output's mask. */
	FValue Whole(const UMaterialExpression* E, TConstArrayView<FVector2f> UVs) const
	{
		using namespace MantleMaterialEvalPrivate;
		if (const FValue* Parameter = Parameters.Find(E))
		{
			return *Parameter;
		}
		if (const UMaterialExpressionConstant* C1 = Cast<UMaterialExpressionConstant>(E))
		{
			return FValue::Scalar(C1->R);
		}
		if (const UMaterialExpressionConstant2Vector* C2 = Cast<UMaterialExpressionConstant2Vector>(E))
		{
			FValue Out;
			Out.C[0] = C2->R;
			Out.C[1] = C2->G;
			Out.N = 2;
			return Out;
		}
		if (const UMaterialExpressionConstant3Vector* C3 = Cast<UMaterialExpressionConstant3Vector>(E))
		{
			FValue Out;
			Out.C[0] = C3->Constant.R;
			Out.C[1] = C3->Constant.G;
			Out.C[2] = C3->Constant.B;
			Out.N = 3;
			return Out;
		}
		if (const UMaterialExpressionConstant4Vector* C4 = Cast<UMaterialExpressionConstant4Vector>(E))
		{
			FValue Out;
			Out.C[0] = C4->Constant.R;
			Out.C[1] = C4->Constant.G;
			Out.C[2] = C4->Constant.B;
			Out.C[3] = C4->Constant.A;
			Out.N = 4;
			return Out;
		}
		if (const UMaterialExpressionTextureCoordinate* Coordinate = Cast<UMaterialExpressionTextureCoordinate>(E))
		{
			const FVector2f UV = UVs.IsValidIndex(Coordinate->CoordinateIndex) ? UVs[Coordinate->CoordinateIndex] : FVector2f::ZeroVector;
			FValue Out;
			Out.C[0] = UV.X * Coordinate->UTiling;
			Out.C[1] = UV.Y * Coordinate->VTiling;
			Out.N = 2;
			return Out;
		}
		if (const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(E))
		{
			FVector2f UV = UVs.IsValidIndex(Sample->ConstCoordinate) ? UVs[Sample->ConstCoordinate] : FVector2f::ZeroVector;
			if (Sample->Coordinates.Expression)
			{
				const FValue At = In(Sample->Coordinates, 0.f, UVs);
				UV = FVector2f(At.Get(0), At.Get(1));
			}
			const TSharedPtr<FImage32>* Image = Samples.Find(E);
			const FLinearColor Pixel = Image && Image->IsValid() ? (*Image)->At(UV) : FLinearColor::Black;
			FValue Out;
			Out.C[0] = Pixel.R;
			Out.C[1] = Pixel.G;
			Out.C[2] = Pixel.B;
			Out.C[3] = Pixel.A;
			Out.N = 4;
			return Out;
		}
		if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(E))
		{
			return In(Reroute->Input, 0.f, UVs);
		}
		if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(E))
		{
			return Each(In(Multiply->A, Multiply->ConstA, UVs), In(Multiply->B, Multiply->ConstB, UVs), [](float A, float B) { return A * B; });
		}
		if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(E))
		{
			return Each(In(Add->A, Add->ConstA, UVs), In(Add->B, Add->ConstB, UVs), [](float A, float B) { return A + B; });
		}
		if (const UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(E))
		{
			return Each(In(Subtract->A, Subtract->ConstA, UVs), In(Subtract->B, Subtract->ConstB, UVs), [](float A, float B) { return A - B; });
		}
		if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(E))
		{
			return Each(In(Divide->A, Divide->ConstA, UVs), In(Divide->B, Divide->ConstB, UVs), [](float A, float B) { return FMath::IsNearlyZero(B) ? 0.f : A / B; });
		}
		if (const UMaterialExpressionMax* Max = Cast<UMaterialExpressionMax>(E))
		{
			return Each(In(Max->A, Max->ConstA, UVs), In(Max->B, Max->ConstB, UVs), [](float A, float B) { return FMath::Max(A, B); });
		}
		if (const UMaterialExpressionMin* Min = Cast<UMaterialExpressionMin>(E))
		{
			return Each(In(Min->A, Min->ConstA, UVs), In(Min->B, Min->ConstB, UVs), [](float A, float B) { return FMath::Min(A, B); });
		}
		if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(E))
		{
			const FValue A = In(Lerp->A, Lerp->ConstA, UVs);
			const FValue B = In(Lerp->B, Lerp->ConstB, UVs);
			const FValue Alpha = In(Lerp->Alpha, Lerp->ConstAlpha, UVs);
			FValue Out;
			Out.N = FMath::Max3(A.N, B.N, Alpha.N);
			for (int32 I = 0; I < Out.N; ++I)
			{
				Out.C[I] = A.Get(I) + (B.Get(I) - A.Get(I)) * Alpha.Get(I);
			}
			return Out;
		}
		if (const UMaterialExpressionIf* If = Cast<UMaterialExpressionIf>(E))
		{
			// As the engine compiles it: with A Equals B connected, within EqualsThreshold is that branch; without it, A >= B takes A > B's.
			const float A = In(If->A, 0.f, UVs).Get(0);
			const float B = In(If->B, If->ConstB, UVs).Get(0);
			if (If->AEqualsB.Expression)
			{
				if (FMath::Abs(A - B) <= If->EqualsThreshold)
				{
					return In(If->AEqualsB, 0.f, UVs);
				}
				return A > B ? In(If->AGreaterThanB, 0.f, UVs) : In(If->ALessThanB, 0.f, UVs);
			}
			return A >= B ? In(If->AGreaterThanB, 0.f, UVs) : In(If->ALessThanB, 0.f, UVs);
		}
		if (const UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(E))
		{
			return Each(In(OneMinus->Input, 0.f, UVs), [](float X) { return 1.f - X; });
		}
		if (const UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(E))
		{
			return Each(In(Saturate->Input, 0.f, UVs), [](float X) { return FMath::Clamp(X, 0.f, 1.f); });
		}
		if (const UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(E))
		{
			const FValue X = In(Clamp->Input, 0.f, UVs);
			const FValue Lo = In(Clamp->Min, Clamp->MinDefault, UVs);
			const FValue Hi = In(Clamp->Max, Clamp->MaxDefault, UVs);
			FValue Out = X;
			for (int32 I = 0; I < Out.N; ++I)
			{
				float V = X.C[I];
				if (Clamp->ClampMode == CMODE_Clamp || Clamp->ClampMode == CMODE_ClampMin)
				{
					V = FMath::Max(V, Lo.Get(I));
				}
				if (Clamp->ClampMode == CMODE_Clamp || Clamp->ClampMode == CMODE_ClampMax)
				{
					V = FMath::Min(V, Hi.Get(I));
				}
				Out.C[I] = V;
			}
			return Out;
		}
		if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(E))
		{
			const FValue X = In(Mask->Input, 0.f, UVs);
			FValue Out;
			Out.N = 0;
			const bool Flags[4] = { Mask->R != 0, Mask->G != 0, Mask->B != 0, Mask->A != 0 };
			for (int32 I = 0; I < 4; ++I)
			{
				if (Flags[I])
				{
					Out.C[Out.N++] = X.Get(I);
				}
			}
			return Out.N > 0 ? Out : FValue::Scalar(0.f);
		}
		if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(E))
		{
			const FValue A = In(Append->A, 0.f, UVs);
			const FValue B = In(Append->B, 0.f, UVs);
			FValue Out = A;
			for (int32 I = 0; I < B.N && Out.N < 4; ++I)
			{
				Out.C[Out.N++] = B.C[I];
			}
			return Out;
		}
		if (const UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(E))
		{
			return Each(In(Power->Base, 0.f, UVs), In(Power->Exponent, Power->ConstExponent, UVs), [](float A, float B) { return FMath::Pow(FMath::Max(A, 0.f), B); });
		}
		return FValue::Scalar(0.f);
	}
};

FMantleEmissiveEval::FMantleEmissiveEval() = default;
FMantleEmissiveEval::~FMantleEmissiveEval() = default;

bool FMantleEmissiveEval::Prepare(const UMaterialInterface* Material, const TArray<FName>& SwitchedOn, FString& OutWhy)
{
	Impl = MakeUnique<FImpl>();
	Unsupported.Reset();
	UMaterial* Base = Material ? const_cast<UMaterial*>(Material->GetMaterial_Concurrent()) : nullptr;
	Impl->Emissive = Base ? Base->GetExpressionInputForProperty(MP_EmissiveColor) : nullptr;
	if (!Impl->Emissive || !Impl->Emissive->Expression)
	{
		OutWhy = TEXT("it has no emissive");
		return false;
	}
	TSet<const UMaterialExpression*> Seen;
	TMap<UTexture*, TSharedPtr<MantleMaterialEvalPrivate::FImage32>> Images;
	return Impl->Visit(Impl->Emissive, Material, SwitchedOn, Seen, &Images, OutWhy, Unsupported, 0);
}

bool FMantleEmissiveEval::ReadsUV(const UMaterialInterface* Material, const int32 Index)
{
	UMaterial* Base = Material ? const_cast<UMaterial*>(Material->GetMaterial_Concurrent()) : nullptr;
	const FExpressionInput* Emissive = Base ? Base->GetExpressionInputForProperty(MP_EmissiveColor) : nullptr;
	if (!Emissive || !Emissive->Expression)
	{
		return false;
	}
	FImpl Look;
	TSet<const UMaterialExpression*> Seen;
	FString Why;
	FString Unsupported;
	Look.Visit(Emissive, Material, TArray<FName>(), Seen, nullptr, Why, Unsupported, 0);
	return Look.UVSets.Contains(Index);
}

bool FMantleEmissiveEval::UsesUV(const int32 Index) const
{
	return Impl.IsValid() && Impl->UVSets.Contains(Index);
}

FLinearColor FMantleEmissiveEval::At(TConstArrayView<FVector2f> UVs) const
{
	if (!Impl.IsValid() || !Impl->Emissive)
	{
		return FLinearColor::Black;
	}
	const MantleMaterialEvalPrivate::FValue Value = Impl->In(*Impl->Emissive, 0.f, UVs);
	return FLinearColor(Value.Get(0), Value.Get(1), Value.Get(2));
}
