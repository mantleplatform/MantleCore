// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptCheck.h"
#include "MantleScriptsLog.h"

#include "MantleLuauRuntime.h"
#include "MantleQuickJSRuntime.h"
#include "MantlePocketPySourceCheck.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogMantleScripts);

namespace
{
	/** The bundled TypeScript compiler's source, loaded once. */
	const FString& ScriptCheckTypeScriptCompilerBundle()
	{
		static const FString Bundle = []
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
			if (!Plugin.IsValid())
			{
				return FString();
			}
			FString Source;
			const FString Path = FPaths::Combine(
				Plugin->GetBaseDir(), TEXT("Source"), TEXT("ThirdParty"), TEXT("TypeScriptCompiler"), TEXT("typescript.js"));
			FFileHelper::LoadFileToString(Source, *Path);
			return Source;
		}();
		return Bundle;
	}
}

bool FMantleScriptCheck::IsUnsupported(EMantleScriptEditorLanguage Language)
{
	return Language == EMantleScriptEditorLanguage::Unknown;
}

bool FMantleScriptCheck::Check(EMantleScriptEditorLanguage Language, const FString& Source, FMantleScriptError& OutError)
{
	switch (Language)
	{
	case EMantleScriptEditorLanguage::Luau:
		return FMantleLuauRuntime::ValidateSource(Source, OutError);

	case EMantleScriptEditorLanguage::JavaScript:
		return FMantleQuickJSRuntime::ValidateSource(Source, OutError);

	case EMantleScriptEditorLanguage::TypeScript:
	{
		FString GeneratedJavaScript;
		return FMantleQuickJSRuntime::TranspileTypeScript(
			ScriptCheckTypeScriptCompilerBundle(), Source, GeneratedJavaScript, OutError);
	}

	case EMantleScriptEditorLanguage::Python:
	{
		FString Message;
		const bool bValid = MantlePocketPyCheckSource(TEXT("Check.py"), Source, Message);
		OutError = bValid ? FMantleScriptError{} : FMantleScriptError{EMantleScriptErrorKind::Compile, Message};
		return bValid;
	}

	default:
		OutError = FMantleScriptError{EMantleScriptErrorKind::Compile, TEXT("unknown script language")};
		return false;
	}
}
