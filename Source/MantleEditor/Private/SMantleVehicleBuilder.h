// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleLightFinder.h"
#include "MantleVehicleShape.h"
#include "MantleVehicleTypeMatch.h"
#include "SMantleBuilderFlow.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWidget.h"

class FMantleVehicleShapeKind;
class SMantleBuilderViewport;
class SVerticalBox;
class UMantleVehicleSource;
class UMaterialInterface;
class UTexture2D;
class UObject;
struct FAssetData;
class FScopedTransaction;

/**
 * Tools > Mantle > Build a vehicle...: eight short steps, one decision each, the way the weapon builder goes. Pick what kind of vehicle it is (a card for every
 * publishable preset). Choose your vehicle's model: a Static or Skeletal Mesh, a vehicle Blueprint, or a file from your computer; there is always a model. Check the fit:
 * which way it faces, its size, which of its parts are wheels, propellers or rotors, and how it feels to drive. Seats and paint. Doors, hood and trunk: which bone of the model is each, so they open. Liveries: images laid over the paint, each named,
 * with the one the car starts in and a guide to paint them on. The Thumbnail every uploader has.
 * Then build it, put it in your level and drive it. From the second step on, the picture shows the creator's own model on the vehicle's frame
 * (FMantleVehicleBuilder::MakeModelPreview) with a dot on each wheel; there is no stand-in vehicle. The result, good or bad, shows in the window; there are no dialogs.
 * The logic is FMantleVehicleBuilder (the vehicle assets), FMantleVehicleShape (the feel) and FMantleVehicleTypeMatchCheck; this is only the page. Native Slate.
 */
class SMantleVehicleBuilder : public SCompoundWidget
{
	/** The tests drive the window's state the way its buttons do. */
	friend struct FMantleVehicleBuilderPanelAccess;
	/** The model step's state, for its own tests. */
	friend struct FMantleVehicleBuilderModelAccess;
	/** The tests of the stale build after an edit, the names a build again reuses and the file import. */
	friend struct FMantleVehicleBuilderFixAccess;
	/** The tests of the new flow: the steps, the type check, the paint list and what Build applies. */
	friend struct FMantleVehicleBuilderFlowAccess;
	/** Mantle.Dev.VehicleBuilderShots: the window, step by step, drawn offscreen. */
	friend struct FMantleVehicleBuilderShotAccess;
	/** The tests of the Liveries step: its images, names and the one the car starts in. */
	friend struct FMantleVehicleBuilderLiveryAccess;

public:
	SLATE_BEGIN_ARGS(SMantleVehicleBuilder) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The nomad tab: registered at module startup, opened from the Tools menu. */
	static const FName TabName;
	static void RegisterTab();
	static void UnregisterTab();
	static void OpenTab();

private:
	TArray<FMantleBuilderStep> MakeSteps();
	// The eight steps.
	TSharedRef<SWidget> MakeTypeStep();
	TSharedRef<SWidget> MakeModelStep();
	TSharedRef<SWidget> MakeFitStep();
	TSharedRef<SWidget> MakeSeatsAndPaintStep();
	TSharedRef<SWidget> MakeDoorsStep();
	TSharedRef<SWidget> MakeLiveriesStep();
	TSharedRef<SWidget> MakeThumbnailStep();
	TSharedRef<SWidget> MakeBuildStep();

	TSharedRef<SWidget> MakePresetCard(const FString& PresetId, const FText& Name, const FText& Description);
	/** A step's content with the picture of the model on its frame beside it: Left is the step's own controls. */
	TSharedRef<SWidget> WithPicture(const TSharedRef<SWidget>& Left, bool bLights = false, bool bLiveries = false);
	/** The picture: the model on its frame once there is one, else the line that says what to do next. */
	TSharedRef<SWidget> MakePicture();
	/** The Size row of a type shaped by one Size (a plane, helicopter or hovercraft). */
	TSharedRef<SWidget> MakeSizeRow();
	/** One feel slider (grip, power, suspension or top speed), -1 to 1. A road vehicle has all four, a hovercraft its top speed. */
	TSharedRef<SWidget> MakeFeelRow(EMantleVehicleFeel Feel);
	TSharedRef<SWidget> MakePassengerRow();
	/** The feel sliders the picked type has, and its passenger tickbox where its preset has a passenger seat, one under the other. */
	void AddFeelRows(const TSharedRef<SVerticalBox>& Rows);

