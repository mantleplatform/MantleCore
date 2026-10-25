// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLandscapeMaterialReader.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionLandscapeLayerCoords.h"
#include "Materials/MaterialExpressionLandscapeLayerSample.h"
#include "Materials/MaterialExpressionLandscapeLayerSwitch.h"
#include "Materials/MaterialExpressionLandscapeLayerWeight.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionRerouteBase.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticBool.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

const FMantleLandscapeLayerLook* FMantleLandscapeMaterialRead::Find(const FName Layer) const
{
	return Layers.FindByPredicate([Layer](const FMantleLandscapeLayerLook& Look) { return Look.Layer == Layer; });
}

float FMantleLandscapeSlopeOverlay::WeightAt(const double NormalZ) const
{
	if (WeightByNormalZ.Num() < 2)
	{
		return 0.0f;
	}
	const double Position = FMath::Clamp(NormalZ, 0.0, 1.0) * (WeightByNormalZ.Num() - 1);
	const int32 Below = FMath::Min(FMath::FloorToInt32(Position), WeightByNormalZ.Num() - 2);
	return FMath::Lerp(WeightByNormalZ[Below], WeightByNormalZ[Below + 1], static_cast<float>(Position - Below));
}

FString FMantleLandscapeSlopeOverlay::BakedNote()
{
	return TEXT("Slope overlay baked from the heightfield: the material shows this look where the ground is steep, so its weight here is taken from each sample's slope.");
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleLandscapeMaterialReaderPrivate
{
	/** Which output a layer's look is read for. */
	enum class ELmProp : uint8
	{
		BaseColor,
		Normal
	};

	/** The roughness of a layer whose material states none, and the glossiest the client is given. */
	constexpr double DefaultLayerRoughness = 0.85;
	constexpr double MinLayerRoughness = 0.01;

	/** One call of a material function, and the call it sits inside: what a function's input resolves through. */
	struct FLmFrame
	{
		const UMaterialExpressionMaterialFunctionCall* Call = nullptr;
		const FLmFrame* Parent = nullptr;
	};

	/** An expression's output as the graph reaches it: through which calls (frames), with reroutes, calls and static switches already resolved. */
	struct FLmRef
	{
		const UMaterialExpression* Expression = nullptr;
		int32 Output = 0;
		const FLmFrame* Frame = nullptr;

		bool IsSet() const { return Expression != nullptr; }
	};

	using FLmVisitKey = TPair<const UMaterialExpression*, const FLmFrame*>;

	/** What a Resolve passed on its way to the expression: the connections' component masks (outermost first, R G B A as bits 1 2 4 8) and the name of the first function input it came out of. */
	struct FLmTrace
	{
		TArray<uint8, TInlineAllocator<4>> Masks;
		FName FirstInputName;

		/** The frame of the last material function call the path entered: where a value that is a function's output ends up. */
		const FLmFrame* EnteredFrame = nullptr;
	};

	/** What a subgraph reads that a flat, tiled layer material cannot keep. */
	struct FLmFlags
	{
		bool bSlope = false;
		bool bHeight = false;
		bool bDistance = false;
	};

	/** One paint layer the material names, and the input that is its look. */
	struct FLmLayerSource
	{
		FName Layer;
		FLmRef Look;
		bool bConstant = false;
		FLinearColor Constant = FLinearColor::Black;
	};

	/** A final Lerp between the layer blend and a look of its own by a mask from the world normal's Z: rock on steep ground. */
	struct FLmSlopeOverlay
	{
		bool bFound = false;

		/** The overlay's look: the Lerp's input that holds no paint layer. */
		FLmRef Look;

		/** The Lerp's alpha (what the flag scan skips: it is baked, not an approximation). */
		FLmRef Alpha;

		/** The overlay is the Lerp's A, so its weight is 1 - alpha; else its B, so alpha. */
		bool bOverlayIsA = false;
	};

	struct FLmLayerSet
	{
		TArray<FLmLayerSource> Sources;
		bool bHeightBlend = false;
		FLmSlopeOverlay Slope;

		/** Texture samples reached outside every layer: a base layer with no mask, a macro variation, an overlay. */
		TArray<FString> GlobalSampleNames;

		FLmLayerSource* Find(const FName Layer)
		{
			return Sources.FindByPredicate([Layer](const FLmLayerSource& Source) { return Source.Layer == Layer; });
		}

		void Add(const FName Layer, const FLmRef& Look)
		{
			if (!Layer.IsNone() && !Find(Layer))
			{
				FLmLayerSource& Source = Sources.AddDefaulted_GetRef();
				Source.Layer = Layer;
				Source.Look = Look;
			}
		}
	};

	/** One texture sample a layer's look blends in. */
	struct FLmHit
	{
		UTexture* Texture = nullptr;
		double Weight = 1.0;
		double TileCm = 0.0;
		int32 Order = 0;
		FString Name;
	};

	struct FLmCollected
	{
		TArray<FLmHit> Hits;
		bool bHasColour = false;
		double ColourWeight = -1.0;
		FLinearColor Colour = FLinearColor::Black;
		int32 NextOrder = 0;
	};

	struct FLmVisit
	{
		TSet<FLmVisitKey> Visited;
		int32 Budget = 20000;

		/** True the first time a node is reached in this walk. */
		bool Enter(const FLmRef& Node)
		{
			if (!Node.IsSet() || --Budget < 0)
			{
				return false;
			}
			bool bAlready = false;
			Visited.Add(FLmVisitKey(Node.Expression, Node.Frame), &bAlready);
			return !bAlready;
		}
	};

	bool ClassNamed(const UObject* Object, const TCHAR* Name)
	{
		return Object && Object->GetClass()->GetFName() == FName(Name);
	}

	bool NameHasAny(const FString& Name, std::initializer_list<const TCHAR*> Words)
	{
		for (const TCHAR* Word : Words)
		{
			if (Name.Contains(Word, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	FString JoinNames(const TArray<FString>& Names, const int32 MaxShown = 3)
	{
		TArray<FString> Shown;
		for (int32 Index = 0; Index < Names.Num() && Index < MaxShown; ++Index)
		{
			Shown.Add(Names[Index]);
		}
		FString Text = FString::Join(Shown, TEXT(", "));
		if (Names.Num() > MaxShown)
		{
			Text += FString::Printf(TEXT(" and %d more"), Names.Num() - MaxShown);
		}
		return Text;
	}

	/** The graph walker: resolves what feeds what through reroutes, material function calls and the instance's static switches, and reads values the way the instance sets them. */
	class FLmGraphReader
	{
	public:
		FLmGraphReader(const UMaterialInterface* InContext, const double InSpacingCm)
			: Context(InContext)
			, SpacingCm(InSpacingCm)
		{
		}

		/** Names of the material functions the walk passed through: some say what they do ("SlopeBlend"). */
		TSet<FString> FunctionNames;

		bool bUnreadableUvs = false;

		// ---- Resolving -----------------------------------------------------------------------------------

		const FLmFrame* FrameFor(const UMaterialExpressionMaterialFunctionCall* Call, const FLmFrame* Parent)
		{
			const TPair<const UMaterialExpressionMaterialFunctionCall*, const FLmFrame*> Key(Call, Parent);
			if (TUniquePtr<FLmFrame>* Existing = Frames.Find(Key))
			{
				return Existing->Get();
			}
			TUniquePtr<FLmFrame> Frame = MakeUnique<FLmFrame>();
			Frame->Call = Call;
			Frame->Parent = Parent;
			return Frames.Add(Key, MoveTemp(Frame)).Get();
		}

		/** The expression Input really reads: reroutes, function calls (into the function's output), function inputs (out to the caller's connection) and static switches resolved. */
		FLmRef Resolve(FExpressionInput Input, const FLmFrame* Frame, FLmTrace* Trace = nullptr)
		{
			for (int32 Depth = 0; Depth < 96; ++Depth)
			{
				UMaterialExpression* Expression = Input.Expression;
				if (!Expression)
				{
					return FLmRef();
				}
				const int32 Output = Input.OutputIndex;
				if (Trace && Input.Mask)
				{
					const uint8 Bits = static_cast<uint8>((Input.MaskR ? 1 : 0) | (Input.MaskG ? 2 : 0) | (Input.MaskB ? 4 : 0) | (Input.MaskA ? 8 : 0));
					if (Bits != 0)
					{
						Trace->Masks.Add(Bits);
					}
				}

				if (const UMaterialExpressionRerouteBase* Reroute = Cast<UMaterialExpressionRerouteBase>(Expression))
				{
					Input = Reroute->TraceInputsToRealInput();
					if (Input.Expression == Expression)
					{
						return FLmRef();
					}
					continue;
				}

				if (const UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
				{
					if (Frame && Frame->Call)
					{
						const FFunctionExpressionInput* Entry = Frame->Call->FunctionInputs.FindByPredicate([FunctionInput](const FFunctionExpressionInput& Candidate)
						{
							return Candidate.ExpressionInput.Get() == FunctionInput || (FunctionInput->Id.IsValid() && Candidate.ExpressionInputId == FunctionInput->Id);
						});
						if (Entry && Entry->Input.Expression)
						{
							if (Trace && Trace->FirstInputName.IsNone())
							{
								Trace->FirstInputName = FunctionInput->InputName;
							}
							Input = Entry->Input;
							Frame = Frame->Parent;
							continue;
						}
					}
					if (FunctionInput->bUsePreviewValueAsDefault && FunctionInput->Preview.Expression)
					{
						Input = FunctionInput->Preview;
						continue;
					}
					return FLmRef{ Expression, Output, Frame };
				}

				if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					const UMaterialExpressionFunctionOutput* FunctionOutput = nullptr;
					if (Call->FunctionOutputs.IsValidIndex(Output))
					{
						FunctionOutput = Call->FunctionOutputs[Output].ExpressionOutput;
						if (!FunctionOutput && Call->MaterialFunction)
						{
							for (const TObjectPtr<UMaterialExpression>& Inner : Call->MaterialFunction->GetExpressions())
							{
								const UMaterialExpressionFunctionOutput* Candidate = Cast<UMaterialExpressionFunctionOutput>(Inner);
								if (Candidate && Candidate->OutputName == Call->FunctionOutputs[Output].Output.OutputName)
								{
									FunctionOutput = Candidate;
									break;
								}
							}
						}
					}
					if (!FunctionOutput)
					{
						return FLmRef();
					}
					if (Call->MaterialFunction)
					{
						FunctionNames.Add(Call->MaterialFunction->GetName());
					}
					Input = FunctionOutput->A;
					Frame = FrameFor(Call, Frame);
					if (Trace)
					{
						Trace->EnteredFrame = Frame;
					}
					continue;
				}

				if (const UMaterialExpressionStaticSwitchParameter* SwitchParameter = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
				{
					bool bValue = SwitchParameter->DefaultValue != 0;
					bool bSet = bValue;
					FGuid Guid;
					if (Context && Context->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(SwitchParameter->ParameterName)), bSet, Guid))
					{
						bValue = bSet;
					}
					Input = bValue ? SwitchParameter->A : SwitchParameter->B;
					continue;
				}
				if (const UMaterialExpressionStaticSwitch* Switch = Cast<UMaterialExpressionStaticSwitch>(Expression))
				{
					bool bValue = Switch->DefaultValue != 0;
					if (Switch->Value.Expression)
					{
						const FLmRef Condition = Resolve(Switch->Value, Frame);
						if (const UMaterialExpressionStaticBool* StaticBool = Cast<UMaterialExpressionStaticBool>(Condition.Expression))
						{
							bValue = StaticBool->Value != 0;
						}
					}
					Input = bValue ? Switch->A : Switch->B;
					continue;
				}

				return FLmRef{ Expression, Output, Frame };
			}
			return FLmRef();
		}

		/**
		 * Every connected input of Node, resolved. For Make/Set Material Attributes only the input Prop reads (and the attributes passed along).
		 * OutInputNames, when given, gets one entry per child: the name of the first function input its connection came out of, or none.
		 */
		void Children(const FLmRef& Node, const ELmProp Prop, TArray<FLmRef>& Out, TArray<FName>* OutInputNames = nullptr)
		{
			UMaterialExpression* Expression = const_cast<UMaterialExpression*>(Node.Expression);
			const bool bAttributes = ClassNamed(Expression, TEXT("MaterialExpressionMakeMaterialAttributes"))
				|| ClassNamed(Expression, TEXT("MaterialExpressionSetMaterialAttributes"));
			for (FExpressionInputIterator It{ Expression }; It; ++It)
			{
				const FExpressionInput* Input = It.Input;
				if (!Input || !Input->Expression)
				{
					continue;
				}
				if (bAttributes)
				{
					const FString Name = Expression->GetInputName(It.Index).ToString().Replace(TEXT(" "), TEXT(""));
					const bool bWanted = Name.Equals(Prop == ELmProp::BaseColor ? TEXT("BaseColor") : TEXT("Normal"), ESearchCase::IgnoreCase)
						|| NameHasAny(Name, { TEXT("Attr"), TEXT("MaterialAttributes") });
					if (!bWanted)
					{
						continue;
					}
				}
				FLmTrace Trace;
				const FLmRef Child = Resolve(*Input, Node.Frame, &Trace);
				if (Child.IsSet())
				{
					Out.Add(Child);
					if (OutInputNames)
					{
						OutInputNames->Add(Trace.FirstInputName);
					}
				}
			}
		}

		// ---- Values --------------------------------------------------------------------------------------

		/** A constant, or a parameter as the instance sets it, as one number (a vector's first component). */
		bool ScalarValue(const FLmRef& Node, double& Out, const int32 Depth = 0)
		{
			if (!Node.IsSet() || Depth > 16)
			{
				return false;
			}
			const UMaterialExpression* Expression = Node.Expression;
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
			if (const UMaterialExpressionConstant2Vector* Vector2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
			{
				Out = Vector2->R;
				return true;
			}
			if (const UMaterialExpressionConstant3Vector* Vector3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
			{
				Out = Vector3->Constant.R;
				return true;
			}
			if (const UMaterialExpressionConstant4Vector* Vector4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
			{
				Out = Vector4->Constant.R;
				return true;
			}
			if (const UMaterialExpressionVectorParameter* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				FLinearColor Value = VectorParameter->DefaultValue;
				if (Context)
				{
					Context->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(VectorParameter->ParameterName)), Value);
				}
				Out = Value.R;
				return true;
			}
			if (const UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				// Only reached unconnected: the function's own default.
				if (FunctionInput->bUsePreviewValueAsDefault && !FunctionInput->Preview.Expression)
				{
					Out = FunctionInput->PreviewValue.X;
					return true;
				}
				return false;
			}
			if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
			{
				return ScalarValue(Resolve(Append->A, Node.Frame), Out, Depth + 1);
			}
			if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				return ScalarValue(Resolve(Mask->Input, Node.Frame), Out, Depth + 1);
			}
			double A = 0.0;
			double B = 0.0;
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				if (ScalarInput(Multiply->A, Multiply->ConstA, Node.Frame, A, Depth) && ScalarInput(Multiply->B, Multiply->ConstB, Node.Frame, B, Depth))
				{
					Out = A * B;
					return true;
				}
				return false;
			}
			if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				if (ScalarInput(Divide->A, Divide->ConstA, Node.Frame, A, Depth) && ScalarInput(Divide->B, Divide->ConstB, Node.Frame, B, Depth) && B != 0.0)
				{
					Out = A / B;
					return true;
				}
				return false;
			}
			if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
			{
				if (ScalarInput(Add->A, Add->ConstA, Node.Frame, A, Depth) && ScalarInput(Add->B, Add->ConstB, Node.Frame, B, Depth))
				{
					Out = A + B;
					return true;
				}
				return false;
			}
			return false;
		}

		/** An input's value: what is connected to it, or its own constant when nothing is. */
		bool ScalarInput(const FExpressionInput& Input, const float Fallback, const FLmFrame* Frame, double& Out, const int32 Depth)
		{
			if (!Input.Expression)
			{
				Out = Fallback;
				return true;
			}
			return ScalarValue(Resolve(Input, Frame), Out, Depth + 1);
		}

		/** A colour constant or parameter (for a layer's flat colour). */
		bool ColourValue(const FLmRef& Node, FLinearColor& Out) const
		{
			const UMaterialExpression* Expression = Node.Expression;
			if (const UMaterialExpressionConstant3Vector* Vector3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
			{
				Out = Vector3->Constant;
				return true;
			}
			if (const UMaterialExpressionConstant4Vector* Vector4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
			{
				Out = Vector4->Constant;
				return true;
			}
			if (const UMaterialExpressionVectorParameter* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				FLinearColor Value = Parameter->DefaultValue;
				if (Context)
				{
					Context->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Value);
				}
				Out = Value;
				return true;
			}
			return false;
		}

		/** A scalar parameter's value as the instance sets it. */
		float ScalarParameterValue(const UMaterialExpressionScalarParameter* Parameter) const
		{
			float Value = Parameter->DefaultValue;
			float Set = 0.0f;
			if (Context && Context->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Set))
			{
				Value = Set;
			}
			return Value;
		}

		/** A vector parameter's value as the instance sets it. */
		FLinearColor VectorParameterValue(const UMaterialExpressionVectorParameter* Parameter) const
		{
			FLinearColor Value = Parameter->DefaultValue;
			if (Context)
			{
				Context->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Value);
			}
			return Value;
		}

		// ---- Textures and their tiling -------------------------------------------------------------------

		UTexture* TextureOf(const UMaterialExpressionTextureBase* Node)
		{
			if (!Node)
			{
				return nullptr;
			}
			if (const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Node))
			{
				UTexture* Value = nullptr;
				if (Context && Context->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Value) && Value)
				{
					return Value;
				}
			}
			return Node->Texture;
		}

		/** A Texture Sample of a 2D texture (not a texture object, which a function passes on), and the texture it reads. */
		UTexture* SampledTexture(const FLmRef& Node, const UMaterialExpressionTextureSample*& OutSample)
		{
			OutSample = nullptr;
			const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Node.Expression);
			if (!Sample || ClassNamed(Sample, TEXT("MaterialExpressionTextureObjectParameter")))
			{
				return nullptr;
			}
			UTexture* Texture = nullptr;
			if (Sample->TextureObject.Expression)
			{
				const FLmRef Object = Resolve(Sample->TextureObject, Node.Frame);
				Texture = TextureOf(Cast<UMaterialExpressionTextureBase>(Object.Expression));
			}
			else
			{
				Texture = TextureOf(Sample);
			}
			if (!Cast<UTexture2D>(Texture))
			{
				return nullptr;
			}
			OutSample = Sample;
			return Texture;
		}

		/**
		 * cm per repeat of a texture sampled at Node's coordinates, as far as the graph says: the landscape's
		 * layer coordinates (one UV unit per landscape sample, so MappingScale x the landscape's spacing), Texture
		 * Coordinate 0 (the same unit, divided by its tiling), or a world position (one UV unit per cm), through
		 * Multiply and Divide by a constant or parameter, offsets, panners, masks and appends.
		 */
		bool TileCm(const FLmRef& Node, double& Out, const int32 Depth = 0)
		{
			if (!Node.IsSet() || Depth > 16)
			{
				return false;
			}
			const UMaterialExpression* Expression = Node.Expression;
			if (const UMaterialExpressionLandscapeLayerCoords* Coords = Cast<UMaterialExpressionLandscapeLayerCoords>(Expression))
			{
				if (Coords->CustomUVType != LCCT_None || (Coords->MappingType != TCMT_Auto && Coords->MappingType != TCMT_XY))
				{
					bUnreadableUvs = true;
					return false;
				}
				Out = SpacingCm * (Coords->MappingScale == 0.0f ? 1.0 : static_cast<double>(FMath::Abs(Coords->MappingScale)));
				return true;
			}
			if (const UMaterialExpressionTextureCoordinate* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Expression))
			{
				if (TexCoord->CoordinateIndex != 0)
				{
					bUnreadableUvs = true;
					return false;
				}
				Out = SpacingCm / FMath::Max(static_cast<double>(FMath::Abs(TexCoord->UTiling)), 0.0001);
				return true;
			}
			if (ClassNamed(Expression, TEXT("MaterialExpressionWorldPosition")))
			{
				Out = 1.0;
				return true;
			}
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				double Scale = 0.0;
				if (TileInput(Multiply->A, Node.Frame, Out, Depth) && ScalarInput(Multiply->B, Multiply->ConstB, Node.Frame, Scale, Depth) && Scale != 0.0)
				{
					Out /= FMath::Abs(Scale);
					return true;
				}
				if (TileInput(Multiply->B, Node.Frame, Out, Depth) && ScalarInput(Multiply->A, Multiply->ConstA, Node.Frame, Scale, Depth) && Scale != 0.0)
				{
					Out /= FMath::Abs(Scale);
					return true;
				}
				return false;
			}
			if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				double Scale = 0.0;
				if (TileInput(Divide->A, Node.Frame, Out, Depth) && ScalarInput(Divide->B, Divide->ConstB, Node.Frame, Scale, Depth))
				{
					Out *= FMath::Abs(Scale);
					return Scale != 0.0;
				}
				return false;
			}
			if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
			{
				return TileInput(Add->A, Node.Frame, Out, Depth) || TileInput(Add->B, Node.Frame, Out, Depth);
			}
			if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
			{
				return TileInput(Append->A, Node.Frame, Out, Depth) || TileInput(Append->B, Node.Frame, Out, Depth);
			}
			if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				return TileInput(Mask->Input, Node.Frame, Out, Depth);
			}
			if (const UMaterialExpressionPanner* Panner = Cast<UMaterialExpressionPanner>(Expression))
			{
				return TileInput(Panner->Coordinate, Node.Frame, Out, Depth);
			}
			return false;
		}

		bool TileInput(const FExpressionInput& Input, const FLmFrame* Frame, double& Out, const int32 Depth)
		{
			return Input.Expression && TileCm(Resolve(Input, Frame), Out, Depth + 1);
		}

		/** The tiling of one sample, in cm. 0 when the graph does not say. */
		double SampleTileCm(const FLmRef& Node, const UMaterialExpressionTextureSample* Sample)
		{
			double Cm = 0.0;
			if (Sample->Coordinates.Expression)
			{
				return TileCm(Resolve(Sample->Coordinates, Node.Frame), Cm) ? Cm : 0.0;
			}
			if (Sample->ConstCoordinate == 0)
			{
				return SpacingCm;
			}
			bUnreadableUvs = true;
			return 0.0;
		}

		// ---- What the graph does that a flat layer cannot keep -------------------------------------------

		/** Flags what Node's subgraph reads from the surface normal, the world height or the camera. Texture samples are not entered: their inputs are UVs. */
		void ScanFlags(const FLmRef& Node, const ELmProp Prop, FLmFlags& Out, FLmVisit& Visit)
		{
			if (!Visit.Enter(Node))
			{
				return;
			}
			const UMaterialExpression* Expression = Node.Expression;
			if (ClassNamed(Expression, TEXT("MaterialExpressionVertexNormalWS")) || ClassNamed(Expression, TEXT("MaterialExpressionPixelNormalWS")))
			{
				Out.bSlope = true;
			}
			else if (ClassNamed(Expression, TEXT("MaterialExpressionWorldPosition")) || ClassNamed(Expression, TEXT("MaterialExpressionActorPositionWS")))
			{
				Out.bHeight = true;
			}
			else if (ClassNamed(Expression, TEXT("MaterialExpressionCameraPositionWS")) || ClassNamed(Expression, TEXT("MaterialExpressionPixelDepth"))
				|| ClassNamed(Expression, TEXT("MaterialExpressionDistanceCullFade")) || ClassNamed(Expression, TEXT("MaterialExpressionDistance")))
			{
				Out.bDistance = true;
			}
			else if (const UMaterialExpressionLandscapeLayerBlend* Blend = Cast<UMaterialExpressionLandscapeLayerBlend>(Expression))
			{
				for (const FLayerBlendInput& Layer : Blend->Layers)
				{
					if (Layer.BlendType == LB_HeightBlend)
					{
						Out.bHeight = true;
					}
				}
			}
			if (Cast<UMaterialExpressionTextureSample>(Expression) && !ClassNamed(Expression, TEXT("MaterialExpressionTextureObjectParameter")))
			{
				return;
			}
			TArray<FLmRef> Children_;
			Children(Node, Prop, Children_);
			for (const FLmRef& Child : Children_)
			{
				ScanFlags(Child, Prop, Out, Visit);
			}
		}

		// ---- The slope overlay ---------------------------------------------------------------------------

		/** Whether Node's subgraph names a paint layer (a layer blend, layer weight or layer sample). Texture samples are not entered. */
		bool ContainsLayers(const FLmRef& Node, const ELmProp Prop, FLmVisit& Visit)
		{
			if (!Visit.Enter(Node))
			{
				return false;
			}
			const UMaterialExpression* Expression = Node.Expression;
			if (Cast<UMaterialExpressionLandscapeLayerBlend>(Expression) || Cast<UMaterialExpressionLandscapeLayerWeight>(Expression)
				|| Cast<UMaterialExpressionLandscapeLayerSample>(Expression))
			{
				return true;
			}
			if (Cast<UMaterialExpressionTextureSample>(Expression) && !ClassNamed(Expression, TEXT("MaterialExpressionTextureObjectParameter")))
			{
				return false;
			}
			TArray<FLmRef> Next;
			Children(Node, Prop, Next);
			for (const FLmRef& Child : Next)
			{
				if (ContainsLayers(Child, Prop, Visit))
				{
					return true;
				}
			}
			return false;
		}

		static bool IsNormalNode(const FLmRef& Node)
		{
			return ClassNamed(Node.Expression, TEXT("MaterialExpressionVertexNormalWS")) || ClassNamed(Node.Expression, TEXT("MaterialExpressionPixelNormalWS"));
		}

		/**
		 * The value of Node, a scalar graph that reads the world normal only as its Z (a mask of the normal's B, or a dot
		 * product of the normal with the up vector), with NormalZ as that Z: Add, Subtract, Multiply, Divide, Power, Clamp,
		 * Saturate, One Minus and Abs over constants and parameters (as the instance sets them), through material
		 * functions. A subgraph that does not read the normal and is not a constant or parameter (the layer's own
		 * colour, used as a height by a height-blended overlay) is taken as Height; bUsedHeight says one was. False for
		 * anything else that reads the normal: it is not a slope mask this can evaluate.
		 */
		bool EvalSlope(const FLmRef& Node, const double NormalZ, const double Height, double& Out, bool& bUsedHeight, const int32 Depth = 0)
		{
			if (!Node.IsSet() || Depth > 24)
			{
				return false;
			}
			const UMaterialExpression* Expression = Node.Expression;
			const FLmFrame* Frame = Node.Frame;
			auto Value = [&](const FExpressionInput& Input, const float Fallback, double& OutValue) -> bool
			{
				if (!Input.Expression)
				{
					OutValue = Fallback;
					return true;
				}
				return EvalSlope(Resolve(Input, Frame), NormalZ, Height, OutValue, bUsedHeight, Depth + 1);
			};

			if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				const FLmRef Source = Resolve(Mask->Input, Frame);
				if (IsNormalNode(Source))
				{
					if (Mask->B && !Mask->R && !Mask->G && !Mask->A)
					{
						Out = NormalZ;
						return true;
					}
					return false;
				}
				return EvalSlope(Source, NormalZ, Height, Out, bUsedHeight, Depth + 1);
			}
			if (IsNormalNode(Node))
			{
				return false;
			}
			if (const UMaterialExpressionDotProduct* Dot = Cast<UMaterialExpressionDotProduct>(Expression))
			{
				// The normal with the up vector (0, 0, 1) is its Z.
				const FLmRef A = Resolve(Dot->A, Frame);
				const FLmRef B = Resolve(Dot->B, Frame);
				const FLmRef& Other = IsNormalNode(A) ? B : A;
				const UMaterialExpressionConstant3Vector* Up = Cast<UMaterialExpressionConstant3Vector>(Other.Expression);
				if ((IsNormalNode(A) || IsNormalNode(B)) && Up && Up->Constant.R == 0.0f && Up->Constant.G == 0.0f)
				{
					Out = NormalZ * Up->Constant.B;
					return true;
				}
				return false;
			}
			double A = 0.0;
			double B = 0.0;
			if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
			{
				if (!Value(Add->A, Add->ConstA, A) || !Value(Add->B, Add->ConstB, B))
				{
					return false;
				}
				Out = A + B;
				return true;
			}
			if (const UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
			{
				if (!Value(Subtract->A, Subtract->ConstA, A) || !Value(Subtract->B, Subtract->ConstB, B))
				{
					return false;
				}
				Out = A - B;
				return true;
			}
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				if (!Value(Multiply->A, Multiply->ConstA, A) || !Value(Multiply->B, Multiply->ConstB, B))
				{
					return false;
				}
				Out = A * B;
				return true;
			}
			if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				if (!Value(Divide->A, Divide->ConstA, A) || !Value(Divide->B, Divide->ConstB, B) || B == 0.0)
				{
					return false;
				}
				Out = A / B;
				return true;
			}
			if (const UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(Expression))
			{
				if (!Value(Power->Base, 0.0f, A) || !Value(Power->Exponent, Power->ConstExponent, B))
				{
					return false;
				}
				Out = FMath::Pow(FMath::Max(A, 0.0), B);
				return true;
			}
			if (const UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(Expression))
			{
				double Low = Clamp->MinDefault;
				double High = Clamp->MaxDefault;
				if (!Value(Clamp->Input, 0.0f, A) || !Value(Clamp->Min, Clamp->MinDefault, Low) || !Value(Clamp->Max, Clamp->MaxDefault, High))
				{
					return false;
				}
				Out = Clamp->ClampMode == CMODE_ClampMin ? FMath::Max(A, Low) : Clamp->ClampMode == CMODE_ClampMax ? FMath::Min(A, High) : FMath::Clamp(A, Low, FMath::Max(Low, High));
				return true;
			}
			if (const UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(Expression))
			{
				if (!Value(Saturate->Input, 0.0f, A))
				{
					return false;
				}
				Out = FMath::Clamp(A, 0.0, 1.0);
				return true;
			}
			if (const UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(Expression))
			{
				if (!Value(OneMinus->Input, 0.0f, A))
				{
					return false;
				}
				Out = 1.0 - A;
				return true;
			}
			if (const UMaterialExpressionAbs* Abs = Cast<UMaterialExpressionAbs>(Expression))
			{
				if (!Value(Abs->Input, 0.0f, A))
				{
					return false;
				}
				Out = FMath::Abs(A);
				return true;
			}
			if (ScalarValue(Node, Out))
			{
				return true;
			}
			// Not a constant and not the normal: it must not read the normal anywhere, and then it is the overlay's height.
			FLmFlags Flags;
			FLmVisit Visit;
			ScanFlags(Node, ELmProp::BaseColor, Flags, Visit);
			if (Flags.bSlope)
			{
				return false;
			}
			bUsedHeight = true;
			Out = Height;
			return true;
		}

		/**
		 * Whether Lerp is a slope overlay: its alpha reads the normal's Z (and is a graph EvalSlope can evaluate), and
		 * exactly one of its inputs holds the paint layers. Fills Out; the caller goes on walking the painted input.
		 */
		bool AsSlopeOverlay(const UMaterialExpressionLinearInterpolate* Lerp, const FLmFrame* Frame, const ELmProp Prop, FLmSlopeOverlay& Out, FLmRef& OutPainted)
		{
			if (!Lerp->Alpha.Expression || !Lerp->A.Expression || !Lerp->B.Expression)
			{
				return false;
			}
			const FLmRef Alpha = Resolve(Lerp->Alpha, Frame);
			FLmFlags Flags;
			FLmVisit FlagVisit;
			ScanFlags(Alpha, Prop, Flags, FlagVisit);
			if (!Flags.bSlope)
			{
				return false;
			}
			const FLmRef A = Resolve(Lerp->A, Frame);
			const FLmRef B = Resolve(Lerp->B, Frame);
			FLmVisit VisitA;
			FLmVisit VisitB;
			const bool bLayersInA = ContainsLayers(A, Prop, VisitA);
			const bool bLayersInB = ContainsLayers(B, Prop, VisitB);
			if (bLayersInA == bLayersInB)
			{
				return false;
			}
			// A flat and a steep ground both give a number, or it is not a mask this can bake.
			double Flat = 0.0;
			double Steep = 0.0;
			bool bUsedHeight = false;
			if (!EvalSlope(Alpha, 1.0, 0.5, Flat, bUsedHeight) || !EvalSlope(Alpha, 0.5, 0.5, Steep, bUsedHeight))
			{
				return false;
			}
			Out.bFound = true;
			Out.Alpha = Alpha;
			Out.bOverlayIsA = !bLayersInA;
			Out.Look = bLayersInA ? B : A;
			OutPainted = bLayersInA ? A : B;
			return true;
		}

		// ---- Finding the layers --------------------------------------------------------------------------

		/** Whether Alpha is a layer's weight (through Saturate, Clamp, Abs, a mask or One Minus, which flips it). */
		bool AsLayerMask(FLmRef Alpha, FName& OutLayer, bool& bOutInverted)
		{
			bOutInverted = false;
			for (int32 Depth = 0; Depth < 8 && Alpha.IsSet(); ++Depth)
			{
				const UMaterialExpression* Expression = Alpha.Expression;
				if (const UMaterialExpressionLandscapeLayerSample* Sample = Cast<UMaterialExpressionLandscapeLayerSample>(Expression))
				{
					OutLayer = Sample->ParameterName;
					return !OutLayer.IsNone();
				}
				if (const UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(Expression))
				{
					bOutInverted = !bOutInverted;
					Alpha = Resolve(OneMinus->Input, Alpha.Frame);
				}
				else if (const UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(Expression))
				{
					Alpha = Resolve(Saturate->Input, Alpha.Frame);
				}
				else if (const UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(Expression))
				{
					Alpha = Resolve(Clamp->Input, Alpha.Frame);
				}
				else if (const UMaterialExpressionAbs* Abs = Cast<UMaterialExpressionAbs>(Expression))
				{
					Alpha = Resolve(Abs->Input, Alpha.Frame);
				}
				else if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
				{
					Alpha = Resolve(Mask->Input, Alpha.Frame);
				}
				else
				{
					break;
				}
			}
			return false;
		}

		/**
		 * The paint layers the graph under Node names and the input that is each one's look: Landscape Layer
		 * Blend's layers, Layer Weight's layer (its base goes on being walked), a Lerp whose alpha is a Layer
		 * Sample (its B is the layer, its A goes on) and Layer Switch's used branch. Anything else is walked,
		 * and the texture samples it reaches outside every layer are noted.
		 */
		void FindLayers(const FLmRef& Node, const ELmProp Prop, FLmLayerSet& Out, FLmVisit& Visit)
		{
			if (!Visit.Enter(Node))
			{
				return;
			}
			const UMaterialExpression* Expression = Node.Expression;
			if (const UMaterialExpressionLandscapeLayerBlend* Blend = Cast<UMaterialExpressionLandscapeLayerBlend>(Expression))
			{
				for (const FLayerBlendInput& Layer : Blend->Layers)
				{
					Out.Add(Layer.LayerName, Layer.LayerInput.Expression ? Resolve(Layer.LayerInput, Node.Frame) : FLmRef());
					if (FLmLayerSource* Source = Out.Find(Layer.LayerName))
					{
						if (!Layer.LayerInput.Expression && !Source->Look.IsSet())
						{
							Source->bConstant = true;
							Source->Constant = FLinearColor(static_cast<float>(Layer.ConstLayerInput.X), static_cast<float>(Layer.ConstLayerInput.Y), static_cast<float>(Layer.ConstLayerInput.Z));
						}
					}
					if (Layer.BlendType == LB_HeightBlend)
					{
						Out.bHeightBlend = true;
					}
				}
				return;
			}
			if (const UMaterialExpressionLandscapeLayerWeight* Weight = Cast<UMaterialExpressionLandscapeLayerWeight>(Expression))
			{
				Out.Add(Weight->ParameterName, Weight->Layer.Expression ? Resolve(Weight->Layer, Node.Frame) : FLmRef());
				if (Weight->Base.Expression)
				{
					FindLayers(Resolve(Weight->Base, Node.Frame), Prop, Out, Visit);
				}
				return;
			}
			if (const UMaterialExpressionLandscapeLayerSwitch* Switch = Cast<UMaterialExpressionLandscapeLayerSwitch>(Expression))
			{
				// The layer's own look is wherever the used branch blends it; the switch only says whether the layer exists.
				if (Switch->LayerUsed.Expression)
				{
					FindLayers(Resolve(Switch->LayerUsed, Node.Frame), Prop, Out, Visit);
				}
				return;
			}
			if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
			{
				FName Layer;
				bool bInverted = false;
				if (Lerp->Alpha.Expression && AsLayerMask(Resolve(Lerp->Alpha, Node.Frame), Layer, bInverted))
				{
					const FExpressionInput& Look = bInverted ? Lerp->A : Lerp->B;
					const FExpressionInput& Rest = bInverted ? Lerp->B : Lerp->A;
					Out.Add(Layer, Look.Expression ? Resolve(Look, Node.Frame) : FLmRef());
					if (Rest.Expression)
					{
						FindLayers(Resolve(Rest, Node.Frame), Prop, Out, Visit);
					}
					return;
				}
				// A Lerp over the paint layers by the surface normal: the overlay is baked into a layer of its own, so only the painted side is walked.
				FLmRef Painted;
				if (!Out.Slope.bFound && AsSlopeOverlay(Lerp, Node.Frame, Prop, Out.Slope, Painted))
				{
					FindLayers(Painted, Prop, Out, Visit);
					return;
				}
			}
			if (Cast<UMaterialExpressionTextureSample>(Expression))
			{
				const UMaterialExpressionTextureSample* Found = nullptr;
				if (UTexture* Texture = SampledTexture(Node, Found))
				{
					const bool bNormalMap = Texture->IsNormalMap();
					if ((Prop == ELmProp::Normal) == bNormalMap)
					{
						Out.GlobalSampleNames.AddUnique(Texture->GetName());
					}
				}
				return;
			}
			TArray<FLmRef> Next;
			Children(Node, Prop, Next);
			for (const FLmRef& Child : Next)
			{
				FindLayers(Child, Prop, Out, Visit);
			}
		}

		// ---- Reading a layer's look ----------------------------------------------------------------------

		/**
		 * The texture samples (and flat colours) under Node, each with the share of the layer it makes up: a Lerp
		 * with a constant alpha splits by it; one by the camera distance favours its A (the near tiling); one by
		 * the surface normal or height favours A (the flat ground); any other splits evenly. The heaviest, then the
		 * finest tiled, is what the layer looks like.
		 */
		void Collect(const FLmRef& Node, const ELmProp Prop, const double Weight, FLmCollected& Out, FLmVisit& Visit)
		{
			if (!Visit.Enter(Node))
			{
				return;
			}
			const UMaterialExpression* Expression = Node.Expression;
			if (Cast<UMaterialExpressionLandscapeLayerBlend>(Expression) || Cast<UMaterialExpressionLandscapeLayerSample>(Expression))
			{
				return; // another blend nested in a layer: its layers are its own
			}
			const UMaterialExpressionTextureSample* Sample = nullptr;
			if (UTexture* Texture = SampledTexture(Node, Sample))
			{
				if ((Prop == ELmProp::Normal) == Texture->IsNormalMap())
				{
					FLmHit& Hit = Out.Hits.AddDefaulted_GetRef();
					Hit.Texture = Texture;
					Hit.Weight = Weight;
					Hit.TileCm = SampleTileCm(Node, Sample);
					Hit.Order = Out.NextOrder++;
					Hit.Name = Texture->GetName();
				}
				return;
			}
			if (Cast<UMaterialExpressionTextureSample>(Expression))
			{
				return; // a texture object a function passes on, or a texture that is not 2D
			}
			FLinearColor Colour;
			if (Prop == ELmProp::BaseColor && ColourValue(Node, Colour))
			{
				if (Weight > Out.ColourWeight)
				{
					Out.bHasColour = true;
					Out.ColourWeight = Weight;
					Out.Colour = Colour;
				}
				return;
			}
			if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
			{
				double WeightA = 0.5;
				double WeightB = 0.5;
				double Alpha = 0.0;
				if (!Lerp->Alpha.Expression)
				{
					Alpha = FMath::Clamp(static_cast<double>(Lerp->ConstAlpha), 0.0, 1.0);
					WeightA = 1.0 - Alpha;
					WeightB = Alpha;
				}
				else
				{
					const FLmRef AlphaRef = Resolve(Lerp->Alpha, Node.Frame);
					if (ScalarValue(AlphaRef, Alpha))
					{
						Alpha = FMath::Clamp(Alpha, 0.0, 1.0);
						WeightA = 1.0 - Alpha;
						WeightB = Alpha;
					}
					else
					{
						FLmFlags Flags;
						FLmVisit AlphaVisit;
						ScanFlags(AlphaRef, Prop, Flags, AlphaVisit);
						if (Flags.bDistance)
						{
							WeightA = 0.9;
							WeightB = 0.1;
						}
						else if (Flags.bSlope || Flags.bHeight)
						{
							WeightA = 0.6;
							WeightB = 0.4;
						}
					}
				}
				if (Lerp->A.Expression)
				{
					Collect(Resolve(Lerp->A, Node.Frame), Prop, Weight * WeightA, Out, Visit);
				}
				if (Lerp->B.Expression)
				{
					Collect(Resolve(Lerp->B, Node.Frame), Prop, Weight * WeightB, Out, Visit);
				}
				return;
			}
			TArray<FLmRef> Next;
			Children(Node, Prop, Next);
			for (const FLmRef& Child : Next)
			{
				Collect(Child, Prop, Weight, Out, Visit);
			}
		}

	private:
		const UMaterialInterface* Context = nullptr;
		double SpacingCm = 100.0;
		TMap<TPair<const UMaterialExpressionMaterialFunctionCall*, const FLmFrame*>, TUniquePtr<FLmFrame>> Frames;
	};

	/** The hit that is the layer's look: the heaviest, then the finest tiled (known sizes first), then the first found. */
	const FLmHit* PickBest(const TArray<FLmHit>& Hits)
	{
		const FLmHit* Best = nullptr;
		auto IsBetter = [](const FLmHit& A, const FLmHit& B)
		{
			const int32 WeightA = FMath::RoundToInt32(A.Weight * 20.0);
			const int32 WeightB = FMath::RoundToInt32(B.Weight * 20.0);
			if (WeightA != WeightB)
			{
				return WeightA > WeightB;
			}
			const bool bKnownA = A.TileCm > 0.0;
			const bool bKnownB = B.TileCm > 0.0;
			if (bKnownA != bKnownB)
			{
				return bKnownA;
			}
			if (bKnownA && !FMath::IsNearlyEqual(A.TileCm, B.TileCm, 0.5))
			{
				return A.TileCm < B.TileCm;
			}
			return A.Order < B.Order;
		};
		for (const FLmHit& Hit : Hits)
		{
			if (!Best || IsBetter(Hit, *Best))
			{
				Best = &Hit;
			}
		}
		return Best;
	}

	/** The textures in Hits other than Best's, by name, once each. */
	TArray<FString> OtherTextures(const TArray<FLmHit>& Hits, const FLmHit* Best, bool& bOutMacro)
	{
		TArray<FString> Names;
		bOutMacro = false;
		for (const FLmHit& Hit : Hits)
		{
			if (!Best || Hit.Texture == Best->Texture)
			{
				continue;
			}
			Names.AddUnique(Hit.Name);
			if (NameHasAny(Hit.Name, { TEXT("macro"), TEXT("variation") }) || (Best->TileCm > 0.0 && Hit.TileCm >= Best->TileCm * 8.0))
			{
				bOutMacro = true;
			}
		}
		return Names;
	}

	/** Hex of the first 4 bytes of an MD5 of Text: a short, stable name part. */
	FString ShortHash(const FString& Text)
	{
		FMD5 Md5;
		const FTCHARToUTF8 Utf8(*Text);
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		return FString::Printf(TEXT("%02x%02x%02x%02x"), Digest[0], Digest[1], Digest[2], Digest[3]);
	}

	FString SafeName(const FString& In)
	{
		FString Out;
		for (const TCHAR Char : In)
		{
			Out.AppendChar(Char < 128 && FChar::IsAlnum(Char) ? Char : TEXT('_'));
		}
		return Out.Left(40);
	}
}

