// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleShape.h"
#include "MantleVehicleSpawnPoint.h"
#include "SMantleBuilderFlow.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWidget.h"

class FMantleVehicleShapeKind;
class SMantleBuilderViewport;
class SVerticalBox;
class UMantleBuiltVehicle;
class UMantleVehicleSource;
class UObject;
struct FAssetData;
class FScopedTransaction;

/**
 * Tools > Mantle > Build a vehicle...: four short steps, one decision each. Pick what you are building (a vehicle type: a card for
 * every publishable preset). Shape it while a grey picture of it changes, and read what it is like: a road vehicle has eight sliders in
 * centimetres and says what it weighs, how it turns and how fast it goes; a plane, helicopter or hovercraft has one Size slider (and a seat or a
 * top-speed feel where it has one) and says what suits it (FMantleVehicleShape::Describe). Drive it: put the grey vehicle in your level with no
 * model at all. Or, as an option, use your own 3D model as its body. The result, good or bad, shows in the window; there are no dialogs.
 * The logic is FMantleVehicleShape (the shape) and FMantleVehicleBuilder (the vehicle assets); this is only the page. Native Slate.
 */
class SMantleVehicleBuilder : public SCompoundWidget
{
	/** The tests drive the window's state the way its buttons do. */
	friend struct FMantleVehicleBuilderPanelAccess;
	/** The model step's state, for its own tests. */
	friend struct FMantleVehicleBuilderModelAccess;
	/** The tests of the stale build after a shape edit, the names a build again reuses and the .fbx import. */
	friend struct FMantleVehicleBuilderFixAccess;

public:
	SLATE_BEGIN_ARGS(SMantleVehicleBuilder) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Cancelling (closing the window other than with Done) takes the grey vehicles this window put in the level back out. */
	virtual ~SMantleVehicleBuilder() override;

	/** The nomad tab: registered at module startup, opened from the Tools menu. */
	static const FName TabName;
	static void RegisterTab();
	static void UnregisterTab();
	static void OpenTab();

private:
	TArray<FMantleBuilderStep> MakeSteps();
	TSharedRef<SWidget> MakeTypeStep();
	TSharedRef<SWidget> MakeShapeStep();
	TSharedRef<SWidget> MakeDriveStep();
	TSharedRef<SWidget> MakeModelStep();
	TSharedRef<SWidget> MakePresetCard(const FString& PresetId, const FText& Name, const FText& Description);
	TSharedRef<SWidget> MakeDimensionRow(EMantleVehicleDimension Dimension);
	/** The rows of a type shaped by one Size (a plane, helicopter or hovercraft): the Size, its feel and its passenger seat, where it has them. */
	TSharedRef<SWidget> MakeSizeRow();
	/** One feel slider (grip, power, suspension or top speed), -1 to 1. A road vehicle has all four, a hovercraft its top speed. */
	TSharedRef<SWidget> MakeFeelRow(EMantleVehicleFeel Feel);
	TSharedRef<SWidget> MakePassengerRow();
	/** The feel sliders the shaped type has, and its passenger tickbox where its preset has a passenger seat, one under the other. */
	void AddFeelRows(const TSharedRef<SVerticalBox>& Rows);

	/** Picks a type: loads its chassis as the archetype and starts the shape from it. Picking the type already picked changes nothing. */
	void PickPreset(const FString& PresetId, const FText& Name);

	// Shape.
	void SetDimension(EMantleVehicleDimension Dimension, double Value);
	void SetShapeSize(double Value);
	void SetFeel(EMantleVehicleFeel Feel, double Value);
	void SetPassengerSeat(bool bOn);
	/**
	 * What a change of the shape that really changed something leaves to do: the vehicle in the level is no longer this one, the build made from the old shape is
	 * forgotten (it would otherwise be put in the level, shown in its settings and kept by Done with the old numbers), and the picture is redrawn.
	 */
	void ShapeChanged();
	FReply OnResetShape();
	/** Shows the shaped chassis in the picture: a transient copy of the grey body, made the way a published chassis is (FMantleVehicleChassisBuilder::MakeAsset). */
	void RebuildPreview(bool bFrame);
	/** The picture is out of date: it is redrawn in a moment, once, however many sizes changed meanwhile (a drag changes them every frame). */
	void MarkPreviewDirty();
	/** Redraws the picture now if it is out of date: at the end of a drag, after a typed size. */
	void FlushPreview();
	EActiveTimerReturnType OnPreviewTimer(double CurrentTime, float DeltaTime);

