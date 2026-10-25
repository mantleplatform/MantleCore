// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlowMaterial.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGlowMaterialPrivate
{
	/** Enough for any real emissive graph; the bound also stops a malformed cycle. */
	constexpr int32 NodeBudget = 4000;

	/** How deep a call's function is followed into the functions it calls. */
	constexpr int32 MaxFunctionDepth = 4;

	/** The node an input reads through reroutes; null when nothing is connected. */
	const UMaterialExpression* Through(const FExpressionInput* Input)
	{
		for (int32 Depth = 0; Input && Input->Expression && Depth < 32; ++Depth)
		{
			if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(Input->Expression))
			{
				Input = &Reroute->Input;
				continue;
			}
			return Input->Expression;
		}
		return nullptr;
	}

	/** Every node an emissive graph reaches, and whether one is a Scene Color or a Fresnel. */
	struct FGraphScan
	{
		TSet<const UMaterialExpression*> Seen;
		int32 Budget = NodeBudget;
		bool bSceneColor = false;
		bool bFresnel = false;

		void ScanInput(const FExpressionInput* Input, const int32 FunctionDepth)
		{
			if (Input && Input->Expression)
			{
				ScanExpression(Input->Expression, FunctionDepth);
			}
		}

		void ScanExpression(const UMaterialExpression* Expression, const int32 FunctionDepth)
		{
			if (!Expression || --Budget < 0 || Seen.Contains(Expression))
			{
				return;
			}
			Seen.Add(Expression);
			bSceneColor = bSceneColor || Expression->IsA<UMaterialExpressionSceneColor>();
			bFresnel = bFresnel || Expression->IsA<UMaterialExpressionFresnel>();
			if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				if (Call->MaterialFunction)
				{
					bFresnel = bFresnel || Call->MaterialFunction->GetName().Contains(TEXT("Fresnel"));
					if (FunctionDepth < MaxFunctionDepth)
					{
						// What a call reaches inside its function: from the outputs back.
						for (const TObjectPtr<UMaterialExpression>& Inner : Call->MaterialFunction->GetExpressions())
						{
							if (Inner && Inner->IsA<UMaterialExpressionFunctionOutput>())
							{
								ScanExpression(Inner, FunctionDepth + 1);
							}
						}
					}
				}
			}
			UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
			for (FExpressionInputIterator It{ Mutable }; It; ++It)
			{
				ScanInput(It.Input, FunctionDepth);
			}
		}
	};

	/** A colour node's value as Context sets it: a Vector Parameter's (the instance's own, else its default) or a Constant3Vector's. */
	bool ColourOf(const UMaterialExpression* Expression, const UMaterialInterface* Context, FLinearColor& Out)
	{
		if (const UMaterialExpressionVectorParameter* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression))
		{
			Out = Parameter->DefaultValue;
			FLinearColor Set;
			if (Context && Context->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Set))
			{
				Out = Set;
			}
			return true;
		}
		if (const UMaterialExpressionConstant3Vector* Constant = Cast<UMaterialExpressionConstant3Vector>(Expression))
		{
			Out = Constant->Constant;
			return true;
		}
		return false;
	}

	/** A constant or a Scalar Parameter, as Context sets it. */
	bool ScalarOf(const UMaterialExpression* Expression, const UMaterialInterface* Context, double& Out)
	{
		if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
		{
			Out = Constant->R;
			return true;
		}
		if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			float Value = Parameter->DefaultValue;
			float Set = 0.0f;
			if (Context && Context->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Set))
			{
				Value = Set;
			}
			Out = Value;
			return true;
		}
		return false;
	}

	/**
	 * The first colour node in pre-order, through the material's own graph (not into functions); only a Vector Parameter
	 * when bParametersOnly (a tint is an exposed parameter far more often than a constant, which is as often a zero or a
	 * one used in the maths).
	 */
	const UMaterialExpression* FirstColour(const FExpressionInput* Input, const UMaterialInterface* Context, const bool bParametersOnly,
		TSet<const UMaterialExpression*>& Seen, int32& Budget)
	{
		const UMaterialExpression* Expression = Through(Input);
		if (!Expression || --Budget < 0 || Seen.Contains(Expression))
		{
			return nullptr;
		}
		Seen.Add(Expression);
		FLinearColor Unused;
		if ((!bParametersOnly || Expression->IsA<UMaterialExpressionVectorParameter>()) && ColourOf(Expression, Context, Unused))
		{
			return Expression;
		}
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			if (const UMaterialExpression* Found = FirstColour(It.Input, Context, bParametersOnly, Seen, Budget))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** Whether Target is in the graph Input reads, through the material's own graph. */
	bool Reaches(const FExpressionInput* Input, const UMaterialExpression* Target, TSet<const UMaterialExpression*>& Seen, int32& Budget)
	{
		const UMaterialExpression* Expression = Through(Input);
		if (!Expression || --Budget < 0 || Seen.Contains(Expression))
		{
			return false;
		}
		if (Expression == Target)
		{
			return true;
		}
		Seen.Add(Expression);
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			if (Reaches(It.Input, Target, Seen, Budget))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The scalar the colour Target is multiplied by: the first Multiply (in pre-order from Input) with Target on one side
	 * and a constant or Scalar Parameter on the other.
	 */
	bool IntensityOf(const FExpressionInput* Input, const UMaterialExpression* Target, const UMaterialInterface* Context,
		TSet<const UMaterialExpression*>& Seen, int32& Budget, double& Out)
	{
		const UMaterialExpression* Expression = Through(Input);
		if (!Expression || --Budget < 0 || Seen.Contains(Expression))
		{
			return false;
		}
		Seen.Add(Expression);
		if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
		{
			for (const bool bTargetIsA : { true, false })
			{
				const FExpressionInput& WithTarget = bTargetIsA ? Multiply->A : Multiply->B;
				const FExpressionInput& Other = bTargetIsA ? Multiply->B : Multiply->A;
				TSet<const UMaterialExpression*> Inside;
				int32 InsideBudget = NodeBudget;
				if (Reaches(&WithTarget, Target, Inside, InsideBudget) && ScalarOf(Through(&Other), Context, Out))
				{
					return true;
				}
			}
		}
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			if (IntensityOf(It.Input, Target, Context, Seen, Budget, Out))
			{
				return true;
			}
		}
		return false;
	}
}