// The compiled surface programs. Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleLandscapeMaterialReaderPrivate
{
	/** A value of one to four components; the ones past an operation's width are not read. */
	struct FLmValue
	{
		double V[4];
	};

	enum class ELmOp : uint8
	{
		Const,
		WorldPosition,
		Normal,
		Add,
		Subtract,
		Multiply,
		Divide,
		Power,
		Min,
		Max,
		Clamp,
		Saturate,
		OneMinus,
		Abs,
		Lerp,
		Dot,
		Mask,
		Append,
		TangentToWorld,
		LandscapeUv,
		Texture
	};

	/** The pixels of a mask texture's source (mip 0), kept so a mask can sample it at the UVs its graph gives: 8 bits a channel, bilinear, wrapped as the texture says. */
	struct FLmTexture
	{
		int32 SizeX = 0;
		int32 SizeY = 0;
		bool bSrgb = false;
		bool bClampX = false;
		bool bClampY = false;
		TArray<FColor> Pixels;

		FLinearColor Texel(int32 X, int32 Y) const
		{
			X = bClampX ? FMath::Clamp(X, 0, SizeX - 1) : ((X % SizeX) + SizeX) % SizeX;
			Y = bClampY ? FMath::Clamp(Y, 0, SizeY - 1) : ((Y % SizeY) + SizeY) % SizeY;
			const FColor Pixel = Pixels[static_cast<int64>(Y) * SizeX + X];
			return bSrgb ? FLinearColor::FromSRGBColor(Pixel) : FLinearColor(Pixel.R / 255.0f, Pixel.G / 255.0f, Pixel.B / 255.0f, Pixel.A / 255.0f);
		}

		/** The bilinear sample at (U, V), with texel centres at (i + 0.5) / size and V counted down from the top row, as a texture sample reads it. */
		void Sample(const double U, const double V, double* OutRgba) const
		{
			const double X = U * SizeX - 0.5;
			const double Y = V * SizeY - 0.5;
			const double FloorX = FMath::FloorToDouble(X);
			const double FloorY = FMath::FloorToDouble(Y);
			const float FractionX = static_cast<float>(X - FloorX);
			const float FractionY = static_cast<float>(Y - FloorY);
			const int32 X0 = static_cast<int32>(FloorX);
			const int32 Y0 = static_cast<int32>(FloorY);
			const FLinearColor Top = FMath::Lerp(Texel(X0, Y0), Texel(X0 + 1, Y0), FractionX);
			const FLinearColor Bottom = FMath::Lerp(Texel(X0, Y0 + 1), Texel(X0 + 1, Y0 + 1), FractionX);
			const FLinearColor Result = FMath::Lerp(Top, Bottom, FractionY);
			OutRgba[0] = Result.R;
			OutRgba[1] = Result.G;
			OutRgba[2] = Result.B;
			OutRgba[3] = Result.A;
		}
	};

	/** One operation: its result is register (its index in the list), built from the registers A, B and C. */
	struct FLmOp
	{
		ELmOp Kind = ELmOp::Const;

		/** Components the result has, and each operand has (an operand of 1 component is used for every component). */
		uint8 Width = 1;
		uint8 WidthA = 1;
		uint8 WidthB = 1;
		uint8 WidthC = 1;

		/** Mask: the operand component each result component is. Clamp: 0 both limits, 1 the minimum only, 2 the maximum only. */
		uint8 Pick[4] = { 0, 0, 0, 0 };

		int32 A = INDEX_NONE;
		int32 B = INDEX_NONE;
		int32 C = INDEX_NONE;
		FLmValue Value = {};

		/** Texture: the pixels A (a UV) samples. */
		TSharedPtr<const FLmTexture> Texture;
	};

	double LmComp(const FLmValue& Value, const uint8 Width, const int32 Index)
	{
		return Width == 1 ? Value.V[0] : Value.V[Index];
	}

	/** Runs Op on its operands' values. Sample is null while folding constants, which the ops that read it are never asked to do. */
	void LmExec(const FLmOp& Op, const FLmValue& A, const FLmValue& B, const FLmValue& C, const FMantleSurfaceSample* Sample, FLmValue& Out)
	{
		const int32 Width = Op.Width;
		switch (Op.Kind)
		{
		case ELmOp::Const:
			Out = Op.Value;
			break;
		case ELmOp::WorldPosition:
			Out.V[0] = Sample->WorldPosition.X;
			Out.V[1] = Sample->WorldPosition.Y;
			Out.V[2] = Sample->WorldPosition.Z;
			break;
		case ELmOp::Normal:
			Out.V[0] = Sample->Normal.X;
			Out.V[1] = Sample->Normal.Y;
			Out.V[2] = Sample->Normal.Z;
			break;
		case ELmOp::Add:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = LmComp(A, Op.WidthA, Index) + LmComp(B, Op.WidthB, Index);
			}
			break;
		case ELmOp::Subtract:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = LmComp(A, Op.WidthA, Index) - LmComp(B, Op.WidthB, Index);
			}
			break;
		case ELmOp::Multiply:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = LmComp(A, Op.WidthA, Index) * LmComp(B, Op.WidthB, Index);
			}
			break;
		case ELmOp::Divide:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				const double Divisor = LmComp(B, Op.WidthB, Index);
				Out.V[Index] = Divisor != 0.0 ? LmComp(A, Op.WidthA, Index) / Divisor : 0.0;
			}
			break;
		case ELmOp::Power:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = FMath::Pow(FMath::Max(LmComp(A, Op.WidthA, Index), 0.0), LmComp(B, Op.WidthB, Index));
			}
			break;
		case ELmOp::Min:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = FMath::Min(LmComp(A, Op.WidthA, Index), LmComp(B, Op.WidthB, Index));
			}
			break;
		case ELmOp::Max:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = FMath::Max(LmComp(A, Op.WidthA, Index), LmComp(B, Op.WidthB, Index));
			}
			break;
		case ELmOp::Clamp:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				double Value = LmComp(A, Op.WidthA, Index);
				if (Op.Pick[0] != 2)
				{
					Value = FMath::Max(Value, LmComp(B, Op.WidthB, Index));
				}
				if (Op.Pick[0] != 1)
				{
					Value = FMath::Min(Value, LmComp(C, Op.WidthC, Index));
				}
				Out.V[Index] = Value;
			}
			break;
		case ELmOp::Saturate:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = FMath::Clamp(LmComp(A, Op.WidthA, Index), 0.0, 1.0);
			}
			break;
		case ELmOp::OneMinus:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = 1.0 - LmComp(A, Op.WidthA, Index);
			}
			break;
		case ELmOp::Abs:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = FMath::Abs(LmComp(A, Op.WidthA, Index));
			}
			break;
		case ELmOp::Lerp:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				const double From = LmComp(A, Op.WidthA, Index);
				Out.V[Index] = From + (LmComp(B, Op.WidthB, Index) - From) * LmComp(C, Op.WidthC, Index);
			}
			break;
		case ELmOp::Dot:
		{
			double Sum = 0.0;
			for (int32 Index = 0; Index < Op.WidthA; ++Index)
			{
				Sum += A.V[Index] * B.V[Index];
			}
			Out.V[0] = Sum;
			break;
		}
		case ELmOp::Mask:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = A.V[Op.Pick[Index]];
			}
			break;
		case ELmOp::Append:
			for (int32 Index = 0; Index < Width; ++Index)
			{
				Out.V[Index] = Index < Op.WidthA ? A.V[Index] : B.V[Index - Op.WidthA];
			}
			break;
		case ELmOp::TangentToWorld:
		{
			const FVector World = Sample->Tangent * A.V[0] + Sample->Binormal * A.V[1] + Sample->Normal * A.V[2];
			Out.V[0] = World.X;
			Out.V[1] = World.Y;
			Out.V[2] = World.Z;
			break;
		}
		case ELmOp::LandscapeUv:
			// Value: the scale of U and V, then their offsets.
			Out.V[0] = Sample->LandscapeSample.X * Op.Value.V[0] + Op.Value.V[2];
			Out.V[1] = Sample->LandscapeSample.Y * Op.Value.V[1] + Op.Value.V[3];
			break;
		case ELmOp::Texture:
			if (Op.Texture.IsValid())
			{
				Op.Texture->Sample(A.V[0], A.V[1], Out.V);
			}
			else
			{
				Out = FLmValue();
			}
			break;
		}
	}
}