	/** Picks a type: loads its frame as the archetype. Picking the type already picked changes nothing. */
	void PickPreset(const FString& PresetId, const FText& Name);

	// Feel and size.
	void SetShapeSize(double Value);
	void SetFeel(EMantleVehicleFeel Feel, double Value);
	void SetPassengerSeat(bool bOn);
	/**
	 * What a change that really changed something leaves to do: the build made before it is forgotten (it would otherwise be put in the level, shown in its settings and
	 * kept by Done with the old numbers), and the picture is redrawn.
	 */
	void ShapeChanged();

	// The picture.
	/** Shows the model on its frame in the picture (FMantleVehicleBuilder::MakeModelPreview), made again from what the page has now. */
	void RebuildPreview(bool bFrame);
	/** The picture is out of date: it is redrawn in a moment, once, however many sizes changed meanwhile (a drag changes them every frame). */
	void MarkPreviewDirty();
	/** Redraws the picture now if it is out of date: at the end of a drag, after a typed size. */
	void FlushPreview();
	EActiveTimerReturnType OnPreviewTimer(double CurrentTime, float DeltaTime);

	// The model.
	bool CanBuild() const;
	FReply OnBuild();
	/**
	 * What the picture and Build my vehicle ask for (FMantleVehicleBuilder::BuildAny's options): for a model worn as a skin, the frame, the facing and the parts as the page has them,
	 * and, when this window already built this type and model, the name that build was saved under, to be updated in place and not added to.
	 */
	FMantleVehicleModelOptions MakeBuildOptions() const;
	/** Remembers the name a build was saved under, for the same type and model, so the next press of Build my vehicle updates it. */
	void RememberBuilt(const FMantleVehicleBuildResult& Result);
	FReply OnShowSettings();
	FReply OnPlaceInLevel();
	/** Import a file...: asks for a file and imports it. */
	FReply OnPickFile();
	/** Imports a model file into the project (FMantleVehicleBuilder::ImportModelFile) and picks the model it made, or says in the window why it could not. */
	void ImportFile(const FString& File);
	void ShowResult(const FMantleVehicleBuildResult& Result);
	/** Picks a Static Mesh, a Skeletal Mesh or a vehicle Blueprint (FMantleVehicleBlueprint::Resolve) as the vehicle's model. */
	void SetMesh(const FAssetData& Asset);
	/**
	 * Reads the picked model for the picked type: the frame's moving parts it can be assigned to, Mantle's suggestions for which part is which and which way
	 * the model faces, whether it fits the type (FMantleVehicleTypeMatchCheck), and the frame's Size or sizes proposed for it. Called when a model or a type is picked.
	 */
	void ReadModel(TOptional<bool> Twin = TOptional<bool>());
	/** The model looks like another type of vehicle: Switch to it. */
	FReply OnSwitchType();
	/** A propeller plane with two engines (a P-38): the frame gets a propeller bone for each, and the parts list a left and a right propeller. */
	void SetTwinEngine(bool bOn);
	/** The model's nose faces this way (a turn about Z, 0 / 90 / 180 / 270); the model's size is read again for it. */
	void SetModelYaw(double Degrees);
	/** Assigns a part of the model (or, for an unset piece, nothing) to one of the frame's moving bones. */
	void SetPart(FName ChassisBone, const FMantleVehicleModelPiece& Piece);
	/** The parts and facing rows, made again for the model and type picked. */
	void RebuildModelRows();
	/** Sets the frame's Size or sizes from the model (FMantleVehicleChassisSkin::Propose) for the way it faces now. */
	void ProposeShape();
	/** A different model is a different vehicle: the last build, its Size and its notes are forgotten, and the result shown is cleared. */
	void ForgetBuild();
	/** The range the built vehicle's Size may take: the picked type's own fit range (schema/vehicle-presets `fit.minScale` and `maxScale`). */
	FVector2f ModelSizeRange() const;
	void SetSize(float NewSize);
	FReply OnResetSize();
	void BeginSizeDrag();
	void EndSizeDrag(float);
	/** Saves the built vehicle when it has unsaved changes, and says on the page whether that worked. */
	void SaveNow();

