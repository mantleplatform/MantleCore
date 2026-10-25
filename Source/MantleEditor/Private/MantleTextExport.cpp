// Copyright Joshua Eger. All Rights Reserved.
#include "MantleTextExport.h"
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceValidator.h"
#include "MantleTargetFeatures.h"

#include "Components/TextRenderComponent.h"
#include "Engine/Font.h"
#include "Engine/TextRenderActor.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTextExportPrivate
{
	/** Value pulled into [Min, Max]; a change is said in OutApproximations as the value, the range a place holds and the clamped result. */
	double TextExportClamp(double Value, double Min, double Max, const TCHAR* What, TArray<FString>& OutApproximations)
	{
		const double Clamped = FMath::Clamp(Value, Min, Max);
		if (Clamped != Value)
		{
			OutApproximations.Add(FString::Printf(TEXT("%s %g is outside what a place holds (%g to %g), so it is %g"), What, Value, Min, Max, Clamped));
		}
		return Clamped;
	}

	/** To four decimals: a float setting such as 0.1 should be written as 0.1, not 0.10000000149. */
	double TextExportRounded(double Value)
	{
		return FMath::RoundToDouble(Value * 10000.0) / 10000.0;
	}

	FString TextExportLabelOf(const AActor* Actor)
	{
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	/** The text components of Actor that publish: the Text actor's one, or a Group's shown ones. */
	void TextExportComponents(const AActor* Actor, EMantleEntityType Type, TArray<const UTextRenderComponent*>& Out)
	{
		if (Type == EMantleEntityType::Text)
		{
			if (const UTextRenderComponent* Component = FMantleTextExport::ActorComponent(Actor))
			{
				Out.Add(Component);
			}
		}
		else if (Type == EMantleEntityType::Group)
		{
			TArray<UTextRenderComponent*> Components;
			Actor->GetComponents(Components);
			for (const UTextRenderComponent* Component : Components)
			{
				if (FMantleTextExport::IsShownText(Component))
				{
					Out.Add(Component);
				}
			}
		}
	}
}

const UTextRenderComponent* FMantleTextExport::ActorComponent(const AActor* Actor)
{
	const ATextRenderActor* TextActor = Actor && Actor->GetClass() == ATextRenderActor::StaticClass() ? Cast<ATextRenderActor>(Actor) : nullptr;
	return TextActor ? TextActor->GetTextRender() : nullptr;
}

bool FMantleTextExport::IsTextActor(const AActor* Actor)
{
	return ActorComponent(Actor) != nullptr;
}

bool FMantleTextExport::IsShownText(const UTextRenderComponent* Component)
{
	return Component
		&& !Component->IsEditorOnly()
		&& !Component->HasAnyFlags(RF_Transient)
		&& Component->IsVisible()
		&& !Component->bHiddenInGame
		&& !PublishedText(Component).TrimStartAndEnd().IsEmpty();
}

FString FMantleTextExport::PublishedText(const UTextRenderComponent* Component)
{
	FString Text = Component->Text.ToString();
	Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
	Text.ReplaceInline(TEXT("<br>"), TEXT("\n"));
	return Text;
}

void FMantleTextExport::Read(const UTextRenderComponent* Component, const FVector& Scale, FMantleTextProps& Out, TArray<FString>& OutApproximations)
{
	using namespace MantleTextExportPrivate;
	Out = FMantleTextProps();
	Out.Text = PublishedText(Component);
	// TextRenderColor is sRGB bytes; the place holds linear RGBA (FLinearColor(FColor) converts the colour, alpha stays linear).
	Out.Color = FLinearColor(Component->TextRenderColor);
	Out.WorldSize = TextExportRounded(TextExportClamp(Component->WorldSize, FMantlePlaceLimits::MinTextWorldSize,
		FMantlePlaceLimits::MaxTextWorldSize, TEXT("its world size"), OutApproximations));

	switch (Component->HorizontalAlignment)
	{
	case EHTA_Center: Out.HorizontalAlignment = EMantleTextHorizontalAlignment::Center; break;
	case EHTA_Right: Out.HorizontalAlignment = EMantleTextHorizontalAlignment::Right; break;
	default: Out.HorizontalAlignment = EMantleTextHorizontalAlignment::Left; break;
	}
	switch (Component->VerticalAlignment)
	{
	case EVRTA_TextCenter: Out.VerticalAlignment = EMantleTextVerticalAlignment::Center; break;
	case EVRTA_TextBottom: Out.VerticalAlignment = EMantleTextVerticalAlignment::Bottom; break;
	case EVRTA_QuadTop:
		// The quad's top edge sits just above the text's own top; the nearest a place holds is the text's top.
		Out.VerticalAlignment = EMantleTextVerticalAlignment::Top;
		OutApproximations.Add(TEXT("its Quad Top alignment is published as the text's top"));
		break;
	default: Out.VerticalAlignment = EMantleTextVerticalAlignment::Top; break;
	}

	// The scale is folded in: along the line is the component's Y, up the glyph its Z.
	if (Scale.Y < 0.0 || Scale.Z < 0.0)
	{
		OutApproximations.Add(TEXT("its negative scale mirrors the text, and Mantle draws it unmirrored"));
	}
	Out.XScale = TextExportRounded(TextExportClamp(Component->XScale * FMath::Abs(Scale.Y), FMantlePlaceLimits::MinTextScale,
		FMantlePlaceLimits::MaxTextScale, TEXT("its horizontal scale"), OutApproximations));
	Out.YScale = TextExportRounded(TextExportClamp(Component->YScale * FMath::Abs(Scale.Z), FMantlePlaceLimits::MinTextScale,
		FMantlePlaceLimits::MaxTextScale, TEXT("its vertical scale"), OutApproximations));

	// What Mantle draws differently: its own font and material, and no spacing adjustments.
	const UTextRenderComponent* Default = GetDefault<UTextRenderComponent>();
	if (Component->Font && Component->Font != Default->Font)
	{
		OutApproximations.Add(FString::Printf(TEXT("drawn in Mantle's font: its font '%s' is not carried"), *Component->Font->GetName()));
	}
	if (Component->TextMaterial && Component->TextMaterial != Default->TextMaterial)
	{
		OutApproximations.Add(FString::Printf(TEXT("drawn in Mantle's font: its text material '%s' is not carried"), *Component->TextMaterial->GetName()));
	}
	if (!FMath::IsNearlyZero(Component->HorizSpacingAdjust) || !FMath::IsNearlyZero(Component->VertSpacingAdjust))
	{
		OutApproximations.Add(TEXT("its letter and line spacing adjustments are not carried"));
	}
}

FString FMantleTextExport::Refusal(const FMantleTextProps& Props)
{
	const int32 Length = FMantlePlaceParser::CodePointLength(Props.Text);
	if (Length > FMantlePlaceLimits::MaxTextLength)
	{
		return FString::Printf(TEXT("has text %d characters long; a place holds at most %d per text."), Length, FMantlePlaceLimits::MaxTextLength);
	}
	return FString();
}

void FMantleTextExport::Extract(const UTextRenderComponent* Component, FMantleEntity& Entity)
{
	FMantleTextProps Props;
	TArray<FString> Ignored;
	Read(Component, Entity.Transform.GetScale3D(), Props, Ignored);
	Entity.Props.Set<FMantleTextProps>(MoveTemp(Props));
	Entity.Transform.SetScale3D(FVector::OneVector);
}

void FMantleTextExport::Admit(const UTextRenderComponent* Component, const FString& Label, const FString& Path, const FVector& Scale,
	int32& InOutTexts, FMantleCoverage* Coverage, TArray<FMantleParseError>& Errors)
{
	FMantleTextProps Props;
	TArray<FString> Notes;
	Read(Component, Scale, Props, Notes);

	if (++InOutTexts == FMantlePlaceLimits::MaxTextsPerPlace + 1)
	{
		Errors.Add(FMantleParseError{
			Path + TEXT("/type"),
			FString::Printf(TEXT("'%s' is text number %d; a place holds at most %d. Remove some, or join lines into fewer texts."),
				*Label, InOutTexts, FMantlePlaceLimits::MaxTextsPerPlace) });
	}
	const FString Why = Refusal(Props);
	if (!Why.IsEmpty())
	{
		Errors.Add(FMantleParseError{ Path + TEXT("/properties/text"), FString::Printf(TEXT("'%s' %s Shorten it."), *Label, *Why) });
	}
	if (!Coverage)
	{
		return;
	}
	if (Notes.Num() == 0)
	{
		Coverage->Add(MantleCoverageCategory::Text, EMantleCoverageOutcome::Kept, Label);
	}
	else
	{
		Coverage->Add(MantleCoverageCategory::Text, EMantleCoverageOutcome::Approximated, Label, FString::Join(Notes, TEXT("; ")) + TEXT("."));
	}
}

void FMantleTextExport::Check(const AActor* Actor, EMantleEntityType Type, int32& InOutTexts, FMantleValidationResult& Result)
{
	using namespace MantleTextExportPrivate;
	if (!FMantleTargetFeatures::Current().Reads(MantleClientFeatures::WorldText))
	{
		return;
	}
	TArray<const UTextRenderComponent*> Components;
	TextExportComponents(Actor, Type, Components);
	const FString Label = TextExportLabelOf(Actor);
	for (const UTextRenderComponent* Component : Components)
	{
		if (!IsShownText(Component))
		{
			continue;
		}
		FMantleTextProps Props;
		TArray<FString> Notes;
		Read(Component, Component->GetComponentScale(), Props, Notes);
		if (++InOutTexts == FMantlePlaceLimits::MaxTextsPerPlace + 1)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Error,
				Label,
				FString::Printf(TEXT("is text number %d; a place holds at most %d."), InOutTexts, FMantlePlaceLimits::MaxTextsPerPlace),
				TEXT("Remove some world text, or put several lines in one text.") });
		}
		const FString Why = Refusal(Props);
		if (!Why.IsEmpty())
		{
			Result.Issues.Add(FMantleValidationIssue{ EMantleIssueSeverity::Error, Label, Why, TEXT("Shorten the text, or split it across several.") });
		}
		if (Notes.Num() > 0)
		{
			Result.Issues.Add(FMantleValidationIssue{
				EMantleIssueSeverity::Warning,
				Label,
				FString::Printf(TEXT("has world text Mantle draws differently: %s."), *FString::Join(Notes, TEXT("; "))),
				TEXT("It publishes with the nearest look Mantle has.") });
		}
	}
}