struct FMantleSurfaceProgram::FImpl
{
	TArray<MantleLandscapeMaterialReaderPrivate::FLmOp> Ops;

	/** The register of each output. */
	TArray<int32> Outputs;
};

FMantleSurfaceProgram::FMantleSurfaceProgram(TSharedPtr<FImpl> InImpl, const int32 InOutputCount)
	: Impl(MoveTemp(InImpl))
	, OutputCount(InOutputCount)
{
}

void FMantleSurfaceProgram::Evaluate(const FMantleSurfaceSample& Sample, double* OutValues) const
{
	using namespace MantleLandscapeMaterialReaderPrivate;

	if (!Impl.IsValid())
	{
		return;
	}
	TArray<FLmValue, TInlineAllocator<160>> Registers;
	Registers.SetNumUninitialized(Impl->Ops.Num());
	const FLmValue Zero = {};
	for (int32 Index = 0; Index < Impl->Ops.Num(); ++Index)
	{
		const FLmOp& Op = Impl->Ops[Index];
		LmExec(Op, Op.A >= 0 ? Registers[Op.A] : Zero, Op.B >= 0 ? Registers[Op.B] : Zero, Op.C >= 0 ? Registers[Op.C] : Zero, &Sample, Registers[Index]);
	}
	for (int32 Output = 0; Output < Impl->Outputs.Num(); ++Output)
	{
		OutValues[Output] = Registers[Impl->Outputs[Output]].V[0];
	}
}

