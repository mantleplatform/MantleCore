// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNativeEye.h"
#include "Dom/JsonObject.h"

namespace MantleNativeEyePrivate
{
#include "MantleNativeEyeParameters.inl"

	bool Finite(const double Value) { return FMath::IsFinite(Value) && FMath::Abs(Value) <= 10000.0; }
	bool Allowed(const FString& Name, TConstArrayView<const TCHAR*> Names)
	{
		for (const TCHAR* Known : Names) if (Name.Equals(Known, ESearchCase::CaseSensitive)) return true;
		return false;
	}
}

TConstArrayView<const TCHAR*> FMantleNativeEye::ScalarParameters() { return MantleNativeEyePrivate::ScalarNames; }
TConstArrayView<const TCHAR*> FMantleNativeEye::VectorParameters() { return MantleNativeEyePrivate::VectorNames; }
TConstArrayView<const TCHAR*> FMantleNativeEye::TextureParameters() { return MantleNativeEyePrivate::TextureNames; }
TConstArrayView<const TCHAR*> FMantleNativeEye::ShaderTextureParameters() { return MantleNativeEyePrivate::ShaderTextureNames; }
TConstArrayView<const TCHAR*> FMantleNativeEye::SharedTextureParameters() { return MantleNativeEyePrivate::SharedTextureNames; }
TConstArrayView<const TCHAR*> FMantleNativeEye::BakedSwitchParameters() { return MantleNativeEyePrivate::SwitchNames; }

TConstArrayView<const TCHAR*> FMantleNativeEye::IrisPatterns() { return MantleNativeEyePrivate::IrisPatterns; }

FString FMantleNativeEye::MaterialPath(const FString& IrisPattern, bool bTint)
{
	using namespace MantleNativeEyePrivate;
	for (int32 I = 0; I < UE_ARRAY_COUNT(MantleNativeEyePrivate::IrisPatterns); ++I)
		if (IrisPattern.Equals(MantleNativeEyePrivate::IrisPatterns[I], ESearchCase::CaseSensitive)) return bTint ? IrisTintMaterials[I] : IrisBakedMaterials[I];
	return FString();
}

FString FMantleNativeEye::SharedTexturePath(FName Parameter, bool bAuthoring, const FString& IrisPattern)
{
	using namespace MantleNativeEyePrivate;
	if (MaterialPath(IrisPattern, false).IsEmpty()) return FString();
	for (int32 I = 0; I < UE_ARRAY_COUNT(MantleNativeEyePrivate::IrisPatterns); ++I)
	{
		if (!IrisPattern.Equals(MantleNativeEyePrivate::IrisPatterns[I], ESearchCase::CaseSensitive)) continue;
		if (Parameter == TEXT("Iris Pattern Masks")) return bAuthoring ? IrisSourceMasks[I] : IrisBundledMasks[I];
		if (Parameter == TEXT("Iris Normal")) return bAuthoring ? IrisSourceNormals[I] : IrisBundledNormals[I];
	}
	for (int32 I = 0; I < UE_ARRAY_COUNT(SharedTextureNames); ++I)
		if (Parameter == FName(SharedTextureNames[I])) return bAuthoring ? SharedSourcePaths[I] : SharedBundledPaths[I];
	return FString();
}

FString FMantleNativeEye::TextureRole(const FName Parameter)
{
	using namespace MantleNativeEyePrivate;
	for (int32 I = 0; I < UE_ARRAY_COUNT(TextureNames); ++I)
		if (Parameter == FName(TextureNames[I])) return TextureRoles[I];
	return FString();
}

bool FMantleNativeEye::ExpectedBakedSwitch(const FName Parameter, bool& OutValue)
{
	using namespace MantleNativeEyePrivate;
	for (int32 I = 0; I < UE_ARRAY_COUNT(SwitchNames); ++I)
		if (Parameter == FName(SwitchNames[I])) { OutValue = SwitchValues[I]; return true; }
	return false;
}

