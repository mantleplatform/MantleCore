// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWaterMaterial.h"

#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPixelDepth.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionRerouteBase.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneDepth.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Engine/Texture.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWaterMaterialPrivate
{
	/** A scene depth far past any fade distance, and a pixel depth at the surface itself: Scene Depth minus Pixel Depth is "deep". */
	constexpr double FarSceneDepth = 1.0e6;

	/** One to four numbers; a scalar (one) broadcasts against a wider value. */
	struct FValue
	{
		double V[4] = { 0.0, 0.0, 0.0, 0.0 };
		int32 N = 1;

		static FValue Scalar(const double Value)
		{
			FValue Out;
			Out.V[0] = Value;
			return Out;
		}

		double Lane(const int32 Index) const
		{
			return N == 1 ? V[0] : (Index < N ? V[Index] : 0.0);
		}
	};

	/** The function packages whose result is a fade by camera distance: at the far end, fully faded in. */
	bool IsCameraDepthFade(const UMaterialFunctionInterface* Function)
	{
		if (!Function)
		{
			return false;
		}
		const FString Package = Function->GetOutermost()->GetName();
		return Package.EndsWith(TEXT("/CameraDepthFade"), ESearchCase::IgnoreCase);
	}

	/** A function or node that fades by depth. */
	bool IsDepthFadeFunction(const UMaterialFunctionInterface* Function)
	{
		return Function && Function->GetOutermost()->GetName().EndsWith(TEXT("DepthFade"), ESearchCase::IgnoreCase);
	}

	bool IsDepthNode(const UMaterialExpression* Expression)
	{
		if (Expression->IsA<UMaterialExpressionDepthFade>() || Expression->IsA<UMaterialExpressionSceneDepth>())
		{
			return true;
		}
		const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		return Call && IsDepthFadeFunction(Call->MaterialFunction);
	}

	/** Whether Input's graph reaches a depth fade (bounded: a material graph has cycles only through bugs). */
	bool ReachesDepthFade(const FExpressionInput* Input, TSet<const UMaterialExpression*>& Seen, int32& Budget)
	{
		if (!Input || !Input->Expression || --Budget < 0)
		{
			return false;
		}
		const UMaterialExpression* Expression = Input->Expression;
		if (Seen.Contains(Expression))
		{
			return false;
		}
		Seen.Add(Expression);
		if (IsDepthNode(Expression))
		{
			return true;
		}
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			if (ReachesDepthFade(It.Input, Seen, Budget))
			{
				return true;
			}
		}
		return false;
	}

	/** The first Depth Fade node Input's graph reaches (bounded like ReachesDepthFade); null when there is none, or only a Scene Depth or a function. */
	const UMaterialExpressionDepthFade* FindDepthFadeNode(const FExpressionInput* Input, TSet<const UMaterialExpression*>& Seen, int32& Budget)
	{
		if (!Input || !Input->Expression || --Budget < 0)
		{
			return nullptr;
		}
		const UMaterialExpression* Expression = Input->Expression;
		if (Seen.Contains(Expression))
		{
			return nullptr;
		}
		Seen.Add(Expression);
		if (const UMaterialExpressionDepthFade* Fade = Cast<UMaterialExpressionDepthFade>(Expression))
		{
			return Fade;
		}
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			if (const UMaterialExpressionDepthFade* Fade = FindDepthFadeNode(It.Input, Seen, Budget))
			{
				return Fade;
			}
		}
		return nullptr;
	}

	/** Evaluates a scalar and vector expression graph with every depth fade at its far end. */
	struct FFarEvaluator
	{
		const UMaterialInterface* Context = nullptr;
		FString Failure;
		int32 Budget = 2048;

		bool Fail(const FString& Why)
		{
			if (Failure.IsEmpty())
			{
				Failure = Why;
			}
			return false;
		}

		static void ApplyMask(FValue& Value, const bool bR, const bool bG, const bool bB, const bool bA)
		{
			FValue Masked;
			Masked.N = 0;
			const bool Keep[4] = { bR, bG, bB, bA };
			for (int32 Lane = 0; Lane < 4; ++Lane)
			{
				if (Keep[Lane])
				{
					Masked.V[Masked.N++] = Value.Lane(Lane);
				}
			}
			if (Masked.N == 0)
			{
				return;
			}
			Value = Masked;
		}

		/** An input's value, or Constant when nothing is connected and the node has a constant for it (bHasConstant). */
		bool Input(const FExpressionInput& In, const bool bHasConstant, const double Constant, FValue& Out)
		{
			if (!In.Expression)
			{
				if (!bHasConstant)
				{
					return Fail(TEXT("an input is not connected"));
				}
				Out = FValue::Scalar(Constant);
				return true;
			}
			if (!Node(In.Expression, In.OutputIndex, Out))
			{
				return false;
			}
			if (In.Mask)
			{
				ApplyMask(Out, In.MaskR != 0, In.MaskG != 0, In.MaskB != 0, In.MaskA != 0);
			}
			return true;
		}

		template <typename TOp>
		bool Binary(const FExpressionInput& A, const double ConstA, const FExpressionInput& B, const double ConstB, FValue& Out, TOp Op)
		{
			FValue Left, Right;
			if (!Input(A, true, ConstA, Left) || !Input(B, true, ConstB, Right))
			{
				return false;
			}
			Out.N = FMath::Max(Left.N, Right.N);
			for (int32 Lane = 0; Lane < Out.N; ++Lane)
			{
				Out.V[Lane] = Op(Left.Lane(Lane), Right.Lane(Lane));
			}
			return true;
		}

		template <typename TOp>
		bool Unary(const FExpressionInput& In, FValue& Out, TOp Op)
		{
			if (!Input(In, false, 0.0, Out))
			{
				return false;
			}
			for (int32 Lane = 0; Lane < Out.N; ++Lane)
			{
				Out.V[Lane] = Op(Out.V[Lane]);
			}
			return true;
		}

		bool Node(const UMaterialExpression* Expression, const int32 Output, FValue& Out)
		{
			if (!Expression || --Budget < 0)
			{
				return Fail(TEXT("the graph is too deep"));
			}
			if (!NodeValue(Expression, Out))
			{
				return false;
			}
			// An output that keeps some channels (a vector parameter's R, G, B and A pins).
			if (Expression->Outputs.IsValidIndex(Output) && Expression->Outputs[Output].Mask)
			{
				const FExpressionOutput& Pin = Expression->Outputs[Output];
				ApplyMask(Out, Pin.MaskR != 0, Pin.MaskG != 0, Pin.MaskB != 0, Pin.MaskA != 0);
			}
			return true;
		}

		bool NodeValue(const UMaterialExpression* Expression, FValue& Out)
		{
			if (const UMaterialExpressionRerouteBase* Reroute = Cast<UMaterialExpressionRerouteBase>(Expression))
			{
				int32 Output = 0;
				const UMaterialExpression* Real = Reroute->TraceInputsToRealExpression(Output);
				return Real ? Node(Real, Output, Out) : Fail(TEXT("a reroute is not connected"));
			}
			if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Expression))
			{
				Out = FValue::Scalar(Constant->R);
				return true;
			}
			if (const UMaterialExpressionConstant2Vector* Constant2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
			{
				Out.N = 2;
				Out.V[0] = Constant2->R;
				Out.V[1] = Constant2->G;
				return true;
			}
			if (const UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
			{
				Out.N = 3;
				Out.V[0] = Constant3->Constant.R;
				Out.V[1] = Constant3->Constant.G;
				Out.V[2] = Constant3->Constant.B;
				return true;
			}
			if (const UMaterialExpressionConstant4Vector* Constant4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
			{
				Out.N = 4;
				Out.V[0] = Constant4->Constant.R;
				Out.V[1] = Constant4->Constant.G;
				Out.V[2] = Constant4->Constant.B;
				Out.V[3] = Constant4->Constant.A;
				return true;
			}
			if (const UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				float Value = Scalar->DefaultValue;
				float Set = 0.0f;
				if (Context && Context->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Scalar->ParameterName)), Set))
				{
					Value = Set;
				}
				Out = FValue::Scalar(Value);
				return true;
			}
			if (const UMaterialExpressionVectorParameter* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				FLinearColor Value = Vector->DefaultValue;
				FLinearColor Set;
				if (Context && Context->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Vector->ParameterName)), Set))
				{
					Value = Set;
				}
				Out.N = 4;
				Out.V[0] = Value.R;
				Out.V[1] = Value.G;
				Out.V[2] = Value.B;
				Out.V[3] = Value.A;
				return true;
			}
			// A parameter collection's scalar, at its default (a project-wide value, as the wind or winter parameters are).
			if (const UMaterialExpressionCollectionParameter* Collection = Cast<UMaterialExpressionCollectionParameter>(Expression))
			{
				bool bFound = false;
				const float Value = Collection->Collection ? Collection->Collection->GetScalarParameterDefaultValue(Collection->ParameterName, bFound) : 0.0f;
				if (!bFound)
				{
					return Fail(TEXT("a collection parameter is not a scalar with a default"));
				}
				Out = FValue::Scalar(Value);
				return true;
			}

			// The depth nodes, at their far end.
			if (const UMaterialExpressionDepthFade* Fade = Cast<UMaterialExpressionDepthFade>(Expression))
			{
				return Input(Fade->InOpacity, true, Fade->OpacityDefault, Out);
			}
			if (Expression->IsA<UMaterialExpressionSceneDepth>())
			{
				Out = FValue::Scalar(FarSceneDepth);
				return true;
			}
			if (Expression->IsA<UMaterialExpressionPixelDepth>())
			{
				Out = FValue::Scalar(0.0);
				return true;
			}
			if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				if (IsCameraDepthFade(Call->MaterialFunction))
				{
					Out = FValue::Scalar(1.0);
					return true;
				}
				return Fail(FString::Printf(TEXT("it calls %s"), Call->MaterialFunction ? *Call->MaterialFunction->GetName() : TEXT("a missing function")));
			}

			// A static switch keeps only the branch the material (instance) selects.
			if (const UMaterialExpressionStaticSwitchParameter* SwitchParameter = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
			{
				bool bValue = SwitchParameter->DefaultValue != 0;
				bool bFound = false;
				FGuid Guid;
				if (Context && Context->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(SwitchParameter->ParameterName)), bFound, Guid))
				{
					bValue = bFound;
				}
				return Input(bValue ? SwitchParameter->A : SwitchParameter->B, false, 0.0, Out);
			}
			if (const UMaterialExpressionStaticSwitch* Switch = Cast<UMaterialExpressionStaticSwitch>(Expression))
			{
				if (Switch->Value.Expression)
				{
					return Fail(TEXT("a static switch is driven by a node"));
				}
				return Input(Switch->DefaultValue ? Switch->A : Switch->B, false, 0.0, Out);
			}

			// Arithmetic.
			if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
			{
				return Binary(Add->A, Add->ConstA, Add->B, Add->ConstB, Out, [](double A, double B) { return A + B; });
			}
			if (const UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
			{
				return Binary(Subtract->A, Subtract->ConstA, Subtract->B, Subtract->ConstB, Out, [](double A, double B) { return A - B; });
			}
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				return Binary(Multiply->A, Multiply->ConstA, Multiply->B, Multiply->ConstB, Out, [](double A, double B) { return A * B; });
			}
			if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				return Binary(Divide->A, Divide->ConstA, Divide->B, Divide->ConstB, Out, [](double A, double B) { return FMath::Abs(B) > UE_DOUBLE_SMALL_NUMBER ? A / B : 0.0; });
			}
			if (const UMaterialExpressionMin* Min = Cast<UMaterialExpressionMin>(Expression))
			{
				return Binary(Min->A, Min->ConstA, Min->B, Min->ConstB, Out, [](double A, double B) { return FMath::Min(A, B); });
			}
			if (const UMaterialExpressionMax* Max = Cast<UMaterialExpressionMax>(Expression))
			{
				return Binary(Max->A, Max->ConstA, Max->B, Max->ConstB, Out, [](double A, double B) { return FMath::Max(A, B); });
			}
			if (const UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(Expression))
			{
				FValue Base, Exponent;
				if (!Input(Power->Base, false, 0.0, Base) || !Input(Power->Exponent, true, Power->ConstExponent, Exponent))
				{
					return false;
				}
				Out.N = Base.N;
				for (int32 Lane = 0; Lane < Out.N; ++Lane)
				{
					Out.V[Lane] = FMath::Pow(FMath::Max(Base.Lane(Lane), 0.0), Exponent.Lane(0));
				}
				return true;
			}
			if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
			{
				FValue A, B, Alpha;
				if (!Input(Lerp->A, true, Lerp->ConstA, A) || !Input(Lerp->B, true, Lerp->ConstB, B) || !Input(Lerp->Alpha, true, Lerp->ConstAlpha, Alpha))
				{
					return false;
				}
				Out.N = FMath::Max3(A.N, B.N, Alpha.N);
				for (int32 Lane = 0; Lane < Out.N; ++Lane)
				{
					Out.V[Lane] = FMath::Lerp(A.Lane(Lane), B.Lane(Lane), Alpha.Lane(Lane));
				}
				return true;
			}
			if (const UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(Expression))
			{
				FValue Low, High;
				if (!Unary(Clamp->Input, Out, [](double A) { return A; }) || !Input(Clamp->Min, true, Clamp->MinDefault, Low) || !Input(Clamp->Max, true, Clamp->MaxDefault, High))
				{
					return false;
				}
				for (int32 Lane = 0; Lane < Out.N; ++Lane)
				{
					const double Value = Out.V[Lane];
					Out.V[Lane] = Clamp->ClampMode == CMODE_ClampMin ? FMath::Max(Value, Low.Lane(Lane))
						: Clamp->ClampMode == CMODE_ClampMax ? FMath::Min(Value, High.Lane(Lane))
						: FMath::Clamp(Value, Low.Lane(Lane), FMath::Max(Low.Lane(Lane), High.Lane(Lane)));
				}
				return true;
			}
			if (const UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(Expression))
			{
				return Unary(Saturate->Input, Out, [](double A) { return FMath::Clamp(A, 0.0, 1.0); });
			}
			if (const UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(Expression))
			{
				return Unary(OneMinus->Input, Out, [](double A) { return 1.0 - A; });
			}
			if (const UMaterialExpressionAbs* Abs = Cast<UMaterialExpressionAbs>(Expression))
			{
				return Unary(Abs->Input, Out, [](double A) { return FMath::Abs(A); });
			}
			if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				if (!Input(Mask->Input, false, 0.0, Out))
				{
					return false;
				}
				ApplyMask(Out, Mask->R != 0, Mask->G != 0, Mask->B != 0, Mask->A != 0);
				return true;
			}
			if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
			{
				FValue A, B;
				if (!Input(Append->A, false, 0.0, A) || !Input(Append->B, false, 0.0, B) || A.N + B.N > 4)
				{
					return Fail(TEXT("an append is wider than four numbers"));
				}
				Out.N = A.N + B.N;
				for (int32 Lane = 0; Lane < A.N; ++Lane)
				{
					Out.V[Lane] = A.V[Lane];
				}
				for (int32 Lane = 0; Lane < B.N; ++Lane)
				{
					Out.V[A.N + Lane] = B.V[Lane];
				}
				return true;
			}

			return Fail(FString::Printf(TEXT("it reads %s"), *Expression->GetClass()->GetName()));
		}
	};

	/** The schema's bound on a uvPanning component (schema/material.schema.json), and on a water normal's panning. */
	constexpr double MaxPan = 10.0;

	/** Reroutes followed to the node that really feeds Input; null when nothing does. */
	const UMaterialExpression* Resolve(const FExpressionInput& Input)
	{
		const UMaterialExpression* Expression = Input.Expression;
		if (const UMaterialExpressionRerouteBase* Reroute = Cast<UMaterialExpressionRerouteBase>(Expression))
		{
			int32 Output = 0;
			return Reroute->TraceInputsToRealExpression(Output);
		}
		return Expression;
	}

	/** A function call's input by its name in the function ("Normal", "Flatness"). */
	const FExpressionInput* CallInputNamed(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		for (const FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (Input.Input.InputName == FName(Name))
			{
				return &Input.Input;
			}
		}
		return nullptr;
	}

	bool IsFunctionNamed(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		return Call->MaterialFunction && Call->MaterialFunction->GetOutermost()->GetName().EndsWith(FString(TEXT("/")) + Name, ESearchCase::IgnoreCase);
	}

	/** A number that is linear in the world position, time and a constant: X x world X + Y x world Y + Z x world Z + T x Time + C. */
	struct FLinear
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		double T = 0.0;
		double C = 0.0;

		bool IsConstant() const
		{
			return X == 0.0 && Y == 0.0 && Z == 0.0 && T == 0.0;
		}

		FLinear Scaled(const double Factor) const
		{
			FLinear Out;
			Out.X = X * Factor;
			Out.Y = Y * Factor;
			Out.Z = Z * Factor;
			Out.T = T * Factor;
			Out.C = C * Factor;
			return Out;
		}
	};

	/** One to four FLinear lanes; a scalar (one) broadcasts against a wider value, a lane past the end is the constant 0. */
	struct FLinearValue
	{
		FLinear V[4];
		int32 N = 1;

		FLinear Lane(const int32 Index) const
		{
			if (N == 1)
			{
				return V[0];
			}
			if (Index < N)
			{
				return V[Index];
			}
			return FLinear();
		}
	};

	/**
	 * Evaluates UV expressions that lay a texture out by world position and time (the Spruce Forest lake's ripples:
	 * World Position XY times a constant, over a scale parameter, plus Time times a speed). Only what is linear in the
	 * world position and time is understood: World Position, Time, Add, Subtract, Multiply and Divide where at most one
	 * side of each lane varies, Component Mask, Append and the engine's AppendMany; everything else is a constant
	 * read by FFarEvaluator (constants, parameters, Lerp, Saturate, collection parameters), or stops the read.
	 */
	struct FWorldEvaluator
	{
		const UMaterialInterface* Context = nullptr;
		FString Failure;
		int32 Budget = 512;

		bool Fail(const FString& Why)
		{
			if (Failure.IsEmpty())
			{
				Failure = Why;
			}
			return false;
		}

		static void ApplyMask(FLinearValue& Value, const bool bR, const bool bG, const bool bB, const bool bA)
		{
			FLinearValue Masked;
			Masked.N = 0;
			const bool Keep[4] = { bR, bG, bB, bA };
			for (int32 Lane = 0; Lane < 4; ++Lane)
			{
				if (Keep[Lane])
				{
					Masked.V[Masked.N++] = Value.Lane(Lane);
				}
			}
			if (Masked.N == 0)
			{
				return;
			}
			Value = Masked;
		}

		bool Input(const FExpressionInput& In, const bool bHasConstant, const double Constant, FLinearValue& Out)
		{
			if (!In.Expression)
			{
				if (!bHasConstant)
				{
					return Fail(TEXT("an input is not connected"));
				}
				Out = FLinearValue();
				Out.V[0].C = Constant;
				return true;
			}
			if (!Node(In.Expression, In.OutputIndex, Out))
			{
				return false;
			}
			if (In.Mask)
			{
				ApplyMask(Out, In.MaskR != 0, In.MaskG != 0, In.MaskB != 0, In.MaskA != 0);
			}
			return true;
		}

		bool Node(const UMaterialExpression* Expression, const int32 Output, FLinearValue& Out)
		{
			if (!Expression || --Budget < 0)
			{
				return Fail(TEXT("the graph is too deep"));
			}
			bool bHandled = false;
			if (!Own(Expression, Output, Out, bHandled))
			{
				return false;
			}
			if (!bHandled)
			{
				// A constant, a parameter or arithmetic that never touches the world: the far evaluator's (it masks the output itself).
				FFarEvaluator Far;
				Far.Context = Context;
				FValue Value;
				if (!Far.Node(Expression, Output, Value))
				{
					return Fail(Far.Failure);
				}
				Out = FLinearValue();
				Out.N = Value.N;
				for (int32 Lane = 0; Lane < Value.N; ++Lane)
				{
					Out.V[Lane].C = Value.V[Lane];
				}
				return true;
			}
			if (Expression->Outputs.IsValidIndex(Output) && Expression->Outputs[Output].Mask)
			{
				const FExpressionOutput& Pin = Expression->Outputs[Output];
				ApplyMask(Out, Pin.MaskR != 0, Pin.MaskG != 0, Pin.MaskB != 0, Pin.MaskA != 0);
			}
			return true;
		}

		/** A sum or difference (SignOfB is 1 or -1), lane by lane. */
		bool Sum(const FExpressionInput& A, const double ConstA, const FExpressionInput& B, const double ConstB, const double SignOfB, FLinearValue& Out)
		{
			FLinearValue Left, Right;
			if (!Input(A, true, ConstA, Left) || !Input(B, true, ConstB, Right))
			{
				return false;
			}
			Out = FLinearValue();
			Out.N = FMath::Max(Left.N, Right.N);
			for (int32 Lane = 0; Lane < Out.N; ++Lane)
			{
				const FLinear L = Left.Lane(Lane);
				const FLinear R = Right.Lane(Lane);
				Out.V[Lane].X = L.X + SignOfB * R.X;
				Out.V[Lane].Y = L.Y + SignOfB * R.Y;
				Out.V[Lane].Z = L.Z + SignOfB * R.Z;
				Out.V[Lane].T = L.T + SignOfB * R.T;
				Out.V[Lane].C = L.C + SignOfB * R.C;
			}
			return true;
		}

		/** A product, or a quotient (bDivide: the divisor must be a number), lane by lane: at most one side of a product may vary. */
		bool Product(const FExpressionInput& A, const double ConstA, const FExpressionInput& B, const double ConstB, const bool bDivide, FLinearValue& Out)
		{
			FLinearValue Left, Right;
			if (!Input(A, true, ConstA, Left) || !Input(B, true, ConstB, Right))
			{
				return false;
			}
			Out = FLinearValue();
			Out.N = FMath::Max(Left.N, Right.N);
			for (int32 Lane = 0; Lane < Out.N; ++Lane)
			{
				const FLinear L = Left.Lane(Lane);
				const FLinear R = Right.Lane(Lane);
				if (bDivide)
				{
					if (!R.IsConstant() || FMath::Abs(R.C) <= UE_DOUBLE_SMALL_NUMBER)
					{
						return Fail(TEXT("it divides by something that is not a number"));
					}
					Out.V[Lane] = L.Scaled(1.0 / R.C);
				}
				else if (R.IsConstant())
				{
					Out.V[Lane] = L.Scaled(R.C);
				}
				else if (L.IsConstant())
				{
					Out.V[Lane] = R.Scaled(L.C);
				}
				else
				{
					return Fail(TEXT("it multiplies two values that change with position or time"));
				}
			}
			return true;
		}

		/** The nodes this evaluator reads itself; bHandled false leaves the node to FFarEvaluator. False is a failed read. */
		bool Own(const UMaterialExpression* Expression, const int32 Output, FLinearValue& Out, bool& bHandled)
		{
			bHandled = true;
			if (const UMaterialExpressionRerouteBase* Reroute = Cast<UMaterialExpressionRerouteBase>(Expression))
			{
				int32 Real = 0;
				const UMaterialExpression* Target = Reroute->TraceInputsToRealExpression(Real);
				return Target ? Node(Target, Real, Out) : Fail(TEXT("a reroute is not connected"));
			}
			if (const UMaterialExpressionWorldPosition* World = Cast<UMaterialExpressionWorldPosition>(Expression))
			{
				if (World->WorldPositionShaderOffset != WPT_Default && World->WorldPositionShaderOffset != WPT_ExcludeAllShaderOffsets)
				{
					return Fail(TEXT("it uses a camera-relative world position"));
				}
				Out = FLinearValue();
				Out.N = 3;
				Out.V[0].X = 1.0;
				Out.V[1].Y = 1.0;
				Out.V[2].Z = 1.0;
				return true;
			}
			if (const UMaterialExpressionTime* Time = Cast<UMaterialExpressionTime>(Expression))
			{
				if (Time->bOverride_Period && Time->Period > 0.0f)
				{
					return Fail(TEXT("its Time loops"));
				}
				Out = FLinearValue();
				Out.V[0].T = 1.0;
				return true;
			}
			if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
			{
				return Sum(Add->A, Add->ConstA, Add->B, Add->ConstB, 1.0, Out);
			}
			if (const UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
			{
				return Sum(Subtract->A, Subtract->ConstA, Subtract->B, Subtract->ConstB, -1.0, Out);
			}
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				return Product(Multiply->A, Multiply->ConstA, Multiply->B, Multiply->ConstB, false, Out);
			}
			if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				return Product(Divide->A, Divide->ConstA, Divide->B, Divide->ConstB, true, Out);
			}
			if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				if (!Input(Mask->Input, false, 0.0, Out))
				{
					return false;
				}
				ApplyMask(Out, Mask->R != 0, Mask->G != 0, Mask->B != 0, Mask->A != 0);
				return true;
			}
			if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
			{
				FLinearValue A, B;
				if (!Input(Append->A, false, 0.0, A) || !Input(Append->B, false, 0.0, B) || A.N + B.N > 4)
				{
					return Fail(TEXT("an append is wider than four numbers"));
				}
				Out = FLinearValue();
				Out.N = A.N + B.N;
				for (int32 Lane = 0; Lane < A.N; ++Lane)
				{
					Out.V[Lane] = A.V[Lane];
				}
				for (int32 Lane = 0; Lane < B.N; ++Lane)
				{
					Out.V[A.N + Lane] = B.V[Lane];
				}
				return true;
			}
			if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				if (IsFunctionNamed(Call, TEXT("AppendMany")))
				{
					// Its outputs are RG, RGB and RGBA (output 0, 1, 2).
					const int32 Count = FMath::Clamp(Output + 2, 2, 4);
					const TCHAR* const Names[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };
					Out = FLinearValue();
					Out.N = Count;
					for (int32 Lane = 0; Lane < Count; ++Lane)
					{
						const FExpressionInput* In = CallInputNamed(Call, Names[Lane]);
						FLinearValue Value;
						if (!In || !Input(*In, false, 0.0, Value))
						{
							return Fail(TEXT("AppendMany has an input that is not connected"));
						}
						Out.V[Lane] = Value.Lane(0);
					}
					return true;
				}
			}
			bHandled = false;
			return true;
		}
	};

	/**
	 * A texture sample laid out by world position: its Coordinates, evaluated as FLinear lanes, are 1 / size x world X
	 * (U) and 1 / size x world Y (V) plus speed x Time. That is the water key's space world, a size and a pan. False
	 * (and nothing recorded) when the coordinates are not that: the first UV set, a rotated or swapped layout, world Z.
	 * A layout the document cannot say exactly (a size that differs on the two axes, a mirrored axis, a size out of
	 * range) is returned with bApproximated: its size is the mean of the two.
	 */
	bool WorldLayerOf(const UMaterialInterface* Context, const FExpressionInput& Coordinates, FMantleWaterNormalLayer& Out)
	{
		if (!Coordinates.Expression)
		{
			return false;
		}
		FWorldEvaluator Evaluator;
		Evaluator.Context = Context;
		FLinearValue Value;
		if (!Evaluator.Input(Coordinates, false, 0.0, Value) || Value.N < 2)
		{
			return false;
		}
		const FLinear U = Value.Lane(0);
		const FLinear V = Value.Lane(1);
		if (U.Z != 0.0 || V.Z != 0.0 || U.Y != 0.0 || V.X != 0.0 || U.X == 0.0 || V.Y == 0.0)
		{
			return false;
		}
		if (FMath::Abs(U.T) > MaxPan || FMath::Abs(V.T) > MaxPan)
		{
			return false;
		}
		const double SizeU = 1.0 / FMath::Abs(U.X);
		const double SizeV = 1.0 / FMath::Abs(V.Y);
		double Size = 0.5 * (SizeU + SizeV);
		Out.bWorld = true;
		Out.bApproximated = U.X < 0.0 || V.Y < 0.0 || FMath::Abs(SizeU - SizeV) > 0.02 * Size;
		if (Size < 1.0 || Size > 100000.0)
		{
			Size = FMath::Clamp(Size, 1.0, 100000.0);
			Out.bApproximated = true;
		}
		Out.WorldSizeCm = Size;
		Out.Speed[0] = U.T;
		Out.Speed[1] = V.T;
		return true;
	}

	/** Whether Input's graph reaches a texture sample (bounded). */
	bool LeadsToTexture(const FExpressionInput* Input, TSet<const UMaterialExpression*>& Seen, int32& Budget)
	{
		if (!Input || !Input->Expression || --Budget < 0)
		{
			return false;
		}
		const UMaterialExpression* Expression = Input->Expression;
		if (Seen.Contains(Expression))
		{
			return false;
		}
		Seen.Add(Expression);
		if (Expression->IsA<UMaterialExpressionTextureBase>())
		{
			return true;
		}
		UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
		for (FExpressionInputIterator It{ Mutable }; It; ++It)
		{
			if (LeadsToTexture(It.Input, Seen, Budget))
			{
				return true;
			}
		}
		return false;
	}

	bool LeadsToTexture(const FExpressionInput& Input)
	{
		TSet<const UMaterialExpression*> Seen;
		int32 Budget = 256;
		return LeadsToTexture(&Input, Seen, Budget);
	}

	/**
	 * The strength a called function multiplies its normal by (the Spruce Forest ripples: a Lerp from 0.01 to 0.05 by a
	 * wind parameter). Walked from the function's first output down the AppendVector and Multiply nodes that lead to a
	 * texture sample; a Multiply whose other side evaluates to a number (constants, parameters, Lerp, Saturate, a
	 * collection parameter's default) contributes it. 1 when nothing reads.
	 */
	double FunctionStrength(const UMaterialInterface* Context, const UMaterialFunctionInterface* Function)
	{
		const UMaterialExpressionFunctionOutput* Output = nullptr;
		for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
		{
			const UMaterialExpressionFunctionOutput* Candidate = Cast<UMaterialExpressionFunctionOutput>(Expression.Get());
			if (Candidate && (!Output || Candidate->SortPriority < Output->SortPriority))
			{
				Output = Candidate;
			}
		}
		if (!Output)
		{
			return 1.0;
		}
		double Strength = 1.0;
		const FExpressionInput* Input = &Output->A;
		for (int32 Step = 0; Step < 12 && Input; ++Step)
		{
			const UMaterialExpression* Expression = Resolve(*Input);
			const FExpressionInput* Next = nullptr;
			if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
			{
				Next = LeadsToTexture(Append->A) ? &Append->A : (LeadsToTexture(Append->B) ? &Append->B : nullptr);
			}
			else if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				const bool bFirst = LeadsToTexture(Multiply->A);
				const bool bSecond = LeadsToTexture(Multiply->B);
				if (bFirst == bSecond)
				{
					break;
				}
				FFarEvaluator Probe;
				Probe.Context = Context;
				FValue Factor;
				if (!Probe.Input(bFirst ? Multiply->B : Multiply->A, true, bFirst ? Multiply->ConstB : Multiply->ConstA, Factor))
				{
					break;
				}
				Strength *= Factor.Lane(0);
				Next = bFirst ? &Multiply->A : &Multiply->B;
			}
			Input = Next;
		}
		return Strength > 0.0 ? Strength : 1.0;
	}

	/**
	 * The Normal output's texture samples with the pans that move them (the Spruce Forest water: a Lerp to an ice
	 * normal by a winter parameter over FlattenNormal over BlendAngleCorrectedNormals of two Panners into two
	 * samples of one normal texture). Each sample is followed through the nodes that only choose or combine
	 * (static switches, a Lerp's A side, FlattenNormal, BlendAngleCorrectedNormals); anything else ends the read,
	 * except a called function that lays a normal map out by world position (its lake's MF_Wind_Ripples), whose
	 * first such sample is the layer, whatever the function is called. Found holds the samples in the graph's order.
	 */
	struct FNormalFinder
	{
		const UMaterialInterface* Context = nullptr;
		TArray<FMantleWaterNormalLayer> Found;
		double Scale = 1.0;
		FString Failure;
		int32 Budget = 256;

		bool Fail(const FString& Why)
		{
			if (Failure.IsEmpty())
			{
				Failure = Why;
			}
			return false;
		}

		UTexture* TextureOf(const UMaterialExpressionTextureSample* Sample) const
		{
			const UMaterialExpressionTextureBase* Source = Sample;
			if (Sample->TextureObject.Expression)
			{
				Source = Cast<UMaterialExpressionTextureBase>(Resolve(Sample->TextureObject));
			}
			if (!Source)
			{
				return nullptr;
			}
			if (const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Source))
			{
				UTexture* Value = nullptr;
				if (Context && Context->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Value) && Value)
				{
					return Value;
				}
			}
			return Source->Texture;
		}

		/** UVs as the first UV set times a tiling: Texture Coordinate 0, or a Multiply of that by a number or a 2-vector. */
		bool Tiling(const FExpressionInput& Input, const uint32 ConstCoordinate, double (&Out)[2], const int32 Depth = 0)
		{
			if (!Input.Expression)
			{
				Out[0] = Out[1] = 1.0;
				return ConstCoordinate == 0;
			}
			const UMaterialExpression* Expression = Resolve(Input);
			if (const UMaterialExpressionTextureCoordinate* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Expression))
			{
				if (TexCoord->CoordinateIndex != 0 || TexCoord->UnMirrorU || TexCoord->UnMirrorV)
				{
					return Fail(TEXT("its UVs are not the first UV set"));
				}
				Out[0] = TexCoord->UTiling;
				Out[1] = TexCoord->VTiling;
				return true;
			}
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				if (Depth < 8)
				{
					const FExpressionInput* Uvs[2] = { &Multiply->A, &Multiply->B };
					const FExpressionInput* Numbers[2] = { &Multiply->B, &Multiply->A };
					const double Constants[2] = { Multiply->ConstB, Multiply->ConstA };
					for (int32 Side = 0; Side < 2; ++Side)
					{
						// The side that is UVs (a Texture Coordinate, or a Multiply of one), and the side that is a number.
						FFarEvaluator Probe;
						Probe.Context = Context;
						FValue Factor;
						double Inner[2] = { 1.0, 1.0 };
						if (Uvs[Side]->Expression && Probe.Input(*Numbers[Side], true, Constants[Side], Factor) && Tiling(*Uvs[Side], 0, Inner, Depth + 1))
						{
							Out[0] = Inner[0] * Factor.Lane(0);
							Out[1] = Inner[1] * Factor.Lane(1);
							return true;
						}
					}
				}
			}
			return Fail(TEXT("its UVs are not Texture Coordinate 0 times a number"));
		}

		bool Sample(const UMaterialExpressionTextureSample* TextureSample)
		{
			FMantleWaterNormalLayer Normal;
			UTexture* Texture = TextureOf(TextureSample);
			if (!Texture)
			{
				return Fail(TEXT("a normal sample has no texture"));
			}
			Normal.Texture = Texture;
			// Laid out by world position (and time) instead of the mesh's UVs: space world.
			FMantleWaterNormalLayer World;
			if (WorldLayerOf(Context, TextureSample->Coordinates, World))
			{
				World.Texture = Texture;
				Found.Add(World);
				return true;
			}
			const UMaterialExpressionPanner* Panner = Cast<UMaterialExpressionPanner>(Resolve(TextureSample->Coordinates));
			if (!Panner)
			{
				if (!Tiling(TextureSample->Coordinates, TextureSample->ConstCoordinate, Normal.Tiling))
				{
					return false;
				}
				Found.Add(Normal);
				return true;
			}
			if (Panner->Time.Expression)
			{
				return Fail(TEXT("a panner has a Time input"));
			}
			if (Panner->Speed.Expression)
			{
				FFarEvaluator Probe;
				Probe.Context = Context;
				FValue Speed;
				if (!Probe.Input(Panner->Speed, false, 0.0, Speed))
				{
					return Fail(FString::Printf(TEXT("a panner's speed: %s"), *Probe.Failure));
				}
				Normal.Speed[0] = Speed.Lane(0);
				Normal.Speed[1] = Speed.Lane(1);
			}
			else
			{
				Normal.Speed[0] = Panner->SpeedX;
				Normal.Speed[1] = Panner->SpeedY;
			}
			if (FMath::Abs(Normal.Speed[0]) > MaxPan || FMath::Abs(Normal.Speed[1]) > MaxPan)
			{
				return Fail(TEXT("a panner is faster than Mantle keeps"));
			}
			if (!Tiling(Panner->Coordinate, Panner->ConstCoordinate, Normal.Tiling))
			{
				return false;
			}
			Found.Add(Normal);
			return true;
		}

		/**
		 * A called function that lays a normal map out by world position (a function of its own, not the engine's): its
		 * first texture sample whose coordinates are world position and time is the layer, with the strength the
		 * function multiplies it by. It is the one sample of the function's several that the document can carry.
		 */
		bool WorldFunction(const UMaterialExpressionMaterialFunctionCall* Call)
		{
			const UMaterialFunctionInterface* Function = Call->MaterialFunction;
			if (!Function)
			{
				return Fail(TEXT("the normal calls a missing function"));
			}
			for (const TObjectPtr<UMaterialExpression>& Expression : Function->GetExpressions())
			{
				const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression.Get());
				UTexture* Texture = TextureSample ? TextureOf(TextureSample) : nullptr;
				FMantleWaterNormalLayer Layer;
				if (Texture && WorldLayerOf(Context, TextureSample->Coordinates, Layer))
				{
					Layer.Texture = Texture;
					Layer.Strength = FunctionStrength(Context, Function);
					// The function's other samples and its own blend are not carried.
					Layer.bApproximated = true;
					Found.Add(Layer);
					return true;
				}
			}
			return Fail(FString::Printf(TEXT("the normal calls %s"), *Function->GetName()));
		}

		bool Visit(const FExpressionInput& Input)
		{
			if (--Budget < 0)
			{
				return Fail(TEXT("the normal graph is too deep"));
			}
			const UMaterialExpression* Expression = Resolve(Input);
			if (!Expression)
			{
				return Fail(TEXT("a normal input is not connected"));
			}
			if (const UMaterialExpressionStaticSwitchParameter* SwitchParameter = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
			{
				bool bValue = SwitchParameter->DefaultValue != 0;
				bool bSet = false;
				FGuid Guid;
				if (Context && Context->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(SwitchParameter->ParameterName)), bSet, Guid))
				{
					bValue = bSet;
				}
				return Visit(bValue ? SwitchParameter->A : SwitchParameter->B);
			}
			if (const UMaterialExpressionStaticSwitch* Switch = Cast<UMaterialExpressionStaticSwitch>(Expression))
			{
				return Switch->Value.Expression ? Fail(TEXT("a static switch is driven by a node")) : Visit(Switch->DefaultValue ? Switch->A : Switch->B);
			}
			// A Lerp toward another normal (the ice a winter parameter blends in): the water is its A side.
			if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
			{
				return Visit(Lerp->A);
			}
			if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				if (IsFunctionNamed(Call, TEXT("FlattenNormal")))
				{
					const FExpressionInput* Normal = CallInputNamed(Call, TEXT("Normal"));
					const FExpressionInput* Flatness = CallInputNamed(Call, TEXT("Flatness"));
					FFarEvaluator Probe;
					Probe.Context = Context;
					FValue Flat;
					if (Flatness && Flatness->Expression && Probe.Input(*Flatness, false, 0.0, Flat))
					{
						// Lerp(Normal, flat, Flatness): its tilt is what is left, 1 - Flatness (below 0 amplifies).
						Scale *= FMath::Clamp(1.0 - Flat.Lane(0), 0.0, 10.0);
					}
					return Normal ? Visit(*Normal) : Fail(TEXT("FlattenNormal has no normal"));
				}
				if (IsFunctionNamed(Call, TEXT("BlendAngleCorrectedNormals")))
				{
					const FExpressionInput* Base = CallInputNamed(Call, TEXT("BaseNormal"));
					const FExpressionInput* Additional = CallInputNamed(Call, TEXT("AdditionalNormal"));
					return Base && Additional && Visit(*Base) && Visit(*Additional);
				}
				return WorldFunction(Call);
			}
			if (const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression))
			{
				return Sample(TextureSample);
			}
			return Fail(FString::Printf(TEXT("the normal reads %s"), *Expression->GetClass()->GetName()));
		}
	};
}