FString FMantleLandscapeAutoChain::BakedNote()
{
	return TEXT("Auto layer baked from the material's slope and height rules at the heightfield's resolution: its weight at each sample is worked out from that sample's slope and height.");
}

namespace MantleLandscapeMaterialReaderPrivate
{
	/** A mask of the graph and the value it is given instead of being worked out. */
	struct FLmMaskValue
	{
		FLmRef Mask;
		double Value = 0.0;
	};

	/** Compiles the graph under an input into FLmOps: constants and parameters folded, static switches and function calls resolved, function inputs left unconnected at their defaults. */
	class FLmProgramBuilder
	{
	public:
		explicit FLmProgramBuilder(FLmGraphReader& InReader)
			: Reader(InReader)
		{
		}

		/** Why compiling failed, in words that follow "it": empty while it has not. */
		FString Why;

		/** What the program takes as an approximation (a noise texture's average, a Fresnel term as 0). */
		TArray<FString> Notes;

		/** Texture samples are taken as their averages, whatever their UVs: for a value that is not worked out per sample (a layer's roughness). */
		bool bAverageTextures = false;

		/** Masks whose value is given, not worked out: a node of the graph (or one out of the same function call) that is this constant wherever it is read. */
		TArray<FLmMaskValue> MaskValues;

		/** The register of what Input is, or INDEX_NONE (and Why) when it cannot be evaluated. An unconnected Input is Fallback, or fails without one. */
		int32 CompileInput(const FExpressionInput& Input, const FLmFrame* Frame, const bool bHasFallback, const float Fallback)
		{
			if (!Input.Expression)
			{
				return bHasFallback ? Constant(1, Fallback) : Fail(TEXT("has an input of one of its blends that is not connected"));
			}
			FLmTrace Trace;
			const FLmRef Ref = Reader.Resolve(Input, Frame, &Trace);
			if (!Ref.IsSet())
			{
				return bHasFallback ? Constant(1, Fallback) : Fail(TEXT("has an input of one of its blends that leads nowhere"));
			}
			for (const FLmMaskValue& Given : MaskValues)
			{
				// The same mask node, or one that comes out of the same call of a mask function (it may give the roughness an output of its own).
				const bool bSameNode = Ref.Expression == Given.Mask.Expression && Ref.Output == Given.Mask.Output && Ref.Frame == Given.Mask.Frame;
				const bool bSameCall = Ref.Frame != nullptr && Ref.Frame == Given.Mask.Frame && Trace.EnteredFrame == Ref.Frame;
				if (bSameNode || bSameCall)
				{
					return Constant(1, Given.Value);
				}
			}
			int32 Reg = CompileNode(Ref);
			if (Reg == INDEX_NONE)
			{
				return Reg;
			}
			// A World Position's XY and Z outputs are the full position under a mask; a graph built in code may leave the mask off.
			if (Trace.Masks.Num() == 0 && Ref.Output > 0 && ClassNamed(Ref.Expression, TEXT("MaterialExpressionWorldPosition")))
			{
				Trace.Masks.Add(static_cast<uint8>(Ref.Output == 1 ? 3 : 4));
			}
			for (int32 Index = Trace.Masks.Num() - 1; Index >= 0; --Index)
			{
				Reg = ApplyMask(Reg, Trace.Masks[Index]);
			}
			return Reg;
		}

		int32 WidthOf(const int32 Reg) const
		{
			return Ops.IsValidIndex(Reg) ? Ops[Reg].Width : 0;
		}

		/** Node as a register: the root of a program that is not one of a Lerp's inputs. */
		int32 CompileRef(const FLmRef& Node)
		{
			return CompileNode(Node);
		}

		/** Reg's value when it is a constant (nothing in it reads the sample), else false. */
		bool ConstantValue(const int32 Reg, double& OutValue) const
		{
			if (!Ops.IsValidIndex(Reg) || Ops[Reg].Kind != ELmOp::Const)
			{
				return false;
			}
			OutValue = Ops[Reg].Value.V[0];
			return true;
		}

		/** 1 - Reg. */
		int32 OneMinus(const int32 Reg)
		{
			FLmOp Op;
			Op.Kind = ELmOp::OneMinus;
			Op.Width = Op.WidthA = Ops[Reg].Width;
			Op.A = Reg;
			return Emit(Op);
		}

		void AddOutput(const int32 Reg)
		{
			Outputs.Add(Reg);
		}

		/** The program of the outputs added, without the operations nothing reads. */
		TSharedRef<FMantleSurfaceProgram> Finish() const
		{
			TArray<bool> Live;
			Live.Init(false, Ops.Num());
			for (const int32 Output : Outputs)
			{
				Live[Output] = true;
			}
			for (int32 Index = Ops.Num() - 1; Index >= 0; --Index)
			{
				if (Live[Index])
				{
					for (const int32 Operand : { Ops[Index].A, Ops[Index].B, Ops[Index].C })
					{
						if (Operand >= 0)
						{
							Live[Operand] = true;
						}
					}
				}
			}
			TArray<int32> Remap;
			Remap.Init(INDEX_NONE, Ops.Num());
			TSharedRef<FMantleSurfaceProgram::FImpl> Impl = MakeShared<FMantleSurfaceProgram::FImpl>();
			for (int32 Index = 0; Index < Ops.Num(); ++Index)
			{
				if (!Live[Index])
				{
					continue;
				}
				FLmOp Op = Ops[Index];
				Op.A = Op.A >= 0 ? Remap[Op.A] : INDEX_NONE;
				Op.B = Op.B >= 0 ? Remap[Op.B] : INDEX_NONE;
				Op.C = Op.C >= 0 ? Remap[Op.C] : INDEX_NONE;
				Remap[Index] = Impl->Ops.Add(Op);
			}
			for (const int32 Output : Outputs)
			{
				Impl->Outputs.Add(Remap[Output]);
			}
			return MakeShared<FMantleSurfaceProgram>(Impl, Outputs.Num());
		}

	private:
		FLmGraphReader& Reader;
		TArray<FLmOp> Ops;
		TArray<int32> Outputs;
		TMap<FLmVisitKey, int32> Done;
		int32 Depth = 0;

		/** Above 0 while the coordinates of a texture sample are compiled: the landscape's own UVs are only a value there. */
		int32 UvDepth = 0;

		/** The pixels of each mask texture read so far (null for one that could not be), so a texture sampled twice is read once. */
		TMap<const UTexture*, TSharedPtr<const FLmTexture>> Textures;

		int32 Fail(const FString& Reason)
		{
			if (Why.IsEmpty())
			{
				Why = Reason;
			}
			return INDEX_NONE;
		}

		/** Adds Op; one whose operands are all constants is worked out now and added as a constant. */
		int32 Emit(const FLmOp& Op)
		{
			const bool bReadsSample = Op.Kind == ELmOp::WorldPosition || Op.Kind == ELmOp::Normal || Op.Kind == ELmOp::TangentToWorld || Op.Kind == ELmOp::LandscapeUv;
			auto IsConstant = [this](const int32 Reg) { return Reg < 0 || Ops[Reg].Kind == ELmOp::Const; };
			if (Op.Kind != ELmOp::Const && !bReadsSample && IsConstant(Op.A) && IsConstant(Op.B) && IsConstant(Op.C))
			{
				const FLmValue Zero = {};
				FLmOp Folded;
				Folded.Width = Op.Width;
				LmExec(Op, Op.A >= 0 ? Ops[Op.A].Value : Zero, Op.B >= 0 ? Ops[Op.B].Value : Zero, Op.C >= 0 ? Ops[Op.C].Value : Zero, nullptr, Folded.Value);
				return Ops.Add(Folded);
			}
			return Ops.Add(Op);
		}

		int32 Constant(const int32 Width, const double X, const double Y = 0.0, const double Z = 0.0, const double W = 0.0)
		{
			FLmOp Op;
			Op.Width = static_cast<uint8>(Width);
			Op.Value.V[0] = X;
			Op.Value.V[1] = Y;
			Op.Value.V[2] = Z;
			Op.Value.V[3] = W;
			return Ops.Add(Op);
		}

		/** The components of Reg that Bits (R G B A as 1 2 4 8) picks. A mask that asks for what the value does not have is not a mask of it, and leaves it as it is. */
		int32 ApplyMask(const int32 Reg, const uint8 Bits)
		{
			const int32 Width = Ops[Reg].Width;
			uint8 Pick[4] = { 0, 0, 0, 0 };
			int32 Count = 0;
			for (int32 Component = 0; Component < 4; ++Component)
			{
				if (Bits & (1 << Component))
				{
					if (Component >= Width)
					{
						return Reg;
					}
					Pick[Count++] = static_cast<uint8>(Component);
				}
			}
			bool bIdentity = Count == Width;
			for (int32 Index = 0; Index < Count && bIdentity; ++Index)
			{
				bIdentity = Pick[Index] == Index;
			}
			if (Count == 0 || bIdentity)
			{
				return Reg;
			}
			return EmitMask(Reg, Pick, Count);
		}

		int32 EmitMask(const int32 Reg, const uint8 (&Pick)[4], const int32 Count)
		{
			FLmOp Op;
			Op.Kind = ELmOp::Mask;
			Op.Width = static_cast<uint8>(Count);
			Op.WidthA = Ops[Reg].Width;
			Op.A = Reg;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Op.Pick[Index] = Pick[Index];
			}
			return Emit(Op);
		}

