// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleRuntime.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCoverageCpp
{
	constexpr EMantleCoverageOutcome AllOutcomes[] = {
		EMantleCoverageOutcome::Kept, EMantleCoverageOutcome::Approximated, EMantleCoverageOutcome::Dropped, EMantleCoverageOutcome::Info };

	/** The order the report lists the named categories in. */
	const TArray<FString>& CanonicalOrder()
	{
		static const TArray<FString> Order = {
			MantleCoverageCategory::StaticMeshes, MantleCoverageCategory::SkeletalMeshes, MantleCoverageCategory::InstancedMeshes,
			MantleCoverageCategory::PaintedFoliage, MantleCoverageCategory::SplineMeshes, MantleCoverageCategory::Materials,
			MantleCoverageCategory::Terrain, MantleCoverageCategory::TerrainLayers, MantleCoverageCategory::LandscapeMaterial,
			MantleCoverageCategory::LandscapeGrass, MantleCoverageCategory::WaterCarve, MantleCoverageCategory::Lights, MantleCoverageCategory::Sky,
			MantleCoverageCategory::Fog, MantleCoverageCategory::PostProcess, MantleCoverageCategory::Volumes,
			MantleCoverageCategory::Particles, MantleCoverageCategory::Decals, MantleCoverageCategory::Sounds, MantleCoverageCategory::Text,
			MantleCoverageCategory::SubLevels, MantleCoverageCategory::BlueprintLogic, MantleCoverageCategory::GameplayActors,
			MantleCoverageCategory::OtherComponents, MantleCoverageCategory::OtherActors };
		return Order;
	}

	/** A name or detail on one line: the log and the panel never carry a line break from a creator's label. */
	FString OneLine(const FString& Text)
	{
		FString Out = Text;
		Out.ReplaceInline(TEXT("\r"), TEXT(" "));
		Out.ReplaceInline(TEXT("\n"), TEXT(" "));
		return Out;
	}

	/** "Name x3: detail", or "Name", or "detail" when there is no name. */
	FString ExampleText(const FMantleCoverageExample& Example)
	{
		FString Text = OneLine(Example.Name);
		if (Example.Count > 1)
		{
			Text += FString::Printf(TEXT("%sx%lld"), Text.IsEmpty() ? TEXT("") : TEXT(" "), Example.Count);
		}
		if (!Example.Detail.IsEmpty())
		{
			Text += (Text.IsEmpty() ? TEXT("") : TEXT(": ")) + OneLine(Example.Detail);
		}
		return Text;
	}

	void WriteBucket(TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>& Writer, const FMantleCoverageBucket& Bucket)
	{
		Writer.WriteValue(TEXT("count"), Bucket.Count);
		Writer.WriteArrayStart(TEXT("items"));
		for (const FMantleCoverageExample& Example : Bucket.Examples)
		{
			Writer.WriteObjectStart();
			Writer.WriteValue(TEXT("name"), Example.Name);
			Writer.WriteValue(TEXT("detail"), Example.Detail);
			Writer.WriteValue(TEXT("count"), Example.Count);
			Writer.WriteObjectEnd();
		}
		Writer.WriteArrayEnd();
	}
}

bool FMantleCoverageCategory::IsEmpty() const
{
	for (const FMantleCoverageBucket& Bucket : Buckets)
	{
		if (Bucket.Count > 0)
		{
			return false;
		}
	}
	return true;
}

FMantleCoverageCategory& FMantleCoverage::FindOrAdd(const FString& Category)
{
	for (FMantleCoverageCategory& Existing : Categories)
	{
		if (Existing.Name == Category)
		{
			return Existing;
		}
	}
	FMantleCoverageCategory& Added = Categories.AddDefaulted_GetRef();
	Added.Name = Category;
	return Added;
}

void FMantleCoverage::Add(const FString& Category, EMantleCoverageOutcome Outcome, const FString& Name, const FString& Detail, int64 Count)
{
	if (Count <= 0)
	{
		return;
	}
	FMantleCoverageBucket& Bucket = FindOrAdd(Category).Buckets[static_cast<int32>(Outcome)];
	Bucket.Count += Count;
	if (Name.IsEmpty() && Detail.IsEmpty())
	{
		return;
	}
	for (FMantleCoverageExample& Example : Bucket.Examples)
	{
		if (Example.Name == Name && Example.Detail == Detail)
		{
			Example.Count += Count;
			return;
		}
	}
	if (Bucket.Examples.Num() < MaxExamples)
	{
		Bucket.Examples.Add(FMantleCoverageExample{ Name, Detail, Count });
	}
}

