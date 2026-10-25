// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNativeEyeExport.h"
#include "MantleAvatarConverter.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

bool FMantleNativeEyeExport::AllowsSharedProfile(const UObject* Body, const UObject* Face)
{
	if (!FMantleAvatarConverter::IsConvertOutput(Body) || !FMantleAvatarConverter::IsConvertOutput(Face)) return false;
	const FString* BodyBuilt = Body->GetPackage()->GetMetaData().FindValue(Body, FMantleAvatarConverter::BuiltKey);
	const FString* FaceBuilt = Face->GetPackage()->GetMetaData().FindValue(Face, FMantleAvatarConverter::BuiltKey);
	return BodyBuilt && FaceBuilt && !BodyBuilt->IsEmpty() && BodyBuilt->Equals(*FaceBuilt, ESearchCase::CaseSensitive);
}

bool FMantleNativeEyeExport::IsNative(const UMaterialInterface* Material)
{
	const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	if (!Base) return false;
	// Only the exact engine MetaHuman master is an audited native eye shader.
	// A custom graph renamed to the same family is still a generic material.
	const FString Path = Base->GetPathName();
	return Path == TEXT("/MetaHumanCharacter/Lookdev_UHM/Eye/Materials/M_eye_eyeball_unified.M_eye_eyeball_unified");
}

bool FMantleNativeEyeExport::Read(const UMaterialInterface* Material, FMantleNativeEyeSettings& Out,
	TMap<FName, UTexture2D*>& OutTextures, FString& Error)
{
	Out = FMantleNativeEyeSettings(); OutTextures.Reset();
	if (!IsNative(Material)) { Error = TEXT("Not the supported native MetaHuman eye material"); return false; }
	TArray<FMaterialParameterInfo> Switches; TArray<FGuid> Guids;
	Material->GetAllStaticSwitchParameterInfo(Switches, Guids);
	if (Switches.Num() != FMantleNativeEye::BakedSwitchParameters().Num())
	{ Error = TEXT("Native eye shader has an unsupported static parameter set"); return false; }
	for (const FMaterialParameterInfo& Parameter : Switches)
	{
		bool Expected = false, Actual = false; FGuid Guid;
		if (Parameter.Association != EMaterialParameterAssociation::GlobalParameter || !FMantleNativeEye::ExpectedBakedSwitch(Parameter.Name, Expected)
			|| !Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Parameter), Actual, Guid) || Actual != Expected)
		{ Error = FString::Printf(TEXT("Native eye static switch %s is unsupported by the fixed baked/procedural pair"), *Parameter.Name.ToString()); return false; }
	}
	for (const TCHAR* Parameter : FMantleNativeEye::ScalarParameters())
	{
		float Value = 0;
		if (!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(Parameter)), Value))
		{ Error = FString::Printf(TEXT("Native eye scalar %s could not be resolved"), Parameter); return false; }
		Out.Scalars.Add(FName(Parameter), Value);
	}
	for (const TCHAR* Parameter : FMantleNativeEye::VectorParameters())
	{
		FLinearColor Value;
		if (!Material->GetVectorParameterValue(FHashedMaterialParameterInfo(FName(Parameter)), Value))
		{ Error = FString::Printf(TEXT("Native eye vector %s could not be resolved"), Parameter); return false; }
		Out.Vectors.Add(FName(Parameter), Value);
	}
	for (const TCHAR* Parameter : FMantleNativeEye::TextureParameters())
	{
		UTexture* Texture = nullptr;
		if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(Parameter)), Texture) || !Cast<UTexture2D>(Texture))
		{ Error = FString::Printf(TEXT("Native eye texture %s must resolve to a source Texture2D"), Parameter); return false; }
		OutTextures.Add(FName(Parameter), CastChecked<UTexture2D>(Texture));
	}
	UTexture* Mask = nullptr; UTexture* Normal = nullptr;
	Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(TEXT("Iris Pattern Masks"))), Mask);
	Material->GetTextureParameterValue(FHashedMaterialParameterInfo(FName(TEXT("Iris Normal"))), Normal);
	Out.IrisPattern.Reset();
	for (const TCHAR* Pattern : FMantleNativeEye::IrisPatterns())
	{
		auto Matches = [Pattern](const UTexture* Texture, FName Parameter)
		{
			return Texture && (Texture->GetPathName() == FMantleNativeEye::SharedTexturePath(Parameter, true, Pattern)
				|| Texture->GetPathName() == FMantleNativeEye::SharedTexturePath(Parameter, false, Pattern));
		};
		if (Matches(Mask, TEXT("Iris Pattern Masks")) && Matches(Normal, TEXT("Iris Normal")))
		{ Out.IrisPattern = Pattern; break; }
	}
	if (Out.IrisPattern.IsEmpty())
	{
		Error = TEXT("Native eye iris mask and normal must be the matching audited stock texture pair a through i; custom or mixed pairs cannot be silently replaced.");
		return false;
	}
	for (const TCHAR* Parameter : FMantleNativeEye::SharedTextureParameters())
	{
		UTexture* Texture = nullptr;
		const FName Name(Parameter);
		if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Name), Texture) || !Texture
			|| (Texture->GetPathName() != FMantleNativeEye::SharedTexturePath(Name, true, Out.IrisPattern)
				&& Texture->GetPathName() != FMantleNativeEye::SharedTexturePath(Name, false, Out.IrisPattern)))
		{
			Error = FString::Printf(TEXT("Native eye %s must use its audited stock texture. Custom shared eye inputs are not supported by the fixed native profile; they cannot be silently replaced."), Parameter);
			return false;
		}
		// The game keeps its own copies of these stock textures (a client editor test checks they match MetaHuman
		// Creator's); a creator project has only MetaHuman's, so the path check above is the whole rule here.
	}
	return FMantleNativeEye::Validate(Out, Error);
}