		int32 Binary(const ELmOp Kind, const FExpressionInput& InputA, const float ConstA, const FExpressionInput& InputB, const float ConstB, const FLmFrame* Frame)
		{
			const int32 A = CompileInput(InputA, Frame, true, ConstA);
			const int32 B = A == INDEX_NONE ? INDEX_NONE : CompileInput(InputB, Frame, true, ConstB);
			if (A == INDEX_NONE || B == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			FLmOp Op;
			Op.Kind = Kind;
			Op.A = A;
			Op.B = B;
			Op.WidthA = Ops[A].Width;
			Op.WidthB = Ops[B].Width;
			if (Op.WidthA != Op.WidthB && Op.WidthA != 1 && Op.WidthB != 1)
			{
				return Fail(TEXT("combines vectors of different sizes"));
			}
			Op.Width = FMath::Max(Op.WidthA, Op.WidthB);
			return Emit(Op);
		}

		/** An operation on one required input, with as many components as the input. */
		int32 Unary(const ELmOp Kind, const FExpressionInput& Input, const FLmFrame* Frame)
		{
			const int32 A = CompileInput(Input, Frame, false, 0.0f);
			if (A == INDEX_NONE)
			{
				return INDEX_NONE;
			}
			FLmOp Op;
			Op.Kind = Kind;
			Op.A = A;
			Op.Width = Op.WidthA = Ops[A].Width;
			return Emit(Op);
		}

		int32 LandscapeUv(const double ScaleU, const double ScaleV, const double OffsetU, const double OffsetV)
		{
			FLmOp Op;
			Op.Kind = ELmOp::LandscapeUv;
			Op.Width = 2;
			Op.Value.V[0] = ScaleU;
			Op.Value.V[1] = ScaleV;
			Op.Value.V[2] = OffsetU;
			Op.Value.V[3] = OffsetV;
			return Emit(Op);
		}

		/** The source pixels of Texture at mip 0, 8 bits a channel; null when it has none to read or they are too many. */
		TSharedPtr<const FLmTexture> TexturePixels(UTexture* Texture)
		{
			if (const TSharedPtr<const FLmTexture>* Known = Textures.Find(Texture))
			{
				return *Known;
			}
			TSharedPtr<const FLmTexture> Result;
			FImage Image;
			if (Texture->Source.IsValid() && Texture->Source.GetNumBlocks() <= 1 && Texture->Source.GetNumSlices() <= 1 && Texture->Source.GetMipImage(Image, 0)
				&& Image.SizeX > 0 && Image.SizeY > 0 && static_cast<int64>(Image.SizeX) * Image.SizeY <= (64 << 20))
			{
				const bool bHdr = ERawImageFormat::IsHDR(Image.Format);
				FImage Bgra;
				Image.CopyTo(Bgra, ERawImageFormat::BGRA8, bHdr ? EGammaSpace::sRGB : Image.GammaSpace);
				const TArrayView64<FColor> Pixels = Bgra.AsBGRA8();
				if (Pixels.Num() == static_cast<int64>(Bgra.SizeX) * Bgra.SizeY)
				{
					TSharedRef<FLmTexture> Data = MakeShared<FLmTexture>();
					Data->SizeX = Bgra.SizeX;
					Data->SizeY = Bgra.SizeY;
					// A float source was put in sRGB bytes above, which decode back to what it held.
					Data->bSrgb = bHdr || Texture->SRGB;
					Data->bClampX = Texture->GetTextureAddressX() == TA_Clamp;
					Data->bClampY = Texture->GetTextureAddressY() == TA_Clamp;
					Data->Pixels.Append(Pixels.GetData(), static_cast<int32>(Pixels.Num()));
					Result = Data;
				}
			}
			Textures.Add(Texture, Result);
			return Result;
		}

		/** A mask's texture sample at the UVs the graph gives it (world position, landscape coordinates, and what is built from them). INDEX_NONE, with Why as it was, when the UVs or the pixels cannot be read. */
		int32 CompileTextureSample(const FLmRef& Node, const UMaterialExpressionTextureSample* Sample, UTexture* Texture)
		{
			const TSharedPtr<const FLmTexture> Pixels = bAverageTextures ? nullptr : TexturePixels(Texture);
			if (!Pixels.IsValid())
			{
				return INDEX_NONE;
			}
			const FString SavedWhy = Why;
			int32 Uv = INDEX_NONE;
			if (Sample->Coordinates.Expression)
			{
				++UvDepth;
				Uv = CompileInput(Sample->Coordinates, Node.Frame, false, 0.0f);
				--UvDepth;
			}
			else if (Sample->ConstCoordinate == 0)
			{
				Uv = LandscapeUv(1.0, 1.0, 0.0, 0.0);
			}
			Why = SavedWhy;
			if (Uv == INDEX_NONE || Ops[Uv].Width < 2)
			{
				return INDEX_NONE;
			}
			if (Ops[Uv].Width > 2)
			{
				const uint8 Pick[4] = { 0, 1, 0, 0 };
				Uv = EmitMask(Uv, Pick, 2);
			}
			FLmOp Op;
			Op.Kind = ELmOp::Texture;
			Op.Width = 4;
			Op.WidthA = 2;
			Op.A = Uv;
			Op.Texture = Pixels;
			return Emit(Op);
		}

		int32 CompileNode(const FLmRef& Node)
		{
			const FLmVisitKey Key(Node.Expression, Node.Frame);
			if (const int32* Known = Done.Find(Key))
			{
				return *Known;
			}
			if (Depth > 80)
			{
				return Fail(TEXT("has masks nested too deeply to follow"));
			}
			++Depth;
			const int32 Reg = CompileExpression(Node);
			--Depth;
			if (Reg != INDEX_NONE)
			{
				Done.Add(Key, Reg);
			}
			return Reg;
		}

		int32 CompileExpression(const FLmRef& Node)
		{
			const UMaterialExpression* Expression = Node.Expression;
			const FLmFrame* Frame = Node.Frame;

			if (const UMaterialExpressionConstant* Constant1 = Cast<UMaterialExpressionConstant>(Expression))
			{
				return Constant(1, Constant1->R);
			}
			if (const UMaterialExpressionConstant2Vector* Constant2 = Cast<UMaterialExpressionConstant2Vector>(Expression))
			{
				return Constant(2, Constant2->R, Constant2->G);
			}
			if (const UMaterialExpressionConstant3Vector* Constant3 = Cast<UMaterialExpressionConstant3Vector>(Expression))
			{
				return Constant(3, Constant3->Constant.R, Constant3->Constant.G, Constant3->Constant.B);
			}
			if (const UMaterialExpressionConstant4Vector* Constant4 = Cast<UMaterialExpressionConstant4Vector>(Expression))
			{
				return Constant(4, Constant4->Constant.R, Constant4->Constant.G, Constant4->Constant.B, Constant4->Constant.A);
			}
			if (const UMaterialExpressionScalarParameter* ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
			{
				return Constant(1, Reader.ScalarParameterValue(ScalarParameter));
			}
			if (const UMaterialExpressionVectorParameter* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
			{
				const FLinearColor Value = Reader.VectorParameterValue(VectorParameter);
				return Constant(4, Value.R, Value.G, Value.B, Value.A);
			}
			if (const UMaterialExpressionFunctionInput* FunctionInput = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				// Only reached when nothing is connected to the input: its own default, as the function says it.
				if (!FunctionInput->bUsePreviewValueAsDefault)
				{
					return Fail(FString::Printf(TEXT("has a function input ('%s') with nothing connected and no default"), *FunctionInput->InputName.ToString()));
				}
				const FVector4f Preview = FunctionInput->PreviewValue;
				switch (FunctionInput->InputType)
				{
				case FunctionInput_Scalar:
					return Constant(1, Preview.X);
				case FunctionInput_Vector2:
					return Constant(2, Preview.X, Preview.Y);
				case FunctionInput_Vector3:
					return Constant(3, Preview.X, Preview.Y, Preview.Z);
				case FunctionInput_Vector4:
					return Constant(4, Preview.X, Preview.Y, Preview.Z, Preview.W);
				default:
					return Fail(FString::Printf(TEXT("has a function input ('%s') that is not a number or a vector"), *FunctionInput->InputName.ToString()));
				}
			}
			if (const UMaterialExpressionWorldPosition* WorldPosition = Cast<UMaterialExpressionWorldPosition>(Expression))
			{
				if (WorldPosition->WorldPositionShaderOffset != WPT_Default && WorldPosition->WorldPositionShaderOffset != WPT_ExcludeAllShaderOffsets)
				{
					return Fail(TEXT("reads a camera-relative world position"));
				}
				FLmOp Op;
				Op.Kind = ELmOp::WorldPosition;
				Op.Width = 3;
				return Emit(Op);
			}
			if (ClassNamed(Expression, TEXT("MaterialExpressionVertexNormalWS")) || ClassNamed(Expression, TEXT("MaterialExpressionPixelNormalWS")))
			{
				// The landscape's normal at the vertex stands for the pixel's: the heightfield has no finer one.
				FLmOp Op;
				Op.Kind = ELmOp::Normal;
				Op.Width = 3;
				return Emit(Op);
			}
			if (ClassNamed(Expression, TEXT("MaterialExpressionFresnel")))
			{
				Notes.AddUnique(TEXT("a Fresnel (rim) term in a mask is taken as 0."));
				return Constant(1, 0.0);
			}
			if (Cast<UMaterialExpressionTextureSample>(Expression))
			{
				const UMaterialExpressionTextureSample* Sample = nullptr;
				UTexture* Texture = Reader.SampledTexture(Node, Sample);
				FLinearColor Average;
				if (!Texture || Texture->IsNormalMap())
				{
					return Fail(TEXT("samples a texture it cannot read in one of its masks"));
				}
				// At the UVs the graph gives, when it does and the pixels can be read: the noise that breaks up a mask's edge is kept.
				const int32 Sampled = CompileTextureSample(Node, Sample, Texture);
				if (Sampled != INDEX_NONE)
				{
					return Sampled;
				}
				if (!FMantleLandscapeMaterialReader::AverageColour(Texture, Average))
				{
					return Fail(FString::Printf(TEXT("samples the texture '%s' in one of its masks, which has no source pixels to take an average of"), *Texture->GetName()));
				}
				Notes.AddUnique(FString::Printf(TEXT("edge noise from the texture '%s' in a mask is taken as the texture's average: the noise is not kept."), *Texture->GetName()));
				return Constant(4, Average.R, Average.G, Average.B, Average.A);
			}
			if (UvDepth > 0)
			{
				if (const UMaterialExpressionLandscapeLayerCoords* Coords = Cast<UMaterialExpressionLandscapeLayerCoords>(Expression))
				{
					if (Coords->CustomUVType != LCCT_None || (Coords->MappingType != TCMT_Auto && Coords->MappingType != TCMT_XY) || Coords->MappingRotation != 0.0f)
					{
						return Fail(TEXT("uses landscape coordinates other than the top-down ones"));
					}
					const double Scale = 1.0 / (Coords->MappingScale == 0.0f ? 1.0 : static_cast<double>(FMath::Abs(Coords->MappingScale)));
					return LandscapeUv(Scale, Scale, Coords->MappingPanU, Coords->MappingPanV);
				}
				if (const UMaterialExpressionTextureCoordinate* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Expression))
				{
					if (TexCoord->CoordinateIndex != 0)
					{
						return Fail(TEXT("uses a texture coordinate set other than the first"));
					}
					return LandscapeUv(TexCoord->UTiling, TexCoord->VTiling, 0.0, 0.0);
				}
			}
			if (const UMaterialExpressionTransform* Transform = Cast<UMaterialExpressionTransform>(Expression))
			{
				const bool bTangentToWorld = Transform->TransformSourceType == TRANSFORMSOURCE_Tangent && Transform->TransformType == TRANSFORM_World;
				const bool bWorldToWorld = Transform->TransformSourceType == TRANSFORMSOURCE_World && Transform->TransformType == TRANSFORM_World;
				if (!bTangentToWorld && !bWorldToWorld)
				{
					return Fail(TEXT("transforms a vector between spaces other than tangent to world"));
				}
				const int32 A = CompileInput(Transform->Input, Frame, false, 0.0f);
				if (A == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				if (Ops[A].Width < 3)
				{
					return Fail(TEXT("transforms a value that is not a 3 component vector"));
				}
				if (bWorldToWorld)
				{
					return A;
				}
				FLmOp Op;
				Op.Kind = ELmOp::TangentToWorld;
				Op.Width = 3;
				Op.WidthA = Ops[A].Width;
				Op.A = A;
				return Emit(Op);
			}
			if (const UMaterialExpressionAdd* Add = Cast<UMaterialExpressionAdd>(Expression))
			{
				return Binary(ELmOp::Add, Add->A, Add->ConstA, Add->B, Add->ConstB, Frame);
			}
			if (const UMaterialExpressionSubtract* Subtract = Cast<UMaterialExpressionSubtract>(Expression))
			{
				return Binary(ELmOp::Subtract, Subtract->A, Subtract->ConstA, Subtract->B, Subtract->ConstB, Frame);
			}
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				return Binary(ELmOp::Multiply, Multiply->A, Multiply->ConstA, Multiply->B, Multiply->ConstB, Frame);
			}
			if (const UMaterialExpressionDivide* Divide = Cast<UMaterialExpressionDivide>(Expression))
			{
				return Binary(ELmOp::Divide, Divide->A, Divide->ConstA, Divide->B, Divide->ConstB, Frame);
			}
			if (const UMaterialExpressionMin* MinNode = Cast<UMaterialExpressionMin>(Expression))
			{
				return Binary(ELmOp::Min, MinNode->A, MinNode->ConstA, MinNode->B, MinNode->ConstB, Frame);
			}
			if (const UMaterialExpressionMax* MaxNode = Cast<UMaterialExpressionMax>(Expression))
			{
				return Binary(ELmOp::Max, MaxNode->A, MaxNode->ConstA, MaxNode->B, MaxNode->ConstB, Frame);
			}
			if (const UMaterialExpressionPower* Power = Cast<UMaterialExpressionPower>(Expression))
			{
				const int32 Base = CompileInput(Power->Base, Frame, false, 0.0f);
				const int32 Exponent = Base == INDEX_NONE ? INDEX_NONE : CompileInput(Power->Exponent, Frame, true, Power->ConstExponent);
				if (Base == INDEX_NONE || Exponent == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				if (Ops[Exponent].Width != 1 && Ops[Exponent].Width != Ops[Base].Width)
				{
					return Fail(TEXT("raises a vector to a power of another size"));
				}
				FLmOp Op;
				Op.Kind = ELmOp::Power;
				Op.A = Base;
				Op.B = Exponent;
				Op.Width = Op.WidthA = Ops[Base].Width;
				Op.WidthB = Ops[Exponent].Width;
				return Emit(Op);
			}
			if (const UMaterialExpressionClamp* Clamp = Cast<UMaterialExpressionClamp>(Expression))
			{
				const int32 Value = CompileInput(Clamp->Input, Frame, false, 0.0f);
				const int32 Low = Value == INDEX_NONE ? INDEX_NONE : CompileInput(Clamp->Min, Frame, true, Clamp->MinDefault);
				const int32 High = Low == INDEX_NONE ? INDEX_NONE : CompileInput(Clamp->Max, Frame, true, Clamp->MaxDefault);
				if (Value == INDEX_NONE || Low == INDEX_NONE || High == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				FLmOp Op;
				Op.Kind = ELmOp::Clamp;
				Op.Pick[0] = Clamp->ClampMode == CMODE_ClampMin ? 1 : Clamp->ClampMode == CMODE_ClampMax ? 2 : 0;
				Op.A = Value;
				Op.B = Low;
				Op.C = High;
				Op.Width = Op.WidthA = Ops[Value].Width;
				Op.WidthB = Ops[Low].Width;
				Op.WidthC = Ops[High].Width;
				if ((Op.WidthB != 1 && Op.WidthB != Op.Width) || (Op.WidthC != 1 && Op.WidthC != Op.Width))
				{
					return Fail(TEXT("clamps a vector between limits of another size"));
				}
				return Emit(Op);
			}
			if (const UMaterialExpressionSaturate* Saturate = Cast<UMaterialExpressionSaturate>(Expression))
			{
				return Unary(ELmOp::Saturate, Saturate->Input, Frame);
			}
			if (const UMaterialExpressionOneMinus* OneMinusNode = Cast<UMaterialExpressionOneMinus>(Expression))
			{
				return Unary(ELmOp::OneMinus, OneMinusNode->Input, Frame);
			}
			if (const UMaterialExpressionAbs* Abs = Cast<UMaterialExpressionAbs>(Expression))
			{
				return Unary(ELmOp::Abs, Abs->Input, Frame);
			}
			if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Expression))
			{
				const int32 From = CompileInput(Lerp->A, Frame, true, Lerp->ConstA);
				const int32 To = From == INDEX_NONE ? INDEX_NONE : CompileInput(Lerp->B, Frame, true, Lerp->ConstB);
				const int32 Alpha = To == INDEX_NONE ? INDEX_NONE : CompileInput(Lerp->Alpha, Frame, true, Lerp->ConstAlpha);
				if (From == INDEX_NONE || To == INDEX_NONE || Alpha == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				FLmOp Op;
				Op.Kind = ELmOp::Lerp;
				Op.A = From;
				Op.B = To;
				Op.C = Alpha;
				Op.WidthA = Ops[From].Width;
				Op.WidthB = Ops[To].Width;
				Op.WidthC = Ops[Alpha].Width;
				Op.Width = FMath::Max(Op.WidthA, Op.WidthB);
				if ((Op.WidthA != Op.Width && Op.WidthA != 1) || (Op.WidthB != Op.Width && Op.WidthB != 1) || (Op.WidthC != 1 && Op.WidthC != Op.Width))
				{
					return Fail(TEXT("blends vectors of different sizes"));
				}
				return Emit(Op);
			}
			if (const UMaterialExpressionDotProduct* Dot = Cast<UMaterialExpressionDotProduct>(Expression))
			{
				const int32 A = CompileInput(Dot->A, Frame, false, 0.0f);
				const int32 B = A == INDEX_NONE ? INDEX_NONE : CompileInput(Dot->B, Frame, false, 0.0f);
				if (A == INDEX_NONE || B == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				if (Ops[A].Width != Ops[B].Width)
				{
					return Fail(TEXT("takes the dot product of vectors of different sizes"));
				}
				FLmOp Op;
				Op.Kind = ELmOp::Dot;
				Op.A = A;
				Op.B = B;
				Op.Width = 1;
				Op.WidthA = Op.WidthB = Ops[A].Width;
				return Emit(Op);
			}
			if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
			{
				const int32 A = CompileInput(Mask->Input, Frame, false, 0.0f);
				if (A == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				const bool bPicked[4] = { Mask->R != 0, Mask->G != 0, Mask->B != 0, Mask->A != 0 };
				uint8 Pick[4] = { 0, 0, 0, 0 };
				int32 Count = 0;
				for (int32 Component = 0; Component < 4; ++Component)
				{
					if (bPicked[Component])
					{
						if (Component >= Ops[A].Width)
						{
							return Fail(TEXT("masks a component its value does not have"));
						}
						Pick[Count++] = static_cast<uint8>(Component);
					}
				}
				if (Count == 0)
				{
					return Fail(TEXT("has a component mask that picks nothing"));
				}
				return EmitMask(A, Pick, Count);
			}
			if (const UMaterialExpressionAppendVector* Append = Cast<UMaterialExpressionAppendVector>(Expression))
			{
				const int32 A = CompileInput(Append->A, Frame, false, 0.0f);
				const int32 B = A == INDEX_NONE ? INDEX_NONE : CompileInput(Append->B, Frame, false, 0.0f);
				if (A == INDEX_NONE || B == INDEX_NONE)
				{
					return INDEX_NONE;
				}
				if (Ops[A].Width + Ops[B].Width > 4)
				{
					return Fail(TEXT("appends vectors to more than 4 components"));
				}
				FLmOp Op;
				Op.Kind = ELmOp::Append;
				Op.A = A;
				Op.B = B;
				Op.WidthA = Ops[A].Width;
				Op.WidthB = Ops[B].Width;
				Op.Width = Op.WidthA + Op.WidthB;
				return Emit(Op);
			}
			return Fail(FString::Printf(TEXT("uses %s in one of its masks, which Mantle cannot evaluate"), *Expression->GetClass()->GetName()));
		}
	};

	/** One blend of the chain: Look shows over what is under it by the mask at AlphaInput (or by 1 - mask when bInvert). */
	struct FLmAutoStep
	{
		FLmRef Look;
		FName LookInput;
		FLmRef Alpha;
		const FExpressionInput* AlphaInput = nullptr;
		const FLmFrame* AlphaFrame = nullptr;
		bool bInvert = false;
	};

	/** The slope and height blends of a material with no paint layers, bottom (the base) first. */
	struct FLmAutoChain
	{
		FLmRef Base;
		FName BaseInput;
		TArray<FLmAutoStep> Steps;

		/** What else the material does to the blend's result (a tint, a variation texture), in words. */
		TArray<FString> Notes;

		/**
		 * The other inputs of the nodes that pass the chain on (a tint, a variation, a fade): not part of the chain, but what
		 * Notes says about, so the texture scan leaves them to it.
		 */
		TArray<FLmRef> Covered;
	};

	/** Finds the chain of blends by the normal and the world position under a material's base colour: Lerps whose alpha an FLmProgramBuilder can compile. */
	class FLmAutoChainFinder
	{
	public:
		explicit FLmAutoChainFinder(FLmGraphReader& InReader, const ELmProp InProp = ELmProp::BaseColor)
			: Reader(InReader)
			, Prop(InProp)
		{
		}

		/** Why a blend that reads the normal or the height was not taken, when one was not: words that follow "it". */
		FString Why;

		bool Find(const FLmRef& Root, FLmAutoChain& Out)
		{
			FLmVisit ContainsVisit;
			if (!Root.IsSet() || Reader.ContainsLayers(Root, Prop, ContainsVisit))
			{
				return false;
			}
			const TSharedPtr<const FLmAutoChain> Chain = Walk(Root, 0);
			if (!Chain.IsValid() || Chain->Steps.Num() == 0)
			{
				return false;
			}
			Out = *Chain;
			return true;
		}

	private:
		FLmGraphReader& Reader;
		ELmProp Prop;
		int32 Budget = 200000;
		TMap<FLmVisitKey, TSharedPtr<const FLmAutoChain>> Memo;
		TSet<FLmVisitKey> Active;

		/** The chain under Node, through the nodes that pass it on, or null. */
		TSharedPtr<const FLmAutoChain> Walk(const FLmRef& Node, const int32 Depth)
		{
			if (!Node.IsSet() || Depth > 80 || --Budget < 0)
			{
				return nullptr;
			}
			const FLmVisitKey Key(Node.Expression, Node.Frame);
			if (const TSharedPtr<const FLmAutoChain>* Known = Memo.Find(Key))
			{
				return *Known;
			}
			if (Active.Contains(Key))
			{
				return nullptr;
			}
			TSharedPtr<const FLmAutoChain> Result;
			// A look's texture and the paint layers are not blends of the ground by its shape.
			if (!Cast<UMaterialExpressionTextureSample>(Node.Expression) && !Cast<UMaterialExpressionLandscapeLayerBlend>(Node.Expression)
				&& !Cast<UMaterialExpressionLandscapeLayerWeight>(Node.Expression) && !Cast<UMaterialExpressionLandscapeLayerSample>(Node.Expression))
			{
				Active.Add(Key);
				if (const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Node.Expression))
				{
					Result = WalkBlend(Node, Lerp, Depth);
				}
				if (!Result.IsValid())
				{
					Result = WalkThrough(Node, Depth);
				}
				Active.Remove(Key);
			}
			Memo.Add(Key, Result);
			return Result;
		}

		/** Whether Input, a Lerp's alpha, is a mask of the surface's slope or height that compiles to one number. */
		bool IsShapeMask(const FExpressionInput& Input, const FLmFrame* Frame, const FLmRef& Alpha)
		{
			FLmFlags Flags;
			FLmVisit Visit;
			Reader.ScanFlags(Alpha, Prop, Flags, Visit);
			if ((!Flags.bSlope && !Flags.bHeight) || Flags.bDistance)
			{
				return false;
			}
			FLmProgramBuilder Builder(Reader);
			const int32 Reg = Builder.CompileInput(Input, Frame, false, 0.0f);
			if (Reg == INDEX_NONE || Builder.WidthOf(Reg) != 1)
			{
				if (Why.IsEmpty())
				{
					Why = Reg == INDEX_NONE ? Builder.Why : FString(TEXT("has a mask that is not a single number"));
				}
				return false;
			}
			return true;
		}

		/** A Lerp by a shape mask: its input that holds more of the chain is what is under the look on the other. */
		TSharedPtr<const FLmAutoChain> WalkBlend(const FLmRef& Node, const UMaterialExpressionLinearInterpolate* Lerp, const int32 Depth)
		{
			if (!Lerp->Alpha.Expression || !Lerp->A.Expression || !Lerp->B.Expression)
			{
				return nullptr;
			}
			const FLmFrame* Frame = Node.Frame;
			const FLmRef Alpha = Reader.Resolve(Lerp->Alpha, Frame);
			if (!Alpha.IsSet() || !IsShapeMask(Lerp->Alpha, Frame, Alpha))
			{
				return nullptr;
			}
			FLmTrace TraceA;
			FLmTrace TraceB;
			const FLmRef A = Reader.Resolve(Lerp->A, Frame, &TraceA);
			const FLmRef B = Reader.Resolve(Lerp->B, Frame, &TraceB);
			if (!A.IsSet() || !B.IsSet())
			{
				return nullptr;
			}
			const TSharedPtr<const FLmAutoChain> UnderA = Walk(A, Depth + 1);
			const TSharedPtr<const FLmAutoChain> UnderB = UnderA.IsValid() ? nullptr : Walk(B, Depth + 1);

			TSharedRef<FLmAutoChain> Chain = MakeShared<FLmAutoChain>();
			FLmAutoStep Step;
			Step.Alpha = Alpha;
			Step.AlphaInput = &Lerp->Alpha;
			Step.AlphaFrame = Frame;
			if (UnderA.IsValid())
			{
				*Chain = *UnderA;
				Step.Look = B;
				Step.LookInput = TraceB.FirstInputName;
			}
			else if (UnderB.IsValid())
			{
				// The look is the Lerp's A, so it shows by 1 - alpha.
				*Chain = *UnderB;
				Step.Look = A;
				Step.LookInput = TraceA.FirstInputName;
				Step.bInvert = true;
			}
			else
			{
				// Nothing more under it: A is the base look, B the first layer over it.
				Chain->Base = A;
				Chain->BaseInput = TraceA.FirstInputName;
				Step.Look = B;
				Step.LookInput = TraceB.FirstInputName;
			}
			Chain->Steps.Add(Step);
			return Chain;
		}

		/**
		 * A node with exactly one input that holds the chain, and no other input that reads the normal or the height: a tint or variation over the result.
		 * For the normal map's chain the node may hold two (a near and a far normal of the same blend): the first is taken, and no other input is a
		 * reason to refuse, since the normal is only a look and what the node does to it is not reported.
		 */
		TSharedPtr<const FLmAutoChain> WalkThrough(const FLmRef& Node, const int32 Depth)
		{
			TArray<FLmRef> Inputs;
			Reader.Children(Node, Prop, Inputs);
			int32 Holder = INDEX_NONE;
			TSharedPtr<const FLmAutoChain> Under;
			for (int32 Index = 0; Index < Inputs.Num(); ++Index)
			{
				const TSharedPtr<const FLmAutoChain> Found = Walk(Inputs[Index], Depth + 1);
				if (Found.IsValid())
				{
					if (Holder != INDEX_NONE)
					{
						if (Prop == ELmProp::Normal)
						{
							continue; // the far variant of the same blend: the first is the one kept
						}
						return nullptr; // two chains under one node: not a pass-through
					}
					Holder = Index;
					Under = Found;
				}
			}
			if (Holder == INDEX_NONE)
			{
				return nullptr;
			}
			TArray<FLmRef> Covered;
			FLmCollected Others;
			for (int32 Index = 0; Index < Inputs.Num(); ++Index)
			{
				if (Index == Holder)
				{
					continue;
				}
				if (Prop == ELmProp::Normal)
				{
					Covered.Add(Inputs[Index]);
					continue;
				}
				FLmFlags Flags;
				FLmVisit FlagVisit;
				Reader.ScanFlags(Inputs[Index], Prop, Flags, FlagVisit);
				if (Flags.bSlope || Flags.bHeight)
				{
					if (Why.IsEmpty())
					{
						Why = TEXT("changes the colour by the slope or height after the layers in a way that is not a blend Mantle can read");
					}
					return nullptr;
				}
				Covered.Add(Inputs[Index]);
				FLmVisit CollectVisit;
				Reader.Collect(Inputs[Index], Prop, 1.0, Others, CollectVisit);
			}
			TSharedRef<FLmAutoChain> Chain = MakeShared<FLmAutoChain>(*Under);
			Chain->Covered.Append(Covered);
			if (Inputs.Num() > 1 && Prop == ELmProp::BaseColor)
			{
				TArray<FString> Names;
				for (const FLmHit& Hit : Others.Hits)
				{
					Names.AddUnique(Hit.Name);
				}
				Chain->Notes.AddUnique(Names.Num() > 0
					? FString::Printf(TEXT("it also changes the ground's colour after the layers with %s (a tint or variation), which is not kept."), *JoinNames(Names))
					: FString(TEXT("it also changes the ground's colour after the layers (a tint or blend), which is not kept.")));
			}
			return Chain;
		}
	};

	/** What a name from the graph is as a layer's name: the trailing "BaseColor" or "Color" taken off, and what that leaves of "_" and spaces. */
	FString StripColourWord(FString Name)
	{
		for (const TCHAR* Suffix : { TEXT("BaseColor"), TEXT("Base Color"), TEXT("Colour"), TEXT("Color") })
		{
			if (Name.EndsWith(Suffix, ESearchCase::IgnoreCase))
			{
				Name.LeftChopInline(FCString::Strlen(Suffix));
				break;
			}
		}
		while (Name.EndsWith(TEXT("_")) || Name.EndsWith(TEXT(" ")))
		{
			Name.LeftChopInline(1);
		}
		return Name;
	}

	/** What a parameter or texture's name is as a layer's name: a short leading token ("MW_", "T_", "TEX_") taken off, then StripColourWord. */
	FString LayerNameOfParameter(const FString& Name)
	{
		FString Stripped = Name;
		int32 Underscore = INDEX_NONE;
		if (Stripped.FindChar(TEXT('_'), Underscore) && Underscore >= 1 && Underscore <= 3 && Underscore + 1 < Stripped.Len())
		{
			bool bLetters = true;
			for (int32 Index = 0; Index < Underscore; ++Index)
			{
				bLetters = bLetters && FChar::IsAlpha(Stripped[Index]);
			}
			if (bLetters)
			{
				Stripped.RightChopInline(Underscore + 1);
			}
		}
		const FString Result = StripColourWord(Stripped);
		return Result.IsEmpty() ? StripColourWord(Name) : Result;
	}

	/**
	 * What a look is called, from the nearest named thing: the function input it came out of (InputName) or one just
	 * inside it, else the first parameter, else the first texture. Empty when nothing in it has a name.
	 */
	FString FindLookName(FLmGraphReader& Reader, const FLmRef& Look, const FName InputName)
	{
		if (!InputName.IsNone())
		{
			const FString Name = StripColourWord(InputName.ToString());
			if (!Name.IsEmpty())
			{
				return Name;
			}
		}
		FString Near;
		FString Parameter;
		FString TextureName;
		TArray<TPair<FLmRef, int32>> Queue;
		Queue.Emplace(Look, 0);
		FLmVisit Visit;
		Visit.Budget = 400;
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			const FLmRef Node = Queue[Head].Key;
			const int32 Level = Queue[Head].Value;
			if (!Visit.Enter(Node))
			{
				continue;
			}
			if (const UMaterialExpressionParameter* ParameterNode = Cast<UMaterialExpressionParameter>(Node.Expression))
			{
				if (Parameter.IsEmpty())
				{
					Parameter = LayerNameOfParameter(ParameterNode->ParameterName.ToString());
				}
			}
			else if (const UMaterialExpressionTextureSample* SampleNode = Cast<UMaterialExpressionTextureSample>(Node.Expression))
			{
				const UMaterialExpressionTextureSampleParameter* TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(SampleNode);
				if (!TextureParameter && SampleNode->TextureObject.Expression)
				{
					TextureParameter = Cast<UMaterialExpressionTextureSampleParameter>(Reader.Resolve(SampleNode->TextureObject, Node.Frame).Expression);
				}
				if (TextureParameter && Parameter.IsEmpty())
				{
					Parameter = LayerNameOfParameter(TextureParameter->ParameterName.ToString());
				}
				const UMaterialExpressionTextureSample* Found = nullptr;
				if (const UTexture* Texture = Reader.SampledTexture(Node, Found))
				{
					if (TextureName.IsEmpty())
					{
						TextureName = LayerNameOfParameter(Texture->GetName());
					}
				}
				continue; // its inputs are coordinates
			}
			if (Level >= 8)
			{
				continue;
			}
			TArray<FLmRef> Next;
			TArray<FName> Names;
			Reader.Children(Node, ELmProp::BaseColor, Next, &Names);
			for (int32 Index = 0; Index < Next.Num(); ++Index)
			{
				if (Level < 2 && Near.IsEmpty() && !Names[Index].IsNone())
				{
					Near = StripColourWord(Names[Index].ToString());
				}
				Queue.Emplace(Next[Index], Level + 1);
			}
		}
		return !Near.IsEmpty() ? Near : !Parameter.IsEmpty() ? Parameter : TextureName;
	}

	/** Whether the graph under Node adds a Fresnel term to a colour. Texture samples are not entered. */
	bool ReadsFresnel(FLmGraphReader& Reader, const FLmRef& Node, FLmVisit& Visit)
	{
		if (!Visit.Enter(Node))
		{
			return false;
		}
		if (ClassNamed(Node.Expression, TEXT("MaterialExpressionFresnel")))
		{
			return true;
		}
		if (Cast<UMaterialExpressionTextureSample>(Node.Expression))
		{
			return false;
		}
		TArray<FLmRef> Next;
		Reader.Children(Node, ELmProp::BaseColor, Next);
		for (const FLmRef& Child : Next)
		{
			if (ReadsFresnel(Reader, Child, Visit))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The chain of slope and height blends under Root as OutChain and the program that computes its masks (one output per
	 * blend, bottom first), with what the material does to the blend's result that is not kept in OutNotes. Null, with OutWhy
	 * when a blend that reads the shape of the ground could not be evaluated, when there is no such chain.
	 */
	TSharedPtr<FMantleSurfaceProgram> BuildAutoChain(FLmGraphReader& Reader, const FLmRef& Root, FLmAutoChain& OutChain, TArray<FString>& OutNotes, FString& OutWhy)
	{
		OutChain = FLmAutoChain();
		FLmAutoChainFinder Finder(Reader);
		if (!Finder.Find(Root, OutChain))
		{
			OutChain = FLmAutoChain();
			OutWhy = Finder.Why;
			return nullptr;
		}
		if (OutChain.Steps.Num() + 1 > FMantleLandscapeAutoChain::MaxLayers)
		{
			OutWhy = FString::Printf(TEXT("blends %d layers by slope and height, more than the %d Mantle bakes"), OutChain.Steps.Num() + 1, FMantleLandscapeAutoChain::MaxLayers);
			OutChain = FLmAutoChain();
			return nullptr;
		}
		FLmProgramBuilder Builder(Reader);
		for (const FLmAutoStep& Step : OutChain.Steps)
		{
			const int32 Reg = Builder.CompileInput(*Step.AlphaInput, Step.AlphaFrame, false, 0.0f);
			if (Reg == INDEX_NONE || Builder.WidthOf(Reg) != 1)
			{
				OutWhy = Builder.Why.IsEmpty() ? FString(TEXT("has a mask that is not a single number")) : Builder.Why;
				OutChain = FLmAutoChain();
				return nullptr;
			}
			Builder.AddOutput(Step.bInvert ? Builder.OneMinus(Reg) : Reg);
		}
		OutNotes = OutChain.Notes;
		for (const FString& Note : Builder.Notes)
		{
			OutNotes.AddUnique(Note);
		}
		return Builder.Finish();
	}

	/** The expression that is the Roughness a material attribute set carries (a Make or Set Material Attributes, through the Set's attribute input), or unset. */
	FLmRef FindAttributesRoughness(FLmGraphReader& Reader, const FLmRef& Node, const int32 Depth = 0)
	{
		if (!Node.IsSet() || Depth > 8)
		{
			return FLmRef();
		}
		UMaterialExpression* Expression = const_cast<UMaterialExpression*>(Node.Expression);
		const bool bMake = ClassNamed(Expression, TEXT("MaterialExpressionMakeMaterialAttributes"));
		const bool bSet = ClassNamed(Expression, TEXT("MaterialExpressionSetMaterialAttributes"));
		if (!bMake && !bSet)
		{
			return FLmRef();
		}
		FLmRef Attributes;
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			if (!It.Input || !It.Input->Expression)
			{
				continue;
			}
			const FString Name = Expression->GetInputName(It.Index).ToString().Replace(TEXT(" "), TEXT(""));
			if (Name.Equals(TEXT("Roughness"), ESearchCase::IgnoreCase))
			{
				return Reader.Resolve(*It.Input, Node.Frame);
			}
			if (bSet && NameHasAny(Name, { TEXT("Attr"), TEXT("MaterialAttributes") }))
			{
				Attributes = Reader.Resolve(*It.Input, Node.Frame);
			}
		}
		return bSet ? FindAttributesRoughness(Reader, Attributes, Depth + 1) : FLmRef();
	}

	/**
	 * What the material's Roughness is on the ground of one layer of a slope and height chain: the Roughness graph worked out with
	 * the chain's masks set to show that layer alone (its own mask 1, the masks over it 0). That covers a roughness that blends per
	 * layer by the same masks, and one that multiplies the result by a function of a mask (water's (1 - mask)^4). A texture in it
	 * is taken as its average. False when it is not a number that way (it reads the position, the normal or a mask it cannot match).
	 */
	bool ChainLayerRoughness(FLmGraphReader& Reader, const FLmRef& RoughnessRoot, const FLmAutoChain& Chain, const int32 Layer, double& OutRoughness)
	{
		FLmProgramBuilder Builder(Reader);
		Builder.bAverageTextures = true;
		for (int32 Step = 0; Step < Chain.Steps.Num(); ++Step)
		{
			const FLmAutoStep& Mask = Chain.Steps[Step];
			// Step k blends layer k + 1 over what is under it: its look shows where the mask is 1 (0 when inverted). Only the layer's own
			// blend shows; the others hide, so no other layer's look leaks in.
			const double ShownValue = Mask.bInvert ? 0.0 : 1.0;
			FLmMaskValue& Value = Builder.MaskValues.AddDefaulted_GetRef();
			Value.Mask = Mask.Alpha;
			Value.Value = Step + 1 == Layer ? ShownValue : 1.0 - ShownValue;
		}
		const int32 Reg = Builder.CompileRef(RoughnessRoot);
		return Reg != INDEX_NONE && Builder.ConstantValue(Reg, OutRoughness) && FMath::IsFinite(OutRoughness);
	}
}

FString FMantleLandscapeMaterialReader::LayerNameFromParameter(const FString& Name)
{
	return MantleLandscapeMaterialReaderPrivate::LayerNameOfParameter(Name);
}

bool FMantleLandscapeMaterialReader::Read(const UMaterialInterface* LandscapeMaterial, const double SpacingCm, FMantleLandscapeMaterialRead& Out, FString& OutProblem)
{
	using namespace MantleLandscapeMaterialReaderPrivate;

	Out = FMantleLandscapeMaterialRead();
	OutProblem.Reset();
	if (!LandscapeMaterial)
	{
		OutProblem = TEXT("The landscape has no landscape material, so nothing says what its paint layers look like.");
		return false;
	}
	const UMaterial* Base = LandscapeMaterial->GetMaterial();
	if (!Base || !Base->GetEditorOnlyData())
	{
		OutProblem = FString::Printf(TEXT("Its landscape material '%s' has no editable graph (it is a cooked material), so what its paint layers look like cannot be read."), *LandscapeMaterial->GetName());
		return false;
	}
	Out.MaterialName = LandscapeMaterial->GetName();

	UMaterial* MutableBase = const_cast<UMaterial*>(Base);
	const FExpressionInput* ColourRoot = MutableBase->GetExpressionInputForProperty(Base->bUseMaterialAttributes ? MP_MaterialAttributes : MP_BaseColor);
	const FExpressionInput* NormalRoot = MutableBase->GetExpressionInputForProperty(Base->bUseMaterialAttributes ? MP_MaterialAttributes : MP_Normal);
	const FExpressionInput* RoughnessRoot = MutableBase->GetExpressionInputForProperty(Base->bUseMaterialAttributes ? MP_MaterialAttributes : MP_Roughness);

	FLmGraphReader Reader(LandscapeMaterial, SpacingCm > 0.0 ? SpacingCm : 100.0);

	FLmLayerSet Colour;
	FLmLayerSet Normal;
	FLmFlags Flags;
	FLmFlags NormalFlags; // what only the Normal output reads: its own line when the colour reads none of it
	FLmAutoChain AutoChain;
	TArray<FLmLayerSource> AutoNormals; // the normal look of each layer of the chain, in its order, unset where the Normal chain has none to match
	TSharedPtr<FMantleSurfaceProgram> AutoProgram;
	TArray<FString> AutoNotes;
	FString AutoWhy;
	if (ColourRoot && ColourRoot->Expression)
	{
		const FLmRef Root = Reader.Resolve(*ColourRoot, nullptr);
		FLmVisit Visit;
		Reader.FindLayers(Root, ELmProp::BaseColor, Colour, Visit);

		// With no paint layer, the material may pick its ground from slope and height rules; each look of the chain becomes a layer of its own.
		if (Colour.Sources.Num() == 0 && !Colour.Slope.bFound)
		{
			AutoProgram = BuildAutoChain(Reader, Root, AutoChain, AutoNotes, AutoWhy);
			if (AutoProgram.IsValid())
			{
				TSet<FName> Used;
				auto AddLayer = [&Reader, &Colour, &Used](const FLmRef& Look, const FName InputName)
				{
					FString Wanted = FindLookName(Reader, Look, InputName);
					if (Wanted.IsEmpty())
					{
						Wanted = FString::Printf(TEXT("Auto %d"), Colour.Sources.Num() + 1);
					}
					FString Candidate = Wanted;
					for (int32 Suffix = 2; Used.Contains(FName(*Candidate)); ++Suffix)
					{
						Candidate = FString::Printf(TEXT("%s %d"), *Wanted, Suffix);
					}
					FLmLayerSource& Source = Colour.Sources.AddDefaulted_GetRef();
					Source.Layer = FName(*Candidate);
					Source.Look = Look;
					Used.Add(Source.Layer);
				};
				AddLayer(AutoChain.Base, AutoChain.BaseInput);
				for (const FLmAutoStep& Step : AutoChain.Steps)
				{
					AddLayer(Step.Look, Step.LookInput);
				}

				// The chain's looks and masks are the layers and the baked weights, and a tint over them has its own line: only a texture outside
				// all of those is one "outside the paint layers".
				FLmVisit OutsideVisit;
				OutsideVisit.Enter(AutoChain.Base);
				for (const FLmAutoStep& Step : AutoChain.Steps)
				{
					OutsideVisit.Enter(Step.Look);
					OutsideVisit.Enter(Step.Alpha);
				}
				for (const FLmRef& Covered : AutoChain.Covered)
				{
					OutsideVisit.Enter(Covered);
				}
				FLmLayerSet Outside;
				Reader.FindLayers(Root, ELmProp::BaseColor, Outside, OutsideVisit);
				Colour.GlobalSampleNames = Outside.GlobalSampleNames;
			}
		}

		FLmVisit FlagVisit;
		FlagVisit.Enter(Colour.Slope.Alpha); // the slope overlay is baked, not something the material does that is lost
		for (const FLmAutoStep& Step : AutoChain.Steps)
		{
			FlagVisit.Enter(Step.Alpha); // and so are the chain's masks
		}
		Reader.ScanFlags(Root, ELmProp::BaseColor, Flags, FlagVisit);
	}
	if (NormalRoot && NormalRoot->Expression)
	{
		const FLmRef Root = Reader.Resolve(*NormalRoot, nullptr);
		FLmVisit Visit;
		Reader.FindLayers(Root, ELmProp::Normal, Normal, Visit);
		FLmVisit FlagVisit;
		FlagVisit.Enter(Normal.Slope.Alpha);

		// The Normal output has a parallel chain of blends over the same masks; each of its looks is the normal of the chain layer whose mask it shares.
		FLmAutoChain NormalChain;
		FLmAutoChainFinder NormalFinder(Reader, ELmProp::Normal);
		const bool bNormalChain = AutoProgram.IsValid() && NormalFinder.Find(Root, NormalChain);
		if (bNormalChain)
		{
			auto SameMask = [](const FLmRef& A, const FLmRef& B)
			{
				return A.Expression == B.Expression && A.Output == B.Output && A.Frame == B.Frame;
			};
			// A mask function often gives the normal its own output (a softer mask beside the colour's): the same call of
			// the same function, so the same frame, is the same layer's mask too. Tried only after the exact matches.
			auto SameMaskCall = [](const FLmRef& A, const FLmRef& B)
			{
				return A.Frame != nullptr && A.Frame == B.Frame;
			};
			AutoNormals.SetNum(Colour.Sources.Num());
			TArray<bool> Taken;
			Taken.Init(false, AutoChain.Steps.Num());
			TArray<bool> NormalTaken;
			NormalTaken.Init(false, NormalChain.Steps.Num());
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (int32 NormalIndex = 0; NormalIndex < NormalChain.Steps.Num(); ++NormalIndex)
				{
					const FLmAutoStep& NormalStep = NormalChain.Steps[NormalIndex];
					if (NormalTaken[NormalIndex])
					{
						continue;
					}
					for (int32 StepIndex = 0; StepIndex < AutoChain.Steps.Num(); ++StepIndex)
					{
						const FLmAutoStep& Step = AutoChain.Steps[StepIndex];
						const bool bMatch = Pass == 0 ? SameMask(Step.Alpha, NormalStep.Alpha) : SameMaskCall(Step.Alpha, NormalStep.Alpha);
						if (Taken[StepIndex] || Step.bInvert != NormalStep.bInvert || !bMatch)
						{
							continue;
						}
						Taken[StepIndex] = true;
						NormalTaken[NormalIndex] = true;
						AutoNormals[StepIndex + 1].Look = NormalStep.Look;
						if (NormalIndex == 0 && StepIndex == 0)
						{
							AutoNormals[0].Look = NormalChain.Base; // both are what the lowest blend lies over
						}
						break;
					}
				}
			}

			// The masks of the normal's blends are baked as the colour's are; anything else the Normal output reads from the slope or height is still said.
			for (const FLmAutoStep& Step : NormalChain.Steps)
			{
				FlagVisit.Enter(Step.Alpha);
			}
		}
		Reader.ScanFlags(Root, ELmProp::Normal, NormalFlags, FlagVisit);
	}
	// Only the height is told apart by output; the slope and the camera distance are one line however they are reached.
	Flags.bSlope = Flags.bSlope || NormalFlags.bSlope;
	Flags.bDistance = Flags.bDistance || NormalFlags.bDistance;
	if (Colour.Sources.Num() == 0)
	{
		OutProblem = FString::Printf(TEXT("Its landscape material '%s' has no Landscape Layer Blend (or Layer Weight or Layer Sample) feeding its Base Color, so it does not say what the paint layers look like."),
			*Out.MaterialName);
		if (!AutoWhy.IsEmpty())
		{
			OutProblem += FString::Printf(TEXT(" It does blend its colour by the slope or height of the ground, but it %s, so that is not baked either."), *AutoWhy);
		}
		return false;
	}

	// A layer's look: the texture or colour its input is, and the normal map the Normal output blends for the same layer.
	auto ReadLook = [&Reader](FMantleLandscapeLayerLook& Look, const FLmLayerSource& Source, const FLmLayerSource* NormalSource)
	{
		Look.Layer = Source.Layer;

		FLmCollected Colours;
		if (Source.Look.IsSet())
		{
			FLmVisit Visit;
			Reader.Collect(Source.Look, ELmProp::BaseColor, 1.0, Colours, Visit);
		}
		const FLmHit* Best = PickBest(Colours.Hits);
		if (Best)
		{
			Look.BaseColor = Best->Texture;
			Look.BaseColorTilingCm = Best->TileCm;
			bool bMacro = false;
			const TArray<FString> Others = OtherTextures(Colours.Hits, Best, bMacro);
			if (Others.Num() > 0)
			{
				Look.Approximations.Add(FString::Printf(TEXT("it also blends %d other texture%s (%s)%s, which %s kept: only '%s' is."),
					Others.Num(), Others.Num() == 1 ? TEXT("") : TEXT("s"), *JoinNames(Others),
					bMacro ? TEXT(", such as a macro variation or a far-distance tiling") : TEXT(""), Others.Num() == 1 ? TEXT("is not") : TEXT("are not"),
					*Best->Name));
			}
			if (Best->TileCm <= 0.0)
			{
				Look.Approximations.Add(FString::Printf(TEXT("its texture '%s' is tiled in a way the graph does not say, so it repeats every 400 cm."), *Best->Name));
			}
			FLinearColor Average;
			if (AverageColour(Best->Texture, Average))
			{
				Look.bHasAverage = true;
				Look.Average = Average;
			}
		}
		else if (Colours.bHasColour)
		{
			Look.bHasFlatColour = true;
			Look.FlatColour = Colours.Colour;
			Look.bHasAverage = true;
			Look.Average = Colours.Colour;
		}
		else if (Source.bConstant)
		{
			Look.bHasFlatColour = true;
			Look.FlatColour = Source.Constant;
			Look.bHasAverage = true;
			Look.Average = Source.Constant;
		}
		else
		{
			Look.bUnreadable = true;
		}

		if (NormalSource)
		{
			if (NormalSource->Look.IsSet())
			{
				FLmCollected Normals;
				FLmVisit Visit;
				Reader.Collect(NormalSource->Look, ELmProp::Normal, 1.0, Normals, Visit);
				if (const FLmHit* BestNormal = PickBest(Normals.Hits))
				{
					Look.Normal = BestNormal->Texture;
					Look.NormalTilingCm = BestNormal->TileCm;
					if (Look.BaseColorTilingCm > 0.0 && BestNormal->TileCm > 0.0 && !FMath::IsNearlyEqual(Look.BaseColorTilingCm, BestNormal->TileCm, Look.BaseColorTilingCm * 0.15))
					{
						Look.Approximations.Add(FString::Printf(TEXT("its normal map repeats every %.0f cm but its colour every %.0f cm; Mantle repeats both the same, every %.0f cm."),
							BestNormal->TileCm, Look.BaseColorTilingCm, Look.BaseColorTilingCm));
					}
				}
			}
		}
	};
	for (const FLmLayerSource& Source : Colour.Sources)
	{
		const int32 Index = Out.Layers.Num();
		const FLmLayerSource* NormalSource = Normal.Find(Source.Layer);
		if (AutoProgram.IsValid())
		{
			NormalSource = AutoNormals.IsValidIndex(Index) && AutoNormals[Index].Look.IsSet() ? &AutoNormals[Index] : nullptr;
		}
		ReadLook(Out.Layers.AddDefaulted_GetRef(), Source, NormalSource);
	}

	// The slope and height chain: each of its layers is baked from the heightfield, and says so.
	if (AutoProgram.IsValid())
	{
		Out.AutoChain.bFound = true;
		Out.AutoChain.Program = AutoProgram;
		for (int32 Index = 0; Index < Out.Layers.Num(); ++Index)
		{
			FMantleLandscapeLayerLook& Look = Out.Layers[Index];
			Out.AutoChain.LayerNames.Add(Look.Layer);
			Look.Approximations.Insert(FMantleLandscapeAutoChain::BakedNote(), 0);
			FLmVisit FresnelVisit;
			if (Colour.Sources[Index].Look.IsSet() && ReadsFresnel(Reader, Colour.Sources[Index].Look, FresnelVisit))
			{
				Look.Approximations.Add(TEXT("its colour also adds a Fresnel (rim) term, which is not kept."));
			}
		}
	}

	// A layer's roughness, from the material's Roughness output with the chain's masks set to show that layer alone.
	if (AutoProgram.IsValid() && RoughnessRoot && RoughnessRoot->Expression)
	{
		FLmRef RoughnessNode = Reader.Resolve(*RoughnessRoot, nullptr);
		if (Base->bUseMaterialAttributes)
		{
			RoughnessNode = FindAttributesRoughness(Reader, RoughnessNode);
		}
		if (RoughnessNode.IsSet())
		{
			for (int32 Index = 0; Index < Out.Layers.Num(); ++Index)
			{
				double Roughness = 0.0;
				if (ChainLayerRoughness(Reader, RoughnessNode, AutoChain, Index, Roughness))
				{
					// Mirror smooth is as glossy as the client draws; a roughness of 0 would only be a material it cannot light.
					Out.Layers[Index].bHasRoughness = true;
					Out.Layers[Index].Roughness = static_cast<float>(FMath::Clamp(Roughness, MinLayerRoughness, 1.0));
				}
				else
				{
					Out.Layers[Index].Approximations.Add(TEXT("its roughness is not a plain number or texture average the material states (it reads the position, the normal or a mask Mantle cannot match), so it keeps the default roughness."));
				}
			}
		}
	}

	// The slope overlay: its look read like a layer's, and its weight tabulated by the normal's Z for the exporter to bake from the heightfield.
	bool bSlopeBaked = false;
	if (Colour.Slope.bFound)
	{
		FLmLayerSource OverlaySource;
		OverlaySource.Layer = Colour.Find(TEXT("Slope")) ? FName(TEXT("Slope overlay")) : FName(TEXT("Slope"));
		OverlaySource.Look = Colour.Slope.Look;
		FLmLayerSource OverlayNormal;
		OverlayNormal.Layer = OverlaySource.Layer;
		OverlayNormal.Look = Normal.Slope.Look;
		FMantleLandscapeLayerLook OverlayLook;
		ReadLook(OverlayLook, OverlaySource, Normal.Slope.bFound ? &OverlayNormal : nullptr);

		// The alpha that blends the painted layers over it, as the heights of the overlay's own colour (its average) would give it.
		const double Height = OverlayLook.bHasAverage ? OverlayLook.Average.R : 0.5;
		FMantleLandscapeSlopeOverlay& Overlay = Out.SlopeOverlay;
		Overlay.Layer = OverlaySource.Layer;
		Overlay.WeightByNormalZ.SetNumZeroed(FMantleLandscapeSlopeOverlay::LutSize);
		bool bUsedHeight = false;
		bool bAllEvaluated = true;
		float MostWeight = 0.0f;
		for (int32 Step = 0; Step < FMantleLandscapeSlopeOverlay::LutSize; ++Step)
		{
			double Alpha = 0.0;
			if (!Reader.EvalSlope(Colour.Slope.Alpha, static_cast<double>(Step) / (FMantleLandscapeSlopeOverlay::LutSize - 1), Height, Alpha, bUsedHeight) || !FMath::IsFinite(Alpha))
			{
				bAllEvaluated = false;
				break;
			}
			Alpha = FMath::Clamp(Alpha, 0.0, 1.0);
			const float Weight = static_cast<float>(Colour.Slope.bOverlayIsA ? 1.0 - Alpha : Alpha);
			Overlay.WeightByNormalZ[Step] = Weight;
			MostWeight = FMath::Max(MostWeight, Weight);
		}
		// An overlay that never shows (a mask of 1 everywhere) has nothing to bake.
		if (bAllEvaluated && MostWeight >= 1.0f / 255.0f)
		{
			bSlopeBaked = true;
			Overlay.bFound = true;
			OverlayLook.Approximations.Insert(FMantleLandscapeSlopeOverlay::BakedNote(), 0);
			if (bUsedHeight)
			{
				OverlayLook.Approximations.Add(FString::Printf(TEXT("its blend also reads the overlay colour's own brightness as a height, which is taken as the texture's average (%.2f)."), Height));
			}
			Out.Layers.Add(MoveTemp(OverlayLook));
		}
		else
		{
			Overlay = FMantleLandscapeSlopeOverlay();
		}
	}
	Out.bFoundLayers = Out.Layers.Num() > 0;

	// What the material does to the landscape as a whole.
	TArray<FString>& Said = Out.Approximations;
	for (const FString& Note : AutoNotes)
	{
		Said.AddUnique(Note);
	}
	if (Flags.bSlope)
	{
		Said.Add(TEXT("it changes the ground by the slope of the surface (it reads the surface normal), which Mantle's flat tiled layers cannot do; the overlay is not kept."));
	}
	if (Colour.Slope.bFound && !bSlopeBaked)
	{
		Said.Add(TEXT("its slope overlay never shows, so no layer is baked for it."));
	}
	if (Flags.bHeight || Colour.bHeightBlend || Normal.bHeightBlend)
	{
		Said.Add(TEXT("it changes or blends the ground by height (a height mask or a height-blended layer); Mantle blends the layers by their painted weights alone."));
	}
	else if (NormalFlags.bHeight)
	{
		Said.Add(TEXT("its normal map also varies with the world height (a depth fade or a height mask on the Normal output); Mantle keeps each layer's normal map as it is."));
	}
	if (Flags.bDistance)
	{
		Said.Add(TEXT("it changes texture tiling or detail with the camera distance (near and far textures); Mantle keeps one tiling a layer, the near one."));
	}
	if (Colour.GlobalSampleNames.Num() > 0)
	{
		Said.Add(FString::Printf(TEXT("it applies %d texture%s to the ground outside the paint layers (%s), such as a macro variation, an overlay or an unmasked base layer; %s not kept."),
			Colour.GlobalSampleNames.Num(), Colour.GlobalSampleNames.Num() == 1 ? TEXT("") : TEXT("s"), *JoinNames(Colour.GlobalSampleNames),
			Colour.GlobalSampleNames.Num() == 1 ? TEXT("it is") : TEXT("they are")));
	}
	bool bWorldMapping = false;
	bool bSlopeNamed = false;
	for (const FString& Name : Reader.FunctionNames)
	{
		bWorldMapping = bWorldMapping || NameHasAny(Name, { TEXT("WorldAligned"), TEXT("Triplanar"), TEXT("WorldCoordinate3Way") });
		bSlopeNamed = bSlopeNamed || NameHasAny(Name, { TEXT("Slope"), TEXT("Cliff") });
	}
	if (bSlopeNamed && !Flags.bSlope && !Colour.Slope.bFound && !AutoProgram.IsValid())
	{
		Said.Add(TEXT("it calls a slope function, which changes the ground by the slope of the surface; Mantle's flat tiled layers cannot do that, so it is not kept."));
	}
	if (bWorldMapping)
	{
		Said.Add(TEXT("it maps some textures from the world position (world-aligned or triplanar), which Mantle repeats flat across the terrain instead."));
	}
	if (Reader.bUnreadableUvs)
	{
		Said.Add(TEXT("some of its textures use a mapping other than the landscape's top-down one (a side projection or a custom UV set), which Mantle does not keep."));
	}
	return true;
}

FString FMantleLandscapeMaterialReader::LayerMaterialName(const FMantleLandscapeLayerLook& Look)
{
	using namespace MantleLandscapeMaterialReaderPrivate;
	FString Seed = FString::Printf(TEXT("%s|%s|%s|%d|%f,%f,%f"), *Look.Layer.ToString(), Look.BaseColor ? *Look.BaseColor->GetPathName() : TEXT(""),
		Look.Normal ? *Look.Normal->GetPathName() : TEXT(""), Look.bHasFlatColour ? 1 : 0, Look.FlatColour.R, Look.FlatColour.G, Look.FlatColour.B);
	if (Look.bHasRoughness)
	{
		// Only a stated roughness is part of the name, so every look without one keeps the name it always had.
		Seed += FString::Printf(TEXT("|r%f"), Look.Roughness);
	}
	return FString::Printf(TEXT("MantleTerrainLayer_%s_%s"), *SafeName(Look.Layer.ToString()), *ShortHash(Seed));
}

UMaterialInterface* FMantleLandscapeMaterialReader::MakeLayerMaterial(const FMantleLandscapeLayerLook& Look)
{
	const FString Name = LayerMaterialName(Look);
	if (UMaterial* Existing = FindObject<UMaterial>(GetTransientPackage(), *Name))
	{
		return Existing;
	}

	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), FName(*Name), RF_Transient);
	UMaterialEditorOnlyData* Graph = Material->GetEditorOnlyData();
	if (!Graph)
	{
		return nullptr;
	}
	auto Sample = [Material](UTexture* Texture) -> UMaterialExpressionTextureSample*
	{
		UMaterialExpressionTextureSample* Node = NewObject<UMaterialExpressionTextureSample>(Material);
		Node->Material = Material;
		Node->Texture = Texture;
		Node->AutoSetSampleType();
		Material->GetExpressionCollection().AddExpression(Node);
		return Node;
	};

