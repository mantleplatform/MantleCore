// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMetaHumanBuild.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleAvatarHeight.h"
#include "MantleBodyTopologyData.h"
#include "MantleBodyCorrespondence.h"
#include "MantleMetaHumanConform.h"
#include "MantleMetaHumanPose.h"
#include "MantleRuntime.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Cloud/MetaHumanServiceRequest.h"
#include "DNA.h"
#include "DNAAsset.h"
#include "DNAUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "FileHelpers.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IAssetTools.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterBodyIdentity.h"
#include "MetaHumanCharacterEditorSettings.h"
#include "MetaHumanCharacterFactoryNew.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterPipelineSpecification.h"
#include "MetaHumanCollection.h"
#include "MetaHumanDefaultPipelineBase.h"
#include "MetaHumanInstance.h"
#include "MetaHumanRigEvaluatedState.h"
#include "MetaHumanTypes.h"
#include "MetaHumanWardrobeItem.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "RHIGlobals.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "SkelMeshDNAUtils.h"
#include "SkinnedAssetCompiler.h"
#include "Subsystem/MetaHumanCharacterBuild.h"
#include "TextureAssetActionsBP.h"
#include "UDynamicMesh.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

#include <atomic>

const TCHAR* const FMantleMetaHumanBuild::UnderwearMaskPath = TEXT("/MetaHumanCharacter/Lookdev_UHM/Common/Textures/Placeholders/T_Flat_Black_M.T_Flat_Black_M");

// Shown as a note in the fix list and the panel, with no tick box.
const TCHAR* const FMantleMetaHumanBuild::ConsentLine = TEXT("Uses Epic's MetaHuman cloud (your Epic account) to make the skin and face rig.");

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMetaHumanBuildPrivate
{
	/** Where MetaHuman Optional Content puts the texture synthesis model when the setting is empty. */
	const TCHAR* const BuildDefaultSynthesisDir = TEXT("/MetaHumanCharacter/Optional/TextureSynthesis");

	/** How long a sign-in answer is reused by the fix list (seconds). */
	constexpr double BuildSignInCacheSeconds = 30.0;

	struct FBuildSignInCache
	{
		TOptional<EMantleEpicSignIn> Last;
		double When = 0.0;
	};

	FBuildSignInCache& BuildSignIn()
	{
		static FBuildSignInCache Cache;
		return Cache;
	}

	/** Epic's sign-in check, pumped until it answers or SignInSeconds pass. Never logs in: no window ever opens. */
	EMantleEpicSignIn BuildAskSignIn()
	{
		// Shared with the callback, which may come after the wait gave up.
		struct FAnswer
		{
			std::atomic<int32> State{ -1 };
		};
		const TSharedRef<FAnswer, ESPMode::ThreadSafe> Answer = MakeShared<FAnswer, ESPMode::ThreadSafe>();
		UE::MetaHuman::ServiceAuthentication::CheckHasLoggedInUserAsync(
			UE::MetaHuman::ServiceAuthentication::FOnCheckHasLoggedInUserCompleteDelegate::CreateLambda(
				[Answer](const bool bLoggedIn, FString /*AccountId*/, FString /*UserName*/) { Answer->State = bLoggedIn ? 1 : 0; }));
		const double Until = FPlatformTime::Seconds() + FMantleMetaHumanBuild::SignInSeconds;
		while (Answer->State.load() < 0 && FPlatformTime::Seconds() < Until)
		{
			UE::MetaHuman::ServiceAuthentication::TickAuthClient();
			FPlatformProcess::Sleep(0.05f);
		}
		const int32 State = Answer->State.load();
		return State < 0 ? EMantleEpicSignIn::Unknown : State == 1 ? EMantleEpicSignIn::SignedIn : EMantleEpicSignIn::SignedOut;
	}

	/** Whether the texture synthesis model directory (a content path or a disk path) exists on disk. */
	bool BuildHasOptionalContent()
	{
		const UMetaHumanCharacterEditorSettings* Settings = GetDefault<UMetaHumanCharacterEditorSettings>();
		FString Dir = Settings ? Settings->TextureSynthesisModelDir.Path : FString();
		if (Dir.IsEmpty())
		{
			Dir = BuildDefaultSynthesisDir;
		}
		FString OnDisk;
		if (!FPackageName::TryConvertLongPackageNameToFilename(Dir.EndsWith(TEXT("/")) ? Dir : Dir + TEXT("/"), OnDisk))
		{
			OnDisk = Dir;
		}
		return IFileManager::Get().DirectoryExists(*FPaths::ConvertRelativePathToFull(OnDisk));
	}

	/** The mask override as the character stores it now: enabled, and Body_Underwear_Mask is Mask. */
	bool BuildMaskStored(const UMetaHumanCharacter& Character, const FSoftObjectPath& Mask)
	{
		const FMetaHumanCharacterTextureMaterialOverrides& Overrides = Character.SkinSettings.TextureMaterialOverrides;
		const TSoftObjectPtr<UTexture2D>* Stored = Overrides.TextureOverrides.Body.Find(EBodyTextureType::Body_Underwear_Mask);
		return Overrides.bEnableTextureOverrides && Stored && Stored->ToSoftObjectPath() == Mask;
	}

	/** Saves Packages (only those), recording their names; false when Epic's save refused. */
	bool BuildSave(const TArray<UPackage*>& Packages, TArray<FString>& InOutSaved)
	{
		if (Packages.Num() == 0)
		{
			return true;
		}
		for (const UPackage* Package : Packages)
		{
			InOutSaved.AddUnique(Package->GetName());
		}
		return UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ true);
	}

	/** Saves every dirty package under Folder (FMantleMetaHumanSteps::DirtyUnder) and nothing else. */
	bool BuildSaveUnder(const FString& Folder, TArray<FString>& InOutSaved)
	{
		const TArray<FString> Roots = { Folder + TEXT("/") };
		TArray<UPackage*> Packages;
		for (const FString& Name : FMantleMetaHumanSteps::DirtyUnder(Roots, FString()))
		{
			if (UPackage* Package = FindPackage(nullptr, *Name))
			{
				Packages.Add(Package);
			}
		}
		return BuildSave(Packages, InOutSaved);
	}

	/** Every package now in memory, by name. */
	TSet<FString> BuildPackageNames()
	{
		TSet<FString> Out;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			Out.Add(It->GetName());
		}
		return Out;
	}

	/** The assets under Folder (recursive), in memory or on disk. */
	TArray<FAssetData> BuildAssetsUnder(const FString& Folder)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive*/ true);
		return Assets;
	}

	/** A texture's longer source side (pixels). */
	int32 BuildTextureSide(const UTexture2D& Texture)
	{
		return static_cast<int32>(FMath::Max(Texture.Source.GetSizeX(), Texture.Source.GetSizeY()));
	}

	/** A mesh's LOD 0 vertices (bind pose) added to InOutPositions; false unread. */
	bool BuildAddPositions(const USkeletalMesh* Mesh, TArray<FVector3f>& InOutPositions)
	{
		TArray<FVector3f> Positions;
		FMantleBodyLODTopology Topology;
		if (!Mesh || !UMantleBodyTopologyData::ReadLOD(Mesh, 0, Topology, &Positions) || Positions.Num() == 0)
		{
			return false;
		}
		InOutPositions.Append(Positions);
		return true;
	}
}

FString FMantleMetaHumanBuildPreflight::Problem() const
{
	if (!bOptionalContent)
	{
		return TEXT("MetaHuman Optional Content isn't installed (Epic's skin texture synthesis needs it): add it from Fab or the Epic launcher");
	}
	switch (SignIn)
	{
	case EMantleEpicSignIn::SignedOut:
		return TEXT("Sign in to Epic in MetaHuman Creator first (Convert uses Epic's cloud for the skin and the face rig)");
	case EMantleEpicSignIn::Unknown:
		return TEXT("Couldn't reach Epic's sign-in to check you're signed in (Convert uses Epic's cloud for the skin and the face rig); try again");
	default:
		return FString();
	}
}

FMantleMetaHumanBuildPreflight FMantleMetaHumanBuild::Preflight(const bool bAskEpic)
{
	using namespace MantleMetaHumanBuildPrivate;
	FMantleMetaHumanBuildPreflight Out;
	Out.bOptionalContent = BuildHasOptionalContent();
	FBuildSignInCache& Cache = BuildSignIn();
	const double Now = FPlatformTime::Seconds();
	if (Cache.Last.IsSet() && Now - Cache.When <= BuildSignInCacheSeconds)
	{
		Out.SignIn = Cache.Last.GetValue();
	}
	else if (bAskEpic && Out.bOptionalContent)
	{
		Cache.Last = BuildAskSignIn();
		Cache.When = Now;
		Out.SignIn = Cache.Last.GetValue();
	}
	return Out;
}

bool FMantleMetaHumanBuild::IsSaveScope(const FString& CharacterFolder, const FString& CharacterPackage)
{
	if (!CharacterFolder.StartsWith(TEXT("/Game/")) || CharacterFolder.EndsWith(TEXT("/")) || CharacterFolder.Contains(TEXT("//"))
		|| CharacterFolder.Contains(TEXT("..")))
	{
		return false;
	}
	TArray<FString> Levels;
	CharacterFolder.ParseIntoArray(Levels, TEXT("/"), /*InCullEmpty*/ true);
	// "Game" and at least two folders below it.
	return Levels.Num() >= 3 && CharacterPackage.StartsWith(CharacterFolder + TEXT("/"));
}

namespace MantleMetaHumanBuildPrivate
{
	// Characters whose fixed body was scaled whole: the height steps after the first scale them again rather than setting a
	// Height constraint, which would re-solve the body.
	TSet<TObjectKey<UMetaHumanCharacter>>& WholeScaledBodies()
	{
		static TSet<TObjectKey<UMetaHumanCharacter>> Characters;
		return Characters;
	}

	// A creator's fixed body (their own sculpt) scaled uniformly to TargetCm, head included, instead of Parametric Fit, which
	// would swap a lean, muscled body for a soft, wide-hipped one. Epic's conform takes the body and head
	// vertices scaled about the floor, the joints are scaled with them (Epic's ImportBodyWholeRig order), and the stored body
	// and face rigs are removed: Epic's build makes its meshes from those DNAs when they exist, which kept the old size
	// (and a full-size face on the scaled body). The face is fitted to the new body, and the
	// build's auto-rig makes a new face rig from it.
	bool ScaleBodyWhole(UMetaHumanCharacter& Character, UMetaHumanCharacterEditorSubsystem& Subsystem, const double TargetCm, FString& OutError)
	{
		const TSharedRef<FMetaHumanCharacterEditorData>* Data = Subsystem.GetMetaHumanCharacterEditorData(&Character);
		USkeletalMesh* Body = Data ? (*Data)->BodyMesh.Get() : nullptr;
		USkeletalMesh* Face = Data ? (*Data)->FaceMesh.Get() : nullptr;
		const TArray<FMetaHumanCharacterBodyConstraint> Constraints = Subsystem.GetBodyConstraints(&Character);
		const FMetaHumanCharacterBodyConstraint* Height = Constraints.FindByPredicate([](const FMetaHumanCharacterBodyConstraint& Constraint)
		{
			return Constraint.Name == FName(TEXT("Height"));
		});
		if (!Body || !Face || !Height || Height->TargetMeasurement <= 1.0f)
		{
			OutError = TEXT("MetaHuman Creator gave the fixed body no meshes or height to scale it by");
			return false;
		}
		const double FromCm = Height->TargetMeasurement;
		TArray<FVector3f> Vertices, JointTranslations, JointRotations;
		if (Subsystem.GetMeshForBodyConformingFromTemplate(&Character, Body, Face, /*bInMatchVerticesByUVs*/ false, Vertices) != EImportErrorCode::Success
			|| Subsystem.GetJointsForBodyConformingFromTemplate(Body, JointTranslations, JointRotations) != EImportErrorCode::Success)
		{
			OutError = TEXT("MetaHuman Creator would not read the fixed body's vertices and joints");
			return false;
		}
		const float Scale = static_cast<float>(TargetCm / FromCm);
		for (FVector3f& Vertex : Vertices)
		{
			Vertex *= Scale;
		}
		for (FVector3f& Joint : JointTranslations)
		{
			Joint *= Scale;
		}
		if (!Subsystem.ConformBodyToTarget(&Character, Vertices, JointRotations, /*bTargetIsInAPose*/ true, /*bEstimateJointsFromMesh*/ false)
			|| !Subsystem.SetBodyJoints(&Character, JointTranslations, JointRotations, /*bImportHelperJoints*/ true))
		{
			OutError = TEXT("MetaHuman Creator would not conform the scaled fixed body");
			return false;
		}
		Subsystem.RemoveBodyRig(&Character);
		if (!Subsystem.FitFaceStateFromBodyWithEyesTeethTemplate(&Character, nullptr, nullptr, nullptr, /*bInMatchVerticesByUVs*/ false))
		{
			OutError = TEXT("MetaHuman Creator would not fit the face to the scaled fixed body");
			return false;
		}
		Subsystem.RemoveFaceRig(&Character);
		Subsystem.CommitBodyState(&Character);
		if (Character.HasBodyDNA() || Character.bFixedBodyType)
		{
			OutError = TEXT("MetaHuman Creator kept the fixed body's own rig after it was scaled");
			return false;
		}
		WholeScaledBodies().Add(&Character);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_SOURCE %s: fixed body scaled whole from %.1f to %.1f cm (x%.4f), body and face rigs removed, body committed"),
			*Character.GetName(), FromCm, TargetCm, Scale);
		return true;
	}
}