bool FMantleNativeEye::Validate(const FMantleNativeEyeSettings& Settings, FString& Error)
{
	using namespace MantleNativeEyePrivate;
	if (Settings.Version != Version || MaterialPath(Settings.IrisPattern, false).IsEmpty()
		|| Settings.Scalars.Num() != UE_ARRAY_COUNT(ScalarNames) || Settings.Vectors.Num() != UE_ARRAY_COUNT(VectorNames))
	{ Error = TEXT("Native eyes require the complete version 1 parameter set"); return false; }
	for (const TCHAR* Name : ScalarNames)
	{
		const float* Value = Settings.Scalars.Find(FName(Name));
		if (!Value || !Finite(*Value)) { Error = FString::Printf(TEXT("Native eye scalar %s is absent or invalid"), Name); return false; }
	}
	for (const TCHAR* Name : VectorNames)
	{
		const FLinearColor* Value = Settings.Vectors.Find(FName(Name));
		if (!Value || !Finite(Value->R) || !Finite(Value->G) || !Finite(Value->B) || !Finite(Value->A))
		{ Error = FString::Printf(TEXT("Native eye vector %s is absent or invalid"), Name); return false; }
	}
	return true;
}

bool FMantleNativeEye::ReadSettings(const TSharedPtr<FJsonObject>& Object, FMantleNativeEyeSettings& Out, FString& Error)
{
	using namespace MantleNativeEyePrivate;
	Out = FMantleNativeEyeSettings();
	double InputVersion = 0;
	const TSharedPtr<FJsonObject>* Scalars = nullptr;
	const TSharedPtr<FJsonObject>* Vectors = nullptr;
	if (!Object || !Object->TryGetNumberField(TEXT("version"), InputVersion) || InputVersion != Version
		|| !Object->TryGetStringField(TEXT("irisPattern"), Out.IrisPattern)
		|| !Object->TryGetObjectField(TEXT("scalars"), Scalars) || !Object->TryGetObjectField(TEXT("vectors"), Vectors))
	{ Error = TEXT("Native eye version/scalar/vector fields are required"); return false; }
	for (const auto& Pair : (*Scalars)->Values)
	{
		double Value = 0;
		if (!Allowed(FString(*Pair.Key), ScalarParameters()) || !Pair.Value || !Pair.Value->TryGetNumber(Value) || !Finite(Value))
		{ Error = TEXT("Unsupported or invalid native eye scalar"); return false; }
		Out.Scalars.Add(FName(*Pair.Key), static_cast<float>(Value));
	}
	for (const auto& Pair : (*Vectors)->Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
		if (!Allowed(FString(*Pair.Key), VectorParameters()) || !Pair.Value || !Pair.Value->TryGetArray(Channels) || Channels->Num() != 4)
		{ Error = TEXT("Native eye vectors require four values and a fixed parameter name"); return false; }
		float Values[4];
		for (int32 I = 0; I < 4; ++I)
		{
			double Value = 0;
			if (!(*Channels)[I] || !(*Channels)[I]->TryGetNumber(Value) || !Finite(Value)) { Error = TEXT("Invalid native eye vector channel"); return false; }
			Values[I] = static_cast<float>(Value);
		}
		Out.Vectors.Add(FName(*Pair.Key), FLinearColor(Values[0], Values[1], Values[2], Values[3]));
	}
	return Validate(Out, Error);
}

TSharedRef<FJsonObject> FMantleNativeEye::WriteSettings(const FMantleNativeEyeSettings& Settings)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>(), Scalars = MakeShared<FJsonObject>(), Vectors = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("version"), Settings.Version);
	Out->SetStringField(TEXT("irisPattern"), Settings.IrisPattern);
	for (const TCHAR* Name : ScalarParameters())
		if (const float* Value = Settings.Scalars.Find(FName(Name))) Scalars->SetNumberField(Name, *Value);
	for (const TCHAR* Name : VectorParameters())
	{
		if (const FLinearColor* Value = Settings.Vectors.Find(FName(Name)))
		{
			TArray<TSharedPtr<FJsonValue>> Channels;
			for (const float Channel : {Value->R, Value->G, Value->B, Value->A}) Channels.Add(MakeShared<FJsonValueNumber>(Channel));
			Vectors->SetArrayField(Name, Channels);
		}
	}
	Out->SetObjectField(TEXT("scalars"), Scalars); Out->SetObjectField(TEXT("vectors"), Vectors);
	return Out;
}
