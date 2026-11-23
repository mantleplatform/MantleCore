// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlowMaterial.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGlowMaterialPrivate
{
	/** Enough for any real emissive graph; the bound also stops a malformed cycle. */
	constexpr int32 NodeBudget = 4000;

	/** How deep a call's function is followed into the functions it calls. */
	constexpr int32 MaxFunctionDepth = 4;

	/** The node an input reads through reroutes, and which of its outputs; null when nothing is connected. */
	const UMaterialExpression* ThroughPin(const FExpressionInput* Input, int32& OutputIndex)
	{
		OutputIndex = 0;
		for (int32 Depth = 0; Input && Input->Expression && Depth < 32; ++Depth)
		{
			if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(Input->Expression))
			{
				Input = &Reroute->Input;
				continue;
			}
			OutputIndex = Input->OutputIndex;
			return Input->Expression;
		}
		return nullptr;
	}

	/** The node an input reads through reroutes; null when nothing is connected. */
	const UMaterialExpression* Through(const FExpressionInput* Input)
	{
		int32 OutputIndex = 0;
		return ThroughPin(Input, OutputIndex);
	}

	/** What feeds one output of a function call: the call's inputs that output depends on, and the nodes inside the function on the way. */
	struct FCallFeed
	{
		TArray<const FExpressionInput*> Inputs;
		TArray<const UMaterialExpression*> Inside;
	};

	/**
	 * Follows one output of Call back through its function (and the functions that one calls) to the function inputs it reads, and returns the call's inputs for
	 * those. A glTF material is one call of this kind: its Emissive Color output reads the emissive texture and factor, not the base color, normal or anisotropy
	 * that go into the same call. False when the function cannot be followed (none set, no such output): the caller then takes every input.
	 */
	bool TraceCall(const UMaterialExpressionMaterialFunctionCall* Call, const int32 OutputIndex, const int32 FunctionDepth, FCallFeed& Out)
	{
		if (!Call->MaterialFunction || FunctionDepth >= MaxFunctionDepth)
		{
			return false;
		}
		// The output by name when the call lists it (its outputs and the function's can be ordered alike), else by position.
		int32 Which = OutputIndex;
		if (Call->Outputs.IsValidIndex(OutputIndex))
		{
			for (int32 Each = 0; Each < Call->FunctionOutputs.Num(); ++Each)
			{
				if (Call->FunctionOutputs[Each].ExpressionOutput && Call->FunctionOutputs[Each].ExpressionOutput->OutputName == Call->Outputs[OutputIndex].OutputName)
				{
					Which = Each;
					break;
				}
			}
		}
		if (!Call->FunctionOutputs.IsValidIndex(Which))
		{
			return false;
		}
		// The call's links to the function's nodes are not saved: they are made again when the material loads, and found here by id when they are not there.
		const UMaterialExpressionFunctionOutput* FunctionOutput = Call->FunctionOutputs[Which].ExpressionOutput;
		TArray<const UMaterialExpressionFunctionInput*> FunctionInputs;
		for (const TObjectPtr<UMaterialExpression>& Inner : Call->MaterialFunction->GetExpressions())
		{
			if (const UMaterialExpressionFunctionOutput* Output = Cast<UMaterialExpressionFunctionOutput>(Inner))
			{
				if (!FunctionOutput && Output->Id == Call->FunctionOutputs[Which].ExpressionOutputId)
				{
					FunctionOutput = Output;
				}
			}
			else if (const UMaterialExpressionFunctionInput* Input = Cast<UMaterialExpressionFunctionInput>(Inner))
			{
				FunctionInputs.Add(Input);
			}
		}
		if (!FunctionOutput)
		{
			return false;
		}
		TSet<const UMaterialExpressionFunctionInput*> Reached;
		TSet<TTuple<const UMaterialExpression*, int32>> Seen;
		int32 Budget = NodeBudget;
		TFunction<void(const FExpressionInput*)> Trace = [&](const FExpressionInput* Input)
		{
			int32 Pin = 0;
			const UMaterialExpression* Expression = ThroughPin(Input, Pin);
			bool bAlready = false;
			if (!Expression || --Budget < 0)
			{
				return;
			}
			Seen.Add(MakeTuple(Expression, Pin), &bAlready);
			if (bAlready)
			{
				return;
			}
			Out.Inside.Add(Expression);
			if (const UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				Reached.Add(FunctionInput);
				return;
			}
			if (const UMaterialExpressionMaterialFunctionCall* Nested = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				FCallFeed Inner;
				if (TraceCall(Nested, Pin, FunctionDepth + 1, Inner))
				{
					Out.Inside.Append(Inner.Inside);
					for (const FExpressionInput* Feed : Inner.Inputs)
					{
						Trace(Feed);
					}
					return;
				}
			}
			UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
			for (FExpressionInputIterator It{ Mutable }; It; ++It)
			{
				Trace(It.Input);
			}
		};
		Trace(&FunctionOutput->A);
		for (const FFunctionExpressionInput& CallInput : Call->FunctionInputs)
		{
			const UMaterialExpressionFunctionInput* Node = CallInput.ExpressionInput;
			if (!Node)
			{
				const UMaterialExpressionFunctionInput* const* Found = FunctionInputs.FindByPredicate([&CallInput](const UMaterialExpressionFunctionInput* Each) { return Each->Id == CallInput.ExpressionInputId; });
				Node = Found ? *Found : nullptr;
			}
			if (Node && Reached.Contains(Node))
			{
				Out.Inputs.Add(&CallInput.Input);
			}
		}
		return true;
	}

	/** The inputs of the node Input reads that feed the output Input reads (all of them, unless it is a function call whose function can be followed). */
	void FeedingInputs(const FExpressionInput* Input, TArray<const FExpressionInput*>& Out, TArray<const UMaterialExpression*>* Inside = nullptr)
	{
		int32 Pin = 0;
		const UMaterialExpression* Expression = ThroughPin(Input, Pin);
		if (!Expression)
		{
			return;
		}
		if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
		{
			FCallFeed Feed;
			if (TraceCall(Call, Pin, 0, Feed))
			{
				Out.Append(Feed.Inputs);
				if (Inside)
				{
					Inside->Append(Feed.Inside);
				}
				return;
			}
		}
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			Out.Add(It.Input);
		}
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

	/** A color node's value as Context sets it: a Vector Parameter's (the instance's own, else its default) or a Constant3Vector's. */
	bool ColorOf(const UMaterialExpression* Expression, const UMaterialInterface* Context, FLinearColor& Out)
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

	/** Whether a Scalar Parameter's name is a switch for the glow, and not what places a texture (a glTF material's EmissiveTexture_TexCoord is 0 for the first UV set). */
	bool IsGlowSwitchName(const FName Name)
	{
		const FString Text = Name.ToString();
		return !Text.Contains(TEXT("TexCoord")) && !Text.Contains(TEXT("Rotation")) && !Text.Contains(TEXT("Tiling")) && !Text.Contains(TEXT("UVSet"));
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
	 * The first color node in pre-order, through the material's own graph (not into functions); only a Vector Parameter
	 * when bParametersOnly (a tint is an exposed parameter far more often than a constant, which is as often a zero or a
	 * one used in the maths).
	 */
	const UMaterialExpression* FirstColor(const FExpressionInput* Input, const UMaterialInterface* Context, const bool bParametersOnly,
		TSet<const UMaterialExpression*>& Seen, int32& Budget)
	{
		const UMaterialExpression* Expression = Through(Input);
		if (!Expression || --Budget < 0 || Seen.Contains(Expression))
		{
			return nullptr;
		}
		Seen.Add(Expression);
		FLinearColor Unused;
		if ((!bParametersOnly || Expression->IsA<UMaterialExpressionVectorParameter>()) && ColorOf(Expression, Context, Unused))
		{
			return Expression;
		}
		TArray<const FExpressionInput*> Feeds;
		FeedingInputs(Input, Feeds);
		for (const FExpressionInput* Feed : Feeds)
		{
			if (const UMaterialExpression* Found = FirstColor(Feed, Context, bParametersOnly, Seen, Budget))
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
	 * The scalar the color Target is multiplied by: the first Multiply (in pre-order from Input) with Target on one side
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

	/** Why a rim is dropped when what multiplies its Fresnel is not one constant (F4). */
	const TCHAR* const RimVaries = TEXT("the rim is multiplied by a texture or another value that changes across the surface, and Mantle's rim glow is one flat color");

	/** One value of an additive output as the rim reader sees it (F4): Base + K x F with F the Fresnel, or a known constant, or neither. */
	struct FRimValue
	{
		/** The value depends on the Fresnel; K is what multiplies it (the rest, Base, is the bake's). */
		bool bFresnel = false;
		double K[3] = { 0.0, 0.0, 0.0 };

		/** Without bFresnel: the value is one known constant, C, all over the surface. */
		bool bConstant = false;
		double C[3] = { 0.0, 0.0, 0.0 };

		static FRimValue Constant(const double Value)
		{
			FRimValue Out;
			Out.bConstant = true;
			Out.C[0] = Out.C[1] = Out.C[2] = Value;
			return Out;
		}
	};

	/** Reads an additive output as Base + K x F (FMantleGlowMaterial::ReadRim). The first reason it cannot is kept in Why. */
	struct FRimReader
	{
		const UMaterialInterface* Context = nullptr;
		const UMaterialExpressionFresnel* Fresnel = nullptr;
		double Exponent = 5.0;
		double BaseReflect = 0.04;
		bool bNormalPin = false;
		FString Why;
		int32 Budget = NodeBudget;
		TMap<const UMaterialExpression*, bool> FresnelBelow;

		bool Fail(const FString& Reason)
		{
			if (Why.IsEmpty())
			{
				Why = Reason;
			}
			return false;
		}

		/** Whether the node Input reads has a Fresnel at or below it (into the functions it calls), remembered per node. */
		bool ReachesFresnel(const FExpressionInput* Input, const UMaterialExpression* Expression)
		{
			if (const bool* Known = FresnelBelow.Find(Expression))
			{
				return *Known;
			}
			FGraphScan Scan;
			Scan.ScanInput(Input, 0);
			FresnelBelow.Add(Expression, Scan.bFresnel);
			return Scan.bFresnel;
		}

		/** A constant node's value at Output (0 the color, 1..3 one channel, 4 alpha), as Context sets it. */
		bool ConstantOf(const UMaterialExpression* Expression, const int32 Output, double (&Out)[3]) const
		{
			double Scalar = 0.0;
			if (ScalarOf(Expression, Context, Scalar))
			{
				Out[0] = Out[1] = Out[2] = Scalar;
				return true;
			}
			FLinearColor Color;
			if (!ColorOf(Expression, Context, Color))
			{
				const UMaterialExpressionConstant4Vector* Four = Cast<UMaterialExpressionConstant4Vector>(Expression);
				if (!Four)
				{
					return false;
				}
				Color = Four->Constant;
			}
			if (Output == 0)
			{
				Out[0] = Color.R;
				Out[1] = Color.G;
				Out[2] = Color.B;
				return true;
			}
			if (Output < 1 || Output > 4)
			{
				return false;
			}
			const float Channel = Output == 1 ? Color.R : Output == 2 ? Color.G : Output == 3 ? Color.B : Color.A;
			Out[0] = Out[1] = Out[2] = Channel;
			return true;
		}

		/** A scalar pin of the Fresnel node: its constant (or Scalar Parameter) when wired, else Default. */
		bool FresnelSetting(const FExpressionInput& Pin, const float Default, double& Out) const
		{
			if (!Pin.Expression)
			{
				Out = Default;
				return true;
			}
			int32 Output = 0;
			double Value[3];
			const UMaterialExpression* Expression = ThroughPin(&Pin, Output);
			if (!Expression || !ConstantOf(Expression, Output, Value))
			{
				return false;
			}
			Out = Value[0];
			return true;
		}

		bool ReadFresnel(const UMaterialExpressionFresnel* Node, FRimValue& Out)
		{
			double NodeExponent = 0.0;
			double NodeBaseReflect = 0.0;
			if (!FresnelSetting(Node->ExponentIn, Node->Exponent, NodeExponent))
			{
				return Fail(TEXT("its Fresnel's exponent is not a constant"));
			}
			if (!FresnelSetting(Node->BaseReflectFractionIn, Node->BaseReflectFraction, NodeBaseReflect))
			{
				return Fail(TEXT("its Fresnel's base reflect fraction is not a constant"));
			}
			if (Fresnel && Fresnel != Node && (!FMath::IsNearlyEqual(Exponent, NodeExponent) || !FMath::IsNearlyEqual(BaseReflect, NodeBaseReflect)))
			{
				return Fail(TEXT("it has two Fresnels with different settings, and Mantle's rim glow has one"));
			}
			Fresnel = Node;
			Exponent = NodeExponent;
			BaseReflect = NodeBaseReflect;
			bNormalPin = bNormalPin || Node->Normal.Expression != nullptr;
			Out = FRimValue();
			Out.bFresnel = true;
			Out.K[0] = Out.K[1] = Out.K[2] = 1.0;
			return true;
		}

		/** A pin with nothing connected reads Fallback (a node's ConstA, ConstB or ConstAlpha). */
		bool Read(const FExpressionInput* Input, const double Fallback, FRimValue& Out)
		{
			if (!Input || !Input->Expression)
			{
				Out = FRimValue::Constant(Fallback);
				return true;
			}
			return Read(Input, Out);
		}

		bool Read(const FExpressionInput* Input, FRimValue& Out)
		{
			Out = FRimValue();
			if (--Budget < 0)
			{
				return Fail(TEXT("its glow graph is too large to read"));
			}
			int32 Pin = 0;
			const UMaterialExpression* Expression = ThroughPin(Input, Pin);
			if (!Expression)
			{
				Out = FRimValue::Constant(0.0);
				return true;
			}
			if (!ReachesFresnel(Input, Expression))
			{
				Out.bConstant = ConstantOf(Expression, Pin, Out.C);
				return true;
			}
			if (const UMaterialExpressionFresnel* Node = Cast<UMaterialExpressionFresnel>(Expression))
			{
				return ReadFresnel(Node, Out);
			}
			FRimValue A;
			FRimValue B;
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				return Read(&Multiply->A, Multiply->ConstA, A) && Read(&Multiply->B, Multiply->ConstB, B) && Product(A, B, Out);
			}
			if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				if (!Read(&Divide->A, Divide->ConstA, A) || !Read(&Divide->B, Divide->ConstB, B))
				{
					return false;
				}
				if (B.bFresnel)
				{
					return Fail(TEXT("its glow is divided by a Fresnel"));
				}
				if (!B.bConstant || B.C[0] == 0.0 || B.C[1] == 0.0 || B.C[2] == 0.0)
				{
					return Fail(RimVaries);
				}
				Out.bFresnel = true;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.K[Channel] = A.K[Channel] / B.C[Channel];
				}
				return true;
			}
			const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression);
			const UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression);
			if (Add || Subtract)
			{
				// Whatever has no Fresnel is the bake's (its K is 0), so a texture beside the rim is fine.
				const bool bRead = Add ? Read(&Add->A, Add->ConstA, A) && Read(&Add->B, Add->ConstB, B)
					: Read(&Subtract->A, Subtract->ConstA, A) && Read(&Subtract->B, Subtract->ConstB, B);
				if (!bRead)
				{
					return false;
				}
				Out.bFresnel = true;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.K[Channel] = Add ? A.K[Channel] + B.K[Channel] : A.K[Channel] - B.K[Channel];
				}
				return true;
			}
			if (const UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(Expression))
			{
				if (!Read(&OneMinus->Input, 0.0, A))
				{
					return false;
				}
				Out.bFresnel = true;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.K[Channel] = -A.K[Channel];
				}
				return true;
			}
			if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
			{
				FRimValue Alpha;
				if (!Read(&Lerp->A, Lerp->ConstA, A) || !Read(&Lerp->B, Lerp->ConstB, B) || !Read(&Lerp->Alpha, Lerp->ConstAlpha, Alpha))
				{
					return false;
				}
				Out.bFresnel = true;
				if (Alpha.bFresnel)
				{
					// A + (B - A) x Alpha: the rim is (B - A) times the Fresnel's part of Alpha.
					if (A.bFresnel || B.bFresnel)
					{
						return Fail(TEXT("it blends a Fresnel by a Fresnel"));
					}
					if (!A.bConstant || !B.bConstant)
					{
						return Fail(RimVaries);
					}
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						Out.K[Channel] = (B.C[Channel] - A.C[Channel]) * Alpha.K[Channel];
					}
					return true;
				}
				if (!Alpha.bConstant)
				{
					return Fail(RimVaries);
				}
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.K[Channel] = A.K[Channel] * (1.0 - Alpha.C[Channel]) + B.K[Channel] * Alpha.C[Channel];
				}
				return true;
			}
			if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				return Fail(FString::Printf(TEXT("its Fresnel is inside a material function (%s), which Mantle does not read"),
					Call->MaterialFunction ? *Call->MaterialFunction->GetName() : TEXT("none set")));
			}
			FString Kind = Expression->GetClass()->GetName();
			Kind.RemoveFromStart(TEXT("MaterialExpression"));
			return Fail(FString::Printf(TEXT("its Fresnel goes through a %s node, which Mantle does not read"), *Kind));
		}

		/** A x B, one of them the Fresnel's and the other a constant. */
		bool Product(const FRimValue& A, const FRimValue& B, FRimValue& Out)
		{
			Out = FRimValue();
			if (!A.bFresnel && !B.bFresnel)
			{
				Out.bConstant = A.bConstant && B.bConstant;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.C[Channel] = A.C[Channel] * B.C[Channel];
				}
				return true;
			}
			if (A.bFresnel && B.bFresnel)
			{
				return Fail(TEXT("its Fresnel is multiplied by another Fresnel"));
			}
			const FRimValue& WithFresnel = A.bFresnel ? A : B;
			const FRimValue& Other = A.bFresnel ? B : A;
			if (!Other.bConstant)
			{
				return Fail(RimVaries);
			}
			Out.bFresnel = true;
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Out.K[Channel] = WithFresnel.K[Channel] * Other.C[Channel];
			}
			return true;
		}
	};
}

