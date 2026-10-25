// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The fail-closed checks a script-opened document passes before RmlUi sees it, and the
 * text rules around it, as pure functions over the place's UI file set, testable without RmlUi.
 * In MantleScriptTools so the editor's Check and Publish run the same scans without MantleRml.
 */
namespace MantleRmlDocumentRules
{
	/** The dynamic data model every script-opened document binds to: `<body data-model="data">`. */
	inline constexpr TCHAR DataModelName[] = TEXT("data");

	/** Per UI file, as the backend caps them. */
	constexpr int32 MaxFileBytes = 256 * 1024;

	/**
	 * Element nesting, counted from <rml>. RmlUi's layout time doubles with each nesting level of
	 * flex, inline-block and table content (#549 fuzz finding, release build: depth 12 about 10 ms,
	 * depth 15 about 80 ms, depth 17 about 300 ms), so the budget stays below that cliff. Applied to
	 * each file's text (CheckFileSet) and, #571, to the loaded document (CheckLoadedStructure).
	 */
	constexpr int32 MaxNestingDepth = 16;

	/** Elements in an open document, after it loads and after each SetData. */
	constexpr int32 MaxElements = 4096;

	/** Top-level variables one document may hold. */
	constexpr int32 MaxVariables = 256;

	/** A load, or the relayout after a SetData, slower than this logs a warning ... */
	constexpr double LayoutWarnMs = 50.0;
	/** ... and slower than this closes the document ("layout time"). */
	constexpr double LayoutFailMs = 500.0;

	/**
	 * An open document whose average frame cost (update plus render, over its surface's frame
	 * stats window) stays above this logs one warning that it is costing frame rate. Warning only:
	 * the document is never closed for it.
	 */
	constexpr double FrameCostWarnMs = 4.0;
	/** How long the average must stay above FrameCostWarnMs before the warning: never one spike. */
	constexpr double FrameCostWarnSeconds = 1.0;
	/** Frames the average must span before it counts at all. */
	constexpr int32 FrameCostMinFrames = 30;
	/** Once warned, the warning re-arms only after the average drops below this share of FrameCostWarnMs. */
	constexpr double FrameCostRearmFraction = 0.5;
	/** One frame adds at most this to the time above the threshold, so a single hitch never counts as a second. */
	constexpr double FrameCostMaxStepSeconds = 0.25;

	/** Per-document state of the frame-cost warning. */
	struct FFrameCostWatch
	{
		/** How long the average has stayed above FrameCostWarnMs without a break. */
		double SecondsAbove = 0.0;
		/** Warned, and not yet re-armed. */
		bool bWarned = false;
	};

	/**
	 * Advances Watch by one frame of DeltaSeconds, given the average frame cost AverageMs over the
	 * last AverageFrames frames. True exactly when the warning is due now: the average has spanned at
	 * least FrameCostMinFrames and stayed above FrameCostWarnMs for FrameCostWarnSeconds (each frame
	 * counting at most FrameCostMaxStepSeconds), and no warning is outstanding. After a warning
	 * nothing fires again until the average drops below FrameCostWarnMs * FrameCostRearmFraction.
	 */
	MANTLESCRIPTTOOLS_API bool UpdateFrameCostWatch(FFrameCostWatch& Watch, double AverageMs, int32 AverageFrames, double DeltaSeconds);

	/** UTF-8 size of a file as RmlUi would read it. */
	MANTLESCRIPTTOOLS_API int32 Utf8Bytes(const FString& Text);

	/**
	 * Every file of the set: at most MaxFileBytes, and none of the refused constructs (matched
	 * case-insensitively): a `data-rml` attribute (raw markup injection), `<script`, `@font-face`
	 * (fonts are Mantle's). Every `.rml` file also stays within MaxNestingDepth. False with the
	 * first failure, naming the file, in OutError.
	 */
	MANTLESCRIPTTOOLS_API bool CheckFileSet(const TMap<FString, FString>& Files, FString& OutError);

