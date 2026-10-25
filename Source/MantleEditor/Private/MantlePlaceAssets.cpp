// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"
#include "MantleWaterBodyExport.h"
#include "MantleActorDecompose.h"
#include "MantleAssetLibrary.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleEffect.h"
#include "MantleEffectExport.h"
#include "MantleMeshReduce.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceLimits.h"
#include "MantleScatterVolume.h"
#include "MantleSplineMeshBake.h"
#include "MantleTerrainExport.h"
#include "MantleTerrainGrassExport.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"
#include "MantleWeaponSpawnPoint.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Animation/Skeleton.h"
#include "Components/AudioComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionBumpOffset.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPerInstanceFadeAmount.h"
#include "Materials/MaterialExpressionPerInstanceRandom.h"
#include "Materials/MaterialExpressionReroute.h"
#include "Materials/MaterialExpressionRotator.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"
#include "StaticMeshResources.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceAssetsPrivate
{
	/** Which output of the material a node feeds: it decides which losses a node counts as. */
	enum class ERoot : uint8
	{
		Surface,
		Normal,
		WorldPositionOffset
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

	FString Quoted(const UObject* Object)
	{
		return Object ? FString::Printf(TEXT("'%s'"), *Object->GetName()) : FString(TEXT("(none)"));
	}

	FString WithUsers(const FString& Subject, const FString& UsedBy)
	{
		return UsedBy.IsEmpty() ? Subject : FString::Printf(TEXT("%s (%s)"), *Subject, *UsedBy);
	}

	/** 500000 as "500,000". */
	FString Thousands(int64 Value)
	{
		FString Digits = FString::Printf(TEXT("%lld"), FMath::Abs(Value));
		for (int32 Index = Digits.Len() - 3; Index > 0; Index -= 3)
		{
			Digits.InsertAt(Index, TEXT(','));
		}
		return Value < 0 ? TEXT("-") + Digits : Digits;
	}

	FString SizeText(int32 Width, int32 Height)
	{
		// A superscript two for a square, as the plan's messages read ("downscaled to 2048 squared").
		return Width == Height ? FString::Printf(TEXT("%d\u00B2"), Width) : FString::Printf(TEXT("%dx%d"), Width, Height);
	}

	/** #628: a node whose value belongs to one instance of a mesh, which a bake has none of. */
	bool IsPerInstanceNode(const UMaterialExpression* Expression)
	{
		return Expression
			&& (Expression->IsA<UMaterialExpressionPerInstanceRandom>() || Expression->IsA<UMaterialExpressionPerInstanceCustomData>()
				|| Expression->IsA<UMaterialExpressionPerInstanceCustomData3Vector>() || Expression->IsA<UMaterialExpressionPerInstanceFadeAmount>());
	}

	struct FWalk
	{
		const UMaterialInterface* Context = nullptr;
		FMantleMaterialAnalysis* Out = nullptr;
		bool bTranslucent = false;
		TSet<TPair<const UMaterialExpression*, uint8>> Visited;
		TSet<const UObject*> Dependencies;
		TSet<const UTexture*> Textures;
		TSet<const UTexture*> NormalTextures;
		int32 Budget = 50000;

		/** Vertex Color's RGB reached from the base colour through Multiply and reroute nodes only (#471). */
		bool bBaseColorTint = false;

		void AddDependency(const UObject* Object)
		{
			if (Object && !Dependencies.Contains(Object))
			{
				Dependencies.Add(Object);
				Out->Dependencies.Add(const_cast<UObject*>(Object));
			}
		}

		void Flag(EMantleMaterialLoss Loss)
		{
			Out->Losses |= Loss;
		}

		/** A texture an output uses that is not walked (one #533 carries): a dependency and one of the material's textures. */
		void AddTexture(UTexture* Texture)
		{
			if (!Texture)
			{
				return;
			}
			AddDependency(Texture);
			if (!Textures.Contains(Texture))
			{
				Textures.Add(Texture);
				Out->Textures.Add(Texture);
			}
		}

		/** Every asset an expression's own properties point at (textures, functions, parameter collections, fonts). */
		void CollectReferences(const UMaterialExpression* Expression)
		{
			for (TFieldIterator<FObjectPropertyBase> It(Expression->GetClass()); It; ++It)
			{
				const UObject* Value = It->GetObjectPropertyValue_InContainer(Expression);
				if (Value && Value->IsAsset())
				{
					AddDependency(Value);
				}
			}
		}

		UTexture* ResolveTexture(const UMaterialExpressionTextureBase* Sample) const
		{
			if (const UMaterialExpressionTextureSampleParameter* Parameter = Cast<UMaterialExpressionTextureSampleParameter>(Sample))
			{
				UTexture* Value = nullptr;
				if (Context->GetTextureParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Value) && Value)
				{
					return Value;
				}
			}
			return Sample->Texture;
		}

		void WalkInput(const FExpressionInput* Input, ERoot Root)
		{
			if (Input && Input->Expression)
			{
				Walk(Input->Expression, Root);
			}
		}

		/**
		 * The base colour (#471): Vertex Color's RGB reached through Multiply and reroute nodes only is a
		 * tint the material document carries (vertexColor multiplyBaseColor), so it is noted, not walked
		 * as a loss. Everything else on the way is walked as usual; a Multiply the chain passes through is
		 * not marked visited, so reaching it from another output still walks (and counts) its vertex colour.
		 */
		void WalkBaseColor(const FExpressionInput* Input)
		{
			if (!Input || !Input->Expression || --Budget < 0)
			{
				return;
			}
			const UMaterialExpression* Expression = Input->Expression;
			if (Expression->IsA<UMaterialExpressionVertexColor>() && Input->OutputIndex == 0)
			{
				bBaseColorTint = true;
				return;
			}
			if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
			{
				WalkBaseColor(&Multiply->A);
				WalkBaseColor(&Multiply->B);
				return;
			}
			if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(Expression))
			{
				WalkBaseColor(&Reroute->Input);
				return;
			}
			Walk(Expression, ERoot::Surface);
		}

		/** A function's outputs: what a call can reach inside it. */
		void WalkFunction(const UMaterialFunctionInterface* Function, ERoot Root)
		{
			if (!Function)
			{
				return;
			}
			AddDependency(Function);
			for (const TObjectPtr<UMaterialExpression>& Inner : Function->GetExpressions())
			{
				if (ClassNamed(Inner, TEXT("MaterialExpressionFunctionOutput")))
				{
					Walk(Inner, Root);
				}
			}
		}

		void Walk(const UMaterialExpression* Expression, ERoot Root)
		{
			if (!Expression || --Budget < 0)
			{
				return;
			}
			const TPair<const UMaterialExpression*, uint8> Key(Expression, static_cast<uint8>(Root));
			if (Visited.Contains(Key))
			{
				return;
			}
			Visited.Add(Key);
			CollectReferences(Expression);

			const bool bFlattened = Root != ERoot::WorldPositionOffset;
			if (bFlattened)
			{
				if (Expression->IsA<UMaterialExpressionTime>() || Expression->IsA<UMaterialExpressionPanner>() || Expression->IsA<UMaterialExpressionRotator>())
				{
					Flag(EMantleMaterialLoss::Animation);
				}
				else if (Expression->IsA<UMaterialExpressionVertexColor>())
				{
					Flag(EMantleMaterialLoss::VertexColor);
				}
				else if (Expression->IsA<UMaterialExpressionWorldPosition>())
				{
					Flag(EMantleMaterialLoss::WorldAligned);
				}
				else if (Expression->IsA<UMaterialExpressionBumpOffset>())
				{
					Flag(EMantleMaterialLoss::Parallax);
				}
				else if (IsPerInstanceNode(Expression))
				{
					// #628: no loss to flag; the bake is made at a representative value (NeutralisePerInstanceValues).
					Out->bPerInstanceVariation = true;
				}
				else if (const UMaterialExpressionTextureBase* Sample = Cast<UMaterialExpressionTextureBase>(Expression))
				{
					if (UTexture* Texture = ResolveTexture(Sample))
					{
						AddTexture(Texture);
						if (Root == ERoot::Normal && Texture->IsNormalMap())
						{
							NormalTextures.Add(Texture);
						}
					}
				}
			}

			if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				if (bFlattened && Call->MaterialFunction)
				{
					const FString Name = Call->MaterialFunction->GetName();
					if (NameHasAny(Name, { TEXT("Flipbook"), TEXT("SubUV") }))
					{
						Flag(EMantleMaterialLoss::Animation);
					}
					if (NameHasAny(Name, { TEXT("WorldAligned"), TEXT("Triplanar"), TEXT("WorldCoordinate3Way"), TEXT("TriPlanar") }))
					{
						Flag(EMantleMaterialLoss::WorldAligned);
					}
					if (NameHasAny(Name, { TEXT("Parallax"), TEXT("POM") }))
					{
						Flag(EMantleMaterialLoss::Parallax);
					}
				}
				WalkFunction(Call->MaterialFunction, Root);
			}

			// A static switch keeps only the branch the material (instance) selects.
			if (const UMaterialExpressionStaticSwitchParameter* SwitchParameter = Cast<UMaterialExpressionStaticSwitchParameter>(Expression))
			{
				bool bValue = SwitchParameter->DefaultValue != 0;
				bool bFound = false;
				FGuid Guid;
				if (Context->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(SwitchParameter->ParameterName)), bFound, Guid))
				{
					bValue = bFound;
				}
				WalkInput(bValue ? &SwitchParameter->A : &SwitchParameter->B, Root);
				return;
			}
			if (const UMaterialExpressionStaticSwitch* Switch = Cast<UMaterialExpressionStaticSwitch>(Expression))
			{
				if (Switch->Value.Expression)
				{
					Walk(Switch->Value.Expression, Root);
					WalkInput(&Switch->A, Root);
					WalkInput(&Switch->B, Root);
				}
				else
				{
					WalkInput(Switch->DefaultValue ? &Switch->A : &Switch->B, Root);
				}
				return;
			}

			// A named reroute's usage continues at its declaration.
			if (ClassNamed(Expression, TEXT("MaterialExpressionNamedRerouteUsage")))
			{
				if (const FObjectPropertyBase* Declaration = FindFProperty<FObjectPropertyBase>(Expression->GetClass(), TEXT("Declaration")))
				{
					Walk(Cast<UMaterialExpression>(Declaration->GetObjectPropertyValue_InContainer(Expression)), Root);
				}
			}

			// Make/Set Material Attributes carry every output as a named input.
			const bool bAttributes = ClassNamed(Expression, TEXT("MaterialExpressionMakeMaterialAttributes"))
				|| ClassNamed(Expression, TEXT("MaterialExpressionSetMaterialAttributes"));
			UMaterialExpression* Mutable = const_cast<UMaterialExpression*>(Expression);
			for (FExpressionInputIterator It{ Mutable }; It; ++It)
			{
				const FExpressionInput* Input = It.Input;
				const int32 Index = It.Index;
				if (!Input->Expression)
				{
					continue;
				}
				ERoot InputRoot = Root;
				if (bAttributes)
				{
					const FString Name = Mutable->GetInputName(Index).ToString();
					if (NameHasAny(Name, { TEXT("World Position Offset"), TEXT("WorldPositionOffset") }))
					{
						Flag(EMantleMaterialLoss::WorldPositionOffset);
						InputRoot = ERoot::WorldPositionOffset;
					}
					else if (NameHasAny(Name, { TEXT("Pixel Depth Offset"), TEXT("PixelDepthOffset") }))
					{
						Flag(EMantleMaterialLoss::PixelDepthOffset);
					}
					else if (NameHasAny(Name, { TEXT("Refraction") }))
					{
						if (bTranslucent)
						{
							Flag(EMantleMaterialLoss::Refraction);
						}
					}
					else if (NameHasAny(Name, { TEXT("Displacement") }))
					{
						Flag(EMantleMaterialLoss::Displacement);
					}
					else if (NameHasAny(Name, { TEXT("Normal") }))
					{
						InputRoot = ERoot::Normal;
					}
					else if (Root == ERoot::Surface && NameHasAny(Name, { TEXT("BaseColor"), TEXT("Base Color") }))
					{
						WalkBaseColor(Input);
						continue;
					}
				}
				Walk(Input->Expression, InputRoot);
			}
		}
	};

	// ---- #533: the patterns the material document carries beyond the bake ----------------------------------

	const TCHAR* const SimpleGrassWindPath = TEXT("/Engine/Functions/Engine_MaterialFunctions01/WorldPositionOffset/SimpleGrassWind");
	const TCHAR* const WorldAlignedTexturePath = TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedTexture");
	const TCHAR* const WorldAlignedNormalPath = TEXT("/Engine/Functions/Engine_MaterialFunctions01/Texturing/WorldAlignedNormal");

	/** The schema's #533 bounds (schema/material.schema.json). */
	constexpr double MaxPanSpeed = 10.0;
	constexpr double MaxWindValue = 4.0;
	constexpr double MinTileSize = 1.0;
	constexpr double MaxTileSize = 100000.0;
	constexpr double MinContrast = 1.0;
	constexpr double MaxContrast = 16.0;

	/** The node an input reads through reroutes, and which of its outputs; null when nothing is connected. */
	const UMaterialExpression* Follow(const FExpressionInput* Input, int32& OutOutput)
	{
		for (int32 Depth = 0; Input && Input->Expression && Depth < 32; ++Depth)
		{
			if (const UMaterialExpressionReroute* Reroute = Cast<UMaterialExpressionReroute>(Input->Expression))
			{
				Input = &Reroute->Input;
				continue;
			}
			OutOutput = Input->OutputIndex;
			return Input->Expression;
		}
		OutOutput = 0;
		return nullptr;
	}

	/** 'r', 'g', 'b' or 'a' for a one-channel output, 'c' for RGB, 'x' for all four (or an unmasked output), 0 otherwise. */
	TCHAR OutputChannel(const UMaterialExpression* Expression, const int32 Output)
	{
		if (!Expression || !Expression->Outputs.IsValidIndex(Output))
		{
			return 0;
		}
		const FExpressionOutput& Out = Expression->Outputs[Output];
		if (!Out.Mask)
		{
			return TEXT('x');
		}
		const int32 Count = (Out.MaskR ? 1 : 0) + (Out.MaskG ? 1 : 0) + (Out.MaskB ? 1 : 0) + (Out.MaskA ? 1 : 0);
		if (Count == 1)
		{
			return Out.MaskR ? TEXT('r') : Out.MaskG ? TEXT('g') : Out.MaskB ? TEXT('b') : TEXT('a');
		}
		if (Count == 3 && Out.MaskR && Out.MaskG && Out.MaskB)
		{
			return TEXT('c');
		}
		return Count == 4 ? TEXT('x') : 0;
	}

	/** The one channel a Component Mask keeps ('r', 'g', 'b', 'a'), or 0 when it keeps another number. */
	TCHAR MaskChannel(const UMaterialExpressionComponentMask* Mask)
	{
		const int32 Count = (Mask->R ? 1 : 0) + (Mask->G ? 1 : 0) + (Mask->B ? 1 : 0) + (Mask->A ? 1 : 0);
		return Count != 1 ? 0 : Mask->R ? TEXT('r') : Mask->G ? TEXT('g') : Mask->B ? TEXT('b') : TEXT('a');
	}

	/** A constant, or a scalar parameter as Context sets it. */
	bool ConstantScalar(const FExpressionInput* Input, const UMaterialInterface* Context, double& Out)
	{
		int32 Output = 0;
		const UMaterialExpression* Expression = Follow(Input, Output);
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

	/** Nothing connected, or a constant or parameter value: no texture, nothing that varies over the surface. */
	bool IsFlat(const FExpressionInput* Input)
	{
		int32 Output = 0;
		const UMaterialExpression* Expression = Follow(Input, Output);
		return !Expression || Expression->IsA<UMaterialExpressionConstant>() || Expression->IsA<UMaterialExpressionConstant3Vector>()
			|| ClassNamed(Expression, TEXT("MaterialExpressionConstant2Vector")) || ClassNamed(Expression, TEXT("MaterialExpressionConstant4Vector"))
			|| Expression->IsA<UMaterialExpressionScalarParameter>() || Expression->IsA<UMaterialExpressionVectorParameter>();
	}

	/** A call to the engine function at Path (a package path), and not to an instance of it. */
	const UMaterialExpressionMaterialFunctionCall* CallTo(const UMaterialExpression* Expression, const TCHAR* Path)
	{
		const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
		return Call && Call->MaterialFunction && Call->MaterialFunction->GetOutermost()->GetName() == Path ? Call : nullptr;
	}

	const FFunctionExpressionInput* CallInput(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name)
	{
		for (const FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (Input.Input.InputName == FName(Name))
			{
				return &Input;
			}
		}
		return nullptr;
	}

	bool IsCallOutput(const UMaterialExpressionMaterialFunctionCall* Call, const int32 Output, const TCHAR* Name)
	{
		return Call->FunctionOutputs.IsValidIndex(Output) && Call->FunctionOutputs[Output].Output.OutputName == FName(Name);
	}

	/**
	 * A function call's input as one number: a constant or parameter connected to it (a Constant3Vector or
	 * vector parameter only with equal components), or, unconnected, the function's own default when it has one.
	 */
	bool CallInputValue(const UMaterialExpressionMaterialFunctionCall* Call, const TCHAR* Name, const UMaterialInterface* Context, double& Out)
	{
		const FFunctionExpressionInput* Input = CallInput(Call, Name);
		if (!Input)
		{
			return false;
		}
		if (!Input->Input.Expression)
		{
			const UMaterialExpressionFunctionInput* Declared = Input->ExpressionInput.Get();
			if (!Declared || !Declared->bUsePreviewValueAsDefault || Declared->Preview.Expression)
			{
				return false;
			}
			const FVector4f& Value = Declared->PreviewValue;
			const bool bVector = Declared->InputType == FunctionInput_Vector3 || Declared->InputType == FunctionInput_Vector4;
			if (bVector && !(Value.X == Value.Y && Value.Y == Value.Z))
			{
				return false;
			}
			Out = Value.X;
			return true;
		}
		if (ConstantScalar(&Input->Input, Context, Out))
		{
			return true;
		}
		int32 Output = 0;
		const UMaterialExpression* Expression = Follow(&Input->Input, Output);
		FLinearColor Colour = FLinearColor::Black;
		if (const UMaterialExpressionConstant3Vector* Constant = Cast<UMaterialExpressionConstant3Vector>(Expression))
		{
			Colour = Constant->Constant;
		}
		else if (const UMaterialExpressionVectorParameter* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression))
		{
			Colour = Parameter->DefaultValue;
			if (Context)
			{
				Context->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Colour);
			}
		}
		else
		{
			return false;
		}
		if (!(Colour.R == Colour.G && Colour.G == Colour.B))
		{
			return false;
		}
		Out = Colour.R;
		return true;
	}

	/** A Texture Sample or Texture Sample Parameter 2D (not a cube, volume or virtual texture sample). */
	const UMaterialExpressionTextureSample* PlainSampleClass(const UMaterialExpression* Expression)
	{
		return Expression && (Expression->GetClass() == UMaterialExpressionTextureSample::StaticClass()
			|| Expression->GetClass() == UMaterialExpressionTextureSampleParameter2D::StaticClass())
			? static_cast<const UMaterialExpressionTextureSample*>(Expression) : nullptr;
	}

	/** A sample's UVs as the first UV set scaled by a Texture Coordinate's tiling (or unconnected, UV 0), no mirroring. */
	bool FirstUVTiling(const FExpressionInput& Coordinates, const uint32 ConstCoordinate, double (&OutTiling)[2])
	{
		if (!Coordinates.Expression)
		{
			OutTiling[0] = OutTiling[1] = 1.0;
			return ConstCoordinate == 0;
		}
		int32 Output = 0;
		const UMaterialExpressionTextureCoordinate* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Follow(&Coordinates, Output));
		if (!TexCoord || TexCoord->CoordinateIndex != 0 || TexCoord->UnMirrorU || TexCoord->UnMirrorV)
		{
			return false;
		}
		OutTiling[0] = TexCoord->UTiling;
		OutTiling[1] = TexCoord->VTiling;
		return true;
	}

	/** One recognised texture sample: the sample, which of its outputs is read, its texture, its tiling and (panned) speed. */
	struct FRecognisedSample
	{
		const UMaterialExpressionTextureSample* Sample = nullptr;
		TCHAR Channel = 0;
		UTexture* Texture = nullptr;
		double Tiling[2] = { 1.0, 1.0 };
		double Speed[2] = { 0.0, 0.0 };
	};

	/**
	 * Input reads a Texture Sample of a 2D texture (its own, not a Texture Object input): on plain UVs
	 * (bPanned false) or through a Panner with no Time or Speed input (bPanned true).
	 */
	bool RecogniseSample(const FWalk& Walk, const FExpressionInput* Input, const bool bPanned, FRecognisedSample& Out)
	{
		int32 Output = 0;
		const UMaterialExpressionTextureSample* Sample = PlainSampleClass(Follow(Input, Output));
		if (!Sample || Sample->TextureObject.Expression)
		{
			return false;
		}
		UTexture2D* Texture = Cast<UTexture2D>(Walk.ResolveTexture(Sample));
		if (!Texture)
		{
			return false;
		}
		Out = FRecognisedSample();
		Out.Sample = Sample;
		Out.Channel = OutputChannel(Sample, Output);
		Out.Texture = Texture;
		if (!bPanned)
		{
			return FirstUVTiling(Sample->Coordinates, Sample->ConstCoordinate, Out.Tiling);
		}
		int32 PannerOutput = 0;
		const UMaterialExpressionPanner* Panner = Cast<UMaterialExpressionPanner>(Follow(&Sample->Coordinates, PannerOutput));
		if (!Panner || Panner->Time.Expression || Panner->Speed.Expression || FMath::Abs(Panner->SpeedX) > MaxPanSpeed || FMath::Abs(Panner->SpeedY) > MaxPanSpeed)
		{
			return false;
		}
		Out.Speed[0] = Panner->SpeedX;
		Out.Speed[1] = Panner->SpeedY;
		return FirstUVTiling(Panner->Coordinate, Panner->ConstCoordinate, Out.Tiling);
	}

	/** The one vertex-colour channel Input reads ('r', 'g', 'b', 'a'): an output of Vertex Color, or a one-channel Component Mask of it. */
	TCHAR VertexColorChannel(const FExpressionInput* Input)
	{
		int32 Output = 0;
		const UMaterialExpression* Expression = Follow(Input, Output);
		if (Expression && Expression->IsA<UMaterialExpressionVertexColor>())
		{
			const TCHAR Channel = OutputChannel(Expression, Output);
			return Channel == TEXT('r') || Channel == TEXT('g') || Channel == TEXT('b') || Channel == TEXT('a') ? Channel : 0;
		}
		if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
		{
			int32 InnerOutput = 0;
			const UMaterialExpression* Inner = Follow(&Mask->Input, InnerOutput);
			const TCHAR Kept = MaskChannel(Mask);
			const TCHAR Read = Inner && Inner->IsA<UMaterialExpressionVertexColor>() ? OutputChannel(Inner, InnerOutput) : 0;
			if (Kept && (Read == TEXT('x') || (Read == TEXT('c') && Kept != TEXT('a'))))
			{
				return Kept;
			}
		}
		return 0;
	}

	/** The Material's own inputs this recognises by, as the walk reads them. */
	struct FMaterialRoots
	{
		const FExpressionInput* BaseColor = nullptr;
		const FExpressionInput* Normal = nullptr;
		const FExpressionInput* Emissive = nullptr;
		const FExpressionInput* Roughness = nullptr;
		const FExpressionInput* Metallic = nullptr;
		const FExpressionInput* Occlusion = nullptr;
		const FExpressionInput* Opacity = nullptr;
		const FExpressionInput* WorldPositionOffset = nullptr;
	};

	bool IsConnected(const FExpressionInput* Input)
	{
		return Input && Input->Expression;
	}

	bool ReadsColour(const FRecognisedSample& Sample)
	{
		return Sample.Channel == TEXT('c') || Sample.Channel == TEXT('x');
	}

	/**
	 * The ORM group read one way: Roughness from Read('g'), Metallic from Read('b') or the constant 0, Ambient
	 * Occlusion from Read('r') or unconnected. Read returns false for anything it does not recognise. False when
	 * Roughness is not connected.
	 */
	bool RecogniseOrmGroup(const FMaterialRoots& Roots, const UMaterialInterface* Context, TFunctionRef<bool(const FExpressionInput*, TCHAR)> Read,
		double& OutMetallicFactor, double& OutOcclusionStrength)
	{
		if (!IsConnected(Roots.Roughness) || !Read(Roots.Roughness, TEXT('g')))
		{
			return false;
		}
		// Unconnected, Metallic is 0 in Unreal; the constant 0 is the same.
		double Metallic = 1.0;
		if (IsConnected(Roots.Metallic) && !Read(Roots.Metallic, TEXT('b')))
		{
			if (!ConstantScalar(Roots.Metallic, Context, Metallic) || Metallic != 0.0)
			{
				return false;
			}
		}
		OutMetallicFactor = IsConnected(Roots.Metallic) && Metallic != 0.0 ? 1.0 : 0.0;
		// Unconnected, occlusion is 1: an occlusion strength of 0 whatever the texture's R.
		if (IsConnected(Roots.Occlusion) && !Read(Roots.Occlusion, TEXT('r')))
		{
			return false;
		}
		OutOcclusionStrength = IsConnected(Roots.Occlusion) ? 1.0 : 0.0;
		return true;
	}

	/** The panned sample one root reads, as RGB (or RGBA). */
	bool RecognisePanned(const FWalk& Walk, const FExpressionInput* Input, FRecognisedSample& Out)
	{
		return IsConnected(Input) && RecogniseSample(Walk, Input, true, Out) && ReadsColour(Out);
	}

	/** wind: World Position Offset = SimpleGrassWind(constant intensity and speed, a recognised weight), nothing added. */
	bool RecogniseWind(const FMaterialRoots& Roots, const UMaterialInterface* Context, FMantleMaterialFidelity2& Out)
	{
		int32 Output = 0;
		const UMaterialExpressionMaterialFunctionCall* Call = CallTo(Follow(Roots.WorldPositionOffset, Output), SimpleGrassWindPath);
		double Intensity = 0.0;
		double Speed = 0.0;
		if (!Call || !CallInputValue(Call, TEXT("WindIntensity"), Context, Intensity) || !CallInputValue(Call, TEXT("WindSpeed"), Context, Speed)
			|| Intensity < 0.0 || Intensity > MaxWindValue || Speed < 0.0 || Speed > MaxWindValue)
		{
			return false;
		}
		const FFunctionExpressionInput* Added = CallInput(Call, TEXT("AdditionalWPO"));
		if (!Added)
		{
			return false;
		}
		if (Added->Input.Expression)
		{
			int32 AddedOutput = 0;
			const UMaterialExpressionConstant3Vector* Zero = Cast<UMaterialExpressionConstant3Vector>(Follow(&Added->Input, AddedOutput));
			double Scalar = 1.0;
			const bool bZeroVector = Zero && Zero->Constant.R == 0.0f && Zero->Constant.G == 0.0f && Zero->Constant.B == 0.0f;
			if (!bZeroVector && !(ConstantScalar(&Added->Input, Context, Scalar) && Scalar == 0.0))
			{
				return false;
			}
		}

		// The weight: Vertex Color's R, 1 - TexCoord[0].G (V untiled), or the constant 1.
		const FFunctionExpressionInput* WeightInput = CallInput(Call, TEXT("WindWeight"));
		if (!WeightInput)
		{
			return false;
		}
		FString Weight;
		double Uniform = 0.0;
		int32 WeightOutput = 0;
		const UMaterialExpression* WeightNode = Follow(&WeightInput->Input, WeightOutput);
		if (VertexColorChannel(&WeightInput->Input) == TEXT('r'))
		{
			Weight = TEXT("vertexColorR");
		}
		else if (const UMaterialExpressionOneMinus* OneMinus = Cast<UMaterialExpressionOneMinus>(WeightNode))
		{
			int32 MaskOutput = 0;
			const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Follow(&OneMinus->Input, MaskOutput));
			int32 CoordinateOutput = 0;
			const UMaterialExpressionTextureCoordinate* TexCoord = Mask && MaskChannel(Mask) == TEXT('g')
				? Cast<UMaterialExpressionTextureCoordinate>(Follow(&Mask->Input, CoordinateOutput)) : nullptr;
			if (TexCoord && TexCoord->CoordinateIndex == 0 && TexCoord->VTiling == 1.0f && !TexCoord->UnMirrorV)
			{
				Weight = TEXT("uvV");
			}
		}
		else if (CallInputValue(Call, TEXT("WindWeight"), Context, Uniform) && Uniform == 1.0)
		{
			Weight = TEXT("uniform");
		}
		if (Weight.IsEmpty())
		{
			return false;
		}
		Out.bWind = true;
		Out.WindIntensity = Intensity;
		Out.WindSpeed = Speed;
		Out.WindWeight = Weight;
		return true;
	}

	/** One world-aligned call's texture and its size and contrast (constant, in the schema's bounds), with no WorldPosition input. */
	bool WorldAlignedCall(const FWalk& Walk, const UMaterialExpressionMaterialFunctionCall* Call, const UMaterialInterface* Context, UTexture*& OutTexture,
		double& OutSize, double& OutContrast)
	{
		// The function's own world position (the client's is the same); a creator's would move the pattern.
		for (const FFunctionExpressionInput& Input : Call->FunctionInputs)
		{
			if (Input.Input.Expression && Input.Input.InputName.ToString().Replace(TEXT(" "), TEXT("")).Equals(TEXT("WorldPosition"), ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		const FFunctionExpressionInput* Object = CallInput(Call, TEXT("TextureObject"));
		if (!Object || !CallInputValue(Call, TEXT("TextureSize"), Context, OutSize)
			|| !CallInputValue(Call, TEXT("ProjectionTransitionContrast"), Context, OutContrast)
			|| OutSize < MinTileSize || OutSize > MaxTileSize || OutContrast < MinContrast || OutContrast > MaxContrast)
		{
			return false;
		}
		int32 Output = 0;
		const UMaterialExpressionTextureBase* TextureNode = Cast<UMaterialExpressionTextureBase>(Follow(&Object->Input, Output));
		OutTexture = TextureNode && (ClassNamed(TextureNode, TEXT("MaterialExpressionTextureObject")) || ClassNamed(TextureNode, TEXT("MaterialExpressionTextureObjectParameter")))
			? Cast<UTexture2D>(Walk.ResolveTexture(TextureNode)) : nullptr;
		return OutTexture != nullptr;
	}

	/** Input reads Path's XYZ Texture output directly or through a Component Mask of R, G and B; its call. */
	const UMaterialExpressionMaterialFunctionCall* WorldAlignedXYZ(const FExpressionInput* Input, const TCHAR* Path)
	{
		int32 Output = 0;
		const UMaterialExpression* Expression = Follow(Input, Output);
		if (const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
		{
			if (!(Mask->R && Mask->G && Mask->B && !Mask->A))
			{
				return nullptr;
			}
			Expression = Follow(&Mask->Input, Output);
		}
		const UMaterialExpressionMaterialFunctionCall* Call = CallTo(Expression, Path);
		return Call && IsCallOutput(Call, Output, TEXT("XYZ Texture")) ? Call : nullptr;
	}

	/**
	 * worldAligned (opaque only): Base Color, Normal and the ORM group each world-aligned with one size and
	 * contrast, or flat; Base Color or Normal world-aligned.
	 */
	bool RecogniseWorldAligned(const FWalk& Walk, const FMaterialRoots& Roots, const UMaterialInterface* Context, FMantleMaterialFidelity2& Out)
	{
		bool bFirst = true;
		double Size = 0.0;
		double Contrast = 0.0;
		auto Same = [&](const UMaterialExpressionMaterialFunctionCall* Call, UTexture*& OutTexture)
		{
			double CallSize = 0.0;
			double CallContrast = 0.0;
			if (!Call || !WorldAlignedCall(Walk, Call, Context, OutTexture, CallSize, CallContrast))
			{
				return false;
			}
			if (bFirst)
			{
				bFirst = false;
				Size = CallSize;
				Contrast = CallContrast;
				return true;
			}
			return CallSize == Size && CallContrast == Contrast;
		};

		UTexture* Sources[3] = {};
		if (!IsFlat(Roots.BaseColor) && !Same(WorldAlignedXYZ(Roots.BaseColor, WorldAlignedTexturePath), Sources[0]))
		{
			return false;
		}
		if (!IsFlat(Roots.Normal) && !Same(WorldAlignedXYZ(Roots.Normal, WorldAlignedNormalPath), Sources[1]))
		{
			return false;
		}
		if (!Sources[0] && !Sources[1])
		{
			return false; // nothing world-aligned where it shows
		}
		// The ORM group: channels of one WorldAlignedTexture call, or every one of them flat.
		const UMaterialExpressionMaterialFunctionCall* OrmCall = nullptr;
		double MetallicFactor = 1.0;
		double OcclusionStrength = 1.0;
		auto ReadAligned = [&](const FExpressionInput* Input, const TCHAR Channel)
		{
			int32 Output = 0;
			const UMaterialExpressionComponentMask* Mask = Cast<UMaterialExpressionComponentMask>(Follow(Input, Output));
			if (!Mask || MaskChannel(Mask) != Channel)
			{
				return false;
			}
			int32 CallOutput = 0;
			const UMaterialExpressionMaterialFunctionCall* Call = CallTo(Follow(&Mask->Input, CallOutput), WorldAlignedTexturePath);
			if (!Call || !IsCallOutput(Call, CallOutput, TEXT("XYZ Texture")) || (OrmCall && Call != OrmCall))
			{
				return false;
			}
			OrmCall = Call;
			return true;
		};
		const bool bOrmFlat = IsFlat(Roots.Roughness) && IsFlat(Roots.Metallic) && IsFlat(Roots.Occlusion);
		if (!bOrmFlat)
		{
			if (!RecogniseOrmGroup(Roots, Context, ReadAligned, MetallicFactor, OcclusionStrength) || !Same(OrmCall, Sources[2]))
			{
				return false;
			}
		}
		Out.bWorldAligned = true;
		Out.TileSizeCm = Size;
		Out.Sharpness = Contrast;
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			Out.SourceTextures[Slot] = Sources[Slot];
		}
		Out.OrmMetallicFactor = MetallicFactor;
		Out.OrmOcclusionStrength = OcclusionStrength;
		return true;
	}

	/** One Lerp by a vertex-colour channel between two plain samples, and what each reads. */
	struct FRecognisedLerp
	{
		FRecognisedSample A;
		FRecognisedSample B;
		TCHAR Channel = 0;
	};

	bool RecogniseLerp(const FWalk& Walk, const FExpressionInput* Input, FRecognisedLerp& Out)
	{
		int32 Output = 0;
		const UMaterialExpressionLinearInterpolate* Lerp = Cast<UMaterialExpressionLinearInterpolate>(Follow(Input, Output));
		return Lerp && RecogniseSample(Walk, &Lerp->A, false, Out.A) && RecogniseSample(Walk, &Lerp->B, false, Out.B)
			&& (Out.Channel = VertexColorChannel(&Lerp->Alpha)) != 0;
	}

	/**
	 * layerBlend (opaque only): Base Color a Lerp by one vertex-colour channel between two plain samples; Normal and
	 * the ORM group the same Lerp by the same channel, or one plain sample (the layer keeps it), or nothing.
	 */
	bool RecogniseLayerBlend(const FWalk& Walk, const FMaterialRoots& Roots, const UMaterialInterface* Context, FMantleMaterialFidelity2& Out)
	{
		FRecognisedLerp Base;
		if (!RecogniseLerp(Walk, Roots.BaseColor, Base) || !ReadsColour(Base.A) || !ReadsColour(Base.B))
		{
			return false;
		}
		// The layer samples on the base colour's UVs, so every layer texture must tile as the base colour does.
		auto OnBaseUV = [&Base](const FRecognisedSample& Layer)
		{
			return Layer.Tiling[0] == Base.A.Tiling[0] && Layer.Tiling[1] == Base.A.Tiling[1];
		};
		if (!OnBaseUV(Base.B))
		{
			return false;
		}
		UTexture* Sources[3] = { Base.A.Texture, nullptr, nullptr };
		UTexture* Layers[3] = { Base.B.Texture, nullptr, nullptr };
		double Tiling[3][2] = { { Base.A.Tiling[0], Base.A.Tiling[1] }, { 1.0, 1.0 }, { 1.0, 1.0 } };

		if (IsConnected(Roots.Normal))
		{
			FRecognisedLerp Normal;
			FRecognisedSample Plain;
			if (RecogniseLerp(Walk, Roots.Normal, Normal))
			{
				if (Normal.Channel != Base.Channel || !ReadsColour(Normal.A) || !ReadsColour(Normal.B) || !OnBaseUV(Normal.B))
				{
					return false;
				}
				Sources[1] = Normal.A.Texture;
				Layers[1] = Normal.B.Texture;
				Tiling[1][0] = Normal.A.Tiling[0];
				Tiling[1][1] = Normal.A.Tiling[1];
			}
			else if (RecogniseSample(Walk, Roots.Normal, false, Plain) && ReadsColour(Plain))
			{
				Sources[1] = Layers[1] = Plain.Texture;
				Tiling[1][0] = Plain.Tiling[0];
				Tiling[1][1] = Plain.Tiling[1];
			}
			else
			{
				return false;
			}
		}

		// The ORM group: every channel the same Lerp shape between one A sample and one B sample, or every channel one plain sample.
		const UMaterialExpressionTextureSample* OrmA = nullptr;
		const UMaterialExpressionTextureSample* OrmB = nullptr;
		FRecognisedLerp OrmLerp;
		bool bLayered = false;
		bool bPlain = false;
		auto ReadLayered = [&](const FExpressionInput* Input, const TCHAR Channel)
		{
			FRecognisedLerp Lerp;
			if (bPlain || !RecogniseLerp(Walk, Input, Lerp) || Lerp.Channel != Base.Channel || Lerp.A.Channel != Channel || Lerp.B.Channel != Channel
				|| (OrmA && (Lerp.A.Sample != OrmA || Lerp.B.Sample != OrmB)))
			{
				return false;
			}
			bLayered = true;
			OrmA = Lerp.A.Sample;
			OrmB = Lerp.B.Sample;
			OrmLerp = Lerp;
			return true;
		};
		FRecognisedSample OrmPlain;
		auto ReadPlain = [&](const FExpressionInput* Input, const TCHAR Channel)
		{
			FRecognisedSample Sample;
			if (bLayered || !RecogniseSample(Walk, Input, false, Sample) || Sample.Channel != Channel || (OrmA && Sample.Sample != OrmA))
			{
				return false;
			}
			bPlain = true;
			OrmA = Sample.Sample;
			OrmPlain = Sample;
			return true;
		};
		double MetallicFactor = 1.0;
		double OcclusionStrength = 1.0;
		if (IsConnected(Roots.Roughness) || IsConnected(Roots.Metallic) || IsConnected(Roots.Occlusion))
		{
			bool bRead = RecogniseOrmGroup(Roots, Context, ReadLayered, MetallicFactor, OcclusionStrength);
			if (!bRead)
			{
				OrmA = nullptr;
				OrmB = nullptr;
				bLayered = false;
				bRead = RecogniseOrmGroup(Roots, Context, ReadPlain, MetallicFactor, OcclusionStrength);
			}
			if (!bRead || (bLayered && !OnBaseUV(OrmLerp.B)))
			{
				// Flat roughness, metallic and occlusion need no orm texture at all.
				if (!(IsFlat(Roots.Roughness) && IsFlat(Roots.Metallic) && IsFlat(Roots.Occlusion)))
				{
					return false;
				}
				bLayered = bPlain = false;
			}
			if (bLayered)
			{
				Sources[2] = OrmLerp.A.Texture;
				Layers[2] = OrmLerp.B.Texture;
				Tiling[2][0] = OrmLerp.A.Tiling[0];
				Tiling[2][1] = OrmLerp.A.Tiling[1];
			}
			else if (bPlain)
			{
				Sources[2] = Layers[2] = OrmPlain.Texture;
				Tiling[2][0] = OrmPlain.Tiling[0];
				Tiling[2][1] = OrmPlain.Tiling[1];
			}
		}

		Out.bLayerBlend = true;
		Out.LayerChannel = FString::Chr(Base.Channel);
		for (int32 Slot = 0; Slot < 3; ++Slot)
		{
			Out.SourceTextures[Slot] = Sources[Slot];
			Out.LayerTextures[Slot] = Layers[Slot];
			Out.SourceTiling[Slot][0] = Tiling[Slot][0];
			Out.SourceTiling[Slot][1] = Tiling[Slot][1];
		}
		Out.OrmMetallicFactor = MetallicFactor;
		Out.OrmOcclusionStrength = OcclusionStrength;
		return true;
	}

	const TCHAR* ShadingModelName(EMaterialShadingModel Model)
	{
		switch (Model)
		{
		case MSM_ClearCoat: return TEXT("clear coat");
		case MSM_Hair: return TEXT("hair");
		case MSM_Cloth: return TEXT("cloth");
		case MSM_Eye: return TEXT("eye");
		case MSM_SingleLayerWater: return TEXT("single layer water");
		case MSM_ThinTranslucent: return TEXT("thin translucent");
		default: return TEXT("special");
		}
	}

	const TCHAR* BlendModeName(EBlendMode Mode)
	{
		switch (Mode)
		{
		case BLEND_Additive: return TEXT("additive");
		case BLEND_Modulate: return TEXT("modulate");
		case BLEND_AlphaComposite: return TEXT("alpha composite");
		case BLEND_AlphaHoldout: return TEXT("alpha holdout");
		default: return TEXT("coloured transmittance");
		}
	}

	const TCHAR* const LossRemedy = TEXT("It still publishes. Nothing to do unless the effect matters to how your place looks; if it does, bake the effect into the material's textures or pick another material.");

	const TCHAR* const CarPaintRemedy = TEXT("Nothing to do. Its Clear Coat and Clear Coat Roughness carry over. Without flake parameters it is a plain clear coat with no flakes; for metallic flakes, give it FlakeDensity, FlakeSize or FlakeTint parameters (Mantle's defaults fill in any it lacks).");

	/** The first of Names that Material (its instance chain, then the base material) has as a scalar parameter. */
	bool CarPaintNamedScalar(const UMaterialInterface* Material, std::initializer_list<const TCHAR*> Names, double& Out)
	{
		for (const TCHAR* Name : Names)
		{
			float Value = 0.0f;
			if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Name))), Value))
			{
				Out = Value;
				return true;
			}
		}
		return false;
	}

	bool CarPaintNamedVector(const UMaterialInterface* Material, std::initializer_list<const TCHAR*> Names, FLinearColor& Out)
	{
		for (const TCHAR* Name : Names)
		{
			FLinearColor Value = FLinearColor::White;
			if (Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(Name))), Value))
			{
				Out = Value;
				return true;
			}
		}
		return false;
	}

	/**
	 * What one of the base material's own inputs renders with when that is a single value: a
	 * Constant, a scalar parameter (as Material, an instance, sets it) or the input's own constant.
	 * False when a graph feeds it or it is unset.
	 */
	bool CarPaintInputScalar(const UMaterialInterface* Material, const FScalarMaterialInput& Input, double& Out)
	{
		if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Input.Expression))
		{
			Out = Constant->R;
			return true;
		}
		if (const UMaterialExpressionScalarParameter* Parameter = Cast<UMaterialExpressionScalarParameter>(Input.Expression))
		{
			float Value = Parameter->DefaultValue;
			float Set = 0.0f;
			if (Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(Parameter->ParameterName)), Set))
			{
				Value = Set;
			}
			Out = Value;
			return true;
		}
		if (!Input.Expression && Input.UseConstant)
		{
			Out = Input.Constant;
			return true;
		}
		return false;
	}
}