void FMantleSeeThroughGlass::Glow(double (&Out)[3]) const
{
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		Out[Channel] = FMath::Clamp(Tint[Channel] * Intensity * GlowScale, 0.0, 1.0);
	}
}

bool FMantleGlowMaterial::RecognizeSceneColorGlass(const UMaterialInterface* Context, const FExpressionInput* Emissive, FMantleSeeThroughGlass& Out)
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

	const UMaterialExpression* Color = nullptr;
	for (const bool bParametersOnly : { true, false })
	{
		TSet<const UMaterialExpression*> Seen;
		int32 Budget = NodeBudget;
		Color = FirstColor(Emissive, Context, bParametersOnly, Seen, Budget);
		if (Color)
		{
			break;
		}
	}
	FLinearColor Value;
	if (Color && ColorOf(Color, Context, Value))
	{
		Out.bTint = true;
		Out.Tint[0] = FMath::Max(Value.R, 0.0f);
		Out.Tint[1] = FMath::Max(Value.G, 0.0f);
		Out.Tint[2] = FMath::Max(Value.B, 0.0f);
		TSet<const UMaterialExpression*> Visited;
		int32 IntensityBudget = NodeBudget;
		double Intensity = 1.0;
		if (IntensityOf(Emissive, Color, Context, Visited, IntensityBudget, Intensity) && FMath::IsFinite(Intensity))
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

bool FMantleGlowMaterial::ReadRim(const UMaterialInterface* Context, const FExpressionInput* Emissive, const FExpressionInput* Opacity, FMantleRimGlow& Out)
{
	using namespace MantleGlowMaterialPrivate;

	Out = FMantleRimGlow();
	const bool bInGlow = HasFresnel(Emissive);
	const bool bInOpacity = HasFresnel(Opacity);
	if (!bInGlow && !bInOpacity)
	{
		return false;
	}
	if (bInGlow && bInOpacity)
	{
		Out.Why = TEXT("it has a Fresnel in both its glow and its opacity, and Mantle's rim glow reads one");
		return false;
	}

	// Additive adds emissive x opacity, so the rim is the Fresnel's part of that product.
	FRimReader Reader;
	Reader.Context = Context;
	FRimValue Glow;
	FRimValue Fade;
	if (!Reader.Read(Emissive, 0.0, Glow) || !Reader.Read(Opacity, 1.0, Fade))
	{
		Out.Why = Reader.Why;
		return false;
	}
	if (bInGlow && !Fade.bConstant)
	{
		Out.Why = TEXT("its opacity changes across the surface (a texture), and Mantle's rim glow is one flat color");
		return false;
	}
	FRimValue Total;
	if (!Reader.Product(Glow, Fade, Total))
	{
		Out.Why = Reader.Why;
		return false;
	}

	// What the face-on bake lacks: K x (1 - BRF). A base reflect fraction of 1 makes the Fresnel a constant 1, all of it baked.
	const double Missing = 1.0 - Reader.BaseReflect;
	double Color[3];
	bool bDarker = false;
	bool bAny = false;
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		Color[Channel] = Total.K[Channel] * Missing;
		bDarker = bDarker || Color[Channel] < -1e-6;
		bAny = bAny || Color[Channel] > 1e-6;
	}
	if (bDarker)
	{
		Out.Why = TEXT("its edges are darker than its face (a 1 - Fresnel, or a fade toward the edges), and Mantle's rim glow can only brighten them");
		return false;
	}
	if (!bAny || !FMath::IsFinite(Reader.Exponent))
	{
		// Nothing past the face-on value: the bake holds all of it, and nothing is lost.
		return false;
	}
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		Out.Color[Channel] = FMath::Max(Color[Channel], 0.0);
	}
	Out.Power = Reader.Exponent;
	Out.bNormalPin = Reader.bNormalPin;
	Out.bFound = true;
	return true;
}