	if (Look.BaseColor)
	{
		Graph->BaseColor.Connect(0, Sample(Look.BaseColor));
	}
	else
	{
		// A flat colour, or mid grey where the look could not be read.
		const FLinearColor Colour = Look.bHasFlatColour ? Look.FlatColour : FLinearColor(0.18f, 0.18f, 0.18f, 1.0f);
		UMaterialExpressionConstant3Vector* Constant = NewObject<UMaterialExpressionConstant3Vector>(Material);
		Constant->Material = Material;
		Constant->Constant = Colour;
		Material->GetExpressionCollection().AddExpression(Constant);
		Graph->BaseColor.Connect(0, Constant);
	}
	if (Look.Normal)
	{
		Graph->Normal.Connect(0, Sample(Look.Normal));
	}
	Graph->Roughness.UseConstant = true;
	Graph->Roughness.Constant = Look.bHasRoughness ? Look.Roughness : static_cast<float>(MantleLandscapeMaterialReaderPrivate::DefaultLayerRoughness);

	// The editor's own finishing step: it builds what GLTFExporter reads the graph through. Kept alive for the session so Check,
	// Export and the converter all find it by the same name.
	Material->PostEditChange();
	Material->AddToRoot();
	return Material;
}

bool FMantleLandscapeMaterialReader::AverageColour(UTexture* Texture, FLinearColor& OutColour)
{
	if (!Texture || !Texture->Source.IsValid() || Texture->Source.GetNumBlocks() > 1 || Texture->Source.GetNumSlices() > 1)
	{
		return false;
	}
	// Read once per texture content: Check, Export and the converter each read the landscape.
	static TMap<FString, FLinearColor> Cache;
	const FString Key = Texture->GetPathName() + TEXT("|") + Texture->Source.GetId().ToString();
	if (const FLinearColor* Cached = Cache.Find(Key))
	{
		OutColour = *Cached;
		return true;
	}

	FImage Image;
	if (!Texture->Source.GetMipImage(Image, 0))
	{
		return false;
	}
	FImage Bgra;
	Image.CopyTo(Bgra, ERawImageFormat::BGRA8, ERawImageFormat::IsHDR(Image.Format) ? EGammaSpace::sRGB : Image.GammaSpace);
	const TArrayView64<FColor> Pixels = Bgra.AsBGRA8();
	if (Pixels.Num() == 0 || Bgra.SizeX <= 0 || Bgra.SizeY <= 0)
	{
		return false;
	}

	// A grid of at most 64 x 64 samples is plenty for an average.
	const int32 Steps = 64;
	double Sum[3] = { 0.0, 0.0, 0.0 };
	int32 Count = 0;
	for (int32 Row = 0; Row < Steps; ++Row)
	{
		const int64 Y = FMath::Min<int64>(static_cast<int64>((Row + 0.5) / Steps * Bgra.SizeY), Bgra.SizeY - 1);
		for (int32 Column = 0; Column < Steps; ++Column)
		{
			const int64 X = FMath::Min<int64>(static_cast<int64>((Column + 0.5) / Steps * Bgra.SizeX), Bgra.SizeX - 1);
			const FColor Pixel = Pixels[Y * Bgra.SizeX + X];
			const FLinearColor Linear = Texture->SRGB ? FLinearColor::FromSRGBColor(Pixel) : FLinearColor(Pixel.R / 255.0f, Pixel.G / 255.0f, Pixel.B / 255.0f, 1.0f);
			Sum[0] += Linear.R;
			Sum[1] += Linear.G;
			Sum[2] += Linear.B;
			++Count;
		}
	}
	if (Count == 0)
	{
		return false;
	}
	OutColour = FLinearColor(static_cast<float>(Sum[0] / Count), static_cast<float>(Sum[1] / Count), static_cast<float>(Sum[2] / Count), 1.0f);
	Cache.Add(Key, OutColour);
	return true;
}