bool FMantleMetaHumanBuild::SetHeight(UObject& CharacterObject, const double TargetCm, bool& bOutFixed, FString& OutError)
{
	bOutFixed = false;
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(&CharacterObject);
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	if (!Character || !Subsystem)
	{
		OutError = !Character ? TEXT("there is no MetaHuman Character to set the height of") : TEXT("MetaHuman Creator isn't running: enable the MetaHuman Creator plugin");
		return false;
	}
	const bool bWasEditing = Subsystem->IsObjectAddedForEditing(Character);
	if (!bWasEditing && !Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator would not open the character for editing");
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (!bWasEditing && Subsystem->IsObjectAddedForEditing(Character))
		{
			Subsystem->RemoveObjectToEdit(Character);
		}
	};
	// The character's own public flag, which the subsystem's IsFixedBodyType reads (that function isn't exported from Epic's
	// module: it doesn't link).
	// A MetaHuman never goes As-is. A creator's own fixed body (from body DNA) is scaled whole, and the later
	// height steps scale it again.
	if ((Character->bFixedBodyType && Character->HasBodyDNA()) || MantleMetaHumanBuildPrivate::WholeScaledBodies().Contains(Character))
	{
		return MantleMetaHumanBuildPrivate::ScaleBodyWhole(*Character, *Subsystem, TargetCm, OutError);
	}
	if (Character->bFixedBodyType)
	{
		// One of Epic's fixed compatibility body types has no body DNA to scale, and taking the blendable body to the
		// scaled shape misbuilds it (the built feet end up off the floor). Epic's Parametric
		// Fit (the Body tool's button) fits the parametric body to it, so its height is set like any other's.
		if (!Subsystem->ParametricFitToCompatibilityBody(Character) || Character->bFixedBodyType)
		{
			bOutFixed = true;
			return true;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD %s: fixed compatibility body fitted to the parametric body (Parametric Fit)"), *Character->GetName());
	}
	// The stored body can stand off the floor (an older or imported state: the vertices can sit below the
	// joints), and SetBodyConstraints rewrites the vertices only. One evaluation with Epic's floor offset on,
	// committed in full, gives vertices and joints from the same pass, standing on the floor.
	TSharedRef<FMetaHumanCharacterBodyIdentity::FState> Grounded = Subsystem->CopyBodyState(Character);
	Grounded->SetEvaluatePose(false);
	Grounded->SetApplyFloorOffset(true);
	Subsystem->CommitBodyState(Character, Grounded, UMetaHumanCharacterEditorSubsystem::EBodyMeshUpdateMode::Full);
	TArray<FMetaHumanCharacterBodyConstraint> Constraints = Subsystem->GetBodyConstraints(Character);
	FMetaHumanCharacterBodyConstraint* Height = Constraints.FindByPredicate([](const FMetaHumanCharacterBodyConstraint& Constraint)
	{
		return Constraint.Name == FName(TEXT("Height"));
	});
	if (!Height)
	{
		OutError = TEXT("MetaHuman Creator gave the character no Height body constraint");
		return false;
	}
	// A creator's other active measurements are absolute cm (underbust, waist, neck base ...). Setting the height
	// alone keeps them at the old size, and a shorter body forced into them bunches up (a shelf under the
	// chest). They scale with the height, so the body keeps its author's proportions; the scale comes from the active Height, the
	// body's height those measurements were made for.
	const double FromCm = Height->bIsActive ? Height->TargetMeasurement : 0.0;
	int32 Scaled = 0;
	if (FromCm > 1.0 && !FMath::IsNearlyEqual(FromCm, TargetCm, 0.01))
	{
		const double Scale = TargetCm / FromCm;
		for (FMetaHumanCharacterBodyConstraint& Constraint : Constraints)
		{
			// Fat, Weight, Muscularity and Masculine/Feminine are not lengths: they keep their values.
			const FString Name = Constraint.Name.ToString();
			const bool bLength = Name != TEXT("Fat") && Name != TEXT("Weight") && !Name.Contains(TEXT("Muscul")) && !Name.Contains(TEXT("Masculine"));
			if (&Constraint != Height && Constraint.bIsActive && bLength)
			{
				Constraint.TargetMeasurement = FMath::Clamp(static_cast<float>(Constraint.TargetMeasurement * Scale), Constraint.MinMeasurement,
					FMath::Max(Constraint.MinMeasurement, Constraint.MaxMeasurement));
				++Scaled;
			}
		}
	}
	Height->bIsActive = true;
	Height->TargetMeasurement = static_cast<float>(TargetCm);
	Subsystem->SetBodyConstraints(Character, Constraints);
	Subsystem->CommitBodyState(Character);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_SOURCE %s: Height constraint %.1f cm (range %.1f to %.1f), %d other measurements scaled from %.1f cm, body committed"),
		*Character->GetName(), TargetCm, Height->MinMeasurement, Height->MaxMeasurement, Scaled, FromCm);
	return true;
}

bool FMantleMetaHumanBuild::MeasureEvaluatedHeight(UObject& CharacterObject, double& OutHeightCm, FString& OutError)
{
	OutHeightCm = -1.0;
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(&CharacterObject);
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	if (!Character || !Subsystem)
	{
		OutError = !Character ? TEXT("there is no MetaHuman Character to measure") : TEXT("MetaHuman Creator isn't running: enable the MetaHuman Creator plugin");
		return false;
	}
	const bool bWasEditing = Subsystem->IsObjectAddedForEditing(Character);
	if (!bWasEditing && !Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator would not open the character for editing");
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (!bWasEditing && Subsystem->IsObjectAddedForEditing(Character))
		{
			Subsystem->RemoveObjectToEdit(Character);
		}
	};
	const TSharedRef<const FMetaHumanCharacterBodyIdentity::FState> State = Subsystem->GetBodyState(Character);
	const TArray<int32> LODCounts = State->GetNumVerticesPerLOD();
	const FMetaHumanRigEvaluatedState Geometry = State->GetVerticesAndVertexNormals();
	if (LODCounts.IsEmpty() || LODCounts[0] <= 0 || LODCounts[0] > Geometry.Vertices.Num())
	{
		OutError = TEXT("MetaHuman Creator's evaluated body has no LOD0 vertices");
		return false;
	}
	const int32 Count = LODCounts[0];
	FBox3f Bounds(ForceInit);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Geometry.Vertices[Index].ContainsNaN())
		{
			OutError = TEXT("MetaHuman Creator's evaluated body has invalid geometry");
			return false;
		}
		Bounds += Geometry.Vertices[Index];
	}
	// Creator's evaluated state is in MetaHuman DNA space: Y up, Z forward (Z is the body's depth, about 35 cm), unlike
	// the built Unreal meshes, which are Z up. The standing height is the Y extent.
	const FVector3f Size = Bounds.GetSize();
	OutHeightCm = Size.Y;
	// The state's joints are in its bind pose (CopyBindPose, matrices in DNA joint order) without their names, so the pelvis
	// can't be found by name here: logged as n/a.
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_HEIGHT_PREDICTION %s evaluated LOD0 extents: X %.3f, Y (height) %.3f, Z %.3f cm, lowest %.3f cm, pelvis n/a"),
		*Character->GetName(), Size.X, Size.Y, Size.Z, Bounds.Min.Y);
	return true;
}

bool FMantleMetaHumanBuild::SetPredictedHeight(UObject& Character, const double TargetCm, double& OutRequestedCm, double& OutPredictedCm, bool& bOutFixed, FString& OutError)
{
	OutRequestedCm = TargetCm;
	OutPredictedCm = -1.0;
	if (!SetHeight(Character, TargetCm, bOutFixed, OutError))
	{
		return false;
	}
	if (bOutFixed)
	{
		return true;
	}
	double Request = TargetCm;
	double LastPredicted = -1.0;
	// One more measurement than corrections: the last one measures the last correction rather than leaving it unmeasured.
	for (int32 Step = 1; Step <= FMantleAvatarHeight::MaxPredictionSteps + 1; ++Step)
	{
		double MeasuredCm = -1.0;
		FString MeasureError;
		if (!MeasureEvaluatedHeight(Character, MeasuredCm, MeasureError))
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_HEIGHT_PREDICTION unavailable: %s; building on the request so far"), *MeasureError);
			LastPredicted = -1.0;
			break;
		}
		const double Predicted = MeasuredCm + FMantleAvatarHeight::PredictionOffsetCm;
		if (!FMantleAvatarHeight::IsPlausiblePrediction(Predicted, TargetCm))
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_HEIGHT_PREDICTION unavailable: evaluated %.3f cm is not within %.1f cm of the %.1f cm target; building on the request so far"),
				Predicted, FMantleAvatarHeight::PredictionPlausibleCm, TargetCm);
			LastPredicted = -1.0;
			break;
		}
		LastPredicted = Predicted;
		if (Step > FMantleAvatarHeight::MaxPredictionSteps || !FMantleAvatarHeight::NeedsRetarget(Predicted, TargetCm))
		{
			break;
		}
		const double Next = FMantleAvatarHeight::RetargetCm(Request, Predicted, TargetCm);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_HEIGHT_PREDICTION step %d: evaluated %.3f cm for a %.3f cm request; requesting %.3f cm"), Step, Predicted, Request, Next);
		bool bStillFixed = false;
		if (!SetHeight(Character, Next, bStillFixed, OutError))
		{
			return false;
		}
		Request = Next;
	}
	OutRequestedCm = Request;
	OutPredictedCm = LastPredicted;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_HEIGHT_PREDICTION %s: request %.3f cm, predicted %.3f cm (target %.1f)"), *Character.GetName(), Request, LastPredicted, TargetCm);
	return true;
}

