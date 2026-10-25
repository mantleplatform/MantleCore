// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

class FJsonObject;

/** Resolved values for the fixed native MetaHuman eye shader. No shader path or switch is an input. */
struct MANTLERUNTIME_API FMantleNativeEyeSettings
{
	int32 Version = 1;
	FString IrisPattern = TEXT("i");
	TMap<FName, float> Scalars;
	TMap<FName, FLinearColor> Vectors;
};

/** Whitelist shared with the backend/schema. Textures are verified refs carried by the material. */
struct MANTLERUNTIME_API FMantleNativeEye
{
	static constexpr int32 Version = 1;
	static TConstArrayView<const TCHAR*> ScalarParameters();
	static TConstArrayView<const TCHAR*> VectorParameters();
	/** Four generated per-eye baked textures: the only uploaded native eye dependencies. */
	static TConstArrayView<const TCHAR*> TextureParameters();
	/** All eight shader inputs, including its four stock content references. */
	static TConstArrayView<const TCHAR*> ShaderTextureParameters();
	static TConstArrayView<const TCHAR*> SharedTextureParameters();
	/** Audited authoring or shipped stock path; never accepted from item JSON. */
	static FString SharedTexturePath(FName Parameter, bool bAuthoring, const FString& IrisPattern = TEXT("i"));
	static TConstArrayView<const TCHAR*> IrisPatterns();
	/** Fixed audited shader pair for a semantic stock pattern; empty for an unknown pattern. */
	static FString MaterialPath(const FString& IrisPattern, bool bTint);
	/** Fixed color/normal/data interpretation; empty for an unsupported parameter. */
	static FString TextureRole(FName Parameter);
	static TConstArrayView<const TCHAR*> BakedSwitchParameters();
	static bool ExpectedBakedSwitch(FName Parameter, bool& OutValue);
	static bool Validate(const FMantleNativeEyeSettings& Settings, FString& Error);
	static bool ReadSettings(const TSharedPtr<FJsonObject>& Object, FMantleNativeEyeSettings& Out, FString& Error);
	static TSharedRef<FJsonObject> WriteSettings(const FMantleNativeEyeSettings& Settings);
};
