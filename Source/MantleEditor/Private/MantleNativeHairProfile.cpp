// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNativeHairProfile.h"
#include "MantleCosmeticItem.h"

#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MantleSha256.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Engine/Texture.h"
#include "MantleNativeHairRecipePins.inl"

bool FMantleNativeHairProfile::ValidateParameters(bool bFacial, const FString& BaseMaterialName, EBlendMode BlendMode,
	float MaskClip, TFunctionRef<bool(FName, float&)> Scalar, FString& Description, FString& Error)
{
	const TCHAR* Family = bFacial ? TEXT("M_Facial_Hair") : TEXT("M_hair_v4");
	if (!BaseMaterialName.Equals(Family, ESearchCase::CaseSensitive) || BlendMode != (bFacial ? BLEND_Translucent : BLEND_Masked)
		|| (!bFacial && (!FMath::IsFinite(MaskClip) || !FMath::IsNearlyEqual(MaskClip, .33f, .00001f))))
	{
		Error = TEXT("the native cards material family, blend mode or connected mask threshold is outside metahuman_cards_v1");
		return false;
	}
	Description = FString::Printf(TEXT("%s family=%s blend=%s"), Version, Family, bFacial ? TEXT("translucent") : TEXT("masked"));
	const TArray<TPair<FName, float>> Expected = {
		{FName(TEXT("OpacityNear")), bFacial ? .8f : 2.f}, {FName(TEXT("OpacityFar")), bFacial ? .4f : 2.f},
		{FName(TEXT("OpacityPowNear")), .5f}, {FName(TEXT("OpacityPowFar")), .5f}
	};
	for (const auto& Parameter : Expected)
	{
		float Actual = 0.f;
		if (!Scalar(Parameter.Key, Actual) || !FMath::IsFinite(Actual) || !FMath::IsNearlyEqual(Actual, Parameter.Value, .00001f))
		{
			Error = FString::Printf(TEXT("native cards parameter %s differs from supported %s (expected %.5f)"), *Parameter.Key.ToString(), Version, Parameter.Value);
			return false;
		}
		Description += FString::Printf(TEXT(" %s=%.5f"), *Parameter.Key.ToString(), Actual);
	}
	if (bFacial)
	{
		float RootFade = 0.f;
		if (!Scalar(FName(TEXT("RootFade")), RootFade) || !FMath::IsFinite(RootFade) || !FMath::IsNearlyEqual(RootFade, .7f, .00001f))
		{
			Error = TEXT("native facial RootFade differs from supported metahuman_cards_v1 (expected .7)");
			return false;
		}
		Description += FString::Printf(TEXT(" RootFade=%.5f"), RootFade);
	}
	else Description += FString::Printf(TEXT(" MaskClip=%.5f"), MaskClip);
	return true;
}

bool FMantleNativeHairProfile::ValidateShading(const FMaterialShadingModelField& Effective, const FMaterialShadingModelField& Base,
	ETranslucencyLightingMode SourceLighting, ETranslucencyLightingMode AuthoredLighting, FString& Error,
	const FMaterialShadingModelField* AuthoredModels)
{
	if (!Base.IsValid() || Effective != Base || (AuthoredModels && Base != *AuthoredModels) || SourceLighting != AuthoredLighting)
	{
		Error = TEXT("native cards shading-model override or facial translucency lighting differs from the authored metahuman_cards_v1 parent");
		return false;
	}
	return true;
}

bool FMantleNativeHairProfile::ClassifyFamily(const FString& BasePath, EBlendMode BlendMode, FString& Profile, FString& Error)
{
	Profile.Reset();
	const bool bMasked = BasePath == TEXT("/Game/MetaHumans/Common/Materials/M_hair_v4.M_hair_v4")
		|| BasePath == TEXT("/MetaHumanCharacter/Materials/M_hair_v4.M_hair_v4");
	const bool bTranslucent = BasePath == TEXT("/Game/MetaHumans/Common/Materials/M_Facial_Hair.M_Facial_Hair")
		|| BasePath == TEXT("/MetaHumanCharacter/Materials/M_Facial_Hair.M_Facial_Hair");
	if (bMasked && BlendMode == BLEND_Masked) Profile = MantleCosmetic::NativeHairMaskedCardsV1;
	else if (bTranslucent && BlendMode == BLEND_Translucent) Profile = MantleCosmetic::NativeHairTranslucentCardsV1;
	else { Error = TEXT("the effective cards parent or blend mode is outside the audited native shader families; use standalone cards with an artist-authored albedo/alpha for custom materials"); return false; }
	return true;
}