namespace MantleMetaHumanBuildPrivate
{
	// A creator's own garment can sit in the collection's generic SkeletalMesh slot instead of Outfits (a cloak or boots).
	// It is clothing when its mesh is skinned to the body (it has leg or arm joints); a head or helper mesh there is not.
	bool IsBodySkinnedMeshItem(const FMetaHumanPaletteItemKey& Key)
	{
		FEditorOnlyAssetReference Asset;
		if (Key.ReferencesExternalWardrobeItem())
		{
			FEditorOnlyAssetReference ItemRef;
			const UMetaHumanWardrobeItem* Item = Key.TryGetExternalWardrobeItem(ItemRef) ? Cast<UMetaHumanWardrobeItem>(ItemRef.LoadSynchronous()) : nullptr;
			if (!Item) { return false; }
			Asset = Item->PrincipalAsset;
		}
		else if (!Key.TryGetPrincipalAsset(Asset))
		{
			return false;
		}
		const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.LoadSynchronous());
		if (!Mesh) { return false; }
		const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
		return Skeleton.FindBoneIndex(TEXT("thigh_l")) != INDEX_NONE || Skeleton.FindBoneIndex(TEXT("upperarm_l")) != INDEX_NONE;
	}

	bool ReadClothingSelections(const UMetaHumanCharacter& CharacterSource, TArray<FMetaHumanPipelineSlotSelection>& OutClothing, FString& OutError)
	{
		const UMetaHumanCollection* Collection = CharacterSource.GetInternalCollection();
		if (!Collection || !Collection->GetPipeline())
		{
			OutError = TEXT("the copied MetaHuman has no collection pipeline to identify its clothing slots");
			return false;
		}
		const UMetaHumanCharacterPipelineSpecification* Specification = Collection->GetPipeline()->GetSpecification();
		const UMetaHumanInstance* DefaultInstance = Collection->GetDefaultInstance();
		// Creator promises non-null objects here (TNotNull). Keep a checked boundary before
		// dereferencing either object, so unsupported collection implementations fail explicitly.
		if (!Specification || !DefaultInstance)
		{
			OutError = TEXT("the copied MetaHuman collection has no pipeline specification or default instance");
			return false;
		}
		const TArray<FMetaHumanPipelineSlotSelectionData>& Selections = DefaultInstance->GetSlotSelectionData();
		const auto IsCharacterSlot = [Specification](const FName SlotName)
		{
			using namespace UE::MetaHuman::CharacterPipelineSlots;
			const FName Resolved = Specification->ResolveRealSlotName(SlotName).Get(SlotName);
			return Resolved == Character || Resolved == Hair || Resolved == Eyebrows || Resolved == Beard
				|| Resolved == Mustache || Resolved == Eyelashes || Resolved == Peachfuzz;
		};
		TArray<FMetaHumanPaletteItemPath> OutfitPaths;
		for (const FMetaHumanPipelineSlotSelectionData& Data : Selections)
		{
			if (IsCharacterSlot(Data.Selection.SlotName)) { continue; }
			const TOptional<FName> RealSlot = Specification->ResolveRealSlotName(Data.Selection.SlotName);
			if ((RealSlot.IsSet() && RealSlot.GetValue() == UMetaHumanDefaultPipelineBase::OutfitsSlotName)
				|| Data.Selection.SlotName == UMetaHumanDefaultPipelineBase::TopGarmentSlotName
				|| Data.Selection.SlotName == UMetaHumanDefaultPipelineBase::BottomGarmentSlotName
				|| (Data.Selection.ParentItemPath.IsEmpty() && RealSlot.IsSet() && RealSlot.GetValue() == UMetaHumanDefaultPipelineBase::SkeletalMeshSlotName
					&& IsBodySkinnedMeshItem(Data.Selection.SelectedItem)))
			{
				OutClothing.Add(Data.Selection);
				OutfitPaths.Add(Data.Selection.GetSelectedItemPath());
			}
		}
		for (const FMetaHumanPipelineSlotSelectionData& Data : Selections)
		{
			if (IsCharacterSlot(Data.Selection.SlotName)) { continue; }
			// Child choices belong to their selected outfit. A generic root SkeletalMesh slot is
			// deliberately preserved unless its mesh is skinned to the body (IsBodySkinnedMeshItem): it can carry a head/helper mesh.
			if (OutfitPaths.ContainsByPredicate([&Data](const FMetaHumanPaletteItemPath& Outfit)
				{ return Data.Selection.ParentItemPath.IsEqualOrChildPathOf(Outfit); }))
			{
				OutClothing.AddUnique(Data.Selection);
			}
		}
		return true;
	}
}

bool FMantleMetaHumanBuild::PrepareUnclothedSourceCopy(UObject& CopyObject, const UObject& SourceObject, int32& OutRemovedSelections, FString& OutError)
{
	OutRemovedSelections = 0;
	UMetaHumanCharacter* Copy = Cast<UMetaHumanCharacter>(&CopyObject);
	const UMetaHumanCharacter* Source = Cast<UMetaHumanCharacter>(&SourceObject);
	if (!Copy || !Source || Copy == Source || !Copy->GetInternalCollection() || !Source->GetInternalCollection()
		|| Copy->GetInternalCollection() == Source->GetInternalCollection())
	{
		OutError = TEXT("clothing can be removed only from a distinct MetaHuman source copy with its own collection and instance");
		return false;
	}
	const UMetaHumanInstance* SourceInstance = Source->GetInternalCollection()->GetDefaultInstance();
	UMetaHumanCollection* CopyCollection = Copy->GetMutableInternalCollection();
	if (!CopyCollection)
	{
		OutError = TEXT("the MetaHuman source copy has no mutable collection");
		return false;
	}
	UMetaHumanInstance* CopyInstance = CopyCollection->GetMutableDefaultInstance();
	if (!SourceInstance || !CopyInstance || CopyInstance == SourceInstance)
	{
		OutError = TEXT("the MetaHuman source copy shares the original instance; clothing cannot be removed");
		return false;
	}
	TArray<FMetaHumanPipelineSlotSelection> Clothing;
	if (!MantleMetaHumanBuildPrivate::ReadClothingSelections(*Copy, Clothing, OutError)) { return false; }
	UMetaHumanInstance* Instance = CopyInstance;
	const TArray<FMetaHumanPipelineSlotSelectionData> Before = Instance->GetSlotSelectionData();
	for (const FMetaHumanPipelineSlotSelection& Selection : Clothing)
	{
		// Native SetSingleSlotSelection(null) removes entries; Character build then pins unused slots empty.
		Instance->SetSingleSlotSelection(Selection.ParentItemPath, Selection.SlotName, FMetaHumanPaletteItemKey());
	}
	for (const FMetaHumanPipelineSlotSelectionData& Data : Before)
	{
		const bool bStillSelected = Instance->ContainsSlotSelection(Data.Selection);
		if (Clothing.Contains(Data.Selection))
		{
			if (bStillSelected) { OutError = TEXT("MetaHuman Creator retained a clothing selection on the source copy"); return false; }
			++OutRemovedSelections;
		}
		else if (!bStillSelected)
		{
			OutError = TEXT("removing clothing would also remove an unrelated MetaHuman selection");
			return false;
		}
	}
	Copy->MarkPackageDirty();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_SOURCE %s: removed %d clothing selections from the copy before Epic build"), *Copy->GetName(), OutRemovedSelections);
	return CheckUnclothedSource(*Copy, OutError);
}

bool FMantleMetaHumanBuild::CheckUnclothedSource(const UObject& CharacterObject, FString& OutError)
{
	const UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(&CharacterObject);
	TArray<FMetaHumanPipelineSlotSelection> Clothing;
	if (!Character || !MantleMetaHumanBuildPrivate::ReadClothingSelections(*Character, Clothing, OutError)) { return false; }
	if (!Clothing.IsEmpty())
	{
		OutError = TEXT("Epic's build reintroduced clothing selections on the MetaHuman source copy");
		return false;
	}
	return true;
}

bool FMantleMetaHumanBuild::CheckCompleteBody(const USkeletalMesh* Body, FString& OutError)
{
	const FSkeletalMeshRenderData* RenderData = Body ? Body->GetResourceForRendering() : nullptr;
	if (!RenderData || RenderData->LODRenderData.Num() != FMantleBodyCorrespondence::ExpectedNativeLODs)
	{
		OutError = TEXT("the native Optimized Medium body must contain all three complete LODs before hand-off");
		return false;
	}
	for (int32 LODIndex = 0; LODIndex < FMantleBodyCorrespondence::ExpectedNativeLODs; ++LODIndex)
	{
		const FMantleBodyCorrespondence* Reference = FMantleBodyCorrespondence::GetLOD(LODIndex);
		FMantleBodyLODTopology Topology;
		TArray<FVector2f> UVs;
		FString Detail;
		if (!Reference || !UMantleBodyTopologyData::ReadLOD(Body, LODIndex, Topology, nullptr, &UVs, &Detail)
			|| !RenderData->LODRenderData[LODIndex].MultiSizeIndexContainer.IsIndexBufferValid())
		{
			OutError = FString::Printf(TEXT("body LOD%d has no complete native reference or readable geometry: %s"), LODIndex, *Detail);
			return false;
		}
		TArray<uint32> Indices;
		RenderData->LODRenderData[LODIndex].MultiSizeIndexContainer.GetIndexBuffer(Indices);
		TArray<int32> Mapping;
		bool bMatches = false;
		if (Reference->bLowerCornerTopology)
		{
			const FSkeletalMeshLODRenderData& Native = RenderData->LODRenderData[LODIndex];
			const FSkinWeightVertexBuffer* Skin = Native.GetSkinWeightVertexBuffer();
			const int32 Count = UVs.Num();
			TArray<FVector3f> Positions; TArray<uint16> Bones; TArray<float> Weights;
			TArray<int32> SectionOfVertex; SectionOfVertex.Init(INDEX_NONE, Count);
			bool bValid = Skin && Skin->GetNumVertices() == static_cast<uint32>(Count)
				&& Native.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() == static_cast<uint32>(Count)
				&& Skin->GetMaxBoneInfluences() > 0 && Skin->GetMaxBoneInfluences() <= 32;
			for (int32 SectionIndex = 0; SectionIndex < Native.RenderSections.Num(); ++SectionIndex)
			{
				const FSkelMeshRenderSection& Section = Native.RenderSections[SectionIndex];
				const uint64 End = static_cast<uint64>(Section.BaseVertexIndex) + Section.NumVertices;
				if (End > static_cast<uint64>(Count)) { bValid = false; break; }
				for (uint32 Vertex = Section.BaseVertexIndex; Vertex < End; ++Vertex)
				{
					if (SectionOfVertex[Vertex] != INDEX_NONE) bValid = false;
					SectionOfVertex[Vertex] = SectionIndex;
				}
			}
			const int32 NumInfluences = bValid ? static_cast<int32>(Skin->GetMaxBoneInfluences()) : 0;
			for (int32 Vertex = 0; Vertex < Count && bValid; ++Vertex)
			{
				if (SectionOfVertex[Vertex] == INDEX_NONE) { bValid = false; break; }
				const auto& BoneMap = Native.RenderSections[SectionOfVertex[Vertex]].BoneMap;
				Positions.Add(Native.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex));
				uint32 Offset = 0, InfluenceCount = 0; Skin->GetVertexInfluenceOffsetCount(Vertex, Offset, InfluenceCount);
				if (InfluenceCount > static_cast<uint32>(NumInfluences)) { bValid = false; break; }
				for (int32 Influence = 0; Influence < NumInfluences; ++Influence)
				{
					const uint16 Weight = static_cast<uint32>(Influence) < InfluenceCount ? Skin->GetBoneWeight(Vertex, Influence) : 0;
					const uint32 LocalBone = Weight ? Skin->GetBoneIndex(Vertex, Influence) : 0;
					if (Weight && (LocalBone >= static_cast<uint32>(BoneMap.Num()) || BoneMap[LocalBone] >= Body->GetRefSkeleton().GetRawBoneNum())) { bValid = false; break; }
					Bones.Add(Weight ? BoneMap[LocalBone] : 0); Weights.Add(static_cast<float>(Weight));
				}
			}
			bMatches = bValid && Reference->MatchCorners(UVs, Indices, Mapping, Detail)
				&& Reference->ValidateCornerCopies(Mapping, Positions, Bones, Weights, NumInfluences, Detail);
			if (!bValid) Detail = TEXT("native lower corner position/section/global skin streams are malformed");
		}
		else
		{
			bMatches = Reference->Match(UVs, Indices, Mapping, Detail);
			if (!bMatches && LODIndex == 0 && UVs.Num() > Reference->QuantizedUVs.Num())
			{
				// Validate an export-only quotient. The native body and diagnostic counts stay intact.
				TArray<int32> ReferenceOfInput; TArray<FString> MergedSplits;
				bMatches = Reference->MatchMergedBodyExport(Body, ReferenceOfInput, Mapping, MergedSplits, Detail);
			}
			else if (!bMatches && LODIndex > 0 && UVs.Num() > Reference->QuantizedUVs.Num())
			{
				// Unreal splits a vertex at a UV seam when the computed tangent differs; accept tangent-only twins.
				const FSkeletalMeshLODRenderData& Native = RenderData->LODRenderData[LODIndex];
				const FSkinWeightVertexBuffer* Skin = Native.GetSkinWeightVertexBuffer();
				const auto& Statics = Native.StaticVertexBuffers.StaticMeshVertexBuffer;
				const int32 Count = UVs.Num();
				bool bValid = Skin && Skin->GetNumVertices() == static_cast<uint32>(Count)
					&& Native.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() == static_cast<uint32>(Count)
					&& Statics.GetNumVertices() == static_cast<uint32>(Count) && Statics.GetNumTexCoords() > 0
					&& Skin->GetMaxBoneInfluences() > 0 && Skin->GetMaxBoneInfluences() <= 32
					&& Native.RenderSections.Num() == 1 && Native.RenderSections[0].BaseVertexIndex == 0
					&& Native.RenderSections[0].NumVertices == static_cast<uint32>(Count);
				TArray<FVector3f> Positions, Normals; TArray<FVector2f> UV1s; TArray<uint16> Bones; TArray<float> Weights;
				const int32 NumInfluences = bValid ? static_cast<int32>(Skin->GetMaxBoneInfluences()) : 0;
				for (int32 Vertex = 0; Vertex < Count && bValid; ++Vertex)
				{
					const auto& BoneMap = Native.RenderSections[0].BoneMap;
					Positions.Add(Native.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex));
					Normals.Add(Statics.VertexTangentZ(Vertex));
					if (Statics.GetNumTexCoords() > 1) UV1s.Add(Statics.GetVertexUV(Vertex, 1));
					uint32 Offset = 0, InfluenceCount = 0; Skin->GetVertexInfluenceOffsetCount(Vertex, Offset, InfluenceCount);
					if (InfluenceCount > static_cast<uint32>(NumInfluences)) { bValid = false; break; }
					for (int32 Influence = 0; Influence < NumInfluences; ++Influence)
					{
						const uint16 Weight = static_cast<uint32>(Influence) < InfluenceCount ? Skin->GetBoneWeight(Vertex, Influence) : 0;
						const uint32 LocalBone = Weight ? Skin->GetBoneIndex(Vertex, Influence) : 0;
						if (Weight && (LocalBone >= static_cast<uint32>(BoneMap.Num()) || BoneMap[LocalBone] >= Body->GetRefSkeleton().GetRawBoneNum())) { bValid = false; break; }
						Bones.Add(Weight ? BoneMap[LocalBone] : 0); Weights.Add(static_cast<float>(Weight));
					}
				}
				TArray<FColor> Colors;
				const auto& ColorBuffer = Native.StaticVertexBuffers.ColorVertexBuffer;
				if (bValid && ColorBuffer.GetNumVertices() != 0)
				{
					if (ColorBuffer.GetNumVertices() != static_cast<uint32>(Count)) bValid = false;
					else for (int32 Vertex = 0; Vertex < Count; ++Vertex) Colors.Add(ColorBuffer.VertexColor(Vertex));
				}
				if (bValid)
				{
					TArray<int32> ReferenceOfInput;
					bMatches = Reference->MatchTangentSplits(UVs, UV1s, Indices, Positions, Normals, Colors, Bones, Weights,
						NumInfluences, ReferenceOfInput, Mapping, Detail);
				}
			}
		}
		if (!bMatches)
		{
			OutError = FString::Printf(TEXT("body LOD%d is incomplete or differs from Epic's verified unclothed body: %s"), LODIndex, *Detail);
			return false;
		}
	}
	return true;
}

