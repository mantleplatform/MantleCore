// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEditor.h"
#include "MantleApiClient.h"
#include "MantleAvatarHeightFixer.h"
#include "MantleAvatarPropsFixer.h"
#include "MantleAvatarReposeFixer.h"
#include "MantleAssetLibrary.h"
#include "MantleAvatarAtlasFixer.h"
#include "MantleAvatarBakeFixer.h"
#include "MantleAvatarConverter.h"
#include "MantleAvatarRefitFixer.h"
#include "MantleAvatarRetargetPoseFixer.h"
#include "MantleAvatarRigMapFixer.h"
#include "MantleAvatarStripFixer.h"
#include "MantleMetaHumanBuild.h"
#include "MantleMetaHumanConform.h"
#include "MantleMetaHumanPose.h"
#include "MantleUnriggedAsIs.h"
#include "MantleUnriggedBody.h"
#include "MantleCosmeticPanel.h"
#include "MantleCreatorSignIn.h"
#include "MantleEditorStyle.h"
#include "MantlePreviewClient.h"
#include "SMantlePreviewPanel.h"
#include "MantleLibraryPanel.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantleSettings.h"
#include "MantlePlaceImporter.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceThumbnail.h"
#include "SMantleThumbnailSection.h"
#include "MantlePlacePreview.h"
#include "MantleCheck.h"
#include "MantlePlaceValidator.h"
#include "MantlePublishPanel.h"
#include "MantleReferenceBodies.h"
#include "MantleVehicleConverter.h"
#include "SMantleVehicleBuilder.h"
#include "MantleVehicleSource.h"
#include "MantleWeaponBuilderPanel.h"
#include "MantleWeaponSpawnPoint.h"
#include "MantleScriptPoint.h"
#include "MantlePlaceSecretsDetails.h"
#include "MantleTestLocally.h"
#include "MantleScriptPointDetails.h"
#include "MantleSeatSpot.h"
#include "MantleSeatSpotVisualizer.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "MantleRuntime.h"
#include "Graph/MantleScriptGraphModule.h"
#include "Scripts/MantleScriptsPanelController.h"


#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "ScopedTransaction.h"
#include "ToolMenus.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "LevelEditorViewport.h"
#include "Misc/ScopedSlowTask.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Mantle"

namespace
{
	UWorld* CurrentEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/** Validation issues, phrased for a person, capped so a broken level does not produce a wall of text. */
	FText SummariseIssues(const FMantleValidationResult& Validation, int32 MaxLines = 12)
	{
		if (Validation.Issues.Num() == 0)
		{
			return LOCTEXT("NoIssues", "No problems found. This place is ready to publish.");
		}

		TArray<FString> Lines;
		for (const FMantleValidationIssue& Issue : Validation.Issues)
		{
			if (Lines.Num() >= MaxLines)
			{
				Lines.Add(FString::Printf(
					TEXT("...and %d more."), Validation.Issues.Num() - MaxLines));
				break;
			}
			Lines.Add(Issue.ToString());
		}

		return FText::FromString(FString::Join(Lines, TEXT("\n\n")));
	}

	void OnValidate()
	{
		UWorld* World = CurrentEditorWorld();
		FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
		// GhzGarage/mantle#818: the place file as Publish would write it, against the 8 MiB the backend takes (and the instance limit).
		FMantleCheck::AppendPlaceBudget(World, Validation);

		for (const FMantleValidationIssue& Issue : Validation.Issues)
		{
			UE_LOG(LogMantle, Log, TEXT("%s"), *Issue.ToString());
		}

		// The Script Assets' ids (Mantle Place Settings): a library asset's is known now; an upload's
		// exists only once Publish has uploaded it, and the Publish log names it then.
		const FMantlePlaceAssetLimits Limits = FMantlePlaceAssetLimits::FromContract();
		for (const FMantleScriptAsset& Asset : FMantlePlaceAssets::ScriptAssets(World))
		{
			FString Problem;
			FString Remedy;
			if (!FMantlePlaceAssets::CheckScriptAsset(Asset, Limits, Problem, Remedy))
			{
				continue;
			}
			const FString LibraryId = FMantleAssetLibrary::Get().FindIdByPath(FSoftObjectPath(Asset.Object));
			UE_LOG(LogMantle, Log, TEXT("Script asset %s = %s"), *Asset.Path.ToString(),
				LibraryId.IsEmpty() ? TEXT("(uploaded when you publish; the Publish log gives its id)") : *LibraryId);
		}

		FMessageDialog::Open(EAppMsgType::Ok, SummariseIssues(Validation));
	}