void FMantleCoverage::Append(const FMantleCoverage& Other)
{
	for (const FMantleCoverageCategory& Category : Other.Categories)
	{
		for (const EMantleCoverageOutcome Outcome : MantleCoverageCpp::AllOutcomes)
		{
			const FMantleCoverageBucket& Bucket = Category.Get(Outcome);
			// The examples first, then what they do not account for: the count stays the other report's.
			int64 Named = 0;
			for (const FMantleCoverageExample& Example : Bucket.Examples)
			{
				Add(Category.Name, Outcome, Example.Name, Example.Detail, Example.Count);
				Named += Example.Count;
			}
			if (Bucket.Count > Named)
			{
				Add(Category.Name, Outcome, FString(), FString(), Bucket.Count - Named);
			}
		}
	}
}

const FMantleCoverageCategory* FMantleCoverage::Find(const FString& Category) const
{
	return Categories.FindByPredicate([&Category](const FMantleCoverageCategory& Existing) { return Existing.Name == Category; });
}

int64 FMantleCoverage::Count(const FString& Category, EMantleCoverageOutcome Outcome) const
{
	const FMantleCoverageCategory* Found = Find(Category);
	return Found ? Found->Count(Outcome) : 0;
}

int64 FMantleCoverage::Total(EMantleCoverageOutcome Outcome) const
{
	int64 Total = 0;
	for (const FMantleCoverageCategory& Category : Categories)
	{
		Total += Category.Count(Outcome);
	}
	return Total;
}

bool FMantleCoverage::HasCategory(const FString& Category) const
{
	const FMantleCoverageCategory* Found = Find(Category);
	return Found && !Found->IsEmpty();
}

void FMantleCoverage::Sort()
{
	const TArray<FString>& Order = MantleCoverageCpp::CanonicalOrder();
	// Stable: a category the order does not name keeps its place among the others.
	Categories.StableSort([&Order](const FMantleCoverageCategory& A, const FMantleCoverageCategory& B)
	{
		const int32 IndexA = Order.IndexOfByKey(A.Name);
		const int32 IndexB = Order.IndexOfByKey(B.Name);
		return (IndexA == INDEX_NONE ? Order.Num() : IndexA) < (IndexB == INDEX_NONE ? Order.Num() : IndexB);
	});
}

const TCHAR* FMantleCoverage::OutcomeKey(EMantleCoverageOutcome Outcome)
{
	switch (Outcome)
	{
	case EMantleCoverageOutcome::Kept: return TEXT("kept");
	case EMantleCoverageOutcome::Approximated: return TEXT("approx");
	case EMantleCoverageOutcome::Dropped: return TEXT("dropped");
	default: return TEXT("info");
	}
}

const TCHAR* FMantleCoverage::OutcomeWord(EMantleCoverageOutcome Outcome)
{
	switch (Outcome)
	{
	case EMantleCoverageOutcome::Kept: return TEXT("kept");
	case EMantleCoverageOutcome::Approximated: return TEXT("approximated");
	case EMantleCoverageOutcome::Dropped: return TEXT("dropped");
	default: return TEXT("nothing to export");
	}
}

FString FMantleCoverage::Slug(const FString& Category)
{
	FString Slug;
	bool bGap = false;
	for (const TCHAR Char : Category)
	{
		if (FChar::IsAlnum(Char))
		{
			if (bGap && !Slug.IsEmpty())
			{
				Slug += TEXT('-');
			}
			bGap = false;
			Slug += FChar::ToLower(Char);
		}
		else
		{
			bGap = true;
		}
	}
	return Slug;
}