bool FMantleMaterialGlow::Read(const UMaterialInterface* Material, FMantleMaterialGlow& Out)
{
	using namespace MantleGlowMaterialPrivate;

	Out = FMantleMaterialGlow();
	UMaterial* Base = Material ? const_cast<UMaterial*>(Material->GetMaterial_Concurrent()) : nullptr;
	const FExpressionInput* Emissive = Base ? Base->GetExpressionInputForProperty(MP_EmissiveColor) : nullptr;
	if (!Emissive || !Emissive->Expression)
	{
		return false;
	}
	// What feeds the emissive output, through the functions it calls by the pin it reads (so a glTF material's base color and normal are not on the path).
	TSet<const UMaterialExpression*> Reached;
	{
		int32 ReachBudget = NodeBudget;
		TFunction<void(const FExpressionInput*)> Reach = [&](const FExpressionInput* Input)
		{
			const UMaterialExpression* Expression = Through(Input);
			if (!Expression || --ReachBudget < 0 || Reached.Contains(Expression))
			{
				return;
			}
			Reached.Add(Expression);
			TArray<const FExpressionInput*> Feeds;
			TArray<const UMaterialExpression*> Inside;
			FeedingInputs(Input, Feeds, &Inside);
			Reached.Append(Inside);
			for (const FExpressionInput* Feed : Feeds)
			{
				Reach(Feed);
			}
		};
		Reach(Emissive);
	}
	for (const UMaterialExpression* Expression : Reached)
	{
		if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			double Value = 1.0;
			if (IsGlowSwitchName(Scalar->ParameterName) && ScalarOf(Scalar, Material, Value) && Value == 0.0)
			{
				Out.OffScalars.AddUnique(Scalar->ParameterName);
			}
		}
	}
	// The first texture in pre-order: what the glow is drawn with.
	TFunction<const UMaterialExpressionTextureBase*(const FExpressionInput*, TSet<const UMaterialExpression*>&)> FirstTexture;
	int32 Budget = NodeBudget;
	FirstTexture = [&FirstTexture, &Budget](const FExpressionInput* Input, TSet<const UMaterialExpression*>& Seen) -> const UMaterialExpressionTextureBase*
	{
		const UMaterialExpression* Expression = Through(Input);
		if (!Expression || --Budget < 0 || Seen.Contains(Expression))
		{
			return nullptr;
		}
		Seen.Add(Expression);
		if (const UMaterialExpressionTextureBase* Texture = Cast<UMaterialExpressionTextureBase>(Expression))
		{
			return Texture;
		}
		TArray<const FExpressionInput*> Feeds;
		FeedingInputs(Input, Feeds);
		for (const FExpressionInput* Feed : Feeds)
		{
			if (const UMaterialExpressionTextureBase* Found = FirstTexture(Feed, Seen))
			{
				return Found;
			}
		}
		return nullptr;
	};
	// A texture node's texture as the material (or its instance) sets it.
	const auto TextureOf = [Material](const UMaterialExpressionTextureBase* Sample) -> UTexture*
	{
		UTexture* Texture = Sample->Texture;
		if (const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Sample))
		{
			UTexture* Set = nullptr;
			if (Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Set) && Set)
			{
				Texture = Set;
			}
		}
		return Texture;
	};
	// Every texture on the emissive path, in pre-order. A Scalar Parameter set to 0 belongs to the textures beside it at the first node that has both (the Multiply
	// of a texture and its intensity), so one switched by a parameter of its own is known by its texture; one with no texture at its node goes on up to the next.
	struct FBranch
	{
		TArray<int32> Textures;
		TArray<FName> Loose;
	};
	TMap<const UMaterialExpression*, FBranch> Memo;
	int32 WalkBudget = NodeBudget;
	TFunction<FBranch(const FExpressionInput*)> Walk;
	Walk = [&](const FExpressionInput* Input) -> FBranch
	{
		FBranch Branch;
		const UMaterialExpression* Expression = Through(Input);
		if (!Expression)
		{
			return Branch;
		}
		if (const FBranch* Done = Memo.Find(Expression))
		{
			return *Done;
		}
		if (--WalkBudget < 0)
		{
			return Branch;
		}
		// Empty while it is walked, so a cycle ends.
		Memo.Add(Expression);
		if (const UMaterialExpressionTextureBase* Sample = Cast<UMaterialExpressionTextureBase>(Expression))
		{
			if (UTexture* Texture = TextureOf(Sample))
			{
				const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Sample);
				int32 Index = Out.Textures.IndexOfByPredicate([Texture](const FMantleGlowTexture& Each) { return Each.Texture == Texture; });
				if (Index == INDEX_NONE)
				{
					Index = Out.Textures.AddDefaulted();
					Out.Textures[Index].Texture = Texture;
				}
				if (Parameter && Out.Textures[Index].Parameter.IsNone())
				{
					Out.Textures[Index].Parameter = Parameter->ParameterName;
				}
				Branch.Textures.Add(Index);
			}
			Memo.Add(Expression, Branch);
			return Branch;
		}
		if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
		{
			double Value = 1.0;
			if (IsGlowSwitchName(Scalar->ParameterName) && ScalarOf(Scalar, Material, Value) && Value == 0.0)
			{
				Branch.Loose.Add(Scalar->ParameterName);
			}
		}
		TArray<FBranch> Children;
		TArray<const FExpressionInput*> Feeds;
		FeedingInputs(Input, Feeds);
		for (const FExpressionInput* Feed : Feeds)
		{
			Children.Add(Walk(Feed));
			for (const int32 Texture : Children.Last().Textures)
			{
				Branch.Textures.AddUnique(Texture);
			}
		}
		for (const FBranch& Child : Children)
		{
			for (const FName& Scalar : Child.Loose)
			{
				if (Branch.Textures.Num() == 0)
				{
					Branch.Loose.AddUnique(Scalar);
					continue;
				}
				for (const int32 Texture : Branch.Textures)
				{
					Out.Textures[Texture].OffScalars.AddUnique(Scalar);
				}
			}
		}
		Memo.Add(Expression, Branch);
		return Branch;
	};
	Walk(Emissive);
	if (Out.Textures.Num() > 0)
	{
		Out.Texture = Out.Textures[0].Texture;
	}
	TSet<const UMaterialExpression*> BaseSeen;
	Budget = NodeBudget;
	if (const UMaterialExpressionTextureBase* Sample = FirstTexture(Base->GetExpressionInputForProperty(MP_BaseColor), BaseSeen))
	{
		Out.BaseTexture = TextureOf(Sample);
	}
	for (const bool bParametersOnly : { true, false })
	{
		TSet<const UMaterialExpression*> Seen;
		int32 ColorBudget = NodeBudget;
		FLinearColor Value;
		if (const UMaterialExpression* Color = FirstColor(Emissive, Material, bParametersOnly, Seen, ColorBudget); Color && ColorOf(Color, Material, Value))
		{
			Out.Color = Value;
			// An exposed tint set to black switches the glow off (a glTF's Emissive Factor of 0 ships on every material that does not glow).
			Out.bColorOff = Color->IsA<UMaterialExpressionVectorParameter>() && FMath::Max3(Value.R, Value.G, Value.B) <= 0.0f;
			break;
		}
	}
	Out.bGlows = Out.Texture != nullptr || FMath::Max3(Out.Color.R, Out.Color.G, Out.Color.B) > 0.0f;
	return Out.bGlows;
}