	/** Makes Actor the only selected actor in the level editor. */
	void SelectOnly(AActor* Actor)
	{
		GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
		GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ true);
	}

	/**
	 * Spawns a T a little way in front of the level viewport's camera, facing the way it looks, and labels it. Null when
	 * the spawn fails. The caller opens the undo transaction first.
	 */
	template <typename T>
	T* SpawnInFrontOfViewport(UWorld* World, const TCHAR* Label)
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		if (GCurrentLevelEditingViewportClient)
		{
			Rotation = FRotator(0.0, GCurrentLevelEditingViewportClient->GetViewRotation().Yaw, 0.0);
			Location = GCurrentLevelEditingViewportClient->GetViewLocation() + Rotation.Vector() * 300.0;
		}
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		T* Spawned = World->SpawnActor<T>(Location, Rotation, Parameters);
		if (Spawned)
		{
			Spawned->SetActorLabel(Label);
		}
		return Spawned;
	}

	void OnPlaceSettings()
	{
		if (AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(CurrentEditorWorld()))
		{
			SelectOnly(Settings);
		}
	}

	/**
	 * Tools > Mantle > Add Weapon Spawn (#789): puts a Mantle Weapon Spawn in the level, a little way in front of the viewport's
	 * camera and facing the way it looks, and selects it, so the creator picks its weapon in the Details panel.
	 */
	void OnAddWeaponSpawn()
	{
		UWorld* World = CurrentEditorWorld();
		if (!World)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("AddWeaponSpawnNoLevel", "Open a level first, then add a weapon spawn."));
			return;
		}
		const FScopedTransaction Transaction(LOCTEXT("AddWeaponSpawnUndo", "Add Weapon Spawn"));
		AMantleWeaponSpawnPoint* Spawn = SpawnInFrontOfViewport<AMantleWeaponSpawnPoint>(World, TEXT("Weapon Spawn"));
		if (!Spawn)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("AddWeaponSpawnFailed", "Could not add a weapon spawn to this level."));
			return;
		}
		SelectOnly(Spawn);
		const TArray<FString> Built = Spawn->GetWeaponOptions();
		if (Built.Num() == 0)
		{
			FMessageDialog::Open(EAppMsgType::Ok,
				LOCTEXT("AddWeaponSpawnNoWeapons",
					"Added a weapon spawn.\n\nYou have not built a weapon yet. Use Tools > Mantle > Build a weapon to make one, "
					"then pick it in this spawn's Weapon list."));
		}
	}

	/**
	 * Tools > Mantle > Add Script Point (#838): puts a Mantle Script Point in front of the viewport's camera, facing the way it looks,
	 * and selects it, so the creator picks its package and parameters in the Details panel.
	 */
	void OnAddScriptPoint()
	{
		UWorld* World = CurrentEditorWorld();
		if (!World)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("AddScriptPointNoLevel", "Open a level first, then add a Script Point."));
			return;
		}
		const FScopedTransaction Transaction(LOCTEXT("AddScriptPointUndo", "Add Script Point"));
		AMantleScriptPoint* Point = SpawnInFrontOfViewport<AMantleScriptPoint>(World, TEXT("Script Point"));
		if (!Point)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("AddScriptPointFailed", "Could not add a Script Point to this level."));
			return;
		}
		// The first package on offer, so a level with one package needs no pick at all.
		const TArray<FMantleScriptPointChoice> Choices = FMantleScriptPointDetails::GatherChoices(World);
		if (Choices.Num() > 0)
		{
			Point->StorageItem = Choices[0].Name;
		}
		SelectOnly(Point);
		if (Choices.Num() == 0)
		{
			FMessageDialog::Open(EAppMsgType::Ok,
				LOCTEXT("AddScriptPointNoPackages",
					"Added a Script Point.\n\nThis place has no script package yet. Add one with Tools > Mantle > Library (the Scripts filter), "
					"or make one in a Packages folder beside the level's Scripts folder, then pick it in this point's Storage Item."));
		}
	}

	void OnNewPlace()
	{
		UWorld* World = CurrentEditorWorld();
		const FMantleBuildResult Result =
			FMantlePlaceImporter::ImportFromFile(World, FMantlePlaceImporter::TemplatePath());

		if (Result.Actors.Num() == 0)
		{
			FMessageDialog::Open(
				EAppMsgType::Ok,
				FText::Format(
					LOCTEXT("NewPlaceFailed", "Could not start a new place.\n\n{0}"),
					FText::FromString(Result.ErrorSummary())));
			return;
		}

		FMessageDialog::Open(
			EAppMsgType::Ok,
			LOCTEXT("NewPlaceDone",
				"Added a baseplate, a spawn point and a sun.\n\nBuild from here, then use "
				"'Check what players receive' to check your work."));
	}

	void OnSaveToCloud()
	{
		// Just in time (#94): no token, no upload. Asked before anything else so a cancelled
		// sign-in costs the creator nothing.
		if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("SaveSignInReason", "Sign in to save this place to Mantle.")))
		{
			return;
		}
		// F6 (#340): the same flow as Publish, in the Mantle Publish panel, without going live.
		SMantlePublishPanel::Start(CurrentEditorWorld(), /*bGoLive*/ false, FString());
	}

	/**
	 * Asks for this revision's notes before publishing, showing the thumbnail the publish will take (#786) in the shared
	 * Thumbnail section (#124): Automatic, Choose the angle or a Custom image. Empty notes are fine; false means the creator cancelled, and so nothing is published.
	 */
	bool AskForRevisionNotes(UWorld* World, FString& OutNotes)
	{
		bool bConfirmed = false;
		TSharedPtr<SMultiLineEditableTextBox> NotesBox;
		const TWeakObjectPtr<UWorld> WeakWorld = World;

		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(LOCTEXT("PublishNotesTitle", "Publish to Mantle"))
			.SizingRule(ESizingRule::FixedSize)
			.ClientSize(FVector2f(560.0f, 820.0f))
			.SupportsMinimize(false)
			.SupportsMaximize(false);
		const TWeakPtr<SWindow> WeakWindow = Window;

		// The buttons capture bConfirmed by reference: the window is modal, so this function
		// does not return until it has closed and nothing can click them afterwards.
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(12.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("PublishNotesPrompt",
						"What changed in this version? Optional; the notes are kept with this revision."))
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(NotesBox, SMultiLineEditableTextBox)
					.HintText(LOCTEXT("PublishNotesHint", "Revision notes (optional)"))
					.AutoWrapText(true)
				]
				// #786, #124: the catalogue picture this publish takes: the Thumbnail section every Mantle uploader shows.
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SMantleThumbnailSection)
					.Target(FMantlePlaceThumbnail::Target())
					.Render(FMantlePlaceThumbnail::RenderFor(World))
					.UseViewLabel(LOCTEXT("ThumbnailUseView", "Use current view"))
					.Choice_Lambda([WeakWorld]() { return FMantlePlaceThumbnail::ChoiceFor(WeakWorld.Get()); })
					.OnChoiceChanged_Lambda([WeakWorld](const FMantleThumbnailChoice& Choice)
					{
						AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(WeakWorld.Get());
						if (!Settings)
						{
							return;
						}
						const FScopedTransaction Transaction(LOCTEXT("ThumbnailChanged", "Change the Mantle thumbnail"));
						Settings->Modify();
						const bool bToAngle = Choice.Mode == EMantleThumbnailMode::Angle && Settings->ThumbnailChoice.Mode != EMantleThumbnailMode::Angle;
						Settings->ThumbnailChoice = Choice;
						// Choose the angle starts from where the level viewport is, unless a view was chosen before.
						if (bToAngle && Settings->ThumbnailLocation.IsNearlyZero() && Settings->ThumbnailRotation.IsNearlyZero())
						{
							Settings->SetThumbnailFromViewport();
						}
					})
					.OnUseView_Lambda([WeakWorld]() -> FText
					{
						// Checked first, so the level gets no settings actor for a click that cannot set a view.
						if (!GCurrentLevelEditingViewportClient || !GCurrentLevelEditingViewportClient->IsPerspective())
						{
							return LOCTEXT("ThumbnailNeedsPerspective", "Click into a perspective level viewport, frame the shot there, then choose Use current view again.");
						}
						if (AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::FindOrAdd(WeakWorld.Get()))
						{
							Settings->SetThumbnailFromViewport();
						}
						return FText::GetEmpty();
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("PublishNotesConfirm", "Publish"))
						.OnClicked_Lambda([&bConfirmed, WeakWindow]()
						{
							bConfirmed = true;
							if (const TSharedPtr<SWindow> Open = WeakWindow.Pin())
							{
								Open->RequestDestroyWindow();
							}
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("PublishNotesCancel", "Cancel"))
						.OnClicked_Lambda([WeakWindow]()
						{
							if (const TSharedPtr<SWindow> Open = WeakWindow.Pin())
							{
								Open->RequestDestroyWindow();
							}
							return FReply::Handled();
						})
					]
				]
			]);

		GEditor->EditorAddModalWindow(Window);

		if (!bConfirmed || !NotesBox.IsValid())
		{
			return false;
		}
		OutNotes = NotesBox->GetText().ToString();
		return true;
	}

	void OnPublish()
	{
		UWorld* World = CurrentEditorWorld();
		if (!World)
		{
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("PublishNoLevel", "There is no level open to publish."));
			return;
		}

		// One publish at a time: a click while one runs shows it rather than asking for notes again.
		if (SMantlePublishPanel::IsRunning())
		{
			SMantlePublishPanel::Start(World, /*bGoLive*/ true, FString());
			return;
		}

		// Signed in before the notes are asked for, so notes are never typed into a publish that
		// cannot happen.
		if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("PublishSignInReason", "Sign in to publish this place to Mantle.")))
		{
			return;
		}

		FString Notes;
		if (!AskForRevisionNotes(World, Notes))
		{
			return;
		}

		// F6 (#340): check, convert, upload, wait for Mantle and publish, in the background, with
		// one progress panel. The creator keeps working meanwhile.
		SMantlePublishPanel::Start(World, /*bGoLive*/ true, Notes);
	}

	/**
	 * Convert vehicle (#435): each selected skeletal mesh becomes (or updates) <Mesh>_Vehicle beside it, the asset the
	 * vehicle builder puts in the level and publishing stores in the library (#679). What it found, measured or refused is shown, each refusal saying what to fix. A vehicle
	 * selected in the level (a car Blueprint or pawn placed there) converts its body's mesh with the static meshes attached to
	 * the body as its parts and its body's physics as its chassis (#435 item 2); a mesh selected in the Content Browser takes
	 * its own preview attachments and physics asset.
	 */
	void OnConvertVehicle()
	{
		if (!GEditor)
		{
			return;
		}
		FEditorDelegates::LoadSelectedAssetsIfNeeded.Broadcast();
		TArray<USkeletalMesh*> Meshes;
		if (USelection* Selection = GEditor->GetSelectedObjects())
		{
			Selection->GetSelectedObjects<USkeletalMesh>(Meshes);
		}
		// Level actors with a skeletal body: the root component when it is one, else the first with a mesh.
		struct FActorBody
		{
			USkeletalMesh* Mesh = nullptr;
			FMantleVehicleSurroundings Around;
		};
		TArray<FActorBody> ActorBodies;
		TArray<AActor*> Actors;
		if (USelection* ActorSelection = GEditor->GetSelectedActors())
		{
			ActorSelection->GetSelectedObjects<AActor>(Actors);
		}
		for (const AActor* Actor : Actors)
		{
			const USkeletalMeshComponent* Body = Actor ? Cast<USkeletalMeshComponent>(Actor->GetRootComponent()) : nullptr;
			if (Actor && (!Body || !Body->GetSkeletalMeshAsset()))
			{
				TArray<USkeletalMeshComponent*> Skeletal;
				Actor->GetComponents(Skeletal);
				const USkeletalMeshComponent* const* First = Skeletal.FindByPredicate([](const USkeletalMeshComponent* Candidate)
				{
					return Candidate && Candidate->GetSkeletalMeshAsset();
				});
				Body = First ? *First : nullptr;
			}
			if (!Body)
			{
				continue;
			}
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			const TArray<const UActorComponent*> ConstComponents(Components);
			FActorBody& Entry = ActorBodies.AddDefaulted_GetRef();
			Entry.Mesh = Body->GetSkeletalMeshAsset();
			Entry.Around = FMantleVehicleConverter::SurroundingsOfComponent(*Body, ConstComponents);
			Meshes.Remove(Entry.Mesh);
		}
		if (Meshes.Num() == 0 && ActorBodies.Num() == 0)
		{
			FMessageDialog::Open(EAppMsgType::Ok,
				LOCTEXT("ConvertVehicleNoMesh",
					"Select the vehicle's skeletal mesh in the Content Browser first: its body, with a bone at each wheel "
					"centre named like Wheel_FL, Wheel_FR, Wheel_BL and Wheel_BR. Or select a vehicle placed in the level "
					"(a car Blueprint): the meshes attached to its body come along as its parts."));
			return;
		}

		TArray<TPair<USkeletalMesh*, const FMantleVehicleSurroundings*>> Conversions;
		for (const FActorBody& Entry : ActorBodies)
		{
			Conversions.Emplace(Entry.Mesh, &Entry.Around);
		}
		for (USkeletalMesh* Mesh : Meshes)
		{
			Conversions.Emplace(Mesh, nullptr);
		}
		TArray<FString> Lines;
		for (const TPair<USkeletalMesh*, const FMantleVehicleSurroundings*>& Conversion : Conversions)
		{
			USkeletalMesh* Mesh = Conversion.Key;
			FMantleVehicleConversion Result;
			const UMantleVehicleSource* Source = FMantleVehicleConverter::ConvertAsset(Mesh, Result, Conversion.Value);
			if (Source)
			{
				Lines.Add(FString::Printf(TEXT("%s: made %s (not saved yet). Open Tools > Mantle > Build a vehicle to put it in your level. Publishing your place then adds it to your library and the place's Storage, and the spawn parks that library item."),
					*Mesh->GetName(), *Source->GetName()));
			}
			else
			{
				Lines.Add(FString::Printf(TEXT("%s: not converted."), *Mesh->GetName()));
			}
			for (const FString& Problem : Result.Problems)
			{
				Lines.Add(TEXT("  ") + Problem);
				UE_LOG(LogMantle, Warning, TEXT("Convert vehicle %s: %s"), *Mesh->GetName(), *Problem);
			}
			for (const FString& Note : Result.Notes)
			{
				Lines.Add(TEXT("  ") + Note);
				UE_LOG(LogMantle, Log, TEXT("Convert vehicle %s: %s"), *Mesh->GetName(), *Note);
			}
		}
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Join(Lines, TEXT("\n"))));
	}

	void OnPreview()
	{
		UWorld* World = CurrentEditorWorld();

		// #394: meshes and materials from the project load as players get them, by the ids Mantle
		// holds them under. Asking needs a sign-in; without one they are listed as not uploaded.
		FMantlePlaceAssetSet Uploaded;
		FMantlePreviewOptions Options;
		if (FMantleCreatorSignIn::IsSignedIn())
		{
			FString Problem;
			if (!FMantlePlacePreview::FindUploadedAssets(World, Uploaded, FMantlePlacePreview::ApiDiff(), Problem))
			{
				UE_LOG(LogMantle, Warning, TEXT("Check what players receive: could not ask Mantle which files it holds: %s"), *Problem);
			}
			Options.Assets = &Uploaded;
		}
		FMantlePreviewResult Preview = FMantlePlacePreview::Build(World, Options);

		for (const FMantleValidationIssue& Issue : Preview.Validation.Issues)
		{
			UE_LOG(LogMantle, Log, TEXT("%s"), *Issue.ToString());
		}

		// The rebuilt world is torn down before the dialog opens: this is a report on what
		// players will receive, not a world to walk around in, and the command says so.
		const FText Summary = FText::Format(
			LOCTEXT("PreviewSummary", "{0}\n\n{1}"),
			FText::FromString(FMantlePlacePreview::DescribeForCreator(Preview)),
			SummariseIssues(Preview.Validation));

		FMantlePlacePreview::Destroy(Preview);

		FMessageDialog::Open(EAppMsgType::Ok, Summary);
	}

	void OnSignIn()
	{
		if (FMantleCreatorSignIn::OpenSignInDialog(FText::GetEmpty()))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FMantleCreatorSignIn::DescribeSignedIn());
		}
	}

	void OnSignOut()
	{
		FMantleCreatorSignIn::SignOut();
	}

}