	// Seats and paint.
	/** The paint list for the picked model: every section of its body, ticked where it reads as car paint (FMantleVehicleConverter::LooksLikePaint). Made from the picture. */
	void RefreshPaintChoices();
	void SetPaintSection(int32 Index, bool bOn);
	/** The layer a ticked section is for: 0 the body color, else FMantleVehicleBuilder::PaintPick of a layer. */
	void SetPaintPick(int32 Index, uint8 Pick);
	/** What a row's layer is called in the window. */
	static FText PaintPickLabel(uint8 Pick);
	void SetHideRiders(bool bOn);
	/** Writes the paint ticks, the lights and the "no inside" choice into a built vehicle. */
	void ApplyChoices(UMantleVehicleSource& Source);

	// Doors, hood and trunk.
	/** Picks the model's bone for a part that opens (an unset name: it does not open); what was built is of the old choice. */
	void SetHingeBone(EMantleVehicleHingePart Part, FName Bone);
	/** Flips the way a part swings. */
	void SetHingeFlip(EMantleVehicleHingePart Part, bool bFlipped);
	/** The rows of the Doors step, made again for the model picked and the choice made. */
	void RebuildDoorRows();

	// Lights.
	/** One light's row: its name and the body sections that glow as it, picked from a menu of every section. */
	TSharedRef<SWidget> MakeLightRow(EMantleLightKind Kind);
	/** What the light row's button says: the sections picked, or (none). */
	FText LightRowText(EMantleLightKind Kind) const;
	/** Picks or unpicks a section for a light; a section glows as one light only, so picking it takes it from any other. */
	void ToggleLightSection(EMantleLightKind Kind, const FString& Section);
	/** Mantle's lights for the picked model (the picture's), unless the creator changed them; sections the model no longer has are dropped. */
	void RefreshLights();
	/** The creator changed a light: the picture shows it, and a built vehicle takes it at once. */
	void LightsChanged();
	/** The beams as the picture's points, and a colored marker on each light section. */
	void PushLightsToPicture();
	/** The creator dragged a beam in the picture. */
	void OnPicturePointsChanged();
	FReply OnAddBeam();
	FReply OnRemoveBeam();
	FReply OnResetLights();

	// Liveries.
	/** One image of the Liveries step: its texture and the name a menu shows. */
	struct FLiveryChoice
	{
		TSoftObjectPtr<UTexture2D> Texture;
		FString Name;
	};
	/** Adds a texture of the project as the next livery (named after it); says why in the step when it cannot. */
	void AddLivery(UTexture2D* Texture);
	/** AddLivery's work: false (with OutProblem filled in when there is a reason to show) when nothing was added. */
	bool TryAddLivery(UTexture2D* Texture, FText& OutProblem);
	/** Add image...: asks for a PNG, TGA or DDS (or another image), imports it into the project and adds it. */
	FReply OnAddLiveryImage();
	void RemoveLivery(int32 Index);
	void SetLiveryName(int32 Index, const FString& Name);
	/** The livery the car starts in (INDEX_NONE for the plain paint). */
	void SetStartLivery(int32 Index);
	/** The livery shown on the model in the picture (INDEX_NONE for the plain paint). */
	void ShowLivery(int32 Index);
	/** The rows of the Liveries step, made again after an add or a remove. */
	void RebuildLiveryRows();
	/** The creator changed a livery: the picture shows it, and a built vehicle takes it at once. */
	void LiveriesChanged();
	/** Draws the shown livery over the ticked paint sections in the picture (only while the Liveries step's picture is open). */
	void ApplyLiveryPreview();
	/** Export UV guide...: writes a PNG of the body's UV 0 with the painted sections outlined. */
	FReply OnExportUvGuide();
	/** The Liveries written into a vehicle (names, images and the one it starts in). */
	void ApplyLiveries(UMantleVehicleSource& Source) const;