TArray<FString> FMantleCoverage::ToLogLines() const
{
	using namespace MantleCoverageCpp;
	TArray<FString> Lines;
	for (const FMantleCoverageCategory& Category : Categories)
	{
		if (Category.IsEmpty())
		{
			continue;
		}
		const FString Slug = FMantleCoverage::Slug(Category.Name);
		FString Line = FString::Printf(TEXT("MANTLE_COVERAGE %s kept=%lld approx=%lld dropped=%lld"), *Slug,
			Category.Count(EMantleCoverageOutcome::Kept), Category.Count(EMantleCoverageOutcome::Approximated), Category.Count(EMantleCoverageOutcome::Dropped));
		if (Category.Count(EMantleCoverageOutcome::Info) > 0)
		{
			Line += FString::Printf(TEXT(" info=%lld"), Category.Count(EMantleCoverageOutcome::Info));
		}
		Lines.Add(MoveTemp(Line));
		for (const EMantleCoverageOutcome Outcome : AllOutcomes)
		{
			// A kept bucket's names say nothing a count does not; everything else is named.
			if (Outcome == EMantleCoverageOutcome::Kept)
			{
				continue;
			}
			for (const FMantleCoverageExample& Example : Category.Get(Outcome).Examples)
			{
				Lines.Add(FString::Printf(TEXT("MANTLE_COVERAGE_ITEM %s %s %s"), *Slug, OutcomeKey(Outcome), *ExampleText(Example)));
			}
			const FMantleCoverageBucket& Bucket = Category.Get(Outcome);
			int64 Named = 0;
			for (const FMantleCoverageExample& Example : Bucket.Examples)
			{
				Named += Example.Count;
			}
			if (Bucket.Count > Named)
			{
				Lines.Add(FString::Printf(TEXT("MANTLE_COVERAGE_ITEM %s %s ... and %lld more"), *Slug, OutcomeKey(Outcome), Bucket.Count - Named));
			}
		}
	}
	return Lines;
}

void FMantleCoverage::Log() const
{
	for (const FString& Line : ToLogLines())
	{
		UE_LOG(LogMantle, Display, TEXT("%s"), *Line);
	}
}

FString FMantleCoverage::ToJson() const
{
	using namespace MantleCoverageCpp;
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schemaVersion"), 1);
	Writer->WriteObjectStart(TEXT("totals"));
	for (const EMantleCoverageOutcome Outcome : AllOutcomes)
	{
		Writer->WriteValue(OutcomeKey(Outcome), Total(Outcome));
	}
	Writer->WriteObjectEnd();
	Writer->WriteArrayStart(TEXT("categories"));
	for (const FMantleCoverageCategory& Category : Categories)
	{
		if (Category.IsEmpty())
		{
			continue;
		}
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("category"), Category.Name);
		for (const EMantleCoverageOutcome Outcome : AllOutcomes)
		{
			Writer->WriteObjectStart(OutcomeKey(Outcome));
			WriteBucket(*Writer, Category.Get(Outcome));
			Writer->WriteObjectEnd();
		}
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	return Json;
}

TArray<FString> FMantleCoverage::ToDisplayLines() const
{
	using namespace MantleCoverageCpp;
	TArray<FString> Lines;
	for (const FMantleCoverageCategory& Category : Categories)
	{
		if (Category.IsEmpty())
		{
			continue;
		}
		TArray<FString> Parts;
		for (const EMantleCoverageOutcome Outcome : AllOutcomes)
		{
			if (Category.Count(Outcome) > 0)
			{
				Parts.Add(FString::Printf(TEXT("%lld %s"), Category.Count(Outcome), OutcomeWord(Outcome)));
			}
		}
		Lines.Add(FString::Printf(TEXT("%s: %s"), *Category.Name, *FString::Join(Parts, TEXT(", "))));
		for (const EMantleCoverageOutcome Outcome : AllOutcomes)
		{
			if (Outcome == EMantleCoverageOutcome::Kept)
			{
				continue;
			}
			const FMantleCoverageBucket& Bucket = Category.Get(Outcome);
			int64 Named = 0;
			for (const FMantleCoverageExample& Example : Bucket.Examples)
			{
				Lines.Add(FString::Printf(TEXT("    %s: %s"), OutcomeWord(Outcome), *ExampleText(Example)));
				Named += Example.Count;
			}
			if (Bucket.Count > Named)
			{
				Lines.Add(FString::Printf(TEXT("    %s: and %lld more"), OutcomeWord(Outcome), Bucket.Count - Named));
			}
		}
	}
	return Lines;
}

FString FMantleCoverage::ToSummary() const
{
	TArray<FString> Parts;
	if (Total(EMantleCoverageOutcome::Dropped) > 0)
	{
		Parts.Add(FString::Printf(TEXT("%lld dropped"), Total(EMantleCoverageOutcome::Dropped)));
	}
	if (Total(EMantleCoverageOutcome::Approximated) > 0)
	{
		Parts.Add(FString::Printf(TEXT("%lld approximated"), Total(EMantleCoverageOutcome::Approximated)));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("nothing dropped"));
}