void FMantleEditorModule::StartupModule()
{
	UE_LOG(LogMantle, Log, TEXT("MantleEditor module started"));

	// The API client lives in the runtime module, which knows nothing about editor project
	// settings — and should not. Teaching it where to ask is the editor's job, so the
	// Plugins > Mantle address keeps working without the runtime depending on the editor.
	FMantleApiClient::BaseUrlResolver =
		[] { return UMantleSettings::Get()->ResolveApiBaseUrl(); };

	// #94: the pasted creator token, and forgetting it when the API refuses it.
	FMantleCreatorSignIn::Startup();

	// #563 stage 3: the toolbar's icons, before the menus that name them.
	FMantleEditorStyle::Initialize();

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateStatic(&FMantleEditorModule::RegisterMenus));

	// The "Mantle Scripts" panel (Window menu, toolbar, New Script, Check All).
	FMantleScriptsPanelController::Startup();
	// The "Mantle" Content Browser category and UMantleScriptGraph's asset type actions.
	FMantleScriptGraphModule::Startup();
	// Tools > Mantle > Cosmetics..., the cosmetic authoring and upload panel.
	SMantleCosmeticPanel::RegisterTab();
	SMantleVehicleBuilder::RegisterTab();
	// #614 stage 6: Tools > Mantle > Library..., browse vehicles and weapons and add them to the place's storage.
	SMantleLibraryPanel::RegisterTab();
	// #729: Tools > Mantle > Build a weapon..., the guided weapon builder.
	FMantleWeaponBuilderPanel::RegisterTab();
	// #563 stage 3: the Mantle Preview panel that Test Locally opens.
	SMantlePreviewPanel::RegisterTab();
	// #610: the avatar converter's fixers this module implements (the MetaHuman assembly split, the triangle limit).
	FMantleAvatarConverter::RegisterBuiltInFixers();
	// F6 (#340): the Mantle Publish panel that Publish and Save to Mantle open.
	SMantlePublishPanel::RegisterTab();
	// #838: a Script Point's Details panel: its package from the place's storage and Packages/, its parameters with the package's hints.
	FMantleScriptPointDetails::Register();
	// GhzGarage/MantleCore#142: a Mantle Seat's hip marker. GUnrealEd does not exist yet while modules start up.
	if (GUnrealEd)
	{
		RegisterSeatVisualizer();
	}
	else
	{
		SeatVisualizerHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&FMantleEditorModule::RegisterSeatVisualizer);
	}
	// #1011: Place Settings > Secrets.
	FMantlePlaceSecretsDetails::Register();
	// Chunk 7: As-is's auto-rig (a model with no rig keeps its look, skinned to Epic's MetaHuman body skeleton).
	RegisterMantleUnriggedAsIsFixer();
	// #610: the avatar converter's third-party rig fixer (Mixamo, Character Creator, Daz, guided mapping).
	RegisterMantleAvatarRigMapFixer();
	// #610: the avatar converter's Repose step (reference pose to the skeleton's A-pose).
	RegisterMantleAvatarReposeFixer();
	// #774: As-is avatars are resized to the default height, as Convert does.
	RegisterMantleAvatarHeightFixer();
	// #46: As-is drops cloth simulation, hidden sections and hand-held props, before AsIsHeight measures the body.
	RegisterMantleAvatarPropsFixer();
	// #625: Convert's Strip step (the body cut down to bare skin, once the creator confirms it is bare skin).
	RegisterMantleAvatarStripFixer();
	// #776: Convert's step for a model with no skeleton (arms bent into an A-pose, scaled to the default height; takes Strip's and the A-pose step's place).
	RegisterMantleMetaHumanUnriggedFixer();
	// #625: Convert's A-pose step (the bare-skin body posed into MetaHuman's A-pose, frozen into the conform's input).
	RegisterMantleMetaHumanPoseFixer();
	// #625: Convert's conform step (a MetaHuman Character of the posed body, face landmarks, the hand joints re-estimated).
	RegisterMantleMetaHumanConformFixer();
	// #625: Convert's build step (skin tone, no underwear, Epic's cloud textures and face rig, the build, textures to 2048).
	RegisterMantleMetaHumanBuildFixer();
	// #625: the avatar converter's RetargetPose step (an as-is Mannequin body's limbs lined up with the client's retargeter).
	RegisterMantleAvatarRetargetPoseFixer();
	// #610: the avatar converter's Refit step (clothing and hair refitted onto a reference-topology body).
	RegisterMantleAvatarRefitFixer();
	// #610: the avatar converter's Atlas step (material sections and UDIM tiles baked into atlases).
	RegisterMantleAvatarAtlasFixer();
	// #620: the avatar converter's Bake step (every other section whose material is not plain textures, baked on its own UVs).
	RegisterMantleAvatarBakeFixer();

	// #434: the Mantle toolset for AI agents links Unreal's experimental Toolset Registry, which
	// MantleCore.uplugin names only as an optional, disabled plugin, so its module is never loaded
	// automatically (LoadingPhase None). It is loaded here, once the registry's editor subsystem
	// exists, and only when the project enabled Toolset Registry: without it nothing changes.
	if (IPluginManager::Get().FindEnabledPlugin(TEXT("ToolsetRegistry")).IsValid())
	{
		if (GEngine && GEngine->IsInitialized())
		{
			LoadAgentTools();
		}
		else
		{
			AgentToolsHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&FMantleEditorModule::LoadAgentTools);
		}
	}
}