	void CloseWindow();

	TSharedPtr<SMantleBuilderFlow> Flow;

	/** The vehicle type picked in step 1 (a preset id); empty until one is picked. */
	FString SelectedPresetId;
	FText SelectedPresetName;
	/** Why the picked type's frame could not be read; empty when it was. Shown in red under the cards. */
	FText TypeProblem;

	// The frame and how it feels.
	/** The picked type's frame, the Size or sizes proposed for the model, the feel sliders and everything that follows from them. */
	FMantleVehicleShapeSession Shape;
	bool bPreviewDirty = false;
	bool bPreviewTimerPending = false;
	/** Why the picture could not be made; empty when it could. */
	FText PreviewProblem;
	TSharedPtr<FMantleVehicleShapeKind> PreviewKind;
	/** Weak: the picture lives only while a step that shows it is open. */
	TWeakPtr<SMantleBuilderViewport> PreviewViewport;
	/** The model on its frame, in memory; replaced with every change. */
	FMantleVehicleModelPreview Preview;

	// Your model.
	/** The model picked, as the builder reads it: a Static Mesh or a Skeletal Mesh (the one a Blueprint gave, or its merged pieces). */
	TWeakObjectPtr<UObject> Mesh;
	/** What Build my vehicle is given: the model, or the vehicle Blueprint a Chaos body came from (its wheels and parts ride along). */
	TWeakObjectPtr<UObject> BuildSource;
	/** What Mantle did with a Blueprint (merged its pieces, which pieces move), in plain words; empty for a mesh. */
	TArray<FString> BlueprintNotes;

	/** The library picture's choice, kept for the model (FMantleThumbnailStore). */
	FMantleThumbnailChoice ThumbnailChoice;
	TSharedPtr<class SMantleThumbnailSection> ThumbnailSection;
	/** Reads the saved choice for the picked model and redraws the section. */
	void LoadThumbnailChoice();
	void ThumbnailChoiceChanged(const FMantleThumbnailChoice& Choice);
	/** After a build: a Custom image or a chosen angle replaces the picture the build took by itself. Notes and saving go into Result. */
	void ApplyThumbnailChoice(FMantleVehicleBuildResult& Result);
	TWeakObjectPtr<UMantleVehicleSource> BuiltSource;
	/** The model read for the picked type (ReadModel), the frame's moving parts, which part of it is which, and which way it faces. */
	FMantleVehicleModel ModelInfo;
	bool bHaveModelInfo = false;
	/** Whether the model fits the picked type; Check's answer, kept for the message and the Switch button. */
	FMantleVehicleTypeMatch TypeMatch;
	TArray<FMantleVehiclePartSlot> PartSlots;
	FMantleVehicleModelMap PartMap;
	/** Which of the model's bones are its doors, hood and trunk and which are flipped: Mantle's suggestion from the bones' names (FMantleVehicleChassisSkin::SuggestHinges) until the creator changes one. */
	FMantleVehicleHingeMap HingeChoice;
	/** The rows of the Doors step. */
	TSharedPtr<SVerticalBox> DoorRowsBox;
	/** The bones a Doors row's list offers, None first, and the bone each stands for (kept alive for as long as the rows are). */
	TArray<TSharedPtr<FString>> HingeChoices;
	TArray<FName> HingeValues;
	double ModelYaw = 0.0;
	/** What Mantle proposed for the frame's size from the model, in plain words. */
	FString ProposalText;
	/** The rows of the fit step: the facing, the Size and one row for each part the frame can move. */
	TSharedPtr<SVerticalBox> ModelRowsBox;
	/** The choices of a part row's list, kept alive for as long as the rows are. */
	TArray<TSharedPtr<FString>> PieceChoices;
	TArray<FMantleVehicleModelPiece> PieceValues;
	/** The four ways a model's nose may face and the turn each one needs. */
	TArray<TSharedPtr<FString>> YawChoices;
	TArray<double> YawValues;
	/** The picked model is worn as a skin on the frame (else its own wheels are fitted). Worked out when the model or the type is picked. */
	bool bUseSkin = false;
	/** The frame is the twin-engined plane (FMantleVehicleChassisSkin::AsTwin): set when the model has two propellers, or by the creator. */
	bool bTwinEngine = false;
	TSharedPtr<SVerticalBox> ResultBox;
	/** What Import a file... did, in plain words, under the picker; red when it failed. */
	FText ImportStatus;
	bool bImportFailed = false;
	/** The folder the last file was picked from, so the next dialog opens there. */
	FString LastImportFolder;
	/**
	 * The name the last build of a model on a frame was saved under, and which type and model it was for: pressing Build again for the same pair updates those assets
	 * instead of adding a new set each press.
	 */
	FString BuiltAssetName;
	FString BuiltAssetKey;
	/** Why the model picked cannot be used (not a model, no geometry, a Blueprint with nothing to take); empty when it is fine. Shown in red under the picker. */
	FText MeshProblem;
	/** Where Put it in my level says it worked or why not, under the button; rebuilt with the result. */
	TSharedPtr<SVerticalBox> PlaceResultBox;
	/** The last Build my vehicle's result, so the step shows it again when it is opened again. */
	FMantleVehicleBuildResult LastBuild;
	bool bHaveLastBuild = false;