	// Drive (the grey vehicle, no model).
	FReply OnPutGreyInLevel();

	// Your own model (the vehicle on a creator's static or skeletal mesh).
	bool CanBuild() const;
	FReply OnBuild();
	/**
	 * What Build my vehicle asks for (FMantleVehicleBuilder::BuildAny's options): for a model worn as a skin, the shape, the facing and the parts as the page has them,
	 * and, when this window already built this type and model, the name that build was saved under, to be updated in place and not added to.
	 */
	FMantleVehicleModelOptions MakeBuildOptions() const;
	/** Remembers the name a build was saved under, for the same type and model, so the next press of Build my vehicle updates it. */
	void RememberBuilt(const FMantleVehicleBuildResult& Result);
	FReply OnShowSettings();
	FReply OnPlaceInLevel();
	/** Pick an .fbx file...: asks for a file and imports it. */
	FReply OnPickFbx();
	/** Imports an .fbx file into the project (FMantleVehicleBuilder::ImportModelFile) and picks the model it made, or says in the window why it could not. */
	void ImportFbx(const FString& File);
	void ShowResult(const FMantleVehicleBuildResult& Result);
	void SetMesh(const FAssetData& Asset);
	/**
	 * Reads the picked model for the picked type: the chassis's moving parts it can be assigned to, Mantle's suggestions for which part is which and which way
	 * the model faces, and, when the model is worn as a skin on the chassis, the Size or sizes the chassis is proposed at (set on the shape, where the creator already
	 * has the sliders). Called when a model or a type is picked.
	 */
	void ReadModel(TOptional<bool> Twin = TOptional<bool>());
	/** A propeller plane with two engines (a P-38): the grey vehicle gets a propeller bone for each, and the parts list a left and a right propeller. */
	void SetTwinEngine(bool bOn);
	/** The model's nose faces this way (a turn about Z, 0 / 90 / 180 / 270); the model's size is read again for it. */
	void SetModelYaw(double Degrees);
	/** Assigns a part of the model (or, for an unset piece, nothing) to one of the chassis's moving bones. */
	void SetPart(FName ChassisBone, const FMantleVehicleModelPiece& Piece);
	/** The parts and facing rows under the picker, made again for the model and type picked. */
	void RebuildModelRows();
	/** Sets the chassis's Size or sizes on the shape from the model (FMantleVehicleChassisSkin::Propose) for the way it faces now; the sliders show and change them. */
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

	/** Done: closes the window, first taking out the grey vehicle when the model's vehicle is in the level instead. */
	void FinishWindow();
	void CloseWindow();
	/** Takes every grey vehicle this window put in the level out of it again (one undo step). */
	void RemoveGreySpawns();

	TSharedPtr<SMantleBuilderFlow> Flow;

	/** The vehicle type picked in step 1 (a preset id); empty until one is picked. */
	FString SelectedPresetId;
	FText SelectedPresetName;
	/** Why the picked type's chassis could not be read; empty when it was. Shown in red under the cards. */
	FText TypeProblem;

	// Shape.
	/** The picked type's chassis, the sizes the sliders show and everything that follows from them (the summary, the warnings). */
	FMantleVehicleShapeSession Shape;
	bool bPreviewDirty = false;
	bool bPreviewTimerPending = false;
	/** Why the picture could not be redrawn at these sizes; empty when it could. */
	FText PreviewProblem;
	TSharedPtr<FMantleVehicleShapeKind> ShapeKind;
	/** Weak: the picture lives only while the Shape step is open. */
	TWeakPtr<SMantleBuilderViewport> ShapeViewport;
	/** The transient grey vehicle the picture shows; replaced with every change. */
	TStrongObjectPtr<UMantleBuiltVehicle> PreviewVehicle;