void FMantleSeeThroughGlass::Glow(double (&Out)[3]) const
{
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		Out[Channel] = FMath::Clamp(Tint[Channel] * Intensity * GlowScale, 0.0, 1.0);
	}
}

bool FMantleGlowMaterial::RecogniseSceneColourGlass(const UMaterialInterface* Context, const FExpressionInput* Emissive, FMantleSeeThroughGlass& Out)
{
	using namespace MantleGlowMaterialPrivate;

	Out = FMantleSeeThroughGlass();
	if (!Emissive || !Emissive->Expression)
	{
		return false;
	}
	FGraphScan Scan;
	Scan.ScanInput(Emissive, 0);
	if (!Scan.bSceneColor)
	{
		return false;
	}
	Out.bFound = true;

	const UMaterialExpression* Colour = nullptr;
	for (const bool bParametersOnly : { true, false })
	{
		TSet<const UMaterialExpression*> Seen;
		int32 Budget = NodeBudget;
		Colour = FirstColour(Emissive, Context, bParametersOnly, Seen, Budget);
		if (Colour)
		{
			break;
		}
	}
	FLinearColor Value;
	if (Colour && ColourOf(Colour, Context, Value))
	{
		Out.bTint = true;
		Out.Tint[0] = FMath::Max(Value.R, 0.0f);
		Out.Tint[1] = FMath::Max(Value.G, 0.0f);
		Out.Tint[2] = FMath::Max(Value.B, 0.0f);
		TSet<const UMaterialExpression*> Visited;
		int32 IntensityBudget = NodeBudget;
		double Intensity = 1.0;
		if (IntensityOf(Emissive, Colour, Context, Visited, IntensityBudget, Intensity) && FMath::IsFinite(Intensity))
		{
			Out.Intensity = FMath::Max(Intensity, 0.0);
		}
	}
	return true;
}

bool FMantleGlowMaterial::HasFresnel(const FExpressionInput* Emissive)
{
	using namespace MantleGlowMaterialPrivate;

	if (!Emissive || !Emissive->Expression)
	{
		return false;
	}
	FGraphScan Scan;
	Scan.ScanInput(Emissive, 0);
	return Scan.bFresnel;
}
