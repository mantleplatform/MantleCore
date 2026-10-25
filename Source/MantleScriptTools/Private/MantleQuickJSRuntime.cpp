// Copyright Joshua Eger. All Rights Reserved.
#include "MantleQuickJSRuntime.h"

THIRD_PARTY_INCLUDES_START
extern "C"
{
#include "quickjs.h"
}
THIRD_PARTY_INCLUDES_END

namespace
{
#if WITH_EDITOR
	constexpr uint64 TypeScriptCompilerMemoryBytes = 256ull * 1024ull * 1024ull;
	constexpr uint64 TypeScriptCompilerInterruptChecks = 1000000;

	struct FTypeScriptCompilerBudget
	{
		uint64 Checks = 0;
		bool bExceeded = false;
	};
#endif

	bool ReadString(JSContext* Context, JSValueConst Value, FString& Out)
	{
		size_t Length = 0;
		const char* Utf8 = JS_ToCStringLen(Context, &Length, Value);
		if (!Utf8)
		{
			return false;
		}
		const FUTF8ToTCHAR Converted(Utf8, static_cast<int32>(Length));
		Out = FString(Converted.Length(), Converted.Get());
		JS_FreeCString(Context, Utf8);
		return true;
	}

	bool AddSafeIntrinsics(JSContext* Context)
	{
		// #386: deleting the global names below is not enough on its own -- Function,
		// GeneratorFunction, AsyncFunction and AsyncGeneratorFunction stay reachable through
		// any function literal's .constructor. The vendored engine's per-context switch refuses
		// every string-to-code request made by running JS; this file's own JS_Eval calls
		// (module compile, syntax check) are unaffected. Same call as JS/MantleQuickJS.cpp.
		JS_MantleDenyStringCodeGeneration(Context);

		if (JS_AddIntrinsicBaseObjects(Context) < 0
			|| JS_AddIntrinsicEval(Context) < 0)
		{
			return false;
		}
		JS_AddIntrinsicRegExpCompiler(Context);
		const bool bAdded = JS_AddIntrinsicRegExp(Context) >= 0
			&& JS_AddIntrinsicJSON(Context) >= 0
			&& JS_AddIntrinsicProxy(Context) >= 0
			&& JS_AddIntrinsicMapSet(Context) >= 0
			&& JS_AddIntrinsicTypedArrays(Context) >= 0
			&& JS_AddIntrinsicPromise(Context) >= 0;
		if (!bAdded)
		{
			return false;
		}

		// The C API's parser/evaluator is installed with the eval intrinsic, but creator
		// code needs none of eval, Function or Promise. Delete these global handles
		// after initialization, before any creator module is evaluated.
		JSValue Global = JS_GetGlobalObject(Context);
		for (const char* Name : {"eval", "Function", "Promise"})
		{
			const JSAtom Atom = JS_NewAtom(Context, Name);
			JS_DeleteProperty(Context, Global, Atom, 0);
			JS_FreeAtom(Context, Atom);
		}
		JS_FreeValue(Context, Global);
		return true;
	}

	FString ExceptionMessage(JSContext* Context)
	{
		JSValue Exception = JS_GetException(Context);
		FString Message;
		ReadString(Context, Exception, Message);
		JSValue Stack = JS_GetPropertyStr(Context, Exception, "stack");
		FString StackText;
		if (!JS_IsException(Stack) && !JS_IsUndefined(Stack))
		{
			ReadString(Context, Stack, StackText);
		}
		JS_FreeValue(Context, Stack);
		JS_FreeValue(Context, Exception);
		if (!StackText.IsEmpty() && !StackText.Contains(Message))
		{
			Message += TEXT("\n") + StackText;
		}
		return Message.IsEmpty() ? TEXT("JavaScript exception") : Message;
	}

#if WITH_EDITOR
	int TypeScriptCompilerInterrupt(JSRuntime*, void* Opaque)
	{
		FTypeScriptCompilerBudget& Budget = *static_cast<FTypeScriptCompilerBudget*>(Opaque);
		// #492: TypeScriptCompilerInterruptChecks stays in upstream's 10000-poll unit.
		if (++Budget.Checks <= TypeScriptCompilerInterruptChecks * JS_MANTLE_INTERRUPTS_PER_BUDGET_UNIT)
		{
			return 0;
		}
		Budget.bExceeded = true;
		return 1;
	}