void FMantleEditorModule::RegisterSeatVisualizer()
{
	if (GUnrealEd)
	{
		GUnrealEd->RegisterComponentVisualizer(UMantleSeatSpotComponent::StaticClass()->GetFName(), MakeShareable(new FMantleSeatSpotVisualizer));
	}
}

void FMantleEditorModule::LoadAgentTools()
{
	const TCHAR* const AgentToolsModule = TEXT("MantleAgentTools");
	if (FModuleManager::Get().ModuleExists(AgentToolsModule))
	{
		FModuleManager::Get().LoadModule(AgentToolsModule);
	}
}

void FMantleEditorModule::ShutdownModule()
{
	// #563: an unload without an editor exit would otherwise leave the preview running with nothing to stop it.
	MantlePreviewClient::Stop();
	FMantleTestLocally::Get().Stop();
	FCoreDelegates::GetOnPostEngineInit().Remove(AgentToolsHandle);
	FCoreDelegates::GetOnPostEngineInit().Remove(SeatVisualizerHandle);
	SMantleCosmeticPanel::UnregisterTab();
	SMantleVehicleBuilder::UnregisterTab();
	SMantleLibraryPanel::UnregisterTab();
	FMantleWeaponBuilderPanel::UnregisterTab();
	SMantlePreviewPanel::UnregisterTab();
	SMantlePublishPanel::UnregisterTab();
	FMantleScriptPointDetails::Unregister();
	if (GUnrealEd)
	{
		GUnrealEd->UnregisterComponentVisualizer(UMantleSeatSpotComponent::StaticClass()->GetFName());
	}
	FMantlePlaceSecretsDetails::Unregister();
	FMantleScriptGraphModule::Shutdown();
	FMantleScriptsPanelController::Shutdown();

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	if (!IsEngineExitRequested() && UObjectInitialized())
	{
		UToolMenus::Get()->RemoveSection(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"), TEXT("MantleTools"));
	}
	FMantleEditorStyle::Shutdown();

	FMantleCreatorSignIn::Shutdown();
	FMantleReferenceBodies::Shutdown();

	UE_LOG(LogMantle, Log, TEXT("MantleEditor module shut down"));
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleEditorMenus
{
/**
 * Every Mantle entry, in the order creators see them: Tools > Mantle and the toolbar's Mantle dropdown (#563 stage 3) both
 * call this, so the two lists cannot drift. Entries that show or hide are read each time the menu opens.
 */
void FillMantleSection(FToolMenuSection& Section)
{
	Section.AddMenuEntry(
		TEXT("MantleTestLocally"),
		LOCTEXT("TestLocallyMenuLabel", "Test Locally"),
		LOCTEXT("TestLocallyMenuTooltip",
			"Export this level and run it in the Mantle client on this PC, with a Mantle Preview panel showing what the client reports. "
			"Nothing is uploaded or published."),
		FMantleEditorStyle::TestLocallyMenuIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(&SMantlePreviewPanel::TestLocally),
			FCanExecuteAction::CreateLambda([] { return !FMantleTestLocally::Get().IsBusy(); })));

	Section.AddMenuEntry(
		TEXT("MantleStopPreview"),
		LOCTEXT("StopPreviewLabel", "Stop Preview"),
		LOCTEXT("StopPreviewTooltip", "Close the preview this editor started. A Mantle you started yourself is never touched."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(&SMantlePreviewPanel::StopPreview),
			FCanExecuteAction::CreateLambda([] { return MantlePreviewClient::IsRunning(); })));

	Section.AddSeparator(TEXT("MantlePreviewSeparator"));

	Section.AddMenuEntry(
		TEXT("MantleNewPlace"),
		LOCTEXT("NewPlaceLabel", "Start a new place"),
		LOCTEXT("NewPlaceTooltip", "Add a baseplate, a spawn point and a sun to this level."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnNewPlace)));

	Section.AddMenuEntry(
		TEXT("MantlePlaceSettings"),
		LOCTEXT("PlaceSettingsLabel", "Place settings"),
		LOCTEXT("PlaceSettingsTooltip",
			"Select this level's Mantle Place Settings (max players, gravity, respawn), adding them if the level has none."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnPlaceSettings)));

	Section.AddMenuEntry(
		TEXT("MantleAddWeaponSpawn"),
		LOCTEXT("AddWeaponSpawnLabel", "Add Weapon Spawn"),
		LOCTEXT("AddWeaponSpawnTooltip",
			"Put a weapon in this level where players can pick it up. It appears in front of you in the viewport; pick which of your "
			"built weapons it holds in its Weapon list. The weapon is sent with your place when you publish."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnAddWeaponSpawn)));

	Section.AddMenuEntry(
		TEXT("MantleAddScriptPoint"),
		LOCTEXT("AddScriptPointLabel", "Add Script Point"),
		LOCTEXT("AddScriptPointTooltip",
			"Anchor a script package in this level. It appears in front of you in the viewport; pick the package in its Storage Item "
			"(the place's stored packages, or one in the level's Packages folder) and set the parameters the package lists."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnAddScriptPoint)));

	Section.AddMenuEntry(
		TEXT("MantleValidate"),
		LOCTEXT("ValidateLabel", "Check this place"),
		LOCTEXT("ValidateTooltip", "Look for anything in this level that Mantle cannot publish."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnValidate)));

	Section.AddMenuEntry(
		TEXT("MantlePreview"),
		LOCTEXT("PreviewLabel", "Check what players receive"),
		LOCTEXT("PreviewTooltip",
			"Rebuild this level from its place file the way the Mantle client will, then report how many "
			"objects reach players and list everything that will not."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnPreview)));

	Section.AddMenuEntry(
		TEXT("MantleSaveToCloud"),
		LOCTEXT("SaveLabel", "Save to Mantle"),
		LOCTEXT("SaveTooltip", "Upload this level as a private draft. Nobody else can see it."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnSaveToCloud)));

	Section.AddMenuEntry(
		TEXT("MantlePublish"),
		LOCTEXT("PublishLabel", "Publish"),
		LOCTEXT("PublishTooltip", "Check this level, upload everything it needs (meshes, materials and textures from your project included) and make it live for players to join. Runs in the background; nothing already on Mantle is sent twice."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnPublish)));

	Section.AddMenuEntry(
		TEXT("MantleBuildVehicle"),
		LOCTEXT("BuildVehicleLabel", "Build a vehicle..."),
		LOCTEXT("BuildVehicleTooltip",
			"Make a vehicle players can drive: pick the kind of vehicle, shape it with sliders and put it in your level, or put your own car model on it."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&SMantleVehicleBuilder::OpenTab)));

	Section.AddMenuEntry(
		TEXT("MantleConvertVehicle"),
		LOCTEXT("ConvertVehicleLabel", "Convert vehicle"),
		LOCTEXT("ConvertVehicleTooltip",
			"Make a Mantle vehicle from the skeletal mesh selected in the Content Browser, or from a vehicle selected in the level: "
			"finds the wheel bones (Wheel_FL and the like), measures the wheels, carries the meshes attached to the body as its parts, "
			"reads its collision and centre of mass from its physics, and writes <Mesh>_Vehicle with default tuning to adjust."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&OnConvertVehicle)));

	Section.AddMenuEntry(
		TEXT("MantleCosmetics"),
		LOCTEXT("CosmeticsLabel", "Cosmetics..."),
		LOCTEXT("CosmeticsTooltip",
			"Publish hair, clothing, accessories, avatars and emotes to the Mantle catalog: pick the asset, "
			"and the plugin checks, exports and uploads everything."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&SMantleCosmeticPanel::OpenTab)));

	Section.AddMenuEntry(
		TEXT("MantleLibrary"),
		LOCTEXT("LibraryLabel", "Library..."),
		LOCTEXT("LibraryTooltip",
			"Find vehicles, weapons and script packages in the Mantle library, get other creators' public ones, and add them to this place's "
			"storage. Nothing of another creator's is saved into your project."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&SMantleLibraryPanel::OpenTab)));

	Section.AddMenuEntry(
		TEXT("MantleBuildWeapon"),
		LOCTEXT("BuildWeaponLabel", "Build a weapon..."),
		LOCTEXT("BuildWeaponTooltip", "Turn a 3D model into a weapon players can use: a gun, a bow, a blade or another weapon."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateStatic(&FMantleWeaponBuilderPanel::TryInvokeTab)));

	// Sign-in (#94). The entries show and hide as the state changes: the menu is built each time
	// it opens, and every visibility and label below is read then.
	Section.AddMenuEntry(
		TEXT("MantleSignIn"),
		LOCTEXT("SignInLabel", "Sign in with creator token..."),
		LOCTEXT("SignInTooltip",
			"Paste a creator token made in the Mantle game (Settings > Account > Creator tokens) to save and publish "
			"places as that player. The token is held in memory until the editor closes, never saved."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(&OnSignIn),
			FCanExecuteAction(),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateLambda([] { return !FMantleCreatorSignIn::IsSignedIn(); })));

	Section.AddMenuEntry(
		TEXT("MantleSignedIn"),
		TAttribute<FText>::CreateLambda([] { return FMantleCreatorSignIn::DescribeSignedIn(); }),
		TAttribute<FText>::CreateLambda([]
		{
			return FMantleCreatorSignIn::IsUsingEnvironmentToken()
				? LOCTEXT("SignedInEnvironmentTooltip",
					"This editor was started with MANTLE_AUTH_TOKEN, which it uses for every request. Unset it and restart to paste a token instead.")
				: LOCTEXT("SignedInTooltip", "Places you save and publish belong to this Mantle player.");
		}),
		FSlateIcon(),
		FUIAction(
			FExecuteAction(),
			FCanExecuteAction::CreateLambda([] { return false; }),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateLambda([] { return FMantleCreatorSignIn::IsSignedIn(); })));

	Section.AddMenuEntry(
		TEXT("MantleSignOut"),
		LOCTEXT("SignOutLabel", "Sign out"),
		LOCTEXT("SignOutTooltip", "Forget the creator token this editor holds. The next save or publish asks for one again."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(&OnSignOut),
			FCanExecuteAction(),
			FGetActionCheckState(),
			FIsActionButtonVisible::CreateLambda([]
			{
				return FMantleCreatorSignIn::IsSignedIn() && !FMantleCreatorSignIn::IsUsingEnvironmentToken();
			})));
}

