// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlowMaterial.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
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
			if (ScalarOf(Scalar, Material, Value) && Value == 0.0)
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
			if (ScalarOf(Scalar, Material, Value) && Value == 0.0)
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