FMantlePlaceAssetLimits FMantlePlaceAssetLimits::FromContract()
{
	FMantlePlaceAssetLimits Limits;
	if (const FMantleAvatarContract* Contract = FMantleAvatarContract::Get())
	{
		Limits.MaxTriangles = Contract->PlaceMeshMaxTriangles;
		Limits.MaxVertices = Contract->PlaceMeshMaxVertices;
		Limits.MaxSections = Contract->PlaceMeshMaxSections;
		Limits.MaxLODs = Contract->PlaceMeshMaxLODs;
		Limits.LODMaxTriangles = Contract->PlaceMeshLODMaxTriangles;
		Limits.MaxCollisionPrimitives = Contract->PlaceMeshMaxCollisionPrimitives;
		Limits.MaxConvexHulls = Contract->PlaceMeshMaxConvexHulls;
		Limits.MaxHullVertices = Contract->PlaceMeshMaxHullVertices;
		Limits.MaxTotalHullVertices = Contract->PlaceMeshMaxTotalHullVertices;
		Limits.MaxTextureSize = Contract->MaxTextureSize;
		Limits.MaxUploadBytes = Contract->MaxUploadBytes;
		Limits.MaxTextureBytes = FMath::Min<int64>(Limits.MaxTextureBytes, Contract->MaxUploadBytes);
		Limits.MinSoundSampleRate = Contract->PlaceSoundMinSampleRate;
		Limits.MaxSoundSampleRate = Contract->PlaceSoundMaxSampleRate;
		Limits.MaxSoundSeconds = Contract->PlaceSoundMaxSeconds;
		Limits.MaxSoundSourceBytes = Contract->PlaceSoundMaxSourceBytes;
		Limits.MaxPlaceSoundDecodedBytes = Contract->PlaceSoundMaxDecodedBytes;
		Limits.MaxSkeletalTriangles = Contract->PlaceSkeletalMaxTriangles;
		Limits.MaxSkeletalSections = Contract->PlaceSkeletalMaxSections;
		Limits.MaxSkeletalBones = Contract->PlaceSkeletalMaxBones;
		Limits.MaxClipSeconds = Contract->PlaceClipMaxSeconds;
		Limits.MaxClipFrameRate = Contract->PlaceClipMaxFrameRate;
		Limits.MaxClipCookedBytes = Contract->PlaceClipMaxCookedBytes;
	}
	return Limits;
}