	bool ReadStringProperty(JSContext* Context, JSValueConst Object, const char* Name, FString& Out)
	{
		JSValue Value = JS_GetPropertyStr(Context, Object, Name);
		if (JS_IsException(Value))
		{
			return false;
		}
		const bool bRead = JS_IsString(Value) && ReadString(Context, Value, Out);
		JS_FreeValue(Context, Value);
		return bRead;
	}
#endif
}

bool FMantleQuickJSRuntime::ValidateSource(
	const FString& Source,
	FMantleScriptError& OutError,
	uint64 MaximumSourceBytes)
{
	const FTCHARToUTF8 Utf8(*Source);
	if (static_cast<uint64>(Utf8.Length()) > MaximumSourceBytes)
	{
		OutError = FMantleScriptError{
			EMantleScriptErrorKind::MemoryBudgetExceeded,
			FString::Printf(TEXT("script source is %d bytes; maximum is %llu"), Utf8.Length(), MaximumSourceBytes) };
		return false;
	}

	JSRuntime* Runtime = JS_NewRuntime();
	JSContext* Context = Runtime ? JS_NewContextRaw(Runtime) : nullptr;
	if (!Context || !AddSafeIntrinsics(Context))
	{
		if (Context) JS_FreeContext(Context);
		if (Runtime) JS_FreeRuntime(Runtime);
		OutError = FMantleScriptError{EMantleScriptErrorKind::MemoryBudgetExceeded, TEXT("could not create compile-check VM")};
		return false;
	}

    // QuickJS resolves static imports even with COMPILE_ONLY, before module linking or
    // evaluation. This single-file syntax check has no place catalogue. Satisfy only
    // that resolution step with inert modules; never read files or fetch dependencies.
    // Real import existence, exact case, side and exports remain the runtime loader's
    // responsibility. This loader belongs only to this disposable validation VM.
    JS_SetModuleLoaderFunc(Runtime, nullptr,
        [](JSContext* Ctx, const char* Name, void*) -> JSModuleDef*
        {
            return JS_NewCModule(Ctx, Name, [](JSContext* InitCtx, JSModuleDef*) -> int
            {
                // A syntax-only module must never be evaluated, even accidentally.
                JS_ThrowInternalError(InitCtx, "syntax-check dependency cannot execute");
                return -1;
            });
        }, nullptr);

	JSValue Compiled = JS_Eval(
		Context,
		Utf8.Get(),
		static_cast<size_t>(Utf8.Length()),
		"mantle-script.js",
		JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_BACKTRACE_BARRIER);
	const bool bSuccess = !JS_IsException(Compiled);
	if (!bSuccess)
	{
		OutError = FMantleScriptError{EMantleScriptErrorKind::Compile, ExceptionMessage(Context)};
	}
	JS_FreeValue(Context, Compiled);
	JS_FreeContext(Context);
	JS_FreeRuntime(Runtime);
	return bSuccess;
}