bool FMantleMetaHumanBuild::Build(UObject& CharacterObject, const FString& CharacterFolder, FMantleMetaHumanBuildResult& OutResult, FString& OutError,
	const bool bDeferHeightWarning, const double HeightToleranceCm)
{
	using namespace MantleMetaHumanBuildPrivate;
	OutResult = FMantleMetaHumanBuildResult();
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(&CharacterObject);
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	if (!Character || !Subsystem)
	{
		OutError = !Character ? TEXT("there is no MetaHuman Character to build") : TEXT("MetaHuman Creator isn't running: enable the MetaHuman Creator plugin");
		return false;
	}
	// 0. The save scope: everything this step saves lies under CharacterFolder, so it must be the character's own folder.
	if (!IsSaveScope(CharacterFolder, Character->GetPackage()->GetName()))
	{
		OutError = FString::Printf(TEXT("the build saves only under the character's own folder, and \"%s\" isn't one for %s"), *CharacterFolder,
			*Character->GetPackage()->GetName());
		return false;
	}
	// 1. The preflight, asking Epic now (a sign-out since the fix list was shown is caught): nothing goes to Epic unless
	// Optional Content and the sign-in are there.
	BuildSignIn().Last.Reset();
	const FString Problem = Preflight(/*bAskEpic*/ true).Problem();
	if (!Problem.IsEmpty())
	{
		OutError = Problem;
		return false;
	}
	UTexture2D* Mask = LoadObject<UTexture2D>(nullptr, UnderwearMaskPath);
	if (!Mask)
	{
		OutError = FString::Printf(TEXT("Epic's underwear mask %s is missing from the MetaHuman Creator plugin"), UnderwearMaskPath);
		return false;
	}

	// 2. Opened for editing (closed again only if this step opened it); the skin: Epic's centre skin tone, no underwear.
	const bool bWasEditing = Subsystem->IsObjectAddedForEditing(Character);
	if (!bWasEditing && !Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator would not open the character for editing");
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (!bWasEditing && Subsystem->IsObjectAddedForEditing(Character))
		{
			Subsystem->RemoveObjectToEdit(Character);
		}
	};
	FMetaHumanCharacterSkinSettings Skin = Character->SkinSettings;
	Skin.Skin.U = SkinToneU;
	Skin.Skin.V = SkinToneV;
	Skin.Skin.bShowTopUnderwear = false;
	// 5.8 reads TextureMaterialOverrides only (SkinSettings.TextureOverrides is deprecated and never read).
	Skin.TextureMaterialOverrides.TextureOverrides.Body.Add(EBodyTextureType::Body_Underwear_Mask, TSoftObjectPtr<UTexture2D>(Mask));
	Skin.TextureMaterialOverrides.bEnableTextureOverrides = true;
	Subsystem->CommitSkinSettings(Character, Skin);
	const FSoftObjectPath MaskPath(Mask);
	if (!BuildMaskStored(*Character, MaskPath) || Character->SkinSettings.Skin.bShowTopUnderwear)
	{
		OutError = TEXT("MetaHuman Creator did not keep the no-underwear settings (the underwear mask override or the top underwear switch)");
		return false;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD %s: skin committed (tone %.2f, %.2f; top underwear off; underwear mask %s stored)"), *Character->GetName(),
		Character->SkinSettings.Skin.U, Character->SkinSettings.Skin.V, *MaskPath.ToString());

	// 3. Epic's cloud: the skin's texture sources, then the face rig (blocking).
	FMetaHumanCharacterTextureRequestParams TextureRequest;
	TextureRequest.bReportProgress = false;
	TextureRequest.bBlocking = true;
	double Started = FPlatformTime::Seconds();
	Subsystem->RequestTextureSources(Character, TextureRequest);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD texture sources: %.1f s, high resolution %d"), FPlatformTime::Seconds() - Started,
		Character->HasHighResolutionTextures() ? 1 : 0);
	if (!Character->HasHighResolutionTextures())
	{
		OutError = TEXT("Epic's cloud gave no skin textures (sign-in, licence agreement, network or service). Nothing was built; try again.");
		return false;
	}
	FMetaHumanCharacterAutoRiggingRequestParams RigRequest;
	RigRequest.RigType = EMetaHumanRigType::JointsAndBlendShapes;
	RigRequest.bReportProgress = false;
	RigRequest.bBlocking = true;
	// The service answers 'Busy' at once while the same account's other auto-rig is running (two editors converting
	// side by side): a quick refusal waits and asks again, up to five more times; a slow failure is the service's real answer.
	EMetaHumanCharacterRigState Rig = EMetaHumanCharacterRigState::Unrigged;
	for (int32 Ask = 0; Ask < 6; ++Ask)
	{
		Started = FPlatformTime::Seconds();
		Subsystem->RequestAutoRigging(Character, RigRequest);
		Rig = Subsystem->GetRiggingState(Character);
		if (Rig == EMetaHumanCharacterRigState::Rigged || FPlatformTime::Seconds() - Started > 5.0)
		{
			break;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD auto-rig refused at once (state %d): asking again in 30 s"), static_cast<int32>(Rig));
		FPlatformProcess::Sleep(30.0f);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD auto-rig (joints and blend shapes): %.1f s, state %d"), FPlatformTime::Seconds() - Started, static_cast<int32>(Rig));
	if (Rig != EMetaHumanCharacterRigState::Rigged)
	{
		OutError = TEXT("Epic's auto-rig didn't finish (sign-in, licence agreement, network or service). Nothing was built; try again.");
		return false;
	}

	// 4. Saved, compiled, Epic's own go-ahead, then the build: Optimized / Medium on the default path.
	if (!BuildSave({ Character->GetPackage() }, OutResult.Saved))
	{
		OutError = FString::Printf(TEXT("%s could not be saved before the build"), *Character->GetPackage()->GetName());
		return false;
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	OutResult.bMaskStored = BuildMaskStored(*Character, MaskPath);
	if (!OutResult.bMaskStored)
	{
		OutError = TEXT("the underwear mask override was lost before the build");
		return false;
	}
	FText Refusal;
	if (!Subsystem->CanBuildMetaHuman(Character, Refusal))
	{
		OutError = FString::Printf(TEXT("MetaHuman Creator won't build this character: %s"), *Refusal.ToString());
		return false;
	}
	// What is there before the build: the root's assets (never discarded), every package, every dirty package.
	const FString Root = CharacterFolder / TEXT("Unpacked") / Character->GetName();
	TSet<FName> Before;
	for (const FAssetData& Asset : BuildAssetsUnder(Root))
	{
		Before.Add(Asset.PackageName);
	}
	const TSet<FString> PackagesBefore = BuildPackageNames();
	const TArray<FString> Everywhere = { TEXT("/") };
	const TSet<FString> DirtyBefore = FMantleMetaHumanSteps::DirtyUnder(Everywhere, CharacterFolder);
	FMetaHumanCharacterEditorBuildParameters BuildParameters;
	BuildParameters.PipelineType = EMetaHumanDefaultPipelineType::Optimized;
	BuildParameters.PipelineQuality = EMetaHumanQualityLevel::Medium;
	Started = FPlatformTime::Seconds();
	Subsystem->BuildMetaHuman(Character, BuildParameters);
	FAssetCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD build (Optimized / Medium, default path): %.1f s"), FPlatformTime::Seconds() - Started);

	// 5. Only our own packages saved (first); nothing left dirty anywhere else; then the root, found.
	const bool bSaved = BuildSaveUnder(CharacterFolder, OutResult.Saved);
	for (const FAssetData& Asset : BuildAssetsUnder(Root))
	{
		if (!Before.Contains(Asset.PackageName))
		{
			if (UObject* Object = Asset.GetAsset())
			{
				OutResult.Created.Add(Object);
			}
		}
	}
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("the built MetaHuman could not be saved under %s"), *CharacterFolder);
		return false;
	}
	FString Elsewhere;
	if (!FMantleMetaHumanSteps::CheckUnchanged(Everywhere, CharacterFolder, DirtyBefore, TEXT("Epic's build"), Elsewhere))
	{
		OutError = Elsewhere;
		return false;
	}
	const TArray<FAssetData> Built = BuildAssetsUnder(Root);
	if (Built.Num() == 0)
	{
		// Say where the build went instead: the packages it made.
		TArray<FString> Made;
		for (const FString& Name : BuildPackageNames())
		{
			if (!PackagesBefore.Contains(Name) && !Name.StartsWith(TEXT("/Engine/Transient")) && Made.Num() < 5)
			{
				Made.Add(Name);
			}
		}
		OutError = FString::Printf(TEXT("Epic's build put nothing in %s (it made %s)"), *Root, Made.Num() > 0 ? *FString::Join(Made, TEXT(", ")) : TEXT("nothing"));
		return false;
	}
	OutResult.BuildRoot = Root;
	for (const FAssetData& Asset : Built)
	{
		UObject* Object = Asset.GetAsset();
		const FString Short = Object ? Object->GetName() : FString();
		if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object))
		{
			if (Short.EndsWith(TEXT("_BodyMesh")))
			{
				OutResult.Body = Mesh;
			}
			else if (Short.EndsWith(TEXT("_FaceMesh")))
			{
				OutResult.Face = Mesh;
			}
		}
		UTexture2D* Texture = Cast<UTexture2D>(Object);
		if (!Texture)
		{
			continue;
		}
		const int32 Side = BuildTextureSide(*Texture);
		if (Side > MaxTextureSize)
		{
			const ETextureAssetActionsBlueprintReturn Resized = UTextureAssetActionsBlueprintFns::ResizeTextureSource(Texture, MaxTextureSize);
			if (Resized == ETextureAssetActionsBlueprintReturn::Error)
			{
				OutError = FString::Printf(TEXT("Epic's Resize Source failed on %s (%d)"), *Texture->GetPathName(), Side);
				return false;
			}
			OutResult.Resized.Add(FString::Printf(TEXT("%s %d to %d"), *Texture->GetPathName(), Side, BuildTextureSide(*Texture)));
			if (!BuildSave({ Texture->GetPackage() }, OutResult.Saved))
			{
				OutError = FString::Printf(TEXT("%s could not be saved after the resize"), *Texture->GetPackage()->GetName());
				return false;
			}
		}
		OutResult.LargestTexture = FMath::Max(OutResult.LargestTexture, BuildTextureSide(*Texture));
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD %s built in %s: %d assets (%d new), %d packages saved, resized %s; largest texture %d"), *Character->GetName(),
		*Root, Built.Num(), OutResult.Created.Num(), OutResult.Saved.Num(), OutResult.Resized.Num() > 0 ? *FString::Join(OutResult.Resized, TEXT(", ")) : TEXT("none"),
		OutResult.LargestTexture);
	if (OutResult.LargestTexture > MaxTextureSize)
	{
		OutError = FString::Printf(TEXT("a built texture is still %d after the resize (the limit is %d)"), OutResult.LargestTexture, MaxTextureSize);
		return false;
	}

	// 6. The built body: its height with its face against the default height (outside the
	// tolerance a warning, never a stop) and the guide's hand numbers, for the record.
	USkeletalMesh* Body = OutResult.Body.Get();
	USkeletalMesh* Face = OutResult.Face.Get();
	if (Body)
	{
		TArray<USkinnedAsset*> Compiling = { Body };
		if (Face)
		{
			Compiling.Add(Face);
		}
		FSkinnedAssetCompilingManager::Get().FinishCompilation(Compiling);
		// The z height over the body's and the face's vertices together.
		TArray<FVector3f> Positions;
		if (BuildAddPositions(Body, Positions) && BuildAddPositions(Face, Positions))
		{
			OutResult.BuiltHeightCm = FMantleAvatarHeight::ZHeight(Positions);
			OutResult.LowestPointCm = FMantleAvatarHeight::LowestZ(Positions);
		}
		FMantleMetaHumanConform::MeasureHands(*Body, OutResult.Hands);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD built body %s: height %.2f cm with its face; %s, lowest point %s cm"), Body ? *Body->GetPathName() : TEXT("(none)"),
		OutResult.BuiltHeightCm, *OutResult.Hands.ToString(),
		OutResult.LowestPointCm.IsSet() ? *FString::Printf(TEXT("%.2f"), OutResult.LowestPointCm.GetValue()) : TEXT("n/a"));
	const double TargetHeight = FMantleAvatarHeight::TargetCm();
	OutResult.bHeightWithin = FMantleAvatarHeight::IsWithin(OutResult.BuiltHeightCm, TargetHeight, HeightToleranceCm);
	OutResult.HeightLine = FMantleAvatarHeight::BuiltHeightLine(OutResult.BuiltHeightCm, TargetHeight, HeightToleranceCm);
	if (OutResult.bHeightWithin || bDeferHeightWarning)
	{
		// Retargeting records every measurement; its caller warns only about the final result.
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD %s"), *OutResult.HeightLine);
	}
	else
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_BUILD %s"), *OutResult.HeightLine);
	}
	if (!Body || !Face)
	{
		OutError = FString::Printf(TEXT("Epic's build made no body or face mesh under %s"), *Root);
		return false;
	}
	// The upload's feet check (CheckAvatarBodyBounds), at Convert: the height above is a max minus a min, so a body that sits wholly
	// below its own skeleton still measures right. A stop, not a height problem: the retarget loop breaks on a failed build.
	const FMantleAvatarContract* FeetContract = FMantleAvatarContract::Get();
	if (FeetContract && OutResult.LowestPointCm.IsSet() && !FMantleAvatarHeight::FeetOnFloor(OutResult.LowestPointCm.GetValue(), FeetContract->FeetToleranceCm))
	{
		const double Lowest = OutResult.LowestPointCm.GetValue();
		OutError = FString::Printf(TEXT("the built body's feet are %.1f cm %s the floor (%s); it can't be uploaded like this"), FMath::Abs(Lowest),
			Lowest < 0.0 ? TEXT("below") : TEXT("above"),
			Lowest < 0.0 ? TEXT("the mesh sits below its own skeleton") : TEXT("the mesh floats above its own skeleton"));
		return false;
	}
	return true;
}