void FMantlePlaceDependencies::AddScatter(const AActor* Actor)
{
	const AMantleScatterVolume* Volume = Cast<AMantleScatterVolume>(Actor);
	if (!Volume)
	{
		return;
	}
	const FString Label = Actor->GetActorLabel();
	for (const FMantleScatterMeshSettings& Entry : Volume->Meshes)
	{
		const UStaticMesh* Mesh = Entry.Mesh;
		if (!Mesh)
		{
			continue; // Check names an entry without a mesh
		}
		auto Override = [&Entry](int32 Slot) -> UMaterialInterface*
		{
			return Entry.Materials.IsValidIndex(Slot) ? Entry.Materials[Slot].Get() : nullptr;
		};
		if (FMantlePlaceAssets::IsLibraryAsset(Mesh))
		{
			// A library mesh brings its own materials; only overrides are published (and uploaded when not library).
			for (int32 Slot = 0; Slot < Entry.Materials.Num(); ++Slot)
			{
				AddMaterial(Override(Slot), Label);
			}
			continue;
		}

		// A project mesh uploads like a Static Mesh Actor's, with each slot's effective material.
		const FString Key = FMantlePlaceAssets::MeshKey(Mesh);
		FMantleGatheredMesh* Existing = Meshes.FindByPredicate([&Key](const FMantleGatheredMesh& Gathered) { return Gathered.Key == Key; });
		if (!Existing)
		{
			Existing = &Meshes.AddDefaulted_GetRef();
			Existing->Mesh = const_cast<UStaticMesh*>(Mesh);
			Existing->Key = Key;
			Existing->Name = Mesh->GetName();
		}
		Existing->UsedBy.AddUnique(Label);
		for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
		{
			UMaterialInterface* Material = Override(Slot);
			AddMaterial(Material ? Material : FMantlePlaceAssets::MeshSlotMaterial(Mesh, Slot), Label);
		}
	}
}

void FMantlePlaceDependencies::AddDecal(const AActor* Actor)
{
	FMantleDecalProps Props;
	UTexture* BaseColor = nullptr;
	UTexture* Normal = nullptr;
	FString Problem;
	if (!Actor || !FMantlePlaceExporter::ReadDecal(Actor, Props, BaseColor, Normal, Problem))
	{
		return; // Check names it and Export refuses it; there is nothing to convert
	}
	const FString Label = Actor->GetActorLabel();
	for (const bool bNormal : { false, true })
	{
		UTexture* Texture = bNormal ? Normal : BaseColor;
		if (!Texture)
		{
			continue;
		}
		const FString Key = FMantlePlaceAssets::DecalTextureKey(Texture, bNormal);
		FMantleGatheredTexture* Existing = Textures.FindByPredicate([&Key](const FMantleGatheredTexture& Entry) { return Entry.Key == Key; });
		if (!Existing)
		{
			Existing = &Textures.AddDefaulted_GetRef();
			Existing->Texture = Texture;
			Existing->Key = Key;
			Existing->Name = Texture->GetName();
			Existing->bNormal = bNormal;
		}
		Existing->UsedBy.AddUnique(Label);
	}
}

void FMantlePlaceDependencies::AddEffectTexture(UTexture* Texture, const FString& Label)
{
	if (!Texture)
	{
		return;
	}
	const FString Key = FMantlePlaceAssets::DecalTextureKey(Texture, /*bNormal*/ false);
	FMantleGatheredTexture* Existing = Textures.FindByPredicate([&Key](const FMantleGatheredTexture& Entry) { return Entry.Key == Key; });
	if (!Existing)
	{
		Existing = &Textures.AddDefaulted_GetRef();
		Existing->Texture = Texture;
		Existing->Key = Key;
		Existing->Name = Texture->GetName();
	}
	Existing->UsedBy.AddUnique(Label);
}

void FMantlePlaceDependencies::AddActor(const AStaticMeshActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	AddComponent(Actor->GetStaticMeshComponent(), Actor->GetActorLabel());
}

void FMantlePlaceDependencies::AddComponent(const UStaticMeshComponent* Component, const FString& Label, const UStaticMesh* MeshOverride)
{
	// A mirrored copy of the component's mesh (#628, FMantleMirrorBake) when it publishes with a negative scale.
	const UStaticMesh* Mesh = MeshOverride ? MeshOverride : (Component ? Component->GetStaticMesh() : nullptr);
	if (!Mesh)
	{
		return;
	}

	if (FMantlePlaceAssets::IsLibraryAsset(Mesh))
	{
		// A library mesh brings its own materials; only overrides are published (and uploaded when not library).
		for (UMaterialInterface* Override : Component->OverrideMaterials)
		{
			AddMaterial(Override, Label);
		}
		return;
	}

	const FString Key = FMantlePlaceAssets::MeshKey(Mesh);
	FMantleGatheredMesh* Existing = Meshes.FindByPredicate([&Key](const FMantleGatheredMesh& Entry) { return Entry.Key == Key; });
	if (!Existing)
	{
		Existing = &Meshes.AddDefaulted_GetRef();
		Existing->Mesh = const_cast<UStaticMesh*>(Mesh);
		Existing->Key = Key;
		Existing->Name = Mesh->GetName();
	}
	Existing->UsedBy.AddUnique(Label);

	for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
	{
		AddMaterial(FMantlePlaceAssets::EffectiveMaterial(Component, Slot), Label);
	}
}

void FMantlePlaceDependencies::AddMaterial(UMaterialInterface* Material, const FString& Label)
{
	if (!Material || FMantlePlaceAssets::IsLibraryAsset(Material))
	{
		return;
	}
	const FString Key = FMantlePlaceAssets::MaterialKey(Material);
	FMantleGatheredMaterial* Existing = Materials.FindByPredicate([&Key](const FMantleGatheredMaterial& Entry) { return Entry.Key == Key; });
	if (!Existing)
	{
		Existing = &Materials.AddDefaulted_GetRef();
		Existing->Material = Material;
		Existing->Key = Key;
		Existing->Name = Material->GetName();
	}
	Existing->UsedBy.AddUnique(Label);
}

void FMantlePlaceDependencies::AddMesh(const UStaticMesh* Mesh, const FString& Label)
{
	if (!Mesh || FMantlePlaceAssets::IsLibraryAsset(Mesh))
	{
		return;
	}
	const FString Key = FMantlePlaceAssets::MeshKey(Mesh);
	FMantleGatheredMesh* Existing = Meshes.FindByPredicate([&Key](const FMantleGatheredMesh& Entry) { return Entry.Key == Key; });
	if (!Existing)
	{
		Existing = &Meshes.AddDefaulted_GetRef();
		Existing->Mesh = const_cast<UStaticMesh*>(Mesh);
		Existing->Key = Key;
		Existing->Name = Mesh->GetName();
	}
	Existing->UsedBy.AddUnique(Label);

	// No component overrides anything, so each slot uploads the mesh's own material, which a script
	// can then put back on what it spawns.
	for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Mesh))
	{
		AddMaterial(FMantlePlaceAssets::MeshSlotMaterial(Mesh, Slot), Label);
	}
}

void FMantlePlaceDependencies::AddScriptAssets(UWorld* World)
{
	const FString Label = FMantlePlaceAssets::ScriptAssetsLabel;
	for (const FMantleScriptAsset& Asset : FMantlePlaceAssets::ScriptAssets(World))
	{
		if (const UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.Object))
		{
			AddMesh(Mesh, Label);
		}
		else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset.Object))
		{
			AddMaterial(Material, Label);
		}
		else if (const USoundBase* Sound = Cast<USoundBase>(Asset.Object))
		{
			AddSound(Sound, Label);
		}
	}
}