	/**
	 * #571: the budgets on a loaded document's structure, which the script document host applies
	 * after the load and after each SetData, and the editor's check applies to its offscreen load:
	 * Elements (MantleRml::CountDescendants) within MaxElements, and Depth
	 * (MantleRml::LoadedNestingDepth: templates expanded, data-for views counted) within
	 * MaxNestingDepth. Empty when both hold; otherwise the detail (which names the count or the
	 * depth), with OutReason the reason a closed document reports: "element budget" for both.
	 */
	MANTLESCRIPTTOOLS_API FString CheckLoadedStructure(int32 Elements, int32 Depth, FString& OutReason);

	/** The first refused construct in Text, or null. */
	MANTLESCRIPTTOOLS_API const TCHAR* FindRefusedConstruct(const FString& Text);

	/**
	 * Adds to InOut, once each, every property RmlUi parses but Slate does not draw (filter, backdrop-filter, mask-image) that Text declares: as a declaration (the
	 * name, then ':', after the start of the text, '{', ';' or a quote, whitespace aside, so a
	 * selector like `.filter:hover` or a value like `transition: filter 1s` does not count) or as a
	 * `data-style-<name>` attribute. CSS and HTML comments are skipped; names match ignoring case.
	 * A text scan rather than RmlUi's parsed styles, so rules that match nothing yet (a :hover, an
	 * empty data-for) count too, and it runs where RmlUi does not.
	 */
	MANTLESCRIPTTOOLS_API void FindUndrawnProperties(const FString& Text, TArray<FString>& InOut);

	/** The href of every <link> tag in Rml, in order (comments skipped): the sheets a document pulls in. */
	MANTLESCRIPTTOOLS_API TArray<FString> LinkHrefs(const FString& Rml);

	/**
	 * The deepest element nesting in RML markup, by a cheap tag scan: comments, `<!...>`, `<?...?>`
	 * and CDATA are skipped, quoted attribute values may hold '>', self-closed tags and HTML void
	 * elements (br, img, input, hr, link, meta, col, source) do not nest, and a '<' not followed by
	 * a tag name is text (a data expression like `{{ a < b }}`).
	 */
	MANTLESCRIPTTOOLS_API int32 MaxTagDepth(const FString& Rml);

	/**
	 * The document text with ` data-model="data"` added to its <body> tag when the tag has no
	 * data-model attribute. A body that names another model is left as written and OutOtherModel
	 * gets its name (script data will not bind); no <body> tag leaves the text as it is.
	 */
	MANTLESCRIPTTOOLS_API FString ApplyDefaultDataModel(const FString& Rml, FString& OutOtherModel);

	/**
	 * The document text with `<style>` StyleSheet `</style>` as the first child of its <head>.
	 * RmlUi 6.3 keeps a document's <link> and <style> sheets in one list in document order
	 * (XMLNodeHandlerHead) and merges it, later sheets winning at equal specificity, when </head>
	 * closes (ElementDocument::ProcessHeader), before any body element exists. So the base is in
	 * place for the first layout and the document's own sheets override it. A document without a
	 * <head> gets one right after its <rml> tag, or before its <body>; with neither, the text is
	 * returned unchanged.
	 */
	MANTLESCRIPTTOOLS_API FString InjectStyleSheet(const FString& Rml, const FString& StyleSheet);

	/**
	 * A document's source as RmlUi is handed it, by the script document host and the editor's check
	 * (#549 P6a) alike: ApplyDefaultDataModel, then BaseStyleSheet injected (InjectStyleSheet).
	 * OutOtherModel as ApplyDefaultDataModel sets it.
	 */
	MANTLESCRIPTTOOLS_API FString PrepareMarkup(const FString& Source, FString& OutOtherModel);

	/**
	 * A name RmlUi accepts for a top-level data variable (its LegalVariableName): a letter, then
	 * letters, digits and '_', and not one of it, it_index, ev, true, false, size or literal
	 * (ignoring case). False with the reason in OutError.
	 */
	MANTLESCRIPTTOOLS_API bool IsLegalVariableName(const FString& Name, FString& OutError);

	/**
	 * Mantle's base stylesheet (RCSS), put under every script-opened document's own sheets
	 * (InjectStyleSheet) so plain markup behaves like a web page and the document's rules override
	 * it. ASCII only, and no '<' or '&', so it is safe inside a <style> element.
	 */
	MANTLESCRIPTTOOLS_API const char* BaseStyleSheet();
}