/** The Mantle toolbar dropdown's content: the shared entries, built each time it opens. */
void FillMantleToolbarMenu(UToolMenu* Menu)
{
	FToolMenuSection& Section = Menu->AddSection(TEXT("Mantle"), LOCTEXT("MantleSection", "Mantle"));
	FillMantleSection(Section);
}
}

void FMantleEditorModule::RegisterMenus()
{
	UToolMenus* ToolMenus = UToolMenus::Get();

	if (UToolMenu* ToolsMenu = ToolMenus->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
	{
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection(
			TEXT("Mantle"), LOCTEXT("MantleSection", "Mantle"));
		MantleEditorMenus::FillMantleSection(Section);
	}

	// #563 stage 3: Test Locally and the Mantle dropdown sit in the Play toolbar, next to Play, which is where a creator
	// starts a test run; the User toolbar is the editor's catch-all for plugins and is further from it.
	if (UToolMenu* Toolbar = ToolMenus->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar")))
	{
		FToolMenuSection& Section = Toolbar->FindOrAddSection(TEXT("MantleTools"));

		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			TEXT("MantleTestLocallyButton"),
			FUIAction(
				FExecuteAction::CreateStatic(&SMantlePreviewPanel::TestLocally),
				FCanExecuteAction::CreateLambda([] { return !FMantleTestLocally::Get().IsBusy(); })),
			LOCTEXT("TestLocallyLabel", "Test Locally"),
			LOCTEXT("TestLocallyTooltip",
				"Export this level and run it in the Mantle client on this PC. Nothing is uploaded or published; the Mantle Preview panel shows what happens."),
			FMantleEditorStyle::TestLocallyIcon()));

		Section.AddEntry(FToolMenuEntry::InitComboButton(
			TEXT("MantleMenuButton"),
			FUIAction(),
			FNewToolMenuChoice(FNewToolMenuDelegate::CreateStatic(&MantleEditorMenus::FillMantleToolbarMenu)),
			LOCTEXT("MantleComboLabel", "Mantle"),
			LOCTEXT("MantleComboTooltip", "Test Locally, check, save and publish this place, and the rest of Mantle's tools."),
			FMantleEditorStyle::LogoIcon()));
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMantleEditorModule, MantleEditor)