FMantlePlaceDependencies FMantlePlaceDependencies::Gather(UWorld* World)
{
	FMantlePlaceDependencies Out;
	if (!World)
	{
		return Out;
	}
	// The exporter's own filter (FMantlePlaceExporter::Export pass 1), so what is converted is what is published.
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const AActor* Actor = *It;
		if (!IsValid(Actor) || FMantlePlaceExporter::IsEngineInfrastructure(Actor) || FMantlePlaceExporter::IsProvidedByMantle(Actor))
		{
			continue;
		}
		EMantleEntityType Type;
		if (!FMantlePlaceExporter::ClassifyActor(Actor, Type))
		{
			continue;
		}
		if (Type == EMantleEntityType::StaticMesh)
		{
			// A negative scale publishes as the mirrored copy of the mesh (#628): the one Export writes, or nothing when it can't be made.
			const FMantleMirrorResult Mirror = FMantleActorDecompose::MirrorForActor(Actor);
			if (!Mirror.bNeeded)
			{
				Out.AddActor(Cast<AStaticMeshActor>(Actor));
			}
			else if (Mirror.bOk)
			{
				Out.AddComponent(Cast<AStaticMeshActor>(Actor)->GetStaticMeshComponent(), Actor->GetActorLabel(), Mirror.Mesh);
			}
		}
		else if (Type == EMantleEntityType::WaterBody)
		{
			// GhzGarage/mantle#997: a custom body's mesh is uploaded.
			FMantleWaterBodyData Body;
			if (FMantleWaterBodyExport::ReadBody(Actor, Body) && Body.Mesh.IsValid())
			{
				Out.AddMesh(Body.Mesh.Get(), Actor->GetActorLabel());
			}
		}
		else if (Type == EMantleEntityType::SoundEmitter)
		{
			Out.AddSoundActor(Actor);
		}
		else if (Type == EMantleEntityType::SkeletalMesh)
		{
			Out.AddSkeletalActor(Actor);
		}
		else if (Type == EMantleEntityType::Group)
		{
			// A Blueprint prefab's mesh and sound parts (#468), which Export writes as StaticMesh
			// and SoundEmitter entities.
			const FString Label = Actor->GetActorLabel();
			TArray<FMantlePrefabPart> Parts;
			FMantlePlaceExporter::CollectPrefabParts(Actor, Parts);
			for (const FMantlePrefabPart& Part : Parts)
			{
				if (Part.Type == EMantleEntityType::StaticMesh)
				{
					Out.AddComponent(Cast<UStaticMeshComponent>(Part.Component), Label, Part.MirroredMesh);
				}
				else if (Part.Type == EMantleEntityType::SkeletalMesh)
				{
					Out.AddSkeletalComponent(Cast<USkeletalMeshComponent>(Part.Component), Label);
				}
				else if (Part.Type == EMantleEntityType::SoundEmitter)
				{
					Out.AddSoundComponent(Cast<UAudioComponent>(Part.Component), Label);
				}
				else if (Part.Type == EMantleEntityType::Effect && Part.EffectProperties.IsValid())
				{
					// #628: a translated particle emitter's texture, named by object path (FMantleEffectExport::Translate).
					FString TexturePath;
					if (Part.EffectProperties->TryGetStringField(TEXT("texture"), TexturePath))
					{
						Out.AddEffectTexture(FMantleEffectExport::TextureForPath(TexturePath), Label);
					}
				}
			}
		}
		else if (Type == EMantleEntityType::Effect)
		{
			// #628: a Mantle Effect actor's sprite texture.
			if (const AMantleEffect* Effect = Cast<AMantleEffect>(Actor); Effect && Effect->Archetype == EMantleEffectArchetype::Sprites)
			{
				Out.AddEffectTexture(Effect->SpritesTexture.Get(), Actor->GetActorLabel());
			}
		}
		else if (Type == EMantleEntityType::Decal)
		{
			Out.AddDecal(Actor);
		}
		else if (Type == EMantleEntityType::VehicleSpawn)
		{
			Out.AddVehicleSpawn(Actor);
		}
		else if (Type == EMantleEntityType::WeaponSpawn)
		{
			Out.AddWeaponSpawn(Actor);
		}
		else if (Type == EMantleEntityType::Scatter)
		{
			Out.AddScatter(Actor);
		}
	}
	// Instanced meshes after the actors, as Export writes them (#467): the same sources, on any actor.
	for (const UInstancedStaticMeshComponent* Component : FMantlePlaceExporter::CollectInstanceSources(World))
	{
		const AActor* Owner = Component->GetOwner();
		Out.AddComponent(Component, Owner->GetActorLabel());
	}
	// Spline meshes baked into static meshes, where Export writes them (#628): they upload like any project mesh.
	Out.AddSplineMeshes(World);
	// The landscape, where Export writes it (#466). Only the first one publishes; Check names the rest.
	const TArray<AActor*> Landscapes = FMantleTerrainExport::CollectLandscapes(World);
	if (Landscapes.Num() > 0)
	{
		Out.AddTerrain(Landscapes[0]);
	}
	// The Script Assets last, as Export lists them: each only once, however often it is also placed.
	Out.AddScriptAssets(World);
	return Out;
}

void FMantlePlaceDependencies::AddSplineMeshes(UWorld* World)
{
	// The bake is cached while its sources are unchanged, so Export finds these same meshes (and keys).
	const FMantleSplineBakeResult Bake = FMantleSplineMeshBake::Bake(World, FMantlePlaceAssetLimits::FromContract());
	for (const FMantleBakedSplineMesh& Baked : Bake.Meshes)
	{
		AddMesh(Baked.Mesh, Baked.OwnerLabel);
	}
}

void FMantlePlaceDependencies::AddTerrain(const AActor* Landscape)
{
	if (!Landscape)
	{
		return;
	}
	const FString Key = FMantleTerrainExport::HeightfieldKey(Landscape);
	if (Terrains.ContainsByPredicate([&Key](const FMantleGatheredTerrain& Terrain) { return Terrain.Key == Key; }))
	{
		return;
	}
	const FString Label = Landscape->GetActorLabel();
	FMantleGatheredTerrain& Terrain = Terrains.AddDefaulted_GetRef();
	Terrain.Landscape = const_cast<AActor*>(Landscape);
	Terrain.Key = Key;
	Terrain.Name = Label;

	// Its grass (#628): the meshes and material overrides Export names, uploaded like a Static Mesh Actor's.
	FMantleTerrainGrassExport::AddAssets(Landscape, *this, Label);

	// The layers drawn from the landscape's own material (#628): the transient material each one is made into, which Export
	// and the converter find again by the same name. Only the layers that are kept, so a folded one uploads nothing.
	{
		FMantleTerrainSource Source;
		FString Problem;
		TArray<FMantleTerrainLayerChoice> Chosen;
		TArray<FMantleValidationIssue> Ignored;
		if (FMantleTerrainExport::ReadLandscape(Landscape, Source, Problem)
			&& FMantleTerrainExport::ChooseLayers(Landscape->GetWorld(), Source, Label, Chosen, Ignored))
		{
			for (const FMantleTerrainLayerChoice& Choice : Chosen)
			{
				if (!Choice.bDerived || !Choice.Material)
				{
					continue;
				}
				const FString DerivedKey = FMantlePlaceAssets::MaterialKey(Choice.Material);
				FMantleGatheredMaterial* Derived = Materials.FindByPredicate([&DerivedKey](const FMantleGatheredMaterial& Entry) { return Entry.Key == DerivedKey; });
				if (!Derived)
				{
					Derived = &Materials.AddDefaulted_GetRef();
					Derived->Material = Choice.Material;
					Derived->Key = DerivedKey;
					Derived->Name = Choice.Material->GetName();
				}
				Derived->UsedBy.AddUnique(Label);
			}
		}
	}

	// Every entry's material, whether or not its layer turns out to be painted: which layers carry
	// paint is only known from the landscape's weights, which the converter reads.
	const AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(Landscape->GetWorld());
	if (!Settings)
	{
		return;
	}
	for (const FMantleTerrainLayerSetting& Layer : Settings->TerrainLayers)
	{
		AddMaterial(Layer.Material.Get(), Label);
	}
}

void FMantlePlaceDependencies::AddWeaponSpawn(const AActor* Actor)
{
	const AMantleWeaponSpawnPoint* Spawn = Cast<AMantleWeaponSpawnPoint>(Actor);
	if (!Spawn || !Spawn->StorageItem.IsEmpty() || !AMantleWeaponSpawnPoint::IsBuilderName(Spawn->Weapon))
	{
		return; // a spawn with an uploaded WeaponId or a stored weapon uploads nothing
	}
	const FString Label = Actor->GetActorLabel();
	AddBuiltWeapon(Spawn->Weapon, Label); // when it cannot be added, Check (FMantlePlaceValidator) names it
}

bool FMantlePlaceDependencies::AddBuiltWeapon(const FString& WeaponName, const FString& Label)
{
	if (!AMantleWeaponSpawnPoint::IsBuilderName(WeaponName))
	{
		return false;
	}
	const FString Key = FMantlePlaceAssets::WeaponKey(WeaponName);
	if (FMantleGatheredWeapon* Existing = Weapons.FindByPredicate([&Key](const FMantleGatheredWeapon& Entry) { return Entry.Key == Key; }))
	{
		Existing->UsedBy.AddUnique(Label);
		return true;
	}
	// Every weapon has its 3D model (#897: a fist weapon's too) beside its settings file.
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *AMantleWeaponSpawnPoint::BuiltMeshObjectPath(WeaponName), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Mesh || !FPaths::FileExists(AMantleWeaponSpawnPoint::BuiltDocumentFile(WeaponName)))
	{
		return false;
	}
	// #898: its extra parts' models and its attack sounds, from its folder. A file that names one that is not there gathers what
	// it can; Check and the converter (WeaponDocumentProblem) name the problem.
	FString Text;
	FMantleBuiltWeaponParts Parts;
	if (FFileHelper::LoadFileToString(Text, *AMantleWeaponSpawnPoint::BuiltDocumentFile(WeaponName)))
	{
		FMantlePlaceAssets::WeaponDocumentProblem(WeaponName, Text, &Parts);
	}
	FMantleGatheredWeapon& Entry = Weapons.AddDefaulted_GetRef();
	Entry.Key = Key;
	Entry.Name = WeaponName;
	Entry.Mesh = Mesh;
	Entry.UsedBy.Add(Label);
	// #892: a weapon names its slots' materials by uploaded id only (the weapon cook refuses a library id such as ast_mat_basic),
	// so a slot on a library material, or on none (Unreal's default surface), uploads that material as a project material does.
	// The extra parts' slots too (#898).
	auto AddWeaponMaterial = [this, &Label](UMaterialInterface* Material)
	{
		Material = Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
		const FString MaterialKey = FMantlePlaceAssets::MaterialKey(Material);
		FMantleGatheredMaterial* Existing = Materials.FindByPredicate([&MaterialKey](const FMantleGatheredMaterial& Gathered) { return Gathered.Key == MaterialKey; });
		if (!Existing)
		{
			Existing = &Materials.AddDefaulted_GetRef();
			Existing->Material = Material;
			Existing->Key = MaterialKey;
			Existing->Name = Material->GetName();
		}
		Existing->UsedBy.AddUnique(Label);
	};
	// Each model uploads as any skeletal mesh (or mesh) does, with the material of each of its slots, before the weapon.
	auto AddSkeletalModel = [this, &Label, &AddWeaponMaterial](USkeletalMesh* Model)
	{
		AddSkeletalMesh(Model, Label);
		const TArray<FSkeletalMaterial>& ModelMaterials = Model->GetMaterials();
		for (const int32 Slot : FMantlePlaceAssets::SkeletalUploadSlots(Model))
		{
			AddWeaponMaterial(ModelMaterials.IsValidIndex(Slot) ? ModelMaterials[Slot].MaterialInterface.Get() : nullptr);
		}
	};
	AddSkeletalModel(Mesh);
	// An extra part's model, or (#905) the round's: a skeletal mesh or a mesh, with every slot's material.
	auto AddPartModel = [this, &Label, &AddWeaponMaterial, &AddSkeletalModel](UObject* Model)
	{
		if (USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Model))
		{
			AddSkeletalModel(Skeletal);
		}
		else if (UStaticMesh* Static = Cast<UStaticMesh>(Model))
		{
			AddMesh(Static, Label);
			for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Static))
			{
				AddWeaponMaterial(FMantlePlaceAssets::MeshSlotMaterial(Static, Slot));
			}
		}
	};
	for (UObject* Model : Parts.Attachments)
	{
		AddPartModel(Model);
	}
	AddPartModel(Parts.Projectile);
	AddSound(Parts.Fire, Label);
	AddSound(Parts.FireSuppressed, Label);
	return true;
}

void FMantlePlaceDependencies::AddVehicleSpawn(const AActor* Actor)
{
	const AMantleVehicleSpawnPoint* Spawn = Cast<AMantleVehicleSpawnPoint>(Actor);
	UMantleVehicleSource* Source = Spawn ? Spawn->Vehicle.LoadSynchronous() : nullptr;
	if (!Source)
	{
		return; // Check names it; a spawn with an uploaded VehicleId uploads nothing
	}
	const FString Label = Actor->GetActorLabel();
	const FString Key = FMantleVehicleConverter::VehicleKey(Source);
	FMantleGatheredVehicle* Existing = Vehicles.FindByPredicate([&Key](const FMantleGatheredVehicle& Entry) { return Entry.Key == Key; });
	if (Existing)
	{
		Existing->UsedBy.AddUnique(Label);
		return;
	}
	Existing = &Vehicles.AddDefaulted_GetRef();
	Existing->Vehicle = Source;
	Existing->Key = Key;
	Existing->Name = Source->GetName();
	Existing->UsedBy.Add(Label);

	// Its liveries (#488): uploaded as they are, as a decal's base colour is (alpha kept), before the vehicle. A livery
	// that does not load gathers nothing: Check says so.
	for (const TSoftObjectPtr<UTexture2D>& Livery : Source->Paint.Liveries)
	{
		UTexture2D* Texture = Livery.LoadSynchronous();
		if (!Texture)
		{
			continue;
		}
		const FString TextureKey = FMantlePlaceAssets::DecalTextureKey(Texture, /*bNormal*/ false);
		FMantleGatheredTexture* Gathered = Textures.FindByPredicate([&TextureKey](const FMantleGatheredTexture& Entry) { return Entry.Key == TextureKey; });
		if (!Gathered)
		{
			Gathered = &Textures.AddDefaulted_GetRef();
			Gathered->Texture = Texture;
			Gathered->Key = TextureKey;
			Gathered->Name = Texture->GetName();
			Gathered->bLivery = true;
		}
		Gathered->UsedBy.AddUnique(Label);
	}

	// Its attached parts (#435 item 2): each mesh uploads as a mesh does, before the vehicle, with the material each of its upload
	// slots publishes with (library ones skipped: the part draws Mantle's default surface there). A part that does not load, or
	// names a library mesh, gathers nothing: Check says so.
	for (const FMantleVehicleSourcePart& Part : Source->Parts)
	{
		const UStaticMesh* PartMesh = Part.Mesh.LoadSynchronous();
		if (!PartMesh || FMantlePlaceAssets::IsLibraryAsset(PartMesh))
		{
			continue;
		}
		const FString MeshKey = FMantlePlaceAssets::MeshKey(PartMesh);
		FMantleGatheredMesh* GatheredMesh = Meshes.FindByPredicate([&MeshKey](const FMantleGatheredMesh& Entry) { return Entry.Key == MeshKey; });
		if (!GatheredMesh)
		{
			GatheredMesh = &Meshes.AddDefaulted_GetRef();
			GatheredMesh->Mesh = const_cast<UStaticMesh*>(PartMesh);
			GatheredMesh->Key = MeshKey;
			GatheredMesh->Name = PartMesh->GetName();
		}
		GatheredMesh->UsedBy.AddUnique(Label);
		for (const int32 Slot : FMantlePlaceAssets::UploadSlots(PartMesh))
		{
			AddMaterial(FMantleVehicleConverter::PartSlotMaterial(Part, PartMesh, Slot), Label);
		}
	}

	// Its horn's sound (#546): uploaded as a SoundEmitter's is, before the vehicle, and counted in the place's sound budget. A
	// library sound, a cue or a MetaSound gathers nothing: Check says why (FMantleVehicleConverter::CheckSource).
	AddSound(Source->Tuning.HornSound.LoadSynchronous(), Label);

	// The materials its body (and any tire mesh) draws with, as the upload reads them. A body that cannot be
	// read gathers none: Check says why (FMantleVehicleConverter::CheckSource).
	FMantleGlbMesh Body;
	TArray<UMaterialInterface*> BodyMaterials;
	FString Error;
	if (!FMantleVehicleConverter::ReadBody(*Source, Body, BodyMaterials, Error))
	{
		return;
	}
	for (UMaterialInterface* Material : BodyMaterials)
	{
		AddMaterial(Material, Label);
	}
}

void FMantlePlaceDependencies::AddSoundActor(const AActor* Actor)
{
	const AAmbientSound* Ambient = Cast<AAmbientSound>(Actor);
	if (!Ambient)
	{
		return;
	}
	AddSoundComponent(Ambient->GetAudioComponent(), Actor->GetActorLabel());
}

void FMantlePlaceDependencies::AddSoundComponent(const UAudioComponent* Audio, const FString& Label)
{
	AddSound(Audio ? Audio->Sound.Get() : nullptr, Label);
}

void FMantlePlaceDependencies::AddSound(const USoundBase* Sound, const FString& Label)
{
	// A Sound Cue uploads as the sound wave it publishes as (#818).
	USoundWave* Wave = const_cast<USoundWave*>(Cast<USoundWave>(FMantlePlaceAssets::PublishedSound(Sound)));
	// MetaSounds (a procedural wave) and generated waves are not uploads: Check says why.
	if (!Wave || Wave->IsA<USoundWaveProcedural>() || FMantlePlaceAssets::IsLibraryAsset(Wave))
	{
		return;
	}
	const FString Key = FMantlePlaceAssets::SoundKey(Wave);
	FMantleGatheredSound* Existing = Sounds.FindByPredicate([&Key](const FMantleGatheredSound& Entry) { return Entry.Key == Key; });
	if (!Existing)
	{
		Existing = &Sounds.AddDefaulted_GetRef();
		Existing->Sound = Wave;
		Existing->Key = Key;
		Existing->Name = Wave->GetName();
	}
	Existing->UsedBy.AddUnique(Label);
}

void FMantlePlaceDependencies::AddSkeletalActor(const AActor* Actor)
{
	const ASkeletalMeshActor* SkeletalActor = Cast<ASkeletalMeshActor>(Actor);
	AddSkeletalComponent(SkeletalActor ? SkeletalActor->GetSkeletalMeshComponent() : nullptr, Actor->GetActorLabel());
}

void FMantlePlaceDependencies::AddSkeletalComponent(const USkeletalMeshComponent* Component, const FString& Label)
{
	FMantleSkeletalPlayback Playback;
	FString Why;
	FString Fix;
	if (!FMantlePlaceAssets::ReadSkeletalComponent(Component, Playback, Why, Fix))
	{
		return; // Check and Export say why.
	}

	const FString MeshKey = FMantlePlaceAssets::SkeletalMeshKey(Playback.Mesh);
	FMantleGatheredSkeletalMesh* Mesh = SkeletalMeshes.FindByPredicate([&MeshKey](const FMantleGatheredSkeletalMesh& Entry) { return Entry.Key == MeshKey; });
	if (!Mesh)
	{
		Mesh = &SkeletalMeshes.AddDefaulted_GetRef();
		Mesh->Mesh = const_cast<USkeletalMesh*>(Playback.Mesh);
		Mesh->Key = MeshKey;
		Mesh->Name = Playback.Mesh->GetName();
	}
	Mesh->UsedBy.AddUnique(Label);

	// Every slot's effective material: an uploaded skeletal mesh carries only its slots' names.
	for (const int32 Slot : FMantlePlaceAssets::SkeletalUploadSlots(Playback.Mesh))
	{
		AddMaterial(FMantlePlaceAssets::EffectiveSkeletalMaterial(Component, Slot), Label);
	}

	if (Playback.Sequence)
	{
		const FString ClipKey = FMantlePlaceAssets::ClipKey(Playback.Sequence, Playback.Mesh);
		FMantleGatheredClip* Clip = Clips.FindByPredicate([&ClipKey](const FMantleGatheredClip& Entry) { return Entry.Key == ClipKey; });
		if (!Clip)
		{
			Clip = &Clips.AddDefaulted_GetRef();
			Clip->Sequence = const_cast<UAnimSequence*>(Playback.Sequence);
			Clip->Mesh = const_cast<USkeletalMesh*>(Playback.Mesh);
			Clip->Key = ClipKey;
			Clip->Name = Playback.Sequence->GetName();
		}
		Clip->UsedBy.AddUnique(Label);
	}
}