namespace MantleWaterMaterialPrivate
{
	/** The deep colour and the normal samples, as every water material is read (a depth fade or Single Layer Water). */
	void ReadSurface(const UMaterialInterface* Context, const FExpressionInput* BaseColor, const FExpressionInput* Normal, FMantleWaterBake& Out)
	{
		if (BaseColor && BaseColor->Expression)
		{
			FFarEvaluator Colour;
			Colour.Context = Context;
			FValue Deep;
			if (Colour.Input(*BaseColor, false, 0.0, Deep) && Deep.N >= 3)
			{
				Out.bColour = true;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.Colour[Channel] = FMath::Clamp(Deep.V[Channel], 0.0, 1.0);
				}
			}
			else if (Out.Why.IsEmpty())
			{
				Out.Why = FString::Printf(TEXT("colour: %s"), Colour.Failure.IsEmpty() ? TEXT("not a colour") : *Colour.Failure);
			}
		}


		if (Normal && Normal->Expression)
		{
			FNormalFinder Finder;
			Finder.Context = Context;
			if (Finder.Visit(*Normal) && Finder.Found.Num() > 0)
			{
				// What the water key carries: every sample, in the graph's order, with the strength FlattenNormal and a called function give.
				Out.Normals = Finder.Found;
				Out.NormalSamples = Finder.Found.Num();
				Out.NormalScale = Finder.Scale * Finder.Found[0].Strength;

				// What a document without the water key can carry (translucent water, or a client that lacks the key): one panned
				// sample on the first UV set. Two samples blended cannot be one texture, so the fastest stands for the water's motion.
				const FMantleWaterNormalLayer* Dominant = nullptr;
				for (const FMantleWaterNormalLayer& Candidate : Finder.Found)
				{
					if (!Candidate.bWorld && (!Dominant || FMath::Square(Candidate.Speed[0]) + FMath::Square(Candidate.Speed[1]) > FMath::Square(Dominant->Speed[0]) + FMath::Square(Dominant->Speed[1])))
					{
						Dominant = &Candidate;
					}
				}
				if (Dominant && Dominant->Moves())
				{
					Out.bNormalPanned = true;
					Out.NormalTexture = Dominant->Texture;
					Out.NormalTiling[0] = Dominant->Tiling[0];
					Out.NormalTiling[1] = Dominant->Tiling[1];
					Out.NormalSpeed[0] = Dominant->Speed[0];
					Out.NormalSpeed[1] = Dominant->Speed[1];
				}
			}
			else
			{
				Out.Why += FString::Printf(TEXT("%snormal: %s"), Out.Why.IsEmpty() ? TEXT("") : TEXT("; "), Finder.Failure.IsEmpty() ? TEXT("no panned sample") : *Finder.Failure);
			}
		}
	}
}