#if WITH_EDITOR
bool FMantleQuickJSRuntime::TranspileTypeScript(
	const FString& CompilerBundle,
	const FString& Source,
	FString& OutJavaScript,
	FMantleScriptError& OutError,
	uint64 MaximumSourceBytes)
{
	OutJavaScript.Reset();
	const FTCHARToUTF8 SourceUtf8(*Source);
	if (static_cast<uint64>(SourceUtf8.Length()) > MaximumSourceBytes)
	{
		OutError = FMantleScriptError{
			EMantleScriptErrorKind::MemoryBudgetExceeded,
			FString::Printf(
				TEXT("script source is %d bytes; maximum is %llu"),
				SourceUtf8.Length(),
				MaximumSourceBytes) };
		return false;
	}
	if (CompilerBundle.IsEmpty())
	{
		OutError = FMantleScriptError{
			EMantleScriptErrorKind::Compile,
			TEXT("the bundled TypeScript compiler resource is missing") };
		return false;
	}

	FTypeScriptCompilerBudget Budget;
	JSRuntime* Runtime = JS_NewRuntime();
	if (!Runtime)
	{
		OutError = FMantleScriptError{
			EMantleScriptErrorKind::MemoryBudgetExceeded,
			TEXT("could not create the TypeScript compiler VM") };
		return false;
	}
	JS_SetMemoryLimit(Runtime, static_cast<size_t>(TypeScriptCompilerMemoryBytes));
	JS_SetMaxStackSize(Runtime, 1024 * 1024);
	JS_SetInterruptHandler(Runtime, &TypeScriptCompilerInterrupt, &Budget);
	JSContext* Context = JS_NewContext(Runtime);
	if (!Context)
	{
		JS_FreeRuntime(Runtime);
		OutError = FMantleScriptError{
			EMantleScriptErrorKind::MemoryBudgetExceeded,
			TEXT("could not create the TypeScript compiler context") };
		return false;
	}

	auto FailFromException = [&]()
	{
		OutError = FMantleScriptError{
			Budget.bExceeded
				? EMantleScriptErrorKind::InstructionBudgetExceeded
				: EMantleScriptErrorKind::Compile,
			Budget.bExceeded
				? TEXT("TypeScript compilation exceeded its execution budget")
				: ExceptionMessage(Context) };
	};
	auto Close = [&]()
	{
		JS_FreeContext(Context);
		JS_FreeRuntime(Runtime);
	};

	const FTCHARToUTF8 CompilerUtf8(*CompilerBundle);
	JSValue CompilerResult = JS_Eval(
		Context,
		CompilerUtf8.Get(),
		static_cast<size_t>(CompilerUtf8.Length()),
		"typescript.js",
		JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_BACKTRACE_BARRIER);
	if (JS_IsException(CompilerResult))
	{
		FailFromException();
		JS_FreeValue(Context, CompilerResult);
		Close();
		return false;
	}
	JS_FreeValue(Context, CompilerResult);

	static const char WrapperSource[] = R"JS(
(function(source) {
    const result = ts.transpileModule(source, {
        compilerOptions: {
            target: ts.ScriptTarget.ES2020,
            module: ts.ModuleKind.ES2020,
            strict: true,
            isolatedModules: true,
            newLine: ts.NewLineKind.LineFeed
        },
        fileName: "mantle-script.ts",
        reportDiagnostics: true
    });
    const errors = (result.diagnostics || []).filter(
        diagnostic => diagnostic.category === ts.DiagnosticCategory.Error);
    const diagnostics = errors.map(diagnostic => {
        let location = "";
        if (diagnostic.file && typeof diagnostic.start === "number") {
            const point = diagnostic.file.getLineAndCharacterOfPosition(diagnostic.start);
            location = `${point.line + 1}:${point.character + 1}: `;
        }
        return location + `TS${diagnostic.code}: `
            + ts.flattenDiagnosticMessageText(diagnostic.messageText, "\n");
    }).join("\n");
    return { outputText: result.outputText || "", diagnostics };
})
)JS";
	JSValue TranspileFunction = JS_Eval(
		Context,
		WrapperSource,
		sizeof(WrapperSource) - 1,
		"mantle-typescript-wrapper.js",
		JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_BACKTRACE_BARRIER);
	if (JS_IsException(TranspileFunction))
	{
		FailFromException();
		JS_FreeValue(Context, TranspileFunction);
		Close();
		return false;
	}

	JSValue Argument = JS_NewStringLen(
		Context,
		SourceUtf8.Get(),
		static_cast<size_t>(SourceUtf8.Length()));
	JSValue Result = JS_Call(Context, TranspileFunction, JS_UNDEFINED, 1, &Argument);
	JS_FreeValue(Context, Argument);
	JS_FreeValue(Context, TranspileFunction);
	if (JS_IsException(Result))
	{
		FailFromException();
		JS_FreeValue(Context, Result);
		Close();
		return false;
	}

	FString Diagnostics;
	const bool bReadResult = JS_IsObject(Result)
		&& ReadStringProperty(Context, Result, "outputText", OutJavaScript)
		&& ReadStringProperty(Context, Result, "diagnostics", Diagnostics);
	JS_FreeValue(Context, Result);
	Close();
	if (!bReadResult)
	{
		OutJavaScript.Reset();
		OutError = FMantleScriptError{
			EMantleScriptErrorKind::Compile,
			TEXT("the TypeScript compiler returned an invalid result") };
		return false;
	}
	if (!Diagnostics.IsEmpty())
	{
		OutJavaScript.Reset();
		OutError = FMantleScriptError{ EMantleScriptErrorKind::Compile, Diagnostics };
		return false;
	}

	// TypeScript output still crosses the same compile-only gate as hand-authored
	// JavaScript. This catches any compiler/configuration regression before publication.
	return ValidateSource(OutJavaScript, OutError, MaximumSourceBytes);
}
#endif