void FMantlePlaceDependencies::AddSkeletalMesh(const USkeletalMesh* Mesh, const FString& Label)
{
	if (!Mesh)
	{
		return;
	}
	const FString MeshKey = FMantlePlaceAssets::SkeletalMeshKey(Mesh);
	FMantleGatheredSkeletalMesh* Entry = SkeletalMeshes.FindByPredicate([&MeshKey](const FMantleGatheredSkeletalMesh& Existing) { return Existing.Key == MeshKey; });
	if (!Entry)
	{
		Entry = &SkeletalMeshes.AddDefaulted_GetRef();
		Entry->Mesh = const_cast<USkeletalMesh*>(Mesh);
		Entry->Key = MeshKey;
		Entry->Name = Mesh->GetName();
	}
	Entry->UsedBy.AddUnique(Label);

	// No component overrides anything, so each slot uploads the mesh's own material.
	const TArray<FSkeletalMaterial>& MeshMaterials = Mesh->GetMaterials();
	for (const int32 Slot : FMantlePlaceAssets::SkeletalUploadSlots(Mesh))
	{
		UMaterialInterface* Material = MeshMaterials.IsValidIndex(Slot) ? MeshMaterials[Slot].MaterialInterface.Get() : nullptr;
		AddMaterial(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface), Label);
	}
}

FString FMantlePlaceAssets::MeshKey(const UStaticMesh* Mesh)
{
	return Mesh ? TEXT("mesh:") + Mesh->GetPathName() : FString();
}

FName FMantlePlaceAssets::UniqueBakedMeshName(UObject* Outer, const FString& IntendedName)
{
	FString Base = IntendedName;
	if (Base.Len() > 0 && FChar::IsDigit(Base[Base.Len() - 1]))
	{
		Base += TEXT("_");
	}
	return MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(*Base));
}

FString FMantlePlaceAssets::PublishedMeshName(const UStaticMesh& Mesh)
{
	if (Mesh.GetOutermost() != GetTransientPackage())
	{
		return Mesh.GetName();
	}
	// Without the engine's number; the underscore UniqueBakedMeshName added after a trailing digit goes too (no intended name ends in one).
	FString Name = Mesh.GetFName().GetPlainNameString();
	Name.RemoveFromEnd(TEXT("_"));
	return Name;
}

FString FMantlePlaceAssets::SkeletalMeshKey(const USkeletalMesh* Mesh)
{
	return Mesh ? TEXT("skeletalmesh:") + Mesh->GetPathName() : FString();
}

FString FMantlePlaceAssets::WeaponKey(const FString& WeaponName)
{
	return TEXT("weapon:/Game/MantleWeapons/") + WeaponName;
}

FString FMantlePlaceAssets::WeaponDocumentProblem(const FString& WeaponName, const FString& DocumentText, FMantleBuiltWeaponParts* OutParts)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DocumentText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return TEXT("has a settings file that cannot be read");
	}
	// #898: the builder copies each extra part's model and each attack sound into the weapon's folder, under the local name it writes.
	auto Load = [&WeaponName](const FString& LocalName) -> UObject*
	{
		bool bPlainName = !LocalName.IsEmpty();
		for (const TCHAR Char : LocalName)
		{
			bPlainName = bPlainName && (FChar::IsAlnum(Char) || Char == TEXT('_'));
		}
		return bPlainName ? LoadObject<UObject>(nullptr, *FString::Printf(TEXT("/Game/MantleWeapons/%s/%s.%s"), *WeaponName, *LocalName, *LocalName),
			nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
	};
	FMantleBuiltWeaponParts Parts;
	const TArray<TSharedPtr<FJsonValue>>* Attachments = nullptr;
	if (Root->TryGetArrayField(TEXT("attachments"), Attachments) && Attachments)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Attachments)
		{
			const TSharedPtr<FJsonObject>* Attachment = nullptr;
			FString LocalName;
			FString Effect;
			if (Value.IsValid() && Value->TryGetObject(Attachment) && Attachment)
			{
				(*Attachment)->TryGetStringField(TEXT("mesh"), LocalName);
				(*Attachment)->TryGetStringField(TEXT("effect"), Effect);
			}
			// #904: an effect-only attachment is a built-in effect: no model to upload (null keeps the parts in the document's order).
			if (LocalName.IsEmpty() && !Effect.IsEmpty())
			{
				Parts.Attachments.Add(nullptr);
				continue;
			}
			UObject* Model = Load(LocalName);
			if (!Cast<UStaticMesh>(Model) && !Cast<USkeletalMesh>(Model))
			{
				return FString::Printf(TEXT("has an extra part whose model '%s' is missing from its folder"), *LocalName);
			}
			Parts.Attachments.Add(Model);
		}
	}
	// #905: the model a launcher's round or an arrow flies as, copied beside the weapon as an extra part's is.
	const TSharedPtr<FJsonObject>* Projectile = nullptr;
	FString ProjectileName;
	if (Root->TryGetObjectField(TEXT("projectile"), Projectile) && Projectile && (*Projectile)->TryGetStringField(TEXT("mesh"), ProjectileName))
	{
		UObject* Model = Load(ProjectileName);
		if (!Cast<UStaticMesh>(Model) && !Cast<USkeletalMesh>(Model))
		{
			return FString::Printf(TEXT("has a round or arrow whose model '%s' is missing from its folder"), *ProjectileName);
		}
		Parts.Projectile = Model;
	}
	const TSharedPtr<FJsonObject>* Sounds = nullptr;
	if (Root->TryGetObjectField(TEXT("sounds"), Sounds) && Sounds)
	{
		for (USoundWave** Into : { &Parts.Fire, &Parts.FireSuppressed })
		{
			FString LocalName;
			if (!(*Sounds)->TryGetStringField(Into == &Parts.Fire ? TEXT("fire") : TEXT("fireSuppressed"), LocalName))
			{
				continue;
			}
			USoundWave* Wave = Cast<USoundWave>(Load(LocalName));
			if (!Wave)
			{
				return FString::Printf(TEXT("has an attack sound '%s' that is missing from its folder"), *LocalName);
			}
			FString Why;
			FString Fix;
			if (!CheckSoundRule(Wave, FMantlePlaceAssetLimits::FromContract(), Why, Fix))
			{
				return FString::Printf(TEXT("has an attack sound '%s', which %s"), *Wave->GetName(), *Why);
			}
			*Into = Wave;
		}
	}
	if (OutParts)
	{
		*OutParts = MoveTemp(Parts);
	}
	return FString();
}

FString FMantlePlaceAssets::ClipKey(const UAnimSequence* Sequence, const USkeletalMesh* Mesh)
{
	return Sequence && Mesh ? FString::Printf(TEXT("animclip:%s@%s"), *Sequence->GetPathName(), *Mesh->GetPathName()) : FString();
}

bool FMantlePlaceAssets::ReadSkeletalComponent(const USkeletalMeshComponent* Component, FMantleSkeletalPlayback& Out, FString& OutWhy, FString& OutFix)
{
	Out = FMantleSkeletalPlayback();
	const USkeletalMesh* Mesh = Component ? Component->GetSkeletalMeshAsset() : nullptr;
	if (!Mesh)
	{
		OutWhy = TEXT("has no skeletal mesh.");
		OutFix = TEXT("Set its Skeletal Mesh Asset, or delete it.");
		return false;
	}
	Out.Mesh = Mesh;

	switch (Component->GetAnimationMode())
	{
	case EAnimationMode::AnimationBlueprint:
		// Without an Anim Class it plays nothing: the reference pose, which publishes as such.
		if (Component->AnimClass)
		{
			OutWhy = FString::Printf(TEXT("plays the Animation Blueprint '%s', which Mantle cannot publish: a place's skeletal mesh plays one animation clip."),
				*Component->AnimClass->GetName());
			OutFix = TEXT("Set its Animation Mode to Use Animation Asset and pick one Animation Sequence as Anim to Play, or clear its Anim Class to publish it holding its reference pose.");
			return false;
		}
		return true;

	case EAnimationMode::AnimationSingleNode:
		break;

	default:
		OutWhy = TEXT("uses a custom animation mode, which Mantle cannot publish.");
		OutFix = TEXT("Set its Animation Mode to Use Animation Asset and pick one Animation Sequence as Anim to Play.");
		return false;
	}

	const FSingleAnimationPlayData& Data = Component->AnimationData;
	const UAnimationAsset* Asset = Data.AnimToPlay;
	if (!Asset)
	{
		return true;
	}
	const UAnimSequence* Sequence = Cast<UAnimSequence>(Asset);
	if (!Sequence)
	{
		OutWhy = FString::Printf(TEXT("plays '%s', a %s; Mantle publishes Animation Sequences only."), *Asset->GetName(), *Asset->GetClass()->GetName());
		OutFix = TEXT("Pick an Animation Sequence as its Anim to Play (a montage or blend space can be baked to one with Create Asset > Create Animation).");
		return false;
	}
	const USkeleton* MeshSkeleton = Mesh->GetSkeleton();
	const USkeleton* SequenceSkeleton = Sequence->GetSkeleton();
	if (SequenceSkeleton != MeshSkeleton && !(MeshSkeleton && SequenceSkeleton && MeshSkeleton->IsCompatibleForEditor(SequenceSkeleton)))
	{
		OutWhy = FString::Printf(TEXT("plays '%s', which was made for the skeleton '%s', not its mesh's ('%s')."), *Sequence->GetName(),
			SequenceSkeleton ? *SequenceSkeleton->GetName() : TEXT("none"), MeshSkeleton ? *MeshSkeleton->GetName() : TEXT("none"));
		OutFix = TEXT("Pick a sequence made for the mesh's skeleton, or retarget this one onto it.");
		return false;
	}
	// Not playing holds the pose at its initial position: a play rate of 0.
	const double Rate = Data.bSavedPlaying ? static_cast<double>(Data.SavedPlayRate) : 0.0;
	if (!(Rate >= 0.0) || Rate > FMantlePlaceLimits::MaxClipPlayRate)
	{
		OutWhy = FString::Printf(TEXT("plays '%s' at a rate of %g; a place plays clips forwards, at 0 to %g times their speed."),
			*Sequence->GetName(), Rate, FMantlePlaceLimits::MaxClipPlayRate);
		OutFix = FString::Printf(TEXT("Set its PlayRate (under Animation, advanced) between 0 and %g."), FMantlePlaceLimits::MaxClipPlayRate);
		return false;
	}
	const double Position = static_cast<double>(Data.SavedPosition);
	if (!(Position >= 0.0) || Position > FMantlePlaceLimits::MaxClipStartOffsetSeconds)
	{
		OutWhy = FString::Printf(TEXT("starts '%s' at %g s; a place starts clips 0 to %g s in."), *Sequence->GetName(), Position,
			FMantlePlaceLimits::MaxClipStartOffsetSeconds);
		OutFix = TEXT("Set its Initial Position within the clip.");
		return false;
	}
	Out.Sequence = Sequence;
	Out.bLoop = Data.bSavedLooping;
	Out.PlayRate = Rate;
	Out.StartOffset = Position;
	return true;
}

TArray<int32> FMantlePlaceAssets::SkeletalUploadSlots(const USkeletalMesh* Mesh)
{
	return FMantleCosmeticExport::UploadMaterialSlots(Mesh, 0);
}

UMaterialInterface* FMantlePlaceAssets::EffectiveSkeletalMaterial(const USkeletalMeshComponent* Component, int32 Slot)
{
	UMaterialInterface* Material = Component ? Component->GetMaterial(Slot) : nullptr;
	return Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
}

void FMantlePlaceAssets::CheckSkeletalMesh(const USkeletalMesh* Mesh, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits,
	TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantlePlaceAssetsPrivate;
	if (!Mesh)
	{
		return;
	}
	const FString Subject = WithUsers(TEXT("Skeletal mesh ") + Quoted(Mesh), UsedBy);
	if (!Mesh->GetResourceForRendering() || !Mesh->GetResourceForRendering()->LODRenderData.IsValidIndex(0))
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			Subject + TEXT(" has no render data, so it cannot be converted."),
			TEXT("Open the mesh in the editor once (or re-import it), then try again.") });
		return;
	}
	const int32 Bones = Mesh->GetRefSkeleton().GetRawBoneNum();
	if (Bones > Limits.MaxSkeletalBones)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s has %d bones; a place skeletal mesh may have at most %d."), *Subject, Bones, Limits.MaxSkeletalBones),
			TEXT("Use a simpler rig, or remove the bones it does not need (Skeleton Tree > Remove Bone from LOD is not enough: the bones must leave the mesh).") });
	}
	const int32 Triangles = FMantleCosmeticExport::CountTriangles(Mesh, 0);
	if (Triangles > Limits.MaxSkeletalTriangles)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s has %s triangles in LOD 0; a place skeletal mesh may have at most %s."),
				*Subject, *Thousands(Triangles), *Thousands(Limits.MaxSkeletalTriangles)),
			TEXT("Use a lower-detail version of it (the skeletal mesh editor's LOD reduction can make one).") });
	}
	const int32 Slots = SkeletalUploadSlots(Mesh).Num();
	if (Slots > Limits.MaxSkeletalSections)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s has %d material slots; a place skeletal mesh may have at most %d."), *Subject, Slots, Limits.MaxSkeletalSections),
			FString::Printf(TEXT("Give it %d materials or fewer."), Limits.MaxSkeletalSections) });
	}
}

void FMantlePlaceAssets::CheckClip(const UAnimSequence* Sequence, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits,
	TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantlePlaceAssetsPrivate;
	if (!Sequence)
	{
		return;
	}
	const double Length = Sequence->GetPlayLength();
	if (Length > Limits.MaxClipSeconds + 1.0e-6)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s is %.2f s long; a place animation clip may be at most %g s."),
				*WithUsers(TEXT("Animation ") + Quoted(Sequence), UsedBy), Length, Limits.MaxClipSeconds),
			TEXT("Trim the animation (Animation editor > Remove frames), or split it.") });
	}
}

FString FMantlePlaceAssets::SoundKey(const USoundBase* Sound)
{
	return Sound ? TEXT("sound:") + Sound->GetPathName() : FString();
}

bool FMantlePlaceAssets::CheckSoundRule(const USoundBase* Sound, const FMantlePlaceAssetLimits& Limits, FString& OutWhy, FString& OutFix)
{
	OutWhy.Reset();
	OutFix.Reset();
	if (!Sound)
	{
		OutWhy = TEXT("is missing");
		OutFix = TEXT("Assign a sound wave imported from a WAV file.");
		return false;
	}
	// A MetaSound Source is a procedural wave, so it is named before the generated-wave rule. Named
	// by class, since MantleCore links no MetaSound module.
	const FString ClassName = Sound->GetClass()->GetName();
	if (ClassName.Contains(TEXT("MetaSound")))
	{
		OutWhy = TEXT("is a MetaSound, and Mantle publishes sound waves only (a MetaSound is a graph players' clients don't run)");
		OutFix = TEXT("Render it to a WAV file, import that, and assign it instead.");
		return false;
	}
	if (Sound->IsA<USoundCue>())
	{
		OutWhy = TEXT("is a Sound Cue, and Mantle publishes sound waves only (a cue is a graph players' clients don't run)");
		OutFix = TEXT("Assign the sound wave it plays instead.");
		return false;
	}
	const USoundWave* Wave = Cast<USoundWave>(Sound);
	if (!Wave)
	{
		OutWhy = FString::Printf(TEXT("is a %s, and Mantle publishes sound waves only"), *ClassName);
		OutFix = TEXT("Assign a sound wave imported from a WAV file instead.");
		return false;
	}
	if (Wave->IsA<USoundWaveProcedural>() || Wave->bProcedural)
	{
		OutWhy = TEXT("is generated while the game runs, and Mantle publishes imported sound waves only");
		OutFix = TEXT("Record it to a WAV file, import that, and assign it instead.");
		return false;
	}
	const int32 Channels = Wave->NumChannels;
	const int32 Rate = static_cast<int32>(Wave->GetImportedSampleRate());
	const double Seconds = static_cast<double>(Wave->Duration);
	if (Channels <= 0 || Rate <= 0 || Seconds <= 0.0)
	{
		OutWhy = TEXT("has no imported audio in this project to publish");
		OutFix = TEXT("Re-import it from its WAV file (Reimport in the Content Browser), then try again.");
		return false;
	}
	if (Channels > 2)
	{
		OutWhy = FString::Printf(TEXT("has %d channels, and Mantle plays mono or stereo sounds"), Channels);
		OutFix = TEXT("Export it from your audio editor as mono or stereo, re-import it, then try again.");
		return false;
	}
	if (Rate < Limits.MinSoundSampleRate || Rate > Limits.MaxSoundSampleRate)
	{
		OutWhy = FString::Printf(TEXT("is recorded at %s Hz, and Mantle plays sounds at %s to %s Hz"), *MantlePlaceAssetsPrivate::Thousands(Rate),
			*MantlePlaceAssetsPrivate::Thousands(Limits.MinSoundSampleRate), *MantlePlaceAssetsPrivate::Thousands(Limits.MaxSoundSampleRate));
		OutFix = TEXT("Export it at 44,100 or 48,000 Hz, re-import it, then try again.");
		return false;
	}
	if (Seconds > Limits.MaxSoundSeconds)
	{
		OutWhy = FString::Printf(TEXT("is %.1f seconds long, and a sound may be at most %g seconds"), Seconds, Limits.MaxSoundSeconds);
		OutFix = FString::Printf(TEXT("Trim it to %g seconds or less (a loop needs only one pass), re-import it, then try again."), Limits.MaxSoundSeconds);
		return false;
	}
	return true;
}

const USoundBase* FMantlePlaceAssets::PublishedSound(const USoundBase* Sound)
{
	const USoundCue* Cue = Cast<USoundCue>(Sound);
	if (!Cue)
	{
		return Sound;
	}
	TArray<const USoundNodeWavePlayer*> Players;
	Cue->RecursiveFindNode<USoundNodeWavePlayer>(Cue->FirstNode.Get(), Players);
	const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
	const USoundWave* Fallback = nullptr;
	for (const USoundNodeWavePlayer* Player : Players)
	{
		const USoundWave* Wave = Player ? Player->GetSoundWave() : nullptr;
		if (!Wave)
		{
			continue;
		}
		FString Why, Fix;
		if (IsLibraryAsset(Wave) || CheckSoundRule(Wave, Limits, Why, Fix))
		{
			return Wave;
		}
		Fallback = Fallback ? Fallback : Wave;
	}
	return Fallback;
}