bool FMantleWaterMaterial::Recognise(const UMaterialInterface* Context, const FExpressionInput* Opacity, const FExpressionInput* BaseColor, const FExpressionInput* Normal, FMantleWaterBake& Out)
{
	using namespace MantleWaterMaterialPrivate;
	Out = FMantleWaterBake();
	if (!Context || !Opacity || !Opacity->Expression)
	{
		return false;
	}
	TSet<const UMaterialExpression*> Seen;
	int32 Budget = 4096;
	if (!ReachesDepthFade(Opacity, Seen, Budget))
	{
		return false;
	}
	Out.bFound = true;

	// The fade distance (a Depth Fade node's), for the water key's coefficients; a Scene Depth fade has none.
	Seen.Reset();
	Budget = 4096;
	if (const UMaterialExpressionDepthFade* Fade = FindDepthFadeNode(Opacity, Seen, Budget))
	{
		FFarEvaluator Distance;
		Distance.Context = Context;
		FValue Fading;
		if (Distance.Input(Fade->FadeDistance, true, Fade->FadeDistanceDefault, Fading) && Fading.Lane(0) > 0.0)
		{
			Out.bFadeDistance = true;
			Out.FadeDistanceCm = Fading.Lane(0);
		}
	}

	FFarEvaluator Evaluator;
	Evaluator.Context = Context;
	FValue Value;
	if (Evaluator.Input(*Opacity, false, 0.0, Value) && Value.Lane(0) >= MinimumOpacity)
	{
		Out.bOpacityRead = true;
		Out.Opacity = FMath::Clamp(Value.Lane(0), 0.0, 1.0);
	}
	else
	{
		Out.Opacity = EstimatedOpacity;
		Out.Why = Evaluator.Failure.IsEmpty() ? FString::Printf(TEXT("the far opacity reads %.3f"), Value.Lane(0)) : Evaluator.Failure;
	}

	MantleWaterMaterialPrivate::ReadSurface(Context, BaseColor, Normal, Out);
	return true;
}