bool FMantleMetaHumanBuild::FixedBodyCantBuild(const bool bFixedBody, const bool bBodyComplete, const bool bHeightWithin)
{
	return bFixedBody && (!bBodyComplete || !bHeightWithin);
}

bool FMantleMetaHumanBuild::IsMetaHumanAssembly(const FString& BodySkeleton, const FString& FaceSkeleton)
{
	return BodySkeleton.Contains(TEXT("metahuman_base_skel"), ESearchCase::IgnoreCase) && FaceSkeleton.Contains(TEXT("Face_Archetype_Skeleton"), ESearchCase::IgnoreCase);
}

FString FMantleMetaHumanBuild::AssemblyFitRefusal(const FString& MeshName, const FString& What, const FString& Code)
{
	return FString::Printf(TEXT("MetaHuman Creator's fit refused the %s of %s (%s): it needs MetaHuman topology. It is not uploaded as it is; a MetaHuman is always built."),
		*What, *MeshName, *Code);
}

FString FMantleMetaHumanBuild::AssemblyNotBuiltMessage(const FString& Name, const FString& Why)
{
	return FString::Printf(TEXT("%s is an assembled MetaHuman (meshes, no MetaHuman Creator character) that could not be built: %s. No build step ran on it."), *Name, *Why);
}

FString FMantleMetaHumanBuild::AssemblyFaceOnlyHint()
{
	return TEXT("Convert it again and pick Body: Put the face on a MetaHuman body, Women's or Men's.");
}

bool FMantleMetaHumanBuild::BodyLooksMergedWithClothing(const int32 BodyVertices, const int32 DnaVertices, const int32 ReferenceVertices)
{
	if (BodyVertices <= 0)
	{
		return false;
	}
	if (DnaVertices > 0 && static_cast<int64>(BodyVertices) * 2 > static_cast<int64>(DnaVertices) * 3)
	{
		return true;
	}
	return ReferenceVertices > 0 && static_cast<int64>(BodyVertices) > static_cast<int64>(ReferenceVertices) * 4;
}

int32 FMantleMetaHumanBuild::FaceDnaAssetIndex(const TConstArrayView<FString> DnaAssetNames)
{
	return DnaAssetNames.Num() == 1 ? 0 : INDEX_NONE;
}

namespace MantleMetaHumanBuildPrivate
{
	/** Bodies Epic's fit refused in this editor session (package paths). */
	TSet<FString>& RefusedBodies()
	{
		static TSet<FString> Refused;
		return Refused;
	}