int64 FMantlePlaceAssets::SoundDecodedBytes(const USoundWave* Sound)
{
	if (!Sound)
	{
		return 0;
	}
	const int64 Frames = FMath::CeilToInt64(static_cast<double>(Sound->Duration) * static_cast<double>(Sound->GetImportedSampleRate()));
	return FMath::Max<int64>(Frames, 0) * FMath::Max(Sound->NumChannels, 0) * 2;
}

void FMantlePlaceAssets::CheckSounds(const FMantlePlaceDependencies& Dependencies, const FMantlePlaceAssetLimits& Limits, TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantlePlaceAssetsPrivate;
	int64 Decoded = 0;
	for (const FMantleGatheredSound& Gathered : Dependencies.Sounds)
	{
		const USoundWave* Wave = Gathered.Sound.Get();
		if (!Wave)
		{
			continue;
		}
		Decoded += SoundDecodedBytes(Wave);
#if WITH_EDITORONLY_DATA
		if (Wave->Transformations.Num() > 0)
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
				FString::Printf(TEXT("%s has waveform edits made in Unreal (trims, fades or other processing), which aren't published: players hear it as it was imported."),
					*WithUsers(TEXT("Sound ") + Quoted(Wave), DescribeUsers(Gathered.UsedBy))),
				TEXT("It still publishes. If the edits matter, make them in the WAV file itself and re-import it.") });
		}
#endif
	}
	if (Decoded > Limits.MaxPlaceSoundDecodedBytes)
	{
		const double OneMegabyte = 1024.0 * 1024.0;
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("This place's %d sound(s) come to %.1f MB of audio once decoded; a place may use at most %.0f MB, which every player's client holds in memory."),
				Dependencies.Sounds.Num(), Decoded / OneMegabyte, Limits.MaxPlaceSoundDecodedBytes / OneMegabyte),
			TEXT("Use fewer or shorter sounds, or mono instead of stereo, or a lower sample rate (22,050 Hz halves a 44,100 Hz sound).") });
	}
}

FString FMantlePlaceAssets::MaterialKey(const UMaterialInterface* Material)
{
	return Material ? TEXT("material:") + Material->GetPathName() : FString();
}

FString FMantlePlaceAssets::DecalTextureKey(const UTexture* Texture, const bool bNormal)
{
	return Texture ? FString::Printf(TEXT("decaltexture:%s:%s"), bNormal ? TEXT("normal") : TEXT("color"), *Texture->GetPathName()) : FString();
}

FString FMantlePlaceAssets::ProvisionalId(const FString& Key)
{
	FMD5 Md5;
	const FTCHARToUTF8 Utf8(*Key);
	Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	uint8 Digest[16];
	Md5.Final(Digest);
	FString Id = TEXT("ast_u");
	for (const uint8 Byte : Digest)
	{
		Id += FString::Printf(TEXT("%02x"), Byte);
	}
	return Id;
}

bool FMantlePlaceAssets::IsLibraryAsset(const UObject* Object)
{
	return Object && !FMantleAssetLibrary::Get().FindIdByPath(FSoftObjectPath(Object)).IsEmpty();
}

namespace MantlePlaceAssetsPrivate
{
	/** The indices of the sections with triangles, by first vertex. */
	TArray<int32> SectionsInVertexOrder(const FStaticMeshLODResources& LOD)
	{
		TArray<int32> Order;
		for (int32 Section = 0; Section < LOD.Sections.Num(); ++Section)
		{
			if (LOD.Sections[Section].NumTriangles > 0)
			{
				Order.Add(Section);
			}
		}
		Order.Sort([&LOD](int32 A, int32 B) { return LOD.Sections[A].MinVertexIndex < LOD.Sections[B].MinVertexIndex; });
		return Order;
	}

	/** FMantleCosmeticExport::ReadStaticMesh's order for one LOD: the distinct slots of the sections with triangles, by first vertex. */
	TArray<int32> UploadSlotsOfLOD(const FStaticMeshLODResources& LOD)
	{
		TArray<int32> Slots;
		const TArray<int32> Order = SectionsInVertexOrder(LOD);
		for (const int32 Section : Order)
		{
			Slots.AddUnique(LOD.Sections[Section].MaterialIndex);
		}
		return Slots;
	}
}

TArray<int32> FMantlePlaceAssets::UploadSlots(const UStaticMesh* Mesh)
{
	const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
	if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
	{
		return TArray<int32>();
	}
	return MantlePlaceAssetsPrivate::UploadSlotsOfLOD(RenderData->LODResources[0]);
}

UMaterialInterface* FMantlePlaceAssets::EffectiveMaterial(const UStaticMeshComponent* Component, int32 Slot)
{
	UMaterialInterface* Material = Component ? Component->GetMaterial(Slot) : nullptr;
	return Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInterface* FMantlePlaceAssets::MeshSlotMaterial(const UStaticMesh* Mesh, int32 Slot)
{
	UMaterialInterface* Material = Mesh ? Mesh->GetMaterial(Slot) : nullptr;
	return Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface);
}

const TCHAR* FMantlePlaceAssets::ScriptAssetsLabel = TEXT("Script Assets");

TArray<FMantleScriptAsset> FMantlePlaceAssets::ScriptAssets(UWorld* World)
{
	TArray<FMantleScriptAsset> Out;
	const AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(World);
	if (!Settings)
	{
		return Out;
	}
	TSet<FSoftObjectPath> Seen;
	for (int32 Index = 0; Index < Settings->ScriptAssets.Num(); ++Index)
	{
		const TSoftObjectPtr<UObject>& Entry = Settings->ScriptAssets[Index];
		const FSoftObjectPath Path = Entry.ToSoftObjectPath();
		// An entry added with + and never picked is not a problem; it just holds nothing.
		if (Path.IsNull() || Seen.Contains(Path))
		{
			continue;
		}
		Seen.Add(Path);
		FMantleScriptAsset& Asset = Out.AddDefaulted_GetRef();
		Asset.Index = Index;
		Asset.Path = Path;
		Asset.Object = Entry.Get();
		if (!Asset.Object)
		{
			Asset.Object = Entry.LoadSynchronous();
		}
	}
	return Out;
}

bool FMantlePlaceAssets::CheckScriptAsset(const FMantleScriptAsset& Asset, const FMantlePlaceAssetLimits& Limits, FString& OutProblem, FString& OutRemedy)
{
	OutProblem.Reset();
	OutRemedy.Reset();
	const UObject* Object = Asset.Object;
	if (!Object)
	{
		OutProblem = TEXT("could not be loaded: nothing is at that path any more");
		OutRemedy = TEXT("Pick the asset again in Mantle Place Settings > Script Assets, or remove the entry.");
		return false;
	}
	if (Object->IsA<UStaticMesh>() || Object->IsA<UMaterialInterface>())
	{
		return true;
	}
	if (const USoundBase* Sound = Cast<USoundBase>(Object))
	{
		// A library sound is Mantle's own and published by its id; a creator's own is held to the #324 rule.
		return IsLibraryAsset(Sound) || CheckSoundRule(Sound, Limits, OutProblem, OutRemedy);
	}
	OutProblem = FString::Printf(TEXT("is a %s, and Script Assets publishes static meshes, materials and sound waves only"),
		*Object->GetClass()->GetName());
	OutRemedy = TEXT("Remove it from Mantle Place Settings > Script Assets. A texture reaches players through a material that uses it.");
	return false;
}

FMantleMaterialAnalysis FMantlePlaceAssets::AnalyzeMaterial(const UMaterialInterface* Material)
{
	using namespace MantlePlaceAssetsPrivate;

	FMantleMaterialAnalysis Out;
	if (!Material)
	{
		return Out;
	}
	FWalk Walk;
	Walk.Context = Material;
	Walk.Out = &Out;
	Out.bCarPaint = PublishesAsCarPaint(Material);

	// The instance chain: each parent, and every texture an instance overrides.
	for (const UMaterialInterface* Link = Material; Link; )
	{
		Walk.AddDependency(Link);
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Link);
		if (!Instance)
		{
			break;
		}
		for (const FTextureParameterValue& Value : Instance->TextureParameterValues)
		{
			if (Value.ParameterValue)
			{
				Walk.AddDependency(Value.ParameterValue);
			}
		}
		Link = Instance->Parent;
	}

	const UMaterial* Base = Material->GetMaterial();
	if (!Base || !Base->GetEditorOnlyData())
	{
		// No graph to read (a cooked material): nothing can be said about its losses.
		return Out;
	}
	Walk.AddDependency(Base);

	const EBlendMode Blend = Material->GetBlendMode();
	Walk.bTranslucent = IsTranslucentBlendMode(Blend);
	// #628: Additive is carried (alphaMode additive), so it is not a BlendMode loss.
	if (Blend == BLEND_Modulate || Blend == BLEND_AlphaComposite || Blend == BLEND_AlphaHoldout
		|| Blend == BLEND_TranslucentColoredTransmittance)
	{
		Out.Losses |= EMantleMaterialLoss::BlendMode;
		Out.BlendModeName = BlendModeName(Blend);
	}
	if (Base->MaterialDomain != MD_Surface)
	{
		Out.Losses |= EMantleMaterialLoss::NotSurface;
	}

	const FMaterialShadingModelField Models = Material->GetShadingModels();
	// #628: additive blending is drawn unlit, so an unlit additive material is exact, and a lit one loses only its lighting.
	const bool bAdditive = Blend == BLEND_Additive;
	Out.Fidelity2.bAdditive = bAdditive;
	Out.Fidelity2.bAdditiveLit = bAdditive && !Models.HasOnlyShadingModel(MSM_Unlit);
	if (Models.HasShadingModel(MSM_Unlit) && !bAdditive)
	{
		Out.Losses |= EMantleMaterialLoss::Unlit;
	}
	// #628: Two Sided Foliage on a masked, two-sided material is carried by the foliage permutations (exact, from the
	// shading model); every other subsurface model, and foliage on anything else, is the loss.
	const bool bFoliage = !Out.bCarPaint && Models.HasOnlyShadingModel(MSM_TwoSidedFoliage) && Blend == BLEND_Masked && Material->IsTwoSided();
	if (!bFoliage && Models.HasAnyShadingModel({ MSM_Subsurface, MSM_PreintegratedSkin, MSM_SubsurfaceProfile, MSM_TwoSidedFoliage }))
	{
		Out.Losses |= EMantleMaterialLoss::Subsurface;
	}
	Out.Fidelity2.bFoliage = bFoliage;
	for (const EMaterialShadingModel Special : { MSM_ClearCoat, MSM_Hair, MSM_Cloth, MSM_Eye, MSM_SingleLayerWater, MSM_ThinTranslucent })
	{
		// Car paint keeps the clear coat (#487); Check says so in a note of its own.
		if (Special == MSM_ClearCoat && Out.bCarPaint)
		{
			continue;
		}
		if (Models.HasShadingModel(Special))
		{
			Out.Losses |= EMantleMaterialLoss::SpecialShading;
			Out.ShadingModelName = ShadingModelName(Special);
			break;
		}
	}

	UMaterial* MutableBase = const_cast<UMaterial*>(Base);
	auto Root = [MutableBase](EMaterialProperty Property) -> const FExpressionInput*
	{
		return MutableBase->GetExpressionInputForProperty(Property);
	};
	auto Connected = [&Root](EMaterialProperty Property)
	{
		const FExpressionInput* Input = Root(Property);
		return Input && Input->Expression;
	};

	// GhzGarage/mantle#818: a Single Layer Water surface material (a Water plugin body's) is water already, whatever its graph: it
	// always publishes with the water key (FMantleWaterMaterial::RecogniseSingleLayer), and its shading is then no loss. The Water
	// plugin's Water_Material is masked (a depth mask, which Mantle's opaque water does without) and builds its outputs with Set
	// Material Attributes, so it has no output roots to read and never reached the branch below; its coefficients and colour come
	// from its named parameters, else from a clear lake's defaults. Its waves are world position offset, which the water key has none
	// of: the offset is dropped and said (bWavesDropped).
	const bool bSingleLayerWater = !Out.bCarPaint && Base->MaterialDomain == MD_Surface && Models.HasShadingModel(MSM_SingleLayerWater);
	auto DropWaves = [&Out]()
	{
		Out.Fidelity2.bWind = false;
		Out.Fidelity2.Water.bWavesDropped = true;
		Out.Losses &= ~EMantleMaterialLoss::WorldPositionOffset;
	};

	if (Base->bUseMaterialAttributes)
	{
		Walk.WalkInput(Root(MP_MaterialAttributes), ERoot::Surface);
		if (bSingleLayerWater && FMantleWaterMaterial::RecogniseSingleLayer(Material, nullptr, nullptr, Out.Fidelity2.Water))
		{
			// Published as Mantle's water, which is Single Layer Water: no longer a loss of special shading. The walk flagged a
			// world position offset pin of the Set Material Attributes node as the loss; it is the water's waves, dropped and said.
			Out.Losses &= ~EMantleMaterialLoss::SpecialShading;
			if (Out.Has(EMantleMaterialLoss::WorldPositionOffset) || Out.Fidelity2.Water.bWavesDropped)
			{
				DropWaves();
			}
		}
	}
	else
	{
		// #533: the patterns the material document carries (see the pattern struct documented in MantlePlaceAssets.h near line 121). An
		// output one of them explains is not walked, so it is no loss; its textures still count (the cache key,
		// the size checks). Everything else is walked as before.
		FMaterialRoots Roots;
		Roots.BaseColor = Root(MP_BaseColor);
		Roots.Normal = Root(MP_Normal);
		Roots.Emissive = Root(MP_EmissiveColor);
		Roots.Roughness = Root(MP_Roughness);
		Roots.Metallic = Root(MP_Metallic);
		Roots.Occlusion = Root(MP_AmbientOcclusion);
		Roots.Opacity = Root(Blend == BLEND_Masked ? MP_OpacityMask : MP_Opacity);
		Roots.WorldPositionOffset = Root(MP_WorldPositionOffset);
		FMantleMaterialFidelity2& Fidelity = Out.Fidelity2;
		TSet<int32> Carried;
		auto Carry = [&Carried](std::initializer_list<EMaterialProperty> Properties)
		{
			for (const EMaterialProperty Property : Properties)
			{
				Carried.Add(static_cast<int32>(Property));
			}
		};
		auto IsCarried = [&Carried](const EMaterialProperty Property) { return Carried.Contains(static_cast<int32>(Property)); };
		const bool bOpaque = Blend == BLEND_Opaque;
		// Car paint's master has none of these (its document refuses them), so nothing is carried on it. An unlit
		// material publishes its emissive as glow and nothing else it shades with, so only wind and a panned
		// emissive carry over.
		const bool bCarries = !Out.bCarPaint;
		const bool bLit = bCarries && !Models.HasShadingModel(MSM_Unlit);

		if (bCarries && Connected(MP_WorldPositionOffset) && !Walk.bTranslucent && RecogniseWind(Roots, Material, Fidelity))
		{
			Carry({ MP_WorldPositionOffset });
		}
		// #628: an additive material's Fresnel (a rim glow) is dropped, and says so; the bake sees the surface face-on, where it is 0.
		Fidelity.bAdditiveRim = Fidelity.bAdditive && FMantleGlowMaterial::HasFresnel(Roots.Emissive);

		// #628: an unlit translucent material whose emissive is the scene behind it (fake refraction) publishes as tinted see-through
		// glass: the bake sees black there, an opaque solid. The whole emissive graph is replaced by the tint, so it is walked no more.
		if (bCarries && Blend == BLEND_Translucent && Models.HasOnlyShadingModel(MSM_Unlit)
			&& FMantleGlowMaterial::RecogniseSceneColourGlass(Material, Roots.Emissive, Fidelity.Glass))
		{
			Carry({ MP_EmissiveColor });
			Out.Losses &= ~EMantleMaterialLoss::Unlit;
		}

		if (!Fidelity.bWind && bOpaque && bLit
			&& (RecogniseWorldAligned(Walk, Roots, Material, Fidelity) || RecogniseLayerBlend(Walk, Roots, Material, Fidelity)))
		{
			Carry({ MP_BaseColor, MP_Normal, MP_Roughness, MP_Metallic, MP_AmbientOcclusion });
			for (int32 Slot = 0; Slot < 3; ++Slot)
			{
				Walk.AddTexture(Fidelity.SourceTextures[Slot].Get());
				Walk.AddTexture(Fidelity.LayerTextures[Slot].Get());
			}
		}

		// #628: a depth-faded translucent material (water): the bake sees the fade's near end and writes opacity 0, so its
		// opacity (and deep colour, when the graph gives one) is carried as constants read at the far end.
		// GhzGarage/mantle#818: a Single Layer Water material (a Water plugin body's) is water already, before any fade is looked for.
		// Its waves are world position offset, which reads as wind: the water key has no waves, so the offset is dropped and said (bWavesDropped).
		if ((bSingleLayerWater && FMantleWaterMaterial::RecogniseSingleLayer(Material, Roots.BaseColor, Roots.Normal, Fidelity.Water))
			|| (bCarries && bLit && Walk.bTranslucent && !Fidelity.bWind && FMantleWaterMaterial::Recognise(Material, Roots.Opacity, Roots.BaseColor, Roots.Normal, Fidelity.Water)))
		{
			if (Fidelity.Water.bSingleLayer)
			{
				// Published as Mantle's water, which is Single Layer Water: no longer a loss of special shading.
				Out.Losses &= ~EMantleMaterialLoss::SpecialShading;
				if (Connected(MP_WorldPositionOffset) || Fidelity.bWind || Fidelity.Water.bWavesDropped)
				{
					DropWaves();
					Carry({ MP_WorldPositionOffset });
				}
			}
			Carry({ MP_Opacity });
			if (Fidelity.Water.bColour)
			{
				Carry({ MP_BaseColor });
			}
			const FMantleWaterBake& Water = Fidelity.Water;
			if (Water.PublishesOpaque())
			{
				// Near-opaque water publishes with the water key, which carries the normal samples (the first two) as layers with
				// their own layout and pan: their source textures are the document's (the converter reads them), in place of the bake.
				bool bLayers = false;
				for (int32 Layer = 0; Layer < Water.Normals.Num() && Layer < 2; ++Layer)
				{
					if (Water.Normals[Layer].Texture.IsValid())
					{
						Walk.AddTexture(Water.Normals[Layer].Texture.Get());
						bLayers = true;
					}
				}
				if (bLayers)
				{
					Carry({ MP_Normal });
				}
			}
			else if (Water.bNormalPanned && Water.NormalTexture.IsValid())
			{
				// Its panned normal publishes as the source texture with the pan, in place of the time-0 bake.
				Fidelity.SourceTextures[1] = Water.NormalTexture;
				Fidelity.SourceTiling[1][0] = Water.NormalTiling[0];
				Fidelity.SourceTiling[1][1] = Water.NormalTiling[1];
				Fidelity.bPanning[1] = true;
				Fidelity.Panning[1][0] = Water.NormalSpeed[0];
				Fidelity.Panning[1][1] = Water.NormalSpeed[1];
				Walk.AddTexture(Water.NormalTexture.Get());
				Carry({ MP_Normal });
			}
		}

		// Panning, on each output nothing above carried.
		auto CarryPanned = [&](const int32 Slot, const FRecognisedSample& Sample)
		{
			Fidelity.SourceTextures[Slot] = Sample.Texture;
			Fidelity.SourceTiling[Slot][0] = Sample.Tiling[0];
			Fidelity.SourceTiling[Slot][1] = Sample.Tiling[1];
			Fidelity.bPanning[Slot] = true;
			Fidelity.Panning[Slot][0] = Sample.Speed[0];
			Fidelity.Panning[Slot][1] = Sample.Speed[1];
			Walk.AddTexture(Sample.Texture);
		};
		FRecognisedSample Panned;
		if (bLit && !IsCarried(MP_BaseColor) && RecognisePanned(Walk, Roots.BaseColor, Panned))
		{
			// On a masked or translucent material the opacity must be the same sample's alpha (it moves with it).
			FRecognisedSample Alpha;
			const bool bAlphaMoves = !bOpaque && RecogniseSample(Walk, Roots.Opacity, true, Alpha) && Alpha.Sample == Panned.Sample && Alpha.Channel == TEXT('a');
			if (bOpaque || bAlphaMoves)
			{
				CarryPanned(0, Panned);
				Fidelity.bSourceAlpha = bAlphaMoves;
				Carry({ MP_BaseColor });
				if (bAlphaMoves)
				{
					Carry({ Blend == BLEND_Masked ? MP_OpacityMask : MP_Opacity });
				}
			}
		}
		if (bLit && !IsCarried(MP_Normal) && RecognisePanned(Walk, Roots.Normal, Panned))
		{
			CarryPanned(1, Panned);
			Carry({ MP_Normal });
		}
		if (bCarries && !IsCarried(MP_EmissiveColor) && RecognisePanned(Walk, Roots.Emissive, Panned))
		{
			CarryPanned(3, Panned);
			Carry({ MP_EmissiveColor });
		}
		if (bLit && !IsCarried(MP_Roughness))
		{
			FRecognisedSample Orm;
			auto ReadPanned = [&](const FExpressionInput* Input, const TCHAR Channel)
			{
				FRecognisedSample Sample;
				if (!RecogniseSample(Walk, Input, true, Sample) || Sample.Channel != Channel || (Orm.Sample && Sample.Sample != Orm.Sample))
				{
					return false;
				}
				Orm = Sample;
				return true;
			};
			double MetallicFactor = 1.0;
			double OcclusionStrength = 1.0;
			if (RecogniseOrmGroup(Roots, Material, ReadPanned, MetallicFactor, OcclusionStrength))
			{
				CarryPanned(2, Orm);
				Fidelity.OrmMetallicFactor = MetallicFactor;
				Fidelity.OrmOcclusionStrength = OcclusionStrength;
				Carry({ MP_Roughness, MP_Metallic, MP_AmbientOcclusion });
			}
		}

		for (const EMaterialProperty Property : { MP_EmissiveColor, MP_Opacity, MP_OpacityMask, MP_Metallic, MP_Specular,
				 MP_Roughness, MP_Anisotropy, MP_AmbientOcclusion, MP_CustomData0, MP_CustomData1, MP_ShadingModel, MP_SurfaceThickness })
		{
			if (!IsCarried(Property))
			{
				Walk.WalkInput(Root(Property), ERoot::Surface);
			}
		}
		if (!IsCarried(MP_BaseColor))
		{
			Walk.WalkBaseColor(Root(MP_BaseColor));
		}
		if (!IsCarried(MP_Normal))
		{
			Walk.WalkInput(Root(MP_Normal), ERoot::Normal);
		}
		Walk.WalkInput(Root(MP_Tangent), ERoot::Normal);
		if (Connected(MP_WorldPositionOffset) && !IsCarried(MP_WorldPositionOffset))
		{
			Out.Losses |= EMantleMaterialLoss::WorldPositionOffset;
			Walk.WalkInput(Root(MP_WorldPositionOffset), ERoot::WorldPositionOffset);
		}
		if (Connected(MP_Refraction) && Walk.bTranslucent)
		{
			Out.Losses |= EMantleMaterialLoss::Refraction;
		}
		if (Connected(MP_PixelDepthOffset))
		{
			Out.Losses |= EMantleMaterialLoss::PixelDepthOffset;
		}
		if (Connected(MP_Displacement))
		{
			Out.Losses |= EMantleMaterialLoss::Displacement;
		}
		Walk.WalkInput(Root(MP_SubsurfaceColor), ERoot::Surface);
	}
	for (int32 Uv = MP_CustomizedUVs0; Uv <= MP_CustomizedUVs7; ++Uv)
	{
		Walk.WalkInput(Root(static_cast<EMaterialProperty>(Uv)), ERoot::Surface);
	}
	Walk.WalkInput(Root(MP_FrontMaterial), ERoot::Surface);

	if (Walk.NormalTextures.Num() >= 2)
	{
		Out.Losses |= EMantleMaterialLoss::DetailNormal;
	}
	// #471: a pure base-colour tint is carried; any other vertex colour use is the loss, and then the tint is not carried either.
	Out.bVertexColorTintsBaseColor = Walk.bBaseColorTint && !Out.Has(EMantleMaterialLoss::VertexColor);
	return Out;
}