bool FMantleWaterMaterial::RecogniseSingleLayer(const UMaterialInterface* Context, const FExpressionInput* BaseColor, const FExpressionInput* Normal, FMantleWaterBake& Out)
{
	using namespace MantleWaterMaterialPrivate;
	Out = FMantleWaterBake();
	if (!Context)
	{
		return false;
	}
	Out.bFound = true;
	Out.bSingleLayer = true;
	Out.bOpacityRead = true;
	Out.Opacity = 1.0;

	// The output node's own coefficients, where the graph is constants and parameters; else the deep colour derives them.
	if (const UMaterial* Base = Context->GetMaterial())
	{
		for (const UMaterialExpression* Expression : Base->GetExpressionCollection().Expressions)
		{
			const UMaterialExpressionSingleLayerWaterMaterialOutput* Output = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(Expression);
			if (!Output)
			{
				continue;
			}
			FFarEvaluator Far;
			Far.Context = Context;
			FValue Scattering;
			FValue Absorption;
			if (Far.Input(Output->ScatteringCoefficients, false, 0.0, Scattering) && Scattering.N >= 3
				&& Far.Input(Output->AbsorptionCoefficients, false, 0.0, Absorption) && Absorption.N >= 3)
			{
				Out.bVolume = true;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.Volume.Scattering[Channel] = FMath::Max(Scattering.V[Channel], 0.0);
					Out.Volume.Absorption[Channel] = FMath::Max(Absorption.V[Channel], 0.0);
				}
			}
			else
			{
				Out.Why = Far.Failure.IsEmpty() ? FString(TEXT("the water output's coefficients are not constants")) : Far.Failure;
			}
			break;
		}
	}
	ReadSurface(Context, BaseColor, Normal, Out);

	// The Water plugin's Water_Material (GhzGarage/mantle#818) builds its outputs with Set Material Attributes (no roots to read) and
	// feeds its output node through foam and depth graphs this cannot evaluate, so what it holds is read by its parameter names
	// instead: "Absorption" and "Scattering" (the output node's coefficients, 1/cm) and "Water Albedo" (the surface colour), as
	// the body's instance chain sets them. Its waves are world position offset in every case: dropped and said.
	auto NamedColour = [Context](const TCHAR* Name, double (&Value)[3])
	{
		FLinearColor Colour;
		if (!Context->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Name))), Colour))
		{
			return false;
		}
		Value[0] = Colour.R;
		Value[1] = Colour.G;
		Value[2] = Colour.B;
		return true;
	};
	if (!Out.bVolume)
	{
		double Absorption[3];
		double Scattering[3];
		if (NamedColour(TEXT("Absorption"), Absorption) && NamedColour(TEXT("Scattering"), Scattering))
		{
			// Believed only within the schema's 0..1 (1/cm): a value above it is one the graph scales on the way to the node
			// (water that absorbs everything within a centimetre is no lake), and then the deep colour derives the coefficients.
			bool bPlausible = true;
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				bPlausible = bPlausible && Absorption[Channel] >= 0.0 && Absorption[Channel] <= 1.0 && Scattering[Channel] >= 0.0 && Scattering[Channel] <= 1.0;
			}
			if (bPlausible)
			{
				Out.bVolume = true;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Out.Volume.Absorption[Channel] = Absorption[Channel];
					Out.Volume.Scattering[Channel] = Scattering[Channel];
				}
			}
			else if (Out.Why.IsEmpty())
			{
				Out.Why = TEXT("the Absorption and Scattering parameters are outside 0..1 per cm");
			}
		}
	}
	if (!Out.bColour)
	{
		double Albedo[3];
		if (NamedColour(TEXT("Water Albedo"), Albedo) || NamedColour(TEXT("WaterAlbedo"), Albedo) || NamedColour(TEXT("Water Color"), Albedo))
		{
			Out.bColour = true;
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Out.Colour[Channel] = FMath::Clamp(Albedo[Channel], 0.0, 1.0);
			}
		}
		else
		{
			// Nothing to read: a clear lake, rather than the bake's colour (white where a texture fed the colour, or the near end of a fade).
			Out.bColour = true;
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Out.Colour[Channel] = ClearLakeColour[Channel];
			}
		}
	}
	const UMaterial* Base = Context->GetMaterial();
	if (Base && Base->GetPathName().StartsWith(TEXT("/Water/Materials/")))
	{
		Out.bWavesDropped = true;
	}
	return true;
}

FMantleWaterCoefficients FMantleWaterMaterial::Coefficients(const double (&Colour)[3], const double FadeDistanceCm)
{
	// The derivation, in the header: sigma = min(3 / D, 1); albedo_c = clamp(C_c, 0, 0.9);
	// scattering_c = sigma albedo_c; absorption_c = sigma (1 - albedo_c). Both stay within the schema's 0..1 because sigma does.
	const double Distance = FadeDistanceCm > 0.0 ? FadeDistanceCm : DefaultFadeDistanceCm;
	const double Sigma = FMath::Min(3.0 / Distance, 1.0);
	FMantleWaterCoefficients Out;
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		const double Albedo = FMath::Clamp(Colour[Channel], 0.0, 0.9);
		Out.Scattering[Channel] = Sigma * Albedo;
		Out.Absorption[Channel] = Sigma * (1.0 - Albedo);
	}
	return Out;
}