bool FMantleNativeHairProfile::ValidateFamilySwitches(bool bTranslucent, TFunctionRef<bool(FName, bool&)> Switch, FString& Error)
{
	TArray<TPair<FName, bool>> Expected = {{FName(TEXT("bIsHairCards")), !bTranslucent}, {FName(TEXT("ColorMask")), false}};
	if (!bTranslucent) Expected.Add({FName(TEXT("IsHelmet")), false});
	for (const auto& Parameter : Expected)
	{
		bool Actual = false;
		if (!Switch(Parameter.Key, Actual) || Actual != Parameter.Value)
		{
			Error = FString::Printf(TEXT("native cards static switch %s differs from the audited material family (expected %s); use an audited cards MI or an artist-authored alpha material"),
				*Parameter.Key.ToString(), Parameter.Value ? TEXT("true") : TEXT("false"));
			return false;
		}
	}
	return true;
}

bool FMantleNativeHairProfile::FromMaterialFamily(const UMaterialInterface* Material, FString& Profile, FString& Description, FString& Error)
{
	Profile.Reset();
	const UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	if (!Base) { Error = TEXT("the effective native cards material could not be resolved"); return false; }
	FString Candidate;
	if (!ClassifyFamily(Base->GetPathName(), Material->GetBlendMode(), Candidate, Error)) return false;
	const bool bTranslucent = Candidate == MantleCosmetic::NativeHairTranslucentCardsV1;
	FMaterialShadingModelField ExpectedModels(MSM_DefaultLit);
	if (!bTranslucent) ExpectedModels.AddShadingModel(MSM_Hair);
	if (!ValidateShading(Material->GetShadingModels(), Base->GetShadingModels(), Base->TranslucencyLightingMode,
		bTranslucent ? TLM_SurfacePerPixelLighting : TLM_VolumetricNonDirectional, Error, &ExpectedModels)) return false;
	if (!ValidateParameters(bTranslucent, Base->GetName(), Material->GetBlendMode(), Material->GetOpacityMaskClipValue(),
		[Material](FName Name, float& Value) { return Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), Value); }, Description, Error)) return false;
	if (!ValidateFamilySwitches(bTranslucent, [Material](FName Name, bool& Value)
		{ FGuid Guid; return Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Name), Value, Guid); }, Error)) return false;
	Description = Candidate + Description.Mid(FCString::Strlen(Version));
	Description += FString::Printf(TEXT(" instance=%s base=%s effectiveShading=%u translucencyLighting=%d bIsHairCards=%s ColorMask=false%s"),
		*Material->GetPathName(), *Base->GetPathName(), Material->GetShadingModels().GetShadingModelField(), int32(Base->TranslucencyLightingMode),
		bTranslucent ? TEXT("false") : TEXT("true"), bTranslucent ? TEXT("") : TEXT(" IsHelmet=false"));
	Profile = MoveTemp(Candidate);
	return true;
}