bool FMantlePlaceAssets::PublishesAsCarPaint(const UMaterialInterface* Material)
{
	const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	if (!Base || Base->MaterialDomain != MD_Surface)
	{
		return false;
	}
	return Material->GetShadingModels().HasOnlyShadingModel(MSM_ClearCoat) && Material->GetBlendMode() == BLEND_Opaque && !Material->IsTwoSided();
}

FMantleCarPaintFinish FMantlePlaceAssets::CarPaintFinish(const UMaterialInterface* Material)
{
	using namespace MantlePlaceAssetsPrivate;
	FMantleCarPaintFinish Out;
	if (!Material)
	{
		return Out;
	}

	// The Clear Coat inputs themselves first: what they render with is what the creator saw.
	const UMaterial* Base = Material->GetMaterial();
	const UMaterialEditorOnlyData* Graph = Base ? Base->GetEditorOnlyData() : nullptr;
	const bool bInputs = Graph && !Base->bUseMaterialAttributes;
	if (!(bInputs && CarPaintInputScalar(Material, Graph->ClearCoat, Out.ClearCoat)))
	{
		CarPaintNamedScalar(Material, { TEXT("ClearCoat"), TEXT("Clear Coat") }, Out.ClearCoat);
	}
	if (!(bInputs && CarPaintInputScalar(Material, Graph->ClearCoatRoughness, Out.ClearCoatRoughness)))
	{
		CarPaintNamedScalar(Material, { TEXT("ClearCoatRoughness"), TEXT("Clear Coat Roughness") }, Out.ClearCoatRoughness);
	}

	// Flakes have no Unreal input: M_MantlePlace_CarPaint's own parameter names.
	const bool bDensity = CarPaintNamedScalar(Material, { TEXT("FlakeDensity"), TEXT("Flake Density") }, Out.FlakeDensity);
	const bool bSize = CarPaintNamedScalar(Material, { TEXT("FlakeSize"), TEXT("Flake Size") }, Out.FlakeSize);
	FLinearColor Tint = FLinearColor::White;
	const bool bTint = CarPaintNamedVector(Material, { TEXT("FlakeTint"), TEXT("Flake Tint") }, Tint);
	if (bTint)
	{
		Out.FlakeTint[0] = Tint.R;
		Out.FlakeTint[1] = Tint.G;
		Out.FlakeTint[2] = Tint.B;
	}
	// No flake parameter at all: a plain clear coat (a glossy floor should not sparkle).
	if (!bDensity && !bSize && !bTint)
	{
		Out.FlakeDensity = 0.0;
	}
	return Out;
}

// ---- #628: per-instance values at bake time -------------------------------------------------------------

namespace MantlePlaceAssetsPrivate
{
	/** What Per Instance Random reads as in a bake of one material for every instance: the middle of its 0..1. */
	constexpr float RepresentativeInstanceRandom = 0.5f;

	/**
	 * Copies the base material and the functions it calls that hold a per-instance node, replacing each such node by a constant
	 * and re-pointing everything that read it. Per Instance Custom Data keeps what feeds its Default Value input, else its
	 * constant default; Per Instance Fade Amount is 1 (the instance fully faded in).
	 */
	struct FPerInstanceNeutraliser
	{
		TMap<const UMaterialFunction*, bool> Contains;
		TMap<const UMaterialFunction*, UMaterialFunction*> Copies;

		bool GraphHasPerInstance(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions)
		{
			for (const TObjectPtr<UMaterialExpression>& Expression : Expressions)
			{
				if (IsPerInstanceNode(Expression))
				{
					return true;
				}
				const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression.Get());
				if (Call && FunctionHasPerInstance(Cast<UMaterialFunction>(Call->MaterialFunction)))
				{
					return true;
				}
			}
			return false;
		}

		bool FunctionHasPerInstance(const UMaterialFunction* Function)
		{
			if (!Function)
			{
				return false;
			}
			if (const bool* Known = Contains.Find(Function))
			{
				return *Known;
			}
			Contains.Add(Function, false);
			const bool bHas = GraphHasPerInstance(Function->GetExpressions());
			Contains.Add(Function, bHas);
			return bHas;
		}

		UMaterialFunction* CopyOf(UMaterialFunction* Function)
		{
			if (!FunctionHasPerInstance(Function))
			{
				return nullptr;
			}
			if (UMaterialFunction** Known = Copies.Find(Function))
			{
				return *Known;
			}
			const FName Name = MakeUniqueObjectName(GetTransientPackage(), UMaterialFunction::StaticClass(), *FString::Printf(TEXT("%s_MantleBake"), *Function->GetName()));
			UMaterialFunction* Copy = DuplicateObject<UMaterialFunction>(Function, GetTransientPackage(), Name);
			Copy->ClearFlags(RF_Public | RF_Standalone);
			Copy->SetFlags(RF_Transient);
			Copies.Add(Function, Copy);
			Rewrite(nullptr, Copy);
			return Copy;
		}

		/** Rewrites one graph (a material's or a function's) in place. */
		void Rewrite(UMaterial* Material, UMaterialFunction* Function)
		{
			UObject* Outer = Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function);
			TArray<UMaterialExpression*> Expressions;
			for (const TObjectPtr<UMaterialExpression>& Expression : (Material ? Material->GetExpressions() : Function->GetExpressions()))
			{
				Expressions.Add(Expression);
			}
			// The functions this graph calls first, so their copies are the ones it calls.
			for (UMaterialExpression* Expression : Expressions)
			{
				if (UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					if (UMaterialFunction* Inner = CopyOf(Cast<UMaterialFunction>(Call->MaterialFunction)))
					{
						Call->SetMaterialFunction(Inner);
					}
				}
			}

			struct FReplacement
			{
				UMaterialExpression* Node = nullptr;
				const FExpressionInput* Forward = nullptr;
			};
			TMap<const UMaterialExpression*, FReplacement> Replacements;
			auto Add = [&](UMaterialExpression* Node)
			{
				Node->Material = Material;
				Node->Function = Function;
				if (Material)
				{
					Material->GetExpressionCollection().AddExpression(Node);
				}
				else
				{
					Function->GetExpressionCollection().AddExpression(Node);
				}
				return Node;
			};
			for (UMaterialExpression* Expression : Expressions)
			{
				if (!IsPerInstanceNode(Expression))
				{
					continue;
				}
				FReplacement Replacement;
				if (const UMaterialExpressionPerInstanceCustomData* Data = Cast<UMaterialExpressionPerInstanceCustomData>(Expression))
				{
					if (Data->DefaultValue.Expression)
					{
						Replacement.Forward = &Data->DefaultValue;
					}
					else
					{
						UMaterialExpressionConstant* Constant = NewObject<UMaterialExpressionConstant>(Outer);
						Constant->R = Data->ConstDefaultValue;
						Replacement.Node = Add(Constant);
					}
				}
				else if (const UMaterialExpressionPerInstanceCustomData3Vector* Data3 = Cast<UMaterialExpressionPerInstanceCustomData3Vector>(Expression))
				{
					if (Data3->DefaultValue.Expression)
					{
						Replacement.Forward = &Data3->DefaultValue;
					}
					else
					{
						UMaterialExpressionConstant3Vector* Constant = NewObject<UMaterialExpressionConstant3Vector>(Outer);
						Constant->Constant = Data3->ConstDefaultValue;
						Replacement.Node = Add(Constant);
					}
				}
				else
				{
					UMaterialExpressionConstant* Constant = NewObject<UMaterialExpressionConstant>(Outer);
					Constant->R = Expression->IsA<UMaterialExpressionPerInstanceRandom>() ? RepresentativeInstanceRandom : 1.0f;
					Replacement.Node = Add(Constant);
				}
				Replacements.Add(Expression, Replacement);
			}
			if (Replacements.Num() == 0)
			{
				return;
			}

			auto Apply = [&Replacements](FExpressionInput& Input)
			{
				const FReplacement* Replacement = Input.Expression ? Replacements.Find(Input.Expression) : nullptr;
				if (!Replacement)
				{
					return;
				}
				if (Replacement->Forward)
				{
					const FExpressionInput& From = *Replacement->Forward;
					Input.Expression = From.Expression;
					Input.OutputIndex = From.OutputIndex;
					Input.Mask = From.Mask;
					Input.MaskR = From.MaskR;
					Input.MaskG = From.MaskG;
					Input.MaskB = From.MaskB;
					Input.MaskA = From.MaskA;
				}
				else
				{
					Input.Connect(0, Replacement->Node);
				}
			};
			for (UMaterialExpression* Expression : Expressions)
			{
				for (FExpressionInputIterator It{ Expression }; It; ++It)
				{
					Apply(*It.Input);
				}
			}
			if (Material)
			{
				for (int32 Property = 0; Property < MP_MAX; ++Property)
				{
					if (FExpressionInput* Input = Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Property)))
					{
						Apply(*Input);
					}
				}
			}
		}
	};
}

TStrongObjectPtr<UMaterialInterface> FMantlePlaceAssets::NeutralisePerInstanceValues(const UMaterialInterface* Material)
{
	using namespace MantlePlaceAssetsPrivate;
	if (!Material)
	{
		return nullptr;
	}
	// The chain from the material up to its base; a dynamic instance (or anything else) has no copy to make.
	TArray<const UMaterialInstanceConstant*> Chain;
	const UMaterialInterface* Link = Material;
	while (const UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Link))
	{
		Chain.Add(Instance);
		Link = Instance->Parent;
	}
	const UMaterial* Base = Cast<UMaterial>(Link);
	if (!Base || !Base->GetEditorOnlyData())
	{
		return nullptr;
	}
	FPerInstanceNeutraliser Neutraliser;
	if (!Neutraliser.GraphHasPerInstance(Base->GetExpressions()))
	{
		return nullptr;
	}

	UMaterial* BaseCopy = DuplicateObject<UMaterial>(Base, GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), *FString::Printf(TEXT("%s_MantleBake"), *Base->GetName())));
	BaseCopy->ClearFlags(RF_Public | RF_Standalone);
	BaseCopy->SetFlags(RF_Transient);
	Neutraliser.Rewrite(BaseCopy, nullptr);
	// The editor's own finishing step: it builds what GLTFExporter reads the graph through.
	BaseCopy->PostEditChange();

	UMaterialInterface* Result = BaseCopy;
	for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
	{
		UMaterialInstanceConstant* Copy = DuplicateObject<UMaterialInstanceConstant>(Chain[Index], GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UMaterialInstanceConstant::StaticClass(), *FString::Printf(TEXT("%s_MantleBake"), *Chain[Index]->GetName())));
		Copy->ClearFlags(RF_Public | RF_Standalone);
		Copy->SetFlags(RF_Transient);
		Copy->SetParentEditorOnly(Result, /*RecacheShader*/ false);
		Copy->PostEditChange();
		Result = Copy;
	}
	return TStrongObjectPtr<UMaterialInterface>(Result);
}

FString FMantlePlaceAssets::DescribePerInstanceVariation()
{
	return TEXT("it varies its look per instance (Per Instance Random, Custom Data or Fade Amount), which a published material cannot, so it publishes at the average: Per Instance Random as 0.5, Custom Data as its default value.");
}

FString FMantlePlaceAssets::DescribeLoss(EMantleMaterialLoss Loss, const FMantleMaterialAnalysis& Analysis)
{
	switch (Loss)
	{
	case EMantleMaterialLoss::Animation:
		return TEXT("time-based animation will be frozen (panners, flipbooks and Time nodes stop moving; only a Panner straight into a texture's UVs, that texture straight into an output, keeps moving).");
	case EMantleMaterialLoss::WorldPositionOffset:
		return TEXT("world position offset (wind or vertex animation) is dropped, so the mesh will not sway or move (only the engine's SimpleGrassWind with constant intensity and speed is kept).");
	case EMantleMaterialLoss::WorldAligned:
		return TEXT("world-aligned or triplanar mapping is baked onto the mesh's own UVs, so its pattern will not line up across neighbouring meshes (only the engine's WorldAlignedTexture and WorldAlignedNormal straight into an opaque material's outputs, with constant sizes, are kept).");
	case EMantleMaterialLoss::Refraction:
		return TEXT("refraction is approximated: it publishes as smooth, tinted glass that is a little more reflective and opaque (by its index of refraction), but the scene behind it is not bent.");
	case EMantleMaterialLoss::Subsurface:
		return TEXT("subsurface scattering is dropped, so skin, wax and leaves look flatter.");
	case EMantleMaterialLoss::Parallax:
		return TEXT("parallax depth is flattened, so the surface looks flat up close.");
	case EMantleMaterialLoss::VertexColor:
		return TEXT("vertex-colour blending is dropped, so layers painted per vertex will not show (only Vertex Color's RGB multiplying the base colour, and on an opaque material a Lerp between two textures by one Vertex Color channel, are kept).");
	case EMantleMaterialLoss::DetailNormal:
		return TEXT("its detail normals are baked at the material's texture size, so fine surface detail may soften.");
	case EMantleMaterialLoss::PixelDepthOffset:
		return TEXT("pixel depth offset is dropped.");
	case EMantleMaterialLoss::Displacement:
		return TEXT("displacement is dropped, so the surface keeps the mesh's own shape.");
	case EMantleMaterialLoss::SpecialShading:
		// Only a clear coat that cannot be car paint (#487) gets here: say what would keep it.
		return Analysis.ShadingModelName == MantlePlaceAssetsPrivate::ShadingModelName(MSM_ClearCoat)
			? FString(TEXT("its clear coat shading becomes standard shading; Mantle keeps a clear coat as car paint only on an opaque, one-sided material."))
			: FString::Printf(TEXT("its %s shading becomes standard shading."), *Analysis.ShadingModelName);
	case EMantleMaterialLoss::Unlit:
		return TEXT("it is unlit; Mantle shows it as a glowing material that lighting still touches.");
	case EMantleMaterialLoss::BlendMode:
		return FString::Printf(TEXT("its %s blending becomes ordinary translucency."), *Analysis.BlendModeName);
	case EMantleMaterialLoss::NotSurface:
		return TEXT("it is not a surface material (a decal, light function or post-process material), so it is flattened as a plain surface.");
	default:
		return FString();
	}
}