	/**
	 * The face DNAs that may belong to Face, best first: the DNA the mesh carries (asset user data, UDNA or the legacy UDNAAsset),
	 * then the only DNA asset in the mesh's own folder (a large asset next to the mesh). OutSources names each.
	 */
	TArray<TSharedPtr<IDNAReader>> FaceDnaReaders(USkeletalMesh& Face, TArray<FString>& OutSources)
	{
		TArray<TSharedPtr<IDNAReader>> Readers;
		if (const TSharedPtr<IDNAReader> Attached = USkelMeshDNAUtils::GetDNAReader(&Face); Attached.IsValid())
		{
			Readers.Add(Attached);
			OutSources.Add(TEXT("the DNA on the face mesh"));
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> InFolder;
		Registry.GetAssetsByPath(FName(*FPackageName::GetLongPackagePath(Face.GetOutermost()->GetName())), InFolder, /*bRecursive*/ false);
		TArray<FAssetData> DnaAssets;
		TArray<FString> DnaNames;
		for (const FAssetData& Asset : InFolder)
		{
			if (Asset.AssetClassPath == UDNA::StaticClass()->GetClassPathName() || Asset.AssetClassPath == UDNAAsset::StaticClass()->GetClassPathName())
			{
				DnaAssets.Add(Asset);
				DnaNames.Add(Asset.AssetName.ToString());
			}
		}
		const int32 Only = FMantleMetaHumanBuild::FaceDnaAssetIndex(DnaNames);
		if (Only != INDEX_NONE)
		{
			UObject* Loaded = DnaAssets[Only].GetAsset();
			TSharedPtr<IDNAReader> Reader;
			if (UDNA* Dna = Cast<UDNA>(Loaded))
			{
				Reader = Dna->GetDNAReader();
			}
			else if (UDNAAsset* Legacy = Cast<UDNAAsset>(Loaded))
			{
				Reader = Legacy->GetDNAReader();
			}
			if (Reader.IsValid())
			{
				Readers.Add(Reader);
				OutSources.Add(FString::Printf(TEXT("the DNA asset %s"), *DnaAssets[Only].GetObjectPathString()));
			}
		}
		return Readers;
	}

	/**
	 * Fits Character's face to Face's DNA (Epic's parametric ImportFromFaceDna, not a whole-rig import: the character stays editable and
	 * builds like a template-fitted one). Each candidate is written to a temp .dna file, which is deleted. bOutHadDna is false when the
	 * face has no DNA at all (nothing was tried); otherwise the code is Success or the last refusal.
	 */
	EImportErrorCode ImportFaceFromDna(UMetaHumanCharacterEditorSubsystem& Subsystem, UMetaHumanCharacter& Character, USkeletalMesh& Face, FString& OutSource,
		bool& bOutHadDna)
	{
		TArray<FString> Sources;
		const TArray<TSharedPtr<IDNAReader>> Readers = FaceDnaReaders(Face, Sources);
		bOutHadDna = Readers.Num() > 0;
		EImportErrorCode Code = EImportErrorCode::InvalidInputData;
		for (int32 Index = 0; Index < Readers.Num(); ++Index)
		{
			const FString Directory = FPaths::ProjectIntermediateDir() / TEXT("MantleFaceDna");
			const FString File = Directory / FString::Printf(TEXT("%s_%s.dna"), *Face.GetName(), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
			WriteDNAToFile(Readers[Index].Get(), EDNADataLayer::All, File);
			ON_SCOPE_EXIT
			{
				IFileManager::Get().Delete(*File, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			};
			if (!IFileManager::Get().FileExists(*File))
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_SOURCE %s: %s could not be written to a temp .dna file"), *Face.GetPathName(), *Sources[Index]);
				continue;
			}
			FImportFromDNAParams Params;
			Params.bImportWholeRig = false;
			Code = Subsystem.ImportFromFaceDna(&Character, File, Params);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_SOURCE %s: face fitted from %s: code %d"), *Face.GetPathName(), *Sources[Index], static_cast<int32>(Code));
			if (Code == EImportErrorCode::Success)
			{
				OutSource = Sources[Index];
				return Code;
			}
		}
		return Code;
	}
}

void FMantleMetaHumanBuild::NoteBodyFitRefused(const FString& BodyPath)
{
	MantleMetaHumanBuildPrivate::RefusedBodies().Add(BodyPath);
}

bool FMantleMetaHumanBuild::BodyFitWasRefused(const FString& BodyPath)
{
	return MantleMetaHumanBuildPrivate::RefusedBodies().Contains(BodyPath);
}

bool FMantleMetaHumanBuild::AssemblyBodyNeedsFaceOnly(const FMantleConvertContext& Context, const int32 PartIndex, FString& OutWhy)
{
	USkeletalMesh* Body = Context.AssemblyBodyToFit.Get();
	if (!Body || !Context.AssemblyFaceToFit.IsValid() || Cast<UMetaHumanCharacter>(Context.Source.Get()) || !FMantleMetaHumanSourceFixer::IsCharacterToBuild(Context, PartIndex))
	{
		return false;
	}
	if (BodyFitWasRefused(Body->GetPathName()))
	{
		OutWhy = FString::Printf(TEXT("MetaHuman Creator's fit refused the body %s"), *Body->GetName());
		return true;
	}
	FMantleBodyLODTopology Topology;
	if (!UMantleBodyTopologyData::ReadLOD(Body, 0, Topology) || Topology.VertexCount <= 0)
	{
		return false;
	}
	int32 DnaVertices = 0;
	if (const TSharedPtr<IDNAReader> Dna = USkelMeshDNAUtils::GetDNAReader(Body); Dna.IsValid())
	{
		DnaVertices = Dna->GetVertexPositionCount(0);
	}
	const FMantleBodyCorrespondence* Reference = FMantleBodyCorrespondence::GetLOD(0);
	const int32 ReferenceVertices = Reference ? Reference->QuantizedUVs.Num() : 0;
	if (!BodyLooksMergedWithClothing(Topology.VertexCount, DnaVertices, ReferenceVertices))
	{
		return false;
	}
	OutWhy = FString::Printf(TEXT("%s has %d vertices, far more than a MetaHuman body (%d): clothing is merged into it"), *Body->GetName(), Topology.VertexCount,
		DnaVertices > 0 ? DnaVertices : ReferenceVertices);
	return true;
}

bool FMantleMetaHumanBuild::MakeCharacterFromAssembly(USkeletalMesh& Body, USkeletalMesh& Face, const FString& PackageName, UObject*& OutCharacter, FString& OutError,
	const bool bKeepFaceOnly, const bool bFeminineBody)
{
	OutCharacter = nullptr;
	UMetaHumanCharacterEditorSubsystem* Subsystem = UMetaHumanCharacterEditorSubsystem::Get();
	if (!Subsystem)
	{
		OutError = TEXT("MetaHuman Creator isn't running: enable the MetaHuman Creator plugin");
		return false;
	}
	auto CodeName = [](const EImportErrorCode Code)
	{
		const UEnum* Enum = StaticEnum<EImportErrorCode>();
		return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Code)) : FString::FromInt(static_cast<int32>(Code));
	};
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(AssetTools.CreateAsset(FPackageName::GetShortName(PackageName),
		FPackageName::GetLongPackagePath(PackageName), UMetaHumanCharacter::StaticClass(), NewObject<UMetaHumanCharacterFactoryNew>()));
	OutCharacter = Character;
	if (!Character)
	{
		OutError = FString::Printf(TEXT("a MetaHuman Creator character could not be made at %s"), *PackageName);
		return false;
	}
	if (!Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator would not open the new character for editing");
		return false;
	}
	bool bFitted = false;
	{
		ON_SCOPE_EXIT
		{
			if (Subsystem->IsObjectAddedForEditing(Character))
			{
				Subsystem->RemoveObjectToEdit(Character);
			}
		};
		// Some packs' UVs drift just past the engine's UV match: the face and body then retry matching by vertex order (the head
		// only has to match, not the eyes and teeth).
		FImportFromTemplateParams FaceParams;
		EImportErrorCode Code = Subsystem->ImportFromTemplate(Character, &Face, nullptr, nullptr, nullptr, FaceParams);
		if (Code != EImportErrorCode::Success)
		{
			FaceParams.bMatchVerticesByUVs = false;
			FaceParams.bUseEyeMeshes = false;
			FaceParams.bUseTeethMesh = false;
			Code = Subsystem->ImportFromTemplate(Character, &Face, nullptr, nullptr, nullptr, FaceParams);
		}
		// A reduced face mesh the template refuses: its face DNA, when it has one (see the header).
		bool bFaceFromDna = false;
		if (Code != EImportErrorCode::Success)
		{
			const EImportErrorCode TemplateCode = Code;
			FString DnaSource;
			bool bHadDna = false;
			const EImportErrorCode DnaCode = MantleMetaHumanBuildPrivate::ImportFaceFromDna(*Subsystem, *Character, Face, DnaSource, bHadDna);
			bFaceFromDna = DnaCode == EImportErrorCode::Success;
			Code = bFaceFromDna ? DnaCode : TemplateCode;
			if (bFaceFromDna)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_SOURCE %s: the face mesh does not fit by template (%s); fitted from %s instead"), *Face.GetPathName(),
					*CodeName(TemplateCode), *DnaSource);
			}
		}
		if (Code != EImportErrorCode::Success)
		{
			OutError = AssemblyFitRefusal(Face.GetName(), TEXT("face"), CodeName(Code));
			return false;
		}
		// A refused body is remembered, so the next Analyze offers the face-only choice.
		auto RefusedBody = [&Body, &OutError](const TCHAR* What, const FString& CodeText)
		{
			NoteBodyFitRefused(Body.GetPathName());
			OutError = AssemblyFitRefusal(Body.GetName(), What, CodeText) + TEXT(" ") + AssemblyFaceOnlyHint();
			return false;
		};
		if (bKeepFaceOnly)
		{
			// The face-only route on an assembly: the face is the character's, the body MetaHuman's own on the chosen scale; nothing of the
			// body mesh is read. The height is set by the build, as for any source.
			FString ChooseError;
			if (!FMantleMetaHumanConform::ChooseParametricBody(*Character, bFeminineBody, ChooseError))
			{
				OutError = FString::Printf(TEXT("%s: the %s body could not be chosen: %s"), *Face.GetName(), bFeminineBody ? TEXT("feminine") : TEXT("masculine"), *ChooseError);
				return false;
			}
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_SOURCE %s: keeping the face only, on MetaHuman's own %s body"), *Face.GetPathName(),
				bFeminineBody ? TEXT("feminine") : TEXT("masculine"));
			bFitted = true;
		}
		else
		{
			// A face fitted from DNA is not a head template for the body either (the same reduced mesh).
			UObject* HeadTemplate = bFaceFromDna ? nullptr : &Face;
			TArray<FVector3f> Vertices;
			Code = Subsystem->GetMeshForBodyConformingFromTemplate(Character, &Body, HeadTemplate, /*bInMatchVerticesByUVs*/ true, Vertices);
			if (Code != EImportErrorCode::Success)
			{
				Code = Subsystem->GetMeshForBodyConformingFromTemplate(Character, &Body, HeadTemplate, /*bInMatchVerticesByUVs*/ false, Vertices);
			}
			if (Code != EImportErrorCode::Success)
			{
				return RefusedBody(TEXT("body"), CodeName(Code));
			}
			TArray<FVector3f> Translations;
			TArray<FVector3f> Rotations;
			Code = Subsystem->GetJointsForBodyConformingFromTemplate(&Body, Translations, Rotations);
			if (Code != EImportErrorCode::Success)
			{
				return RefusedBody(TEXT("body joints"), CodeName(Code));
			}
			if (!Subsystem->ConformBodyToTarget(Character, Vertices, Rotations, /*bTargetIsInAPose*/ true, /*bEstimateJointsFromMesh*/ false)
				|| !Subsystem->SetBodyJoints(Character, Translations, Rotations, /*bImportHelperJoints*/ true))
			{
				return RefusedBody(TEXT("body"), TEXT("the conform returned false"));
			}
			bFitted = true;
		}
	}
	if (!bFitted || !UEditorLoadingAndSavingUtils::SavePackages({ Character->GetPackage() }, /*bOnlyDirty*/ false))
	{
		OutError = FString::Printf(TEXT("the character made from %s could not be saved"), *Body.GetName());
		return false;
	}
	return true;
}

bool FMantleMetaHumanBuild::HandOff(FMantleConvertContext& Context, const int32 PartIndex, const FString& BuildRoot, const USkeletalMesh* BuiltBody,
	const USkeletalMesh* BuiltFace, FString& OutNote, FString& OutError)
{
	if (!Context.Parts.IsValidIndex(PartIndex) || !BuiltBody || !BuiltFace)
	{
		OutError = TEXT("the hand-off has no part, or no built body and face");
		return false;
	}
	// The Assembly fixer reads the build as it reads any MetaHuman Creator character; it must find the build's own body and face.
	FMantleAssemblyScan Scan;
	FString ScanError;
	if (!FMantleAvatarAssemblyFixer::Scan(BuildRoot, Scan, ScanError, /*bAllowConvertedRoot*/ true))
	{
		OutError = FString::Printf(TEXT("the built MetaHuman in %s can't be read as a MetaHuman Creator character: %s"), *BuildRoot, *ScanError);
		return false;
	}
	if (Scan.Body.Get() != BuiltBody || Scan.Face.Get() != BuiltFace)
	{
		OutError = FString::Printf(TEXT("read as a MetaHuman Creator character, %s gives the body %s and the face %s, not the build's %s and %s"), *BuildRoot,
			*Scan.Body->GetName(), *Scan.Face->GetName(), *BuiltBody->GetName(), *BuiltFace->GetName());
		return false;
	}
	TArray<FString> Others;
	for (const TArray<FMantleAssemblyMesh>* List : { &Scan.Garments, &Scan.Hair })
	{
		for (const FMantleAssemblyMesh& Entry : *List)
		{
			if (const USkeletalMesh* Mesh = Entry.Mesh.Get())
			{
				Others.AddUnique(Mesh->GetName());
			}
		}
	}
	Others.Append(Scan.LeftOut);

	FMantleConvertPart& Part = Context.Parts[PartIndex];
	Part.Mesh = Scan.Body;
	Part.Face = Scan.Face;
	Part.SkeletonId = TEXT("metahuman");
	Context.AssemblyFolder = Scan.Folder;
	// The work records the built MetaHuman (FMantleConvertContext::IsBuiltMetaHuman, IsHandedOffBuild), and its body and face
	// carry BuiltKey: what makes them a finished Convert output (FMantleAvatarConverter::IsConvertOutput).
	FMantleConvertMetaHumanWork& Work = Context.MetaHuman.FindOrAdd(PartIndex);
	Work.BuildRoot = Scan.Folder;
	Work.BuiltBody = Scan.Body;
	Work.BuiltFace = Scan.Face;
	for (USkeletalMesh* Built : { Scan.Body.Get(), Scan.Face.Get() })
	{
		Built->GetPackage()->GetMetaData().SetValue(Built, FMantleAvatarConverter::BuiltKey, *Scan.Folder);
		Built->MarkPackageDirty();
	}
	OutNote = FString::Printf(TEXT("%s: handed on as the built MetaHuman, %s with its face %s (the Assembly fixer reads %s)%s."), *Part.Name,
		*Scan.Body->GetName(), *Scan.Face->GetName(), *Scan.Folder,
		Others.Num() > 0 ? *FString::Printf(TEXT("; not uploaded from the build: %s"), *FString::Join(Others, TEXT(", "))) : TEXT(""));
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD %s"), *OutNote);
	return true;
}

namespace MantleMetaHumanBuildPrivate
{
	/**
	 * The build lines' preflight (MHBuild, MHSource), never asking Epic (the last answer if there is one; the build asks when
	 * Convert runs): false with Fix made a line it can't fix (NoRenderer under -NullRHI, else the preflight's problem).
	 */
	bool BuildLineCanRun(FMantleConvertFix& Fix, const TCHAR* NoRenderer, bool& bOutSignInUnchecked)
	{
		if (GUsingNullRHI)
		{
			Fix.Action = NoRenderer;
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			return false;
		}
		const FMantleMetaHumanBuildPreflight Ready = FMantleMetaHumanBuild::Preflight(/*bAskEpic*/ false);
		const FString Problem = Ready.Problem();
		if (!Problem.IsEmpty())
		{
			Fix.Action = FString::Printf(TEXT("Can't: %s."), *Problem);
			Fix.bCanFix = false;
			Fix.bEnabled = false;
			return false;
		}
		bOutSignInUnchecked = Ready.SignIn == EMantleEpicSignIn::NotChecked;
		return true;
	}