// The fixed v2 shader permits source colour controls; every other connected control is a measured recipe constant.
bool FMantleNativeHairProfile::FromMaterialRecipe(const UMaterialInterface* Material, FString& Profile,
    FMantleNativeHairSettings& Settings, FString& Description, FString& Error)
{
    Profile.Reset(); Settings = FMantleNativeHairSettings();
    FString Family;
    if (!FromMaterialFamily(Material, Family, Description, Error)) return false;
    const bool bTranslucent = Family == MantleCosmetic::NativeHairTranslucentCardsV1;
    const UMaterial* Base = Material->GetMaterial();
    if (!Base || !Base->bTangentSpaceNormal)
    { Error = TEXT("native v2 requires the measured tangent-space Normal/Tangent basis"); return false; }
    auto PinnedDependency = [&Error](const UObject* Object)
    {
        if (!Object) return true;
        const FString PackageName = Object->GetOutermost()->GetName(); const FString* Pin = NativeHairRecipePins().Find(PackageName);
        FString File; TArray<uint8> Bytes;
        if (!Pin || Object->GetOutermost()->IsDirty() || !FPackageName::DoesPackageExist(PackageName, &File)
            || !FFileHelper::LoadFileToArray(Bytes, *File) || FMantleSha256::HexDigest(Bytes) != *Pin)
        { Error = TEXT("native v2 function/texture dependency differs from the exact audited bytes: ") + PackageName; return false; }
        return true;
    };
    if (!PinnedDependency(Base))
    { Error = TEXT("native v2 parent differs from the exact audited material bytes or has unsaved graph edits"); return false; }
    TArray<UMaterialFunctionInterface*> Functions; Base->GetDependentFunctions(Functions);
    if (Functions.Num() > 1024) { Error = TEXT("native v2 function traversal exceeds its finite diagnostic bound"); return false; }
    for (const auto* Function : Functions) if (!PinnedDependency(Function)) return false;
    TSet<const UTexture*> Textures; Material->GetReferencedTexturesAndOverrides(Textures);
    for (const auto* Texture : Textures) if (!PinnedDependency(Texture)) return false;
    const TCHAR* ReferencePath = bTranslucent ? TEXT("/MetaHumanCharacter/Materials/MI_Facial_Hair.MI_Facial_Hair")
        : TEXT("/MetaHumanCharacter/Materials/MI_Hair_Cards.MI_Hair_Cards");
    UMaterialInterface* Reference = LoadObject<UMaterialInterface>(nullptr, ReferencePath);
    const TCHAR* ReferenceHash = bTranslucent ? TEXT("ef5f521c611e741ecb610df79769e92de8ffccbfb70192972a4687ae414e0369")
        : TEXT("c885fa585f81260b972b6fc77dfe5cafa378eb6a7e5f7d0008d95f621d3f11a0");
    FString ReferenceFile; TArray<uint8> ReferenceBytes;
    if (!Reference || Reference->GetOutermost()->IsDirty() || !FPackageName::DoesPackageExist(Reference->GetOutermost()->GetName(), &ReferenceFile)
        || !FFileHelper::LoadFileToArray(ReferenceBytes, *ReferenceFile) || FMantleSha256::HexDigest(ReferenceBytes) != ReferenceHash)
    { Error = TEXT("the exact native v2 reference recipe is unavailable or modified"); return false; }
    TArray<FMaterialParameterInfo> Scalars, Vectors; TArray<FGuid> Guids;
    Material->GetAllScalarParameterInfo(Scalars, Guids); Guids.Reset();
    Material->GetAllVectorParameterInfo(Vectors, Guids);
    auto Unique = [&Error](const TArray<FMaterialParameterInfo>& Infos, FName Name, FMaterialParameterInfo& Found)
    {
        int32 Count = 0;
        for (const auto& Info : Infos) if (Info.Name == Name) { Found = Info; ++Count; }
        if (Count != 1 || Found.Association != EMaterialParameterAssociation::GlobalParameter || Found.Index != INDEX_NONE)
        { Error = FString::Printf(TEXT("native v2 parameter %s requires one unique global identity; layer/index overrides are unsupported"), *Name.ToString()); return false; }
        return true;
    };
    auto Scalar = [&](FName Name, float& Value)
    {
        FMaterialParameterInfo Info;
        return Unique(Scalars, Name, Info) && Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Info), Value) && FMath::IsFinite(Value);
    };
    auto Vector = [&](FName Name, FLinearColor& Value)
    {
        FMaterialParameterInfo Info;
        return Unique(Vectors, Name, Info) && Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Info), Value);
    };
    if (!Scalar(FName(TEXT("hairMelanin")), Settings.Melanin) || !Scalar(FName(TEXT("hairRedness")), Settings.Redness)
        || !Vector(FName(TEXT("hairDye")), Settings.DyeColor))
    { if (Error.IsEmpty()) Error = TEXT("native v2 source melanin, redness or dye could not be resolved"); return false; }
    TArray<FMaterialParameterInfo> SwitchInfos; Guids.Reset(); Material->GetAllStaticSwitchParameterInfo(SwitchInfos, Guids);
    if (!ValidateFamilySwitches(bTranslucent, [&](FName Name, bool& Value)
        { FMaterialParameterInfo Info; FGuid Guid; return Unique(SwitchInfos, Name, Info) && Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Info), Value, Guid); }, Error)) return false;
    // Every effective source control must match the frozen recipe except the three explicitly transported colour controls.
    // This also catches unused or newly connected overrides rather than silently dropping settings not in the short display list below.
    auto IdentitySet = [&Error](const TArray<FMaterialParameterInfo>& Actual, const TArray<FMaterialParameterInfo>& ExpectedInfos)
    {
        if (Actual.Num() != ExpectedInfos.Num()) { Error = TEXT("native v2 source parameter set differs from the pinned recipe"); return false; }
        TSet<FName> Names;
        for (const FMaterialParameterInfo& Info : Actual)
        {
            if (Info.Association != EMaterialParameterAssociation::GlobalParameter || Info.Index != INDEX_NONE || Names.Contains(Info.Name))
            { Error = TEXT("native v2 source parameter has a duplicate/layered identity"); return false; }
            Names.Add(Info.Name);
            int32 Matches = 0;
            for (const auto& ExpectedInfo : ExpectedInfos) if (ExpectedInfo == Info) ++Matches;
            if (Matches != 1) { Error = TEXT("native v2 source parameter identity differs from the pinned recipe"); return false; }
        }
        return true;
    };
    TArray<FMaterialParameterInfo> ReferenceInfos; Guids.Reset(); Reference->GetAllScalarParameterInfo(ReferenceInfos, Guids);
    if (!IdentitySet(Scalars, ReferenceInfos)) return false;
    for (const auto& Info : Scalars)
    {
        if (Info.Name == TEXT("hairMelanin") || Info.Name == TEXT("hairRedness")) continue;
        float Actual = 0, ExpectedValue = 0;
        if (!Material->GetScalarParameterValue(FHashedMaterialParameterInfo(Info), Actual) || !Reference->GetScalarParameterValue(FHashedMaterialParameterInfo(Info), ExpectedValue)
            || !FMath::IsFinite(Actual) || !FMath::IsNearlyEqual(Actual, ExpectedValue, 1e-6f))
        { Error = TEXT("native v2 does not transport custom scalar control: ") + Info.Name.ToString(); return false; }
    }
    ReferenceInfos.Reset(); Guids.Reset(); Reference->GetAllVectorParameterInfo(ReferenceInfos, Guids);
    if (!IdentitySet(Vectors, ReferenceInfos)) return false;
    for (const auto& Info : Vectors)
    {
        if (Info.Name == TEXT("hairDye")) continue;
        FLinearColor Actual, ExpectedValue;
        if (!Material->GetVectorParameterValue(FHashedMaterialParameterInfo(Info), Actual) || !Reference->GetVectorParameterValue(FHashedMaterialParameterInfo(Info), ExpectedValue)
            || !Actual.Equals(ExpectedValue, 1e-6f))
        { Error = TEXT("native v2 does not transport custom vector control: ") + Info.Name.ToString(); return false; }
    }
    ReferenceInfos.Reset(); Guids.Reset(); Reference->GetAllStaticSwitchParameterInfo(ReferenceInfos, Guids);
    if (!IdentitySet(SwitchInfos, ReferenceInfos)) return false;
    for (const auto& Info : SwitchInfos)
    {
        bool Actual = false, ExpectedValue = false; FGuid Guid;
        if (!Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Info), Actual, Guid) || !Reference->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(Info), ExpectedValue, Guid)
            || Actual != ExpectedValue)
        { Error = TEXT("native v2 does not transport custom static switch: ") + Info.Name.ToString(); return false; }
    }
    TArray<FMaterialParameterInfo> TextureInfos; Guids.Reset(); Material->GetAllTextureParameterInfo(TextureInfos, Guids);
    ReferenceInfos.Reset(); Guids.Reset(); Reference->GetAllTextureParameterInfo(ReferenceInfos, Guids);
    if (!IdentitySet(TextureInfos, ReferenceInfos)) return false;
    for (const auto& Info : TextureInfos)
    {
        UTexture* Actual = nullptr; UTexture* ExpectedValue = nullptr;
        if (!Material->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), Actual) || !Reference->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), ExpectedValue)
            || Actual != ExpectedValue)
        { Error = TEXT("native v2 does not transport custom texture override: ") + Info.Name.ToString(); return false; }
    }
    TArray<FMaterialParameterInfo> MaskInfos; Guids.Reset(); Material->GetAllStaticComponentMaskParameterInfo(MaskInfos, Guids);
    if (!MaskInfos.IsEmpty()) { Error = TEXT("native v2 component-mask overrides are outside the frozen recipe"); return false; }
    // Native colour only reads RGB; catalog and shader transport have an explicit opaque RGB value.
    Settings.DyeColor.A = 1.f;
    if (!Settings.IsValid()) { Error = TEXT("native v2 melanin/redness/dye RGB must be finite in [0,1]"); return false; }
    TArray<TPair<FName, float>> Expected = {
        {TEXT("MelaninVariationFine"), .7f}, {TEXT("MelaninVariationRough"), .4f}, {TEXT("RedVariation"), 0.f},
        {TEXT("Region"), 0.f}, {TEXT("Ombre"), 0.f}, {TEXT("Highlights"), 0.f}, {TEXT("WhiteAmount"), 0.f}, {TEXT("Desat"), .1f},
        {TEXT("OpacityNear"), bTranslucent ? .8f : 2.f}, {TEXT("OpacityFar"), bTranslucent ? .4f : 2.f},
        {TEXT("OpacityPowNear"), .5f}, {TEXT("OpacityPowFar"), .5f}, {TEXT("PixelDepthOffset"), 1.f}
    };
    if (bTranslucent)
    {
        Expected.Append({{TEXT("RootFade"), .7f}, {TEXT("TangentNoiseTiling"), 16.f}, {TEXT("ScraggleFreq"), 20.f},
            {TEXT("ScraggleMobile"), .175806999f}, {TEXT("Rwidth"), .03f}, {TEXT("Roughness"), .611671984f},
            {TEXT("RoughnessFillLight"), .720000029f}, {TEXT("RIntensity"), .214433998f}, {TEXT("TRTwidth"), .035f},
            {TEXT("RoughnessTRT"), .389999986f}, {TEXT("TRTIntensity"), .1f}, {TEXT("fillLightSaturation"), -.1f},
            {TEXT("TRTSaturation"), 1.2f}, {TEXT("IntensityFillLight"), 0.f}, {TEXT("Intensity"), .0478489995f},
            {TEXT("LightDirection"), .738094986f}, {TEXT("RoughnessOverall"), .506193995f}});
        FLinearColor Fill;
        if (!Vector(FName(TEXT("FillVector")), Fill) || Fill.R != 0.f || Fill.G != 0.f || Fill.B != -1.f)
        { Error = TEXT("native facial v2 FillVector differs from the measured (0,0,-1) recipe"); return false; }
    }
    else Expected.Append({{TEXT("HairRoughness"), .37f}, {TEXT("RoughnessVariation"), .35f}, {TEXT("Spec0"), .45f},
        {TEXT("Spec1"), .75f}, {TEXT("SpecEdge"), .5f}, {TEXT("SpecFront"), .5f}, {TEXT("Scraggle"), -.05f}});
    for (const auto& Parameter : Expected)
    {
        float Value = 0;
        if (!Scalar(Parameter.Key, Value) || !FMath::IsNearlyEqual(Value, Parameter.Value, 1e-6f))
        { Error = FString::Printf(TEXT("native v2 fixed source control %s differs from the measured recipe; use an artist-authored material for custom controls"), *Parameter.Key.ToString()); return false; }
    }
    Profile = bTranslucent ? MantleCosmetic::NativeHairTranslucentCardsV2 : MantleCosmetic::NativeHairMaskedCardsV2;
    Description += FString::Printf(TEXT(" repairedProfile=%s melanin=%.9g redness=%.9g dye=(%.9g,%.9g,%.9g) uniqueGlobalParameters=true"),
        *Profile, Settings.Melanin, Settings.Redness, Settings.DyeColor.R, Settings.DyeColor.G, Settings.DyeColor.B);
    return true;
}
