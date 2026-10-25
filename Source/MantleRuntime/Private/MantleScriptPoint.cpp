// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptPoint.h"

#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"

#if WITH_EDITORONLY_DATA
#include "Components/ArrowComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Texture2D.h"
#include "UObject/ConstructorHelpers.h"
#endif

AMantleScriptPoint::AMantleScriptPoint()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	SetCanBeDamaged(false);

	PointRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PointRoot"));
	SetRootComponent(PointRoot);

#if WITH_EDITORONLY_DATA
	EditorArrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("EditorArrow"));
	if (EditorArrow)
	{
		EditorArrow->SetupAttachment(PointRoot);
		EditorArrow->ArrowColor = FColor(120, 160, 255);
		EditorArrow->ArrowSize = 1.0f;
		EditorArrow->bIsScreenSizeScaled = true;
		EditorArrow->SetIsVisualizationComponent(true);
		EditorArrow->SetHiddenInGame(true);
	}
	EditorBillboard = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("EditorBillboard"));
	if (EditorBillboard)
	{
		EditorBillboard->SetupAttachment(PointRoot);
		// An engine-shipped placement icon, as the spawn points use; cosmetic only.
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> PointIcon(TEXT("/Engine/EditorResources/S_TargetPoint"));
		EditorBillboard->Sprite = PointIcon.Get();
		EditorBillboard->bIsScreenSizeScaled = true;
		EditorBillboard->SetIsVisualizationComponent(true);
		EditorBillboard->SetHiddenInGame(true);
	}

	// A stored package is another creator's item that is never loaded into the project, so the point shows only its
	// name. Two labels back to back: a text render faces one way, +X for the front one.
	for (const bool bFront : { true, false })
	{
		UTextRenderComponent* Label = CreateEditorOnlyDefaultSubobject<UTextRenderComponent>(bFront ? TEXT("EditorLabelFront") : TEXT("EditorLabelBack"));
		(bFront ? EditorLabelFront : EditorLabelBack) = Label;
		if (Label)
		{
			Label->SetupAttachment(PointRoot);
			Label->SetRelativeLocation(FVector(0.0, 0.0, 80.0));
			Label->SetRelativeRotation(FRotator(0.0, bFront ? 0.0 : 180.0, 0.0));
			Label->SetHorizontalAlignment(EHTA_Center);
			Label->SetVerticalAlignment(EVRTA_TextCenter);
			Label->SetWorldSize(28.0f);
			Label->SetTextRenderColor(FColor(150, 190, 255));
			Label->SetText(FText::GetEmpty());
			Label->SetVisibility(false);
			Label->SetIsVisualizationComponent(true);
			Label->SetHiddenInGame(true);
		}
	}
#endif
}

FString AMantleScriptPoint::MakeEditorLabelText() const
{
	if (StorageItem.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("\"%s\"\n(script package)"), *StorageItem);
}

void AMantleScriptPoint::UpdateEditorLabel()
{
#if WITH_EDITORONLY_DATA
	const FString Text = MakeEditorLabelText();
	for (UTextRenderComponent* Label : { EditorLabelFront.Get(), EditorLabelBack.Get() })
	{
		if (Label)
		{
			Label->SetText(FText::FromString(Text));
			Label->SetVisibility(!Text.IsEmpty());
		}
	}
#endif
}

#if WITH_EDITOR
void AMantleScriptPoint::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateEditorLabel();
}

void AMantleScriptPoint::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	UpdateEditorLabel();
}

void AMantleScriptPoint::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateEditorLabel();
}
#endif

void AMantleScriptPoint::ApplyProps(const FMantleScriptPointProps& In)
{
	StorageItem = In.StorageItem;
	Parameters.Reset();
	for (const FMantleScriptPointParameter& Parameter : In.Parameters)
	{
		Parameters.Add(Parameter.Name, Parameter.Value);
	}
	bEnabled = In.bEnabled;
	UpdateEditorLabel();
}

bool AMantleScriptPoint::ReadProps(FMantleScriptPointProps& Out, FString& OutProblem) const
{
	Out = FMantleScriptPointProps();
	Out.StorageItem = StorageItem;
	Out.bEnabled = bEnabled;
	for (const TPair<FString, FString>& Pair : Parameters)
	{
		FMantleScriptPointParameter& Parameter = Out.Parameters.AddDefaulted_GetRef();
		Parameter.Name = Pair.Key;
		Parameter.Value = Pair.Value;
	}
	Out.Parameters.Sort([](const FMantleScriptPointParameter& A, const FMantleScriptPointParameter& B)
	{
		return A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
	});

	if (!FMantlePlaceLimits::IsStorageName(StorageItem))
	{
		OutProblem = StorageItem.IsEmpty()
			? FString(TEXT("names no script package; set Storage Item to the Name of a script package in Mantle Place Settings > Storage"))
			: FString::Printf(TEXT("names storage '%s'; a Storage Item is a letter followed by up to 63 letters, digits or underscores"), *StorageItem);
		return false;
	}
	if (Out.Parameters.Num() > FMantlePlaceLimits::MaxScriptPointParameters)
	{
		OutProblem = FString::Printf(TEXT("has %d parameters; remove some so it has at most %d"),
			Out.Parameters.Num(), FMantlePlaceLimits::MaxScriptPointParameters);
		return false;
	}
	for (const FMantleScriptPointParameter& Parameter : Out.Parameters)
	{
		if (!FMantlePlaceLimits::IsScriptPointParameterName(Parameter.Name))
		{
			OutProblem = FString::Printf(TEXT("has a parameter named '%s'; a parameter name is a letter followed by up to 31 letters, digits or underscores"),
				*Parameter.Name);
			return false;
		}
		if (FMantlePlaceParser::CodePointLength(Parameter.Value) > FMantlePlaceLimits::MaxScriptPointParameterValueLength)
		{
			OutProblem = FString::Printf(TEXT("gives parameter '%s' a value longer than %d characters; shorten it"),
				*Parameter.Name, FMantlePlaceLimits::MaxScriptPointParameterValueLength);
			return false;
		}
	}
	return true;
}