	/**
	 * The Convert route's height correction: the work's posed body scaled by Factor about the origin (as the A-pose
	 * step scaled it, so its k and the conform's head hint follow), the old character and its A-pose exports deleted so the
	 * new one takes their names and build folder, and the posed body conformed again (FMantleMetaHumanConform::Conform) into CharacterFolder. The new
	 * character, recorded in the work and the created assets; null with why.
	 */
	UObject* ReconformForHeight(FMantleConvertContext& Context, const int32 PartIndex, const FString& CharacterFolder, const double Factor, FString& OutError)
	{
		FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(PartIndex);
		UStaticMesh* Posed = Work ? Work->PosedMesh.Get() : nullptr;
		if (!Posed || !Context.Parts.IsValidIndex(PartIndex))
		{
			OutError = TEXT("the posed body to correct the height with is gone");
			return nullptr;
		}
		if (!FMath::IsFinite(Factor) || Factor <= 0.0)
		{
			OutError = FString::Printf(TEXT("the height correction's scale %f is not a positive number"), Factor);
			return nullptr;
		}
		UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
		const TStrongObjectPtr<UDynamicMesh> MeshHeld(Mesh);
		FGeometryScriptMeshReadLOD Read;
		Read.LODType = EGeometryScriptLODType::SourceModel;
		Read.LODIndex = 0;
		EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(Posed, Mesh, FGeometryScriptCopyMeshFromAssetOptions(), Read, Outcome, /*bUseSectionMaterials*/ true);
		if (Outcome != EGeometryScriptOutcomePins::Success || Mesh->GetTriangleCount() == 0)
		{
			OutError = FString::Printf(TEXT("the posed body %s could not be read to correct its height"), *Posed->GetName());
			return nullptr;
		}
		UGeometryScriptLibrary_MeshTransformFunctions::ScaleMesh(Mesh, FVector(Factor), FVector::ZeroVector, /*bFixOrientationForNegativeScale*/ true);
		FGeometryScriptCopyMeshToAssetOptions WriteOptions;
		WriteOptions.bEnableRecomputeNormals = false;
		WriteOptions.bEnableRecomputeTangents = false;
		FGeometryScriptMeshWriteLOD Write;
		Write.LODIndex = 0;
		UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(Mesh, Posed, WriteOptions, Write, Outcome, /*bUseSectionMaterials*/ true);
		if (Outcome != EGeometryScriptOutcomePins::Success)
		{
			OutError = FString::Printf(TEXT("the rescaled posed body could not be written into %s"), *Posed->GetName());
			return nullptr;
		}
		Work->InputScale *= Factor;
		FMantleAvatarHeight::WriteInputScale(*Posed, Work->InputScale);
		Posed->MarkPackageDirty();
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD height correction: %s rescaled by %.5f (k now %.5f)"), *Posed->GetPathName(), Factor, Work->InputScale);

		// The old character and its A-pose exports (the conform never overwrites an export it didn't make in that run).
		TArray<TWeakObjectPtr<UObject>> Old = { Work->Character };
		const FString ExportFolder = CharacterFolder / TEXT("APose/");
		for (const TWeakObjectPtr<UObject>& Created : Context.CreatedAssets)
		{
			const UObject* Object = Created.Get();
			if (Object && Object->GetPackage()->GetName().StartsWith(ExportFolder, ESearchCase::CaseSensitive))
			{
				Old.AddUnique(Created);
			}
		}
		Work->Character.Reset();
		FMantleMetaHumanSteps::Discard(Old);
		FMantleMetaHumanConformResult Conformed;
		const FString Name = FMantleMetaHumanSteps::AssetTag(Context.Parts[PartIndex].Name);
		if (!FMantleMetaHumanConform::Conform(*Posed, Work->ConformHeadHint(), CharacterFolder, Name, Conformed, OutError))
		{
			OutError = FString::Printf(TEXT("conforming again for the height failed: %s"), *OutError);
			FMantleMetaHumanSteps::Discard(Conformed.Created);
			return nullptr;
		}
		for (const TWeakObjectPtr<UObject>& Created : Conformed.Created)
		{
			Context.CreatedAssets.AddUnique(Created);
		}
		Work->Character = Conformed.Character;
		return Conformed.Character.Get();
	}

	/**
	 * Builds Character (in CharacterFolder, FMantleMetaHumanBuild::Build) for Context.Parts[PartIndex] and hands the part on
	 * (HandOff): MHBuild's Apply, and MHSource's after its copy and height. Nothing of Epic's, MetaHuman Creator's or the
	 * source's may change (the part's protected folders); the step writes only into CharacterFolder. With bRetargetHeight
	 * (MHSource, a body whose height can be set) the built height is measured and, when it is further than
	 * FMantleAvatarHeight::AimCm from the target, what the build made is deleted and Character rebuilt with a corrected Height
	 * constraint (FMantleAvatarHeight::RetargetCm), at most FMantleAvatarHeight::MaxHeightBuilds builds. With Reconform
	 * (MHBuild, a conformed body) a build outside FMantleAvatarHeight::ConvertedToleranceCm of the target is deleted, Reconform
	 * makes a new character from the posed body rescaled by target / built, and that is built instead, at most
	 * FMantleAvatarHeight::MaxConvertHeightBuilds builds. The notes on success; false with why, what the build made deleted
	 * and the context as it was.
	 */
	bool BuildAndHandOff(FMantleConvertContext& Context, const int32 PartIndex, UObject& FirstCharacter, const FString& CharacterFolder,
		FString& OutError, const bool bRequireUnclothed = false, const bool bRetargetHeight = false,
		const double FirstRequestCm = -1.0, const double PredictedCm = -1.0,
		const TFunction<UObject*(double BuiltCm, FString& OutReconformError)>& Reconform = TFunction<UObject*(double, FString&)>(),
		const bool bFixedBody = false)
	{
		UObject* Character = &FirstCharacter;
		const bool bReconformHeight = !bRetargetHeight && static_cast<bool>(Reconform);
		const int32 MaxBuilds = bRetargetHeight ? FMantleAvatarHeight::MaxHeightBuilds : bReconformHeight ? FMantleAvatarHeight::MaxConvertHeightBuilds : 1;
		double FirstBuiltCm = -1.0;
		int32 Reconforms = 0;
		const TArray<FString> Roots = FMantleMetaHumanSteps::ProtectedRoots(Context, PartIndex);
		const TSet<FString> DirtyBefore = FMantleMetaHumanSteps::DirtyUnder(Roots, CharacterFolder);
		FMantleMetaHumanBuildResult Result;
		bool bBuilt = false;
		FString Dirtied;
		const double Target = FMantleAvatarHeight::TargetCm();
		// The first request is the one MHSource set (FirstRequestCm when it corrected the constraint on Creator's evaluated body
		// before this build); each corrected rebuild asks for what the last build's error calls for.
		double RequestedCm = FirstRequestCm > 0.0 ? FirstRequestCm : Target;
		int32 Retargets = 0;
		for (int32 Attempt = 0; Attempt < MaxBuilds; ++Attempt)
		{
			bBuilt = FMantleMetaHumanBuild::Build(*Character, CharacterFolder, Result, OutError, /*bDeferHeightWarning*/ bRetargetHeight || bReconformHeight,
				bReconformHeight ? FMantleAvatarHeight::ConvertedToleranceCm : FMantleAvatarHeight::ToleranceCm);
			if (Attempt == 0)
			{
				FirstBuiltCm = Result.BuiltHeightCm;
			}
			if (Attempt == 0 && bBuilt && FirstRequestCm > 0.0)
			{
				// For calibrating FMantleAvatarHeight::PredictionOffsetCm: built minus predicted.
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_HEIGHT_PREDICTION calibration: requested %.3f cm, built %.3f cm, predicted %.3f cm, offset (built minus predicted) %.3f cm"),
					FirstRequestCm, Result.BuiltHeightCm, PredictedCm, (PredictedCm > 0.0 && Result.BuiltHeightCm > 0.0) ? Result.BuiltHeightCm - PredictedCm : 0.0);
			}
			// Checked whether the step finished or not: one that stopped half-way may have changed something too.
			if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, CharacterFolder, DirtyBefore, TEXT("The build"), Dirtied))
			{
				OutError = bBuilt ? Dirtied : FString::Printf(TEXT("%s. %s"), *OutError, *Dirtied);
				bBuilt = false;
			}
			if (!bBuilt || Attempt + 1 >= MaxBuilds)
			{
				break;
			}
			if (bReconformHeight)
			{
				// A conformed body has no Height constraint to correct; its posed body is rescaled and conformed again.
				if (Result.BuiltHeightCm <= 0.0 || FMantleAvatarHeight::IsWithin(Result.BuiltHeightCm, Target, FMantleAvatarHeight::ConvertedToleranceCm))
				{
					break;
				}
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD height correction: built %.3f cm, target %.3f cm; rescaling the posed body by %.5f, conforming and building again"),
					Result.BuiltHeightCm, Target, Target / Result.BuiltHeightCm);
				// This build is thrown away; the next one is made from the new character.
				const double BuiltCm = Result.BuiltHeightCm;
				FMantleMetaHumanSteps::Discard(Result.Created);
				UObject* Next = Reconform(BuiltCm, OutError);
				if (!Next)
				{
					return false;
				}
				if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, CharacterFolder, DirtyBefore, TEXT("Conforming again for the height"), Dirtied))
				{
					OutError = Dirtied;
					return false;
				}
				Character = Next;
				++Reconforms;
				continue;
			}
			if (!bRetargetHeight || !FMantleAvatarHeight::NeedsRetarget(Result.BuiltHeightCm, Target))
			{
				break;
			}
			const double Next = FMantleAvatarHeight::RetargetCm(RequestedCm, Result.BuiltHeightCm, Target);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT_BUILD height retarget %d: built %.3f cm for a %.3f cm request; requesting %.3f cm"),
				Attempt + 1, Result.BuiltHeightCm, RequestedCm, Next);
			// This build is thrown away; the next one is made from Character with the corrected constraint.
			FMantleMetaHumanSteps::Discard(Result.Created);
			bool bStillFixed = false;
			if (!FMantleMetaHumanBuild::SetHeight(*Character, Next, bStillFixed, OutError))
			{
				return false;
			}
			if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, CharacterFolder, DirtyBefore, TEXT("Setting the height"), Dirtied))
			{
				OutError = Dirtied;
				return false;
			}
			RequestedCm = Next;
			++Retargets;
		}
		if (bBuilt && (bRetargetHeight || bReconformHeight) && !Result.bHeightWithin)
		{
			// Exhausted retries and unread heights remain warnings and fail strict preset acceptance.
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_BUILD %s"), *Result.HeightLine);
		}
		if (bBuilt && bRequireUnclothed && !FMantleMetaHumanBuild::CheckUnclothedSource(*Character, OutError)) { bBuilt = false; }
		// A fixed body type (MHSource, a custom MetaHuman Creator body) that isn't the reference topology, or is built off the canonical
		// height (it can't be retargeted, and Mantle refuses a converted avatar that isn't 165.6 cm) fails with the reason.
		if (bBuilt && bRequireUnclothed)
		{
			FString CompleteError;
			const bool bComplete = FMantleMetaHumanBuild::CheckCompleteBody(Result.Body.Get(), CompleteError);
			// A MetaHuman never goes As-is. A fixed body the build can't make canonical fails with the reason.
			if (FMantleMetaHumanBuild::FixedBodyCantBuild(bFixedBody, bComplete, Result.bHeightWithin))
			{
				OutError = FString::Printf(TEXT("%s is a fixed body type %s; a fixed body can't be rebuilt to Mantle's body"), *Context.Parts[PartIndex].Name,
					!bComplete ? *FString::Printf(TEXT("and not the reference topology (%s)"), *CompleteError) : *FString::Printf(TEXT("built at %s, not the canonical height a converted avatar needs"), *Result.HeightLine));
				bBuilt = false;
			}
			else if (!bComplete)
			{
				OutError = CompleteError;
				bBuilt = false;
			}
		}
		// The hand-off: the part becomes the built MetaHuman for the Assembly fixer and the fixers after it (the context is
		// changed only when it succeeds).
		FString HandOffNote;
		if (bBuilt && !FMantleMetaHumanBuild::HandOff(Context, PartIndex, Result.BuildRoot, Result.Body.Get(), Result.Face.Get(), HandOffNote, OutError))
		{
			bBuilt = false;
		}
		if (!bBuilt)
		{
			// The fixer contract: a failed Apply deletes what this run's build made and leaves the context as it was.
			FMantleMetaHumanSteps::Discard(Result.Created);
			return false;
		}
		for (const TWeakObjectPtr<UObject>& Created : Result.Created)
		{
			Context.CreatedAssets.AddUnique(Created);
		}
		const FString& Name = Context.Parts[PartIndex].Name;
		Context.Notes.Add(FString::Printf(TEXT("%s: built with Epic's MetaHuman tools in %s (%d textures resized to %d; %s)."), *Name,
			*Result.BuildRoot, Result.Resized.Num(), FMantleMetaHumanBuild::MaxTextureSize, *Result.HeightLine));
		if (Retargets > 0)
		{
			Context.Notes.Add(FString::Printf(TEXT("%s: Creator's height constraint was corrected %d time(s) (last request %.3f cm); final measured height %.3f cm, target %.3f cm, error %+.3f cm."),
				*Name, Retargets, RequestedCm, Result.BuiltHeightCm, Target, Result.BuiltHeightCm - Target));
		}
		if (Reconforms > 0)
		{
			Context.Notes.Add(FString::Printf(TEXT("%s: the first build came out %.2f cm tall, so the posed body was rescaled by %.5f, conformed again and rebuilt; final measured height %.3f cm, target %.3f cm, error %+.3f cm."),
				*Name, FirstBuiltCm, Target / FirstBuiltCm, Result.BuiltHeightCm, Target, Result.BuiltHeightCm - Target));
		}
		if (!Result.bHeightWithin)
		{
			// A warning, never a stop: the converter never refuses for size.
			Context.Notes.Add(FString::Printf(TEXT("WARNING: %s did not come out at the default height (%s)."), *Name, *Result.HeightLine));
		}
		Context.Notes.Add(HandOffNote);
		return true;
	}
}

void FMantleMetaHumanBuildFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Contract)
	{
		return;
	}
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		if (!FMantleMetaHumanPoseFixer::IsBodyToConvert(Context, PartIndex, *Contract))
		{
			continue;
		}
		FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Problem = TEXT("its MetaHuman isn't built yet");
		bool bSignInUnchecked = false;
		if (!MantleMetaHumanBuildPrivate::BuildLineCanRun(Fix, TEXT("Can't: the steps before it need a renderer, and this editor runs without one."), bSignInUnchecked))
		{
			continue;
		}
		Fix.Action = FString::Printf(TEXT("Builds it with Epic's MetaHuman tools: Epic's centre skin tone (players recolour it in game), no underwear, textures at most 2048. %s%s"),
			FMantleMetaHumanBuild::ConsentLine, bSignInUnchecked ? TEXT(" Your Epic sign-in is checked when you press Convert.") : TEXT(""));
	}
}

bool FMantleMetaHumanBuildFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !Context.Parts.IsValidIndex(Fix.PartIndex))
	{
		OutError = TEXT("the build names no part");
		return false;
	}
	const FMantleConvertMetaHumanWork* Work = Context.MetaHuman.Find(Fix.PartIndex);
	UObject* Character = Work ? Work->Character.Get() : nullptr;
	if (!Character || Work->CharacterFolder.IsEmpty())
	{
		OutError = TEXT("there is no MetaHuman Character to build (the conform step)");
		return false;
	}
	// A copy: the hand-off writes the work's entry.
	const FString CharacterFolder = Work->CharacterFolder;
	const int32 PartIndex = Fix.PartIndex;
	if (Work->bFaceOnly)
	{
		// A face-only character's body is MetaHuman's own, so its height is set on the Height constraint, as MHSource's.
		bool bFixed = false;
		double RequestedCm = -1.0;
		double PredictedCm = -1.0;
		if (!FMantleMetaHumanBuild::SetPredictedHeight(*Character, FMantleAvatarHeight::TargetCm(), RequestedCm, PredictedCm, bFixed, OutError))
		{
			return false;
		}
		return MantleMetaHumanBuildPrivate::BuildAndHandOff(Context, Fix.PartIndex, *Character, CharacterFolder, OutError, /*bRequireUnclothed*/ false,
			/*bRetargetHeight*/ !bFixed, /*FirstRequestCm*/ RequestedCm, PredictedCm);
	}
	// The A-pose step scales the posed body once, by its own height; a build outside the tolerance is corrected by
	// rescaling the posed body by target / built and conforming it again (a conformed body has no Height constraint).
	auto Reconform = [&Context, PartIndex, CharacterFolder](const double BuiltCm, FString& OutReconformError) -> UObject*
	{
		return MantleMetaHumanBuildPrivate::ReconformForHeight(Context, PartIndex, CharacterFolder, FMantleAvatarHeight::TargetCm() / BuiltCm, OutReconformError);
	};
	return MantleMetaHumanBuildPrivate::BuildAndHandOff(Context, Fix.PartIndex, *Character, CharacterFolder, OutError, /*bRequireUnclothed*/ false,
		/*bRetargetHeight*/ false, /*FirstRequestCm*/ -1.0, /*PredictedCm*/ -1.0, Reconform);
}

bool FMantleMetaHumanSourceFixer::IsCharacterToBuild(const FMantleConvertContext& Context, const int32 PartIndex)
{
	const bool bAssembly = Context.AssemblyBodyToFit.IsValid() && Context.AssemblyFaceToFit.IsValid();
	if (Context.Route != EMantleAvatarRoute::Convert || !Context.Parts.IsValidIndex(PartIndex) || (!bAssembly && !Cast<UMetaHumanCharacter>(Context.Source.Get())))
	{
		return false;
	}
	const FMantleConvertPart& Part = Context.Parts[PartIndex];
	return Part.bIncluded && !Part.Mesh.IsValid() && Part.Slot.Equals(TEXT("avatar"), ESearchCase::CaseSensitive) && !Context.IsBuiltMetaHuman(PartIndex);
}

void FMantleMetaHumanSourceFixer::Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const
{
	for (int32 PartIndex = 0; PartIndex < Context.Parts.Num(); ++PartIndex)
	{
		if (!IsCharacterToBuild(Context, PartIndex))
		{
			continue;
		}
		const UObject* Source = Context.Source.Get();
		const bool bAssembly = !Cast<UMetaHumanCharacter>(Source);
		// An assembled MetaHuman is named by its part, and made into a character first.
		const FString SourceName = bAssembly ? Context.Parts[PartIndex].Name : Source->GetName();
		FMantleConvertFix& Fix = OutFixes.AddDefaulted_GetRef();
		Fix.FixerId = GetId();
		Fix.PartIndex = PartIndex;
		Fix.Problem = bAssembly ? FString::Printf(TEXT("%s is an assembled MetaHuman (meshes, no MetaHuman Creator character) that isn't built at Mantle's default height yet"), *SourceName)
			: FString::Printf(TEXT("%s is a MetaHuman Creator character that isn't built at Mantle's default height yet"), *SourceName);
		bool bSignInUnchecked = false;
		if (!MantleMetaHumanBuildPrivate::BuildLineCanRun(Fix, TEXT("Can't: Epic's build needs a renderer, and this editor runs without one."), bSignInUnchecked))
		{
			continue;
		}
		Fix.Action = FString::Printf(TEXT("%s with Epic's MetaHuman tools at the default height (%.1f cm), Epic's centre skin tone (players recolour it in game), no underwear, textures at most 2048; %s itself is never changed. %s%s"),
			bAssembly ? (Context.MetaHuman.Find(PartIndex) && Context.MetaHuman.Find(PartIndex)->bFaceOnly
					? TEXT("Fits a MetaHuman Creator character to its face mesh on MetaHuman's own body, then builds it")
					: TEXT("Fits a MetaHuman Creator character to its face and body meshes, then builds it"))
				: TEXT("Builds a copy of it"),
			FMantleAvatarHeight::TargetCm(), *SourceName, FMantleMetaHumanBuild::ConsentLine,
			bSignInUnchecked ? TEXT(" Your Epic sign-in is checked when you press Convert.") : TEXT(""));
	}
}

bool FMantleMetaHumanSourceFixer::Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const
{
	if (Fix.FixerId != GetId() || !IsCharacterToBuild(Context, Fix.PartIndex))
	{
		OutError = TEXT("there is no MetaHuman Creator character to build");
		return false;
	}
	UMetaHumanCharacter* Source = Cast<UMetaHumanCharacter>(Context.Source.Get());
	USkeletalMesh* AssemblyBody = Source ? nullptr : Context.AssemblyBodyToFit.Get();
	USkeletalMesh* AssemblyFace = Source ? nullptr : Context.AssemblyFaceToFit.Get();
	if (!Source && (!AssemblyBody || !AssemblyFace))
	{
		OutError = FMantleMetaHumanBuild::AssemblyNotBuiltMessage(Context.Parts[Fix.PartIndex].Name, TEXT("its body or face mesh is gone"));
		return false;
	}
	const FString Name = FMantleMetaHumanSteps::AssetTag(Context.Parts[Fix.PartIndex].Name);
	const FString CharacterFolder = Context.OutputFolder / Name;
	// The source (an Epic preset or the creator's character) is never changed: the work is a copy in <output>/<Name>.
	TArray<FString> Roots = FMantleMetaHumanSteps::ProtectedRoots(Context, Fix.PartIndex);
	if (!Source)
	{
		// An assembly's meshes are read, never written.
		for (const USkeletalMesh* Mesh : { AssemblyBody, AssemblyFace })
		{
			Roots.AddUnique(FPackageName::GetLongPackagePath(Mesh->GetOutermost()->GetName()) + TEXT("/"));
		}
	}
	const TSet<FString> DirtyBefore = FMantleMetaHumanSteps::DirtyUnder(Roots, CharacterFolder);
	const FString PackageName = FMantleMetaHumanSteps::UniquePackageName(CharacterFolder, Name);
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Copy = nullptr;
	TArray<TWeakObjectPtr<UObject>> Made;
	if (Source)
	{
		Copy = AssetTools.DuplicateAsset(FPackageName::GetShortName(PackageName), FPackageName::GetLongPackagePath(PackageName), Source);
		if (Copy)
		{
			Made.Add(Copy);
		}
		if (!Cast<UMetaHumanCharacter>(Copy))
		{
			OutError = FString::Printf(TEXT("%s could not be copied to %s"), *Source->GetName(), *CharacterFolder);
			FMantleMetaHumanSteps::Discard(Made);
			return false;
		}
		int32 RemovedClothingSelections = 0;
		// Package metadata belongs to the source object; preserve its actual provenance on this distinct copy.
		FMetaData::CopyMetadata(Source, Copy);
		if (!FMantleMetaHumanBuild::PrepareUnclothedSourceCopy(*Copy, *Source, RemovedClothingSelections, OutError))
		{
			FMantleMetaHumanSteps::Discard(Made);
			return false;
		}
	}
	else
	{
		// A new character fitted to the assembly's face and body; from here it is built exactly as a character source's copy.
		// Strip's FaceOnly / FeminineBody / MasculineBody lines (offered when the body can't be fitted) are recorded on the part's work.
		const FMantleConvertMetaHumanWork* ChoiceWork = Context.MetaHuman.Find(Fix.PartIndex);
		const bool bMade = FMantleMetaHumanBuild::MakeCharacterFromAssembly(*AssemblyBody, *AssemblyFace, PackageName, Copy, OutError,
			ChoiceWork && ChoiceWork->bFaceOnly, ChoiceWork && ChoiceWork->bFeminineBody);
		if (Copy)
		{
			Made.Add(Copy);
		}
		if (!bMade)
		{
			OutError = FMantleMetaHumanBuild::AssemblyNotBuiltMessage(Context.Parts[Fix.PartIndex].Name, OutError);
			FMantleMetaHumanSteps::Discard(Made);
			return false;
		}
	}
	// The default height (the Height body constraint), corrected on Creator's evaluated body before the first build; a fixed
	// body type keeps its own and is measured after the build. The build's own measurement and retry stay the authority.
	bool bFixed = false;
	double RequestedCm = -1.0;
	double PredictedCm = -1.0;
	if (!FMantleMetaHumanBuild::SetPredictedHeight(*Copy, FMantleAvatarHeight::TargetCm(), RequestedCm, PredictedCm, bFixed, OutError))
	{
		FMantleMetaHumanSteps::Discard(Made);
		return false;
	}
	FString Dirtied;
	if (!FMantleMetaHumanSteps::CheckUnchanged(Roots, CharacterFolder, DirtyBefore, TEXT("Copying the character"), Dirtied))
	{
		OutError = Dirtied;
		FMantleMetaHumanSteps::Discard(Made);
		return false;
	}
	// Epic's centre skin tone, as MHBuild: the upload is geometry only, so a preset's own tone would show in its thumbnail and nowhere in game.
	if (!MantleMetaHumanBuildPrivate::BuildAndHandOff(Context, Fix.PartIndex, *Copy, CharacterFolder, OutError, /*bRequireUnclothed*/ true, /*bRetargetHeight*/ !bFixed, /*FirstRequestCm*/ RequestedCm, PredictedCm,
		TFunction<UObject*(double, FString&)>(), /*bFixedBody*/ bFixed))
	{
		FMantleMetaHumanSteps::Discard(Made);
		return false;
	}
	FMantleConvertMetaHumanWork& Work = Context.MetaHuman.FindOrAdd(Fix.PartIndex);
	Work.Character = Copy;
	Work.CharacterFolder = CharacterFolder;
	Context.CreatedAssets.AddUnique(Copy);
	if (bFixed)
	{
		// A warning, never a stop: the converter never refuses for size.
		const FString SourceName = Source ? Source->GetName() : Context.Parts[Fix.PartIndex].Name;
		Context.Notes.Add(FString::Printf(TEXT("WARNING: %s is a fixed body type: its height can't be set, so it was built at its own height (the line above measures it)."),
			*SourceName));
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_CONVERT_SOURCE %s is a fixed body type: built at its own height"), *SourceName);
	}
	return true;
}

void RegisterMantleMetaHumanBuildFixer()
{
	FMantleAvatarFixers::Register(MakeShared<FMantleMetaHumanBuildFixer>());
	FMantleAvatarFixers::Register(MakeShared<FMantleMetaHumanSourceFixer>());
}
