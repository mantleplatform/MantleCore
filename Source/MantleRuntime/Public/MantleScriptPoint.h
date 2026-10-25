// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MantlePlaceTypes.h"

#include "MantleScriptPoint.generated.h"

class UArrowComponent;
class UBillboardComponent;
class UTextRenderComponent;

/**
 * Where a stored script package is anchored in the level (#838): the ScriptPoint place entity. It only holds data:
 * which package (by its name in the place's storage), the parameters the package declares, and whether the point is
 * on. The package's scripts run once per side, never once per point, and loop over their points with Script.Points;
 * each point is an ordinary entity there, tagged with the storage name, its parameters readable with
 * Entity:GetAttribute. Hidden in game, never replicated, and carries nothing a client draws.
 */
UCLASS(ClassGroup = Mantle, meta = (DisplayName = "Mantle Script Point"),
	hidecategories = (Advanced, Cooking, HLOD, Input, LOD, Networking, Physics, Replication, Collision))
class MANTLERUNTIME_API AMantleScriptPoint : public AActor
{
	GENERATED_BODY()

public:
	AMantleScriptPoint();

	/**
	 * The script package this point belongs to, by the Name of its entry in Mantle Place Settings > Storage. Add packages
	 * to the storage with Tools > Mantle > Library. The editor shows the name above the point.
	 */
	UPROPERTY(EditAnywhere, Category = "Script")
	FString StorageItem;

	/**
	 * The values this point gives the package's parameters, at most 16. Each key is a letter followed by up to 31
	 * letters, digits or underscores; each value is text of at most 256 characters, which the package reads as the
	 * type it declares (a number, true or false, or text).
	 */
	UPROPERTY(EditAnywhere, Category = "Script")
	TMap<FString, FString> Parameters;

	/** Untick to keep the point in your place without the package's scripts seeing it. */
	UPROPERTY(EditAnywhere, Category = "Script")
	bool bEnabled = true;

	/** Sets the actor from ScriptPoint properties: the place builder's and the editor importer's. */
	void ApplyProps(const FMantleScriptPointProps& In);

	/**
	 * Reads the actor into ScriptPoint properties, the parameters sorted by key. False, with OutProblem phrased to
	 * follow the actor's name and saying what to fix, when StorageItem is not a storage name, there are too many
	 * parameters, or a key or value breaks the schema's rules. Out is filled either way.
	 */
	bool ReadProps(FMantleScriptPointProps& Out, FString& OutProblem) const;

	/**
	 * What the editor label above the point says: the Storage Item name between quotes, and a second line saying a
	 * script package is anchored here; empty when StorageItem is empty (no label shows). Pure, so a test can pin it.
	 */
	FString MakeEditorLabelText() const;

protected:
#if WITH_EDITOR
	virtual void OnConstruction(const FTransform& Transform) override;
	/** A level opened in the editor registers its actors without running their construction: the label is set here then. */
	virtual void PostRegisterAllComponents() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** Sets the editor-only label from StorageItem. Does nothing in a game build. */
	void UpdateEditorLabel();

	UPROPERTY(VisibleAnywhere, Category = "Script")
	TObjectPtr<USceneComponent> PointRoot;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UArrowComponent> EditorArrow;

	UPROPERTY()
	TObjectPtr<UBillboardComponent> EditorBillboard;

	/** The Storage Item label, readable from the front and from behind (a text render faces one way). */
	UPROPERTY()
	TObjectPtr<UTextRenderComponent> EditorLabelFront;

	UPROPERTY()
	TObjectPtr<UTextRenderComponent> EditorLabelBack;
#endif
};