FIntPoint FMantlePlaceAssets::TextureSize(const UTexture* Texture)
{
	if (!Texture)
	{
		return FIntPoint::ZeroValue;
	}
#if WITH_EDITORONLY_DATA
	if (Texture->Source.IsValid())
	{
		return FIntPoint(static_cast<int32>(Texture->Source.GetSizeX()), static_cast<int32>(Texture->Source.GetSizeY()));
	}
#endif
	return FIntPoint(static_cast<int32>(Texture->GetSurfaceWidth()), static_cast<int32>(Texture->GetSurfaceHeight()));
}

FIntPoint FMantlePlaceAssets::PublishedTextureSize(FIntPoint Size, int32 MaxTextureSize)
{
	const int32 Longest = FMath::Max(Size.X, Size.Y);
	if (MaxTextureSize <= 0 || Longest <= MaxTextureSize)
	{
		return Size;
	}
	const double Scale = static_cast<double>(MaxTextureSize) / Longest;
	return FIntPoint(FMath::Max(1, FMath::RoundToInt(Size.X * Scale)), FMath::Max(1, FMath::RoundToInt(Size.Y * Scale)));
}

bool FMantlePlaceAssets::TextureDownscaleIssue(const FString& What, FIntPoint Size, int32 MaxTextureSize, FMantleValidationIssue& OutIssue)
{
	using namespace MantlePlaceAssetsPrivate;
	const FIntPoint Published = PublishedTextureSize(Size, MaxTextureSize);
	if (Published == Size)
	{
		return false;
	}
	OutIssue = FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
		FString::Printf(TEXT("%s is %dx%d; it is downscaled to %s when published."), *What, Size.X, Size.Y, *SizeText(Published.X, Published.Y)),
		FString::Printf(TEXT("Nothing to do: Mantle textures are at most %d on a side. Detail finer than that is lost."), MaxTextureSize) };
	return true;
}

FString FMantlePlaceAssets::DescribeUsers(const TArray<FString>& UsedBy)
{
	if (UsedBy.Num() == 0)
	{
		return FString();
	}
	if (UsedBy.Num() == 1)
	{
		return FString::Printf(TEXT("used by '%s'"), *UsedBy[0]);
	}
	return FString::Printf(TEXT("used by '%s' and %d more"), *UsedBy[0], UsedBy.Num() - 1);
}

bool FMantlePlaceAssets::CountMeshLOD(const UStaticMesh* Mesh, int32 LODIndex, FMantleMeshLODCount& Out)
{
	using namespace MantlePlaceAssetsPrivate;
	Out = FMantleMeshLODCount();
	const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
	if (!RenderData || !RenderData->LODResources.IsValidIndex(LODIndex))
	{
		return false;
	}
	const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
	Out.Triangles = LOD.GetNumTriangles();
	Out.Slots = UploadSlotsOfLOD(LOD);
	Out.Vertices = LOD.GetNumVertices();

	// Sections that share vertices across materials are split at export, one copy of the shared vertices per section
	// (#628, FMantleCosmeticExport::SplitSharedVertexSections), so what the limit counts is the vertices after the split.
	if (Out.Slots.Num() > 1)
	{
		const TArray<int32> Order = SectionsInVertexOrder(LOD);
		int32 Covered = 0;
		bool bTiles = true;
		for (const int32 Section : Order)
		{
			bTiles = bTiles && static_cast<int32>(LOD.Sections[Section].MinVertexIndex) == Covered;
			Covered = static_cast<int32>(LOD.Sections[Section].MaxVertexIndex) + 1;
		}
		if (!bTiles || Covered != Out.Vertices)
		{
			TArray<uint32> Indices;
			LOD.IndexBuffer.GetCopy(Indices);
			TArray<FMantleSharedVertexSection> Split;
			for (const int32 Section : Order)
			{
				FMantleSharedVertexSection& Entry = Split.AddDefaulted_GetRef();
				Entry.FirstIndex = static_cast<int32>(LOD.Sections[Section].FirstIndex);
				Entry.NumTriangles = static_cast<int32>(LOD.Sections[Section].NumTriangles);
			}
			Out.Vertices = FMantleCosmeticExport::CountSplitVertices(Indices, Split);
		}
	}
	return true;
}

int32 FMantlePlaceAssets::PublishedLODIndex(const UStaticMesh* Mesh, const FMantlePlaceAssetLimits& Limits)
{
	FMantleMeshLODCount First;
	if (!CountMeshLOD(Mesh, 0, First))
	{
		return INDEX_NONE;
	}
	if (First.Triangles <= Limits.MaxTriangles && First.Vertices <= Limits.MaxVertices)
	{
		return 0;
	}
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	const int32 NumLODs = FMath::Min(RenderData->LODResources.Num(), static_cast<int32>(MAX_STATIC_MESH_LODS));
	for (int32 LODIndex = 1; LODIndex < NumLODs; ++LODIndex)
	{
		// A LOD an export cannot read is not one to publish from.
		FMantleMeshLODCount Lower;
		if (!FMantleMeshReduce::HasCpuCopy(RenderData->LODResources[LODIndex]) || !CountMeshLOD(Mesh, LODIndex, Lower))
		{
			continue;
		}
		// It has to draw with every slot LOD 0 does and no other: the materials are chosen per LOD 0 slot.
		bool bSameSlots = Lower.Triangles > 0 && Lower.Slots.Num() == First.Slots.Num();
		for (const int32 Slot : Lower.Slots)
		{
			bSameSlots = bSameSlots && First.Slots.Contains(Slot);
		}
		if (bSameSlots && Lower.Triangles <= Limits.MaxTriangles && Lower.Vertices <= Limits.MaxVertices)
		{
			return LODIndex;
		}
	}
	return INDEX_NONE;
}

void FMantlePlaceAssets::CheckMesh(const UStaticMesh* Mesh, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits, TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantlePlaceAssetsPrivate;
	if (!Mesh)
	{
		return;
	}
	const FString Subject = WithUsers(TEXT("Mesh ") + Quoted(Mesh), UsedBy);
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	FMantleMeshLODCount Counted;
	if (!RenderData || !CountMeshLOD(Mesh, 0, Counted))
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			Subject + TEXT(" has no render data, so it cannot be converted."),
			TEXT("Open the mesh in the editor once (or re-import it), then try again.") });
		return;
	}
	int32 Triangles = Counted.Triangles;
	int32 Vertices = Counted.Vertices;
	const TArray<int32>& Slots = Counted.Slots;

	// #804: over a limit, the mesh publishes from a smaller version of itself where it can (the creator's mesh is untouched).
	// When it cannot be reduced, the plain reason (needing Allow CPU Access, say) goes on the error below.
	FString ReduceFailure;
	if (Triangles > Limits.MaxTriangles || Vertices > Limits.MaxVertices)
	{
		FString Over;
		if (Triangles > Limits.MaxTriangles && Vertices > Limits.MaxVertices)
		{
			Over = FString::Printf(TEXT("%s triangles and %s vertices, more than the %s and %s a place mesh may have"),
				*Thousands(Triangles), *Thousands(Vertices), *Thousands(Limits.MaxTriangles), *Thousands(Limits.MaxVertices));
		}
		else if (Triangles > Limits.MaxTriangles)
		{
			Over = FString::Printf(TEXT("%s triangles, more than the %s a place mesh may have"), *Thousands(Triangles), *Thousands(Limits.MaxTriangles));
		}
		else
		{
			Over = FString::Printf(TEXT("%s vertices, more than the %s a place mesh may have"), *Thousands(Vertices), *Thousands(Limits.MaxVertices));
		}

		FMantleMeshLODCount Smaller;
		const int32 PublishedLOD = PublishedLODIndex(Mesh, Limits);
		if (PublishedLOD > 0 && CountMeshLOD(Mesh, PublishedLOD, Smaller))
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
				FString::Printf(TEXT("%s has %s, so its LOD %d (%s triangles) is published instead."), *Subject, *Over, PublishedLOD, *Thousands(Smaller.Triangles)),
				TEXT("Nothing to do: your mesh is unchanged. To publish a more detailed version, make a LOD 0 that fits the limit in the mesh editor.") });
			Triangles = Smaller.Triangles;
			Vertices = Smaller.Vertices;
		}
		else
		{
			FString Why;
			const UStaticMesh* Reduced = FMantleMeshReduce::ReducedCopy(Mesh, Limits, Why);
			if (Reduced && CountMeshLOD(Reduced, 0, Smaller))
			{
				OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
					FString::Printf(TEXT("%s had %s, so Mantle reduced a copy to %s triangles for publishing. Your mesh is unchanged."),
						*Subject, *Over, *Thousands(Smaller.Triangles)),
					TEXT("Nothing to do. If the reduced mesh looks too rough, make a LOD that fits the limit in the mesh editor and Mantle publishes that instead.") });
				Triangles = Smaller.Triangles;
				Vertices = Smaller.Vertices;
			}
			else
			{
				ReduceFailure = Why.IsEmpty() ? FString(TEXT("it could not be reduced")) : Why;
				ReduceFailure.TrimStartAndEndInline();
				ReduceFailure.RemoveFromEnd(TEXT("."));
			}
		}
	}
	// What the creator can try when Mantle could not shrink the mesh itself.
	const FString ReduceNote = ReduceFailure.IsEmpty() ? FString() : FString::Printf(TEXT(" Mantle couldn't reduce it automatically: %s."), *ReduceFailure);

	if (Mesh->IsNaniteEnabled())
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
			FString::Printf(TEXT("%s uses Nanite, so it exports as its fallback mesh (%s triangles), which is what players will see."),
				*Subject, *Thousands(Triangles)),
			TEXT("If it looks too coarse up close, raise Fallback Triangle Percent in the mesh's Nanite settings.") });
	}
	if (Triangles > Limits.MaxTriangles)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s has %s triangles; a place mesh may have at most %s."),
				*Subject, *Thousands(Triangles), *Thousands(Limits.MaxTriangles)),
			FString(TEXT("Use a lower-detail version of it, or reduce it with the mesh editor's LOD tools (for Nanite meshes, lower Fallback Triangle Percent).")) + ReduceNote });
	}
	if (Vertices > Limits.MaxVertices)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s has %s vertices; a place mesh may have at most %s."),
				*Subject, *Thousands(Vertices), *Thousands(Limits.MaxVertices)),
			FString(TEXT("Use a lower-detail version of it.")) + ReduceNote });
	}
	if (Slots.Num() > Limits.MaxSections)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
			FString::Printf(TEXT("%s has %d material slots; a place mesh may have at most %d."), *Subject, Slots.Num(), Limits.MaxSections),
			FString::Printf(TEXT("Give it %d materials or fewer (Merge Actors can bake several into one)."), Limits.MaxSections) });
	}
}

void FMantlePlaceAssets::CheckMaterial(const UMaterialInterface* Material, const FString& UsedBy, const FMantlePlaceAssetLimits& Limits,
	TArray<FMantleValidationIssue>& OutIssues, TSet<const UTexture*>* ReportedTextures)
{
	using namespace MantlePlaceAssetsPrivate;
	if (!Material)
	{
		return;
	}
	const FMantleMaterialAnalysis Analysis = AnalyzeMaterial(Material);
	const FString Subject = WithUsers(TEXT("Material ") + Quoted(Material), UsedBy);
	for (uint32 Bit = 1; Bit != 0 && Bit <= static_cast<uint32>(EMantleMaterialLoss::NotSurface); Bit <<= 1)
	{
		const EMantleMaterialLoss Loss = static_cast<EMantleMaterialLoss>(Bit);
		if (Analysis.Has(Loss))
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
				FString::Printf(TEXT("%s: %s"), *Subject, *DescribeLoss(Loss, Analysis)), LossRemedy });
		}
	}
	if (Analysis.bCarPaint)
	{
		// A note, not a loss: the validator has no severity below a warning, and this never blocks.
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
			FString::Printf(TEXT("%s: it is a Clear Coat material, so it publishes as Mantle's car-paint shading (a clear coat over its base colour)."), *Subject),
			CarPaintRemedy });
	}
	if (Analysis.bPerInstanceVariation)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
			FString::Printf(TEXT("%s: %s"), *Subject, *DescribePerInstanceVariation()), LossRemedy });
	}
	// #628: notes, not losses of a feature: what an additive or see-through surface gives up in becoming Mantle's.
	if (Analysis.Fidelity2.bAdditiveLit)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
			FString::Printf(TEXT("%s: it is a lit additive material; Mantle draws additive blending unlit (the glow, with no lighting on the surface), so its lighting is lost."), *Subject),
			LossRemedy });
	}
	if (Analysis.Fidelity2.bAdditiveRim)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
			FString::Printf(TEXT("%s: its Fresnel rim glow is dropped, so the edges no longer glow brighter than the face (the baked glow is seen face-on, where a Fresnel is 0)."), *Subject),
			LossRemedy });
	}
	if (Analysis.Fidelity2.Glass.bFound)
	{
		OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(),
			FString::Printf(TEXT("%s: it draws the scene behind it (Scene Color), which Mantle cannot, so it publishes as tinted see-through glass."), *Subject),
			LossRemedy });
	}

	for (const TWeakObjectPtr<UTexture>& Weak : Analysis.Textures)
	{
		const UTexture* Texture = Weak.Get();
		if (!Texture || (ReportedTextures && ReportedTextures->Contains(Texture)))
		{
			continue;
		}
		FMantleValidationIssue Issue;
		if (!TextureDownscaleIssue(FString::Printf(TEXT("Texture %s (in material %s)"), *Quoted(Texture), *Quoted(Material)),
			TextureSize(Texture), Limits.MaxTextureSize, Issue))
		{
			continue;
		}
		if (ReportedTextures)
		{
			ReportedTextures->Add(Texture);
		}
		OutIssues.Add(MoveTemp(Issue));
	}
}

void FMantlePlaceAssets::CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues)
{
	const FMantlePlaceDependencies Dependencies = FMantlePlaceDependencies::Gather(World);
	if (!Dependencies.NeedsUploads())
	{
		return;
	}
	const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
	for (const FMantleGatheredMesh& Mesh : Dependencies.Meshes)
	{
		CheckMesh(Mesh.Mesh.Get(), DescribeUsers(Mesh.UsedBy), Limits, OutIssues);
	}
	TSet<const UTexture*> ReportedTextures;
	for (const FMantleGatheredMaterial& Material : Dependencies.Materials)
	{
		CheckMaterial(Material.Material.Get(), DescribeUsers(Material.UsedBy), Limits, OutIssues, &ReportedTextures);
	}
	// A decal's textures (#472) upload as they are, held to the same size rule, each once.
	for (const FMantleGatheredTexture& Gathered : Dependencies.Textures)
	{
		const UTexture* Texture = Gathered.Texture.Get();
		// A livery (#488) is its vehicle's to report (FMantleVehicleConverter::CheckSource), with the vehicle named.
		if (!Texture || Gathered.bLivery || ReportedTextures.Contains(Texture))
		{
			continue;
		}
		FMantleValidationIssue Issue;
		if (TextureDownscaleIssue(
				MantlePlaceAssetsPrivate::WithUsers(FString::Printf(TEXT("Decal texture %s"), *MantlePlaceAssetsPrivate::Quoted(Texture)), DescribeUsers(Gathered.UsedBy)),
				TextureSize(Texture), Limits.MaxTextureSize, Issue))
		{
			ReportedTextures.Add(Texture);
			OutIssues.Add(MoveTemp(Issue));
		}
	}
	// Each sound's own rule is checked per AmbientSound (FMantlePlaceExporter::ReadSoundEmitter); the place's budget here.
	CheckSounds(Dependencies, Limits, OutIssues);
	// #469: each skeletal mesh and clip once, however many actors use it (the per-actor playback rule is Check's).
	for (const FMantleGatheredSkeletalMesh& Mesh : Dependencies.SkeletalMeshes)
	{
		CheckSkeletalMesh(Mesh.Mesh.Get(), DescribeUsers(Mesh.UsedBy), Limits, OutIssues);
	}
	for (const FMantleGatheredClip& Clip : Dependencies.Clips)
	{
		CheckClip(Clip.Sequence.Get(), DescribeUsers(Clip.UsedBy), Limits, OutIssues);
	}
	// Creator vehicles (#435): each once, however many spawns park it.
	for (const FMantleGatheredVehicle& Vehicle : Dependencies.Vehicles)
	{
		FMantleVehicleConverter::CheckSource(Vehicle.Vehicle.Get(), DescribeUsers(Vehicle.UsedBy), OutIssues);
	}
	// Built weapons (#789): each once. Its 3D model is checked above, as any skeletal mesh is.
	for (const FMantleGatheredWeapon& Weapon : Dependencies.Weapons)
	{
		FString Text;
		const FString Problem = FFileHelper::LoadFileToString(Text, *AMantleWeaponSpawnPoint::BuiltDocumentFile(Weapon.Name))
			? WeaponDocumentProblem(Weapon.Name, Text) : FString(TEXT("has a settings file that cannot be read"));
		if (!Problem.IsEmpty())
		{
			OutIssues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(),
				FString::Printf(TEXT("The weapon '%s' %s (%s)."), *Weapon.Name, *Problem, *DescribeUsers(Weapon.UsedBy)),
				TEXT("Open Tools > Mantle > Build a weapon, press Create weapon again to remake it, then check the place again.") });
		}
	}
}