	// Seats and paint.
	/** "This model has no inside": the driver and passenger are not drawn. Written into the vehicle when it is built, and at once when it is already built. */
	bool bHideRiders = false;
	struct FPaintChoice
	{
		FString Section;
		bool bOn = false;
		/** The layer it is painted as: 0 the body color, else FMantleVehicleBuilder::PaintPick of a layer. */
		uint8 Pick = 0;
	};
	TArray<FPaintChoice> PaintChoices;
	/** The creator ticked or unticked a section: the ticks replace Mantle's own guess on the built vehicle. */
	bool bPaintEdited = false;
	/** The lights: which sections glow as which light and the headlight beams (the body's space, the picture's). Mantle's guess until the creator changes one. */
	FMantleVehicleSourceLights Lights;
	bool bLightsEdited = false;
	/** The picture shows the lights (the seats and paint step): the beams to drag and the light markers. */
	bool bPictureLights = false;

	// Liveries.
	TArray<FLiveryChoice> LiveryChoices;
	int32 StartLivery = INDEX_NONE;
	int32 ShownLivery = INDEX_NONE;
	/** The creator changed the liveries: they replace what the built vehicle had. */
	bool bLiveriesEdited = false;
	/** The picture shows the shown livery on the model (the Liveries step). */
	bool bPictureLiveries = false;
	TSharedPtr<SVerticalBox> LiveryRowsBox;
	/** What the Liveries step says under its buttons (why an image was not added, or where the guide was saved); red when it failed. */
	FText LiveryStatus;
	bool bLiveryStatusFailed = false;
	/** The thumbnails of the rows, kept alive for as long as the rows are. */
	TArray<TSharedPtr<FSlateBrush>> LiveryBrushes;
	/** The picture's stand-in materials for the shown livery, kept alive here. */
	TArray<TStrongObjectPtr<UObject>> LiveryPreviewMaterials;
	/** The last folder the guide was saved in. */
	FString LastGuideFolder;

	/** The size Mantle proposed for the last build, for Reset to suggested; unset when the wheels could not be read. */
	TOptional<float> SuggestedSize;
	/** The "Saved." line under the Size; empty when there is nothing to say (a scratch vehicle). */
	FText SaveStatus;
	/** True when the last save failed, which shows the line in red. */
	bool bSaveFailed = false;
	/** One transaction covers a whole slider drag, so Ctrl+Z undoes the drag in one step. */
	TSharedPtr<FScopedTransaction> SizeDragTransaction;
};