	// Drive.
	/** The name the grey vehicle is saved under (a folder and assets of this name); empty until it is first put in the level, new for each type. */
	FString GreyName;
	FString GreyPresetId;
	/** The vehicle made for the sizes GreySignature names, if it is still there: pressing Put it in my level again for the same sizes remakes nothing. */
	TWeakObjectPtr<UMantleVehicleSource> GreySource;
	FString GreySignature;
	/** What the last build of it said besides that it worked: where it was saved, and its notes. */
	FString GreyExtra;
	bool bGreyNotSaved = false;
	/** What Put it in my level said, under the button, in plain words. */
	FText DriveStatus;
	/** What the placement said about where a flier went and what it needs, kept so a second press repeats it. */
	FString GreyPlaceNote;
	bool bDriveFailed = false;
	/** The grey vehicles Put it in my level added to the level, so Cancel and Done can take them out again. */
	TArray<TWeakObjectPtr<AMantleVehicleSpawnPoint>> GreySpawns;
	/** Done was pressed: closing the window now is not cancelling. */
	bool bFinished = false;

	// Your own model.
	/** The model picked: a static mesh or a skeletal mesh. */
	TWeakObjectPtr<UObject> Mesh;

	/** #124: how this vehicle's library picture is made, kept for the model (FMantleThumbnailStore). */
	FMantleThumbnailChoice ThumbnailChoice;
	TSharedPtr<class SMantleThumbnailSection> ThumbnailSection;
	/** Reads the saved choice for the picked model and redraws the section. */
	void LoadThumbnailChoice();
	void ThumbnailChoiceChanged(const FMantleThumbnailChoice& Choice);
	/** After a build: a Custom image or a chosen angle replaces the picture the build took by itself. Notes and saving go into Result. */
	void ApplyThumbnailChoice(FMantleVehicleBuildResult& Result);
	TWeakObjectPtr<UMantleVehicleSource> BuiltSource;
	/** The model read for the picked type (ReadModel), the chassis's moving parts, which part of it is which, and which way it faces. */
	FMantleVehicleModel ModelInfo;
	bool bHaveModelInfo = false;
	TArray<FMantleVehiclePartSlot> PartSlots;
	FMantleVehicleModelMap PartMap;
	double ModelYaw = 0.0;
	/** What Mantle proposed for the chassis's size from the model, in plain words. */
	FString ProposalText;
	/** The rows under the picker: the facing, the Size and one row for each part the chassis can move. */
	TSharedPtr<SVerticalBox> ModelRowsBox;
	/** The choices of a part row's list, kept alive for as long as the rows are. */
	TArray<TSharedPtr<FString>> PieceChoices;
	TArray<FMantleVehicleModelPiece> PieceValues;
	/** The four ways a model's nose may face and the turn each one needs. */
	TArray<TSharedPtr<FString>> YawChoices;
	TArray<double> YawValues;
	/** The picked model is worn as a skin on the chassis (else its own wheels are fitted). Worked out when the model or the type is picked. */
	bool bUseSkin = false;
	/** The grey vehicle is the twin-engined plane (FMantleVehicleChassisSkin::AsTwin): set when the model has two propellers, or by the creator. */
	bool bTwinEngine = false;
	TSharedPtr<SVerticalBox> ResultBox;
	/** What Pick an .fbx file... did, in plain words, under the picker; red when it failed. */
	FText ImportStatus;
	bool bImportFailed = false;
	/** The folder the last .fbx was picked from, so the next dialog opens there. */
	FString LastFbxFolder;
	/**
	 * The name the last build of a model on a chassis was saved under, and which type and model it was for: pressing Build again for the same pair updates those assets
	 * instead of adding a new set each press.
	 */
	FString BuiltAssetName;
	FString BuiltAssetKey;
	/** Why the model picked cannot be used (a solid-piece model, or not a model at all); empty when it is fine. Shown in red under the picker. */
	FText MeshProblem;
	/** Where Put it in my level says it worked or why not, under the button; rebuilt with the result. */
	TSharedPtr<SVerticalBox> PlaceResultBox;
	/** The last Build my vehicle's result, so the step shows it again when it is opened again. */
	FMantleVehicleBuildResult LastBuild;
	bool bHaveLastBuild = false;

	/** The size Mantle proposed for the last build, for Reset to suggested; unset when the wheels could not be read. */
	TOptional<float> SuggestedSize;
	/** The "Saved." line under the Size and checkbox; empty when there is nothing to say (a scratch vehicle). */
	FText SaveStatus;
	/** True when the last save failed, which shows the line in red. */
	bool bSaveFailed = false;
	/** One transaction covers a whole slider drag, so Ctrl+Z undoes the drag in one step. */
	TSharedPtr<FScopedTransaction> SizeDragTransaction;
};
