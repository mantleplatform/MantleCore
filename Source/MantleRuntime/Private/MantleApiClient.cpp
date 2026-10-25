// Copyright Joshua Eger. All Rights Reserved.
#include "MantleApiClient.h"
#include "MantleConnection.h"
#include "MantleCosmeticItem.h"
#include "MantlePreviewLaunch.h"
#include "MantleRuntime.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/ConfigCacheIni.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

float FMantleApiClient::TimeoutSeconds = 30.0f;

#if WITH_DEV_AUTOMATION_TESTS
TFunction<void(const FString&, const FString&, FMantleApiCallback)> FMantleApiClient::ServerTokenRequestForTesting;
#endif

TFunction<FString()> FMantleApiClient::BaseUrlResolver =
	[] { return FMantleConnection::ResolveApiBaseUrl(); };

bool FMantleApiClient::IsConfigured(FString& OutWhyNot)
{
	// No check for a missing address: every resolver falls back to a default one
	// (FMantleConnection::DefaultBaseUrl), so an empty address cannot happen and a message
	// telling the creator to set one could never be shown (#265).
	if (FMantleConnection::ResolveAuthToken().IsEmpty())
	{
#if WITH_EDITOR
		OutWhyNot = TEXT("You are not signed in to Mantle. Choose Tools > Mantle > Sign in with creator token and paste a token from Mantle's Settings > Account, or set MANTLE_AUTH_TOKEN to one (CI).");
#else
		OutWhyNot = TEXT("You are not signed in to Mantle. Sign in and try again.");
#endif
		return false;
	}

	return true;
}

namespace
{
	/**
	 * Offline preview (#563): a preview process makes no production requests. The attempt is counted
	 * in the status file (path only, never a query string or token) and answers preview_offline.
	 */
	FMantleApiResult RefusePreviewRequest(const FString& Verb, const FString& Path)
	{
		int32 QueryStart = INDEX_NONE;
		const FString Target = Path.FindChar(TEXT('?'), QueryStart) ? Path.Left(QueryStart) : Path;
		FMantlePreviewStatus::Get().NoteBlockedRequest(Verb + TEXT(" ") + Target);
		FMantleApiResult Result;
		Result.Code = TEXT("preview_offline");
		Result.Message = TEXT("Online services are unavailable in creator preview.");
		return Result;
	}

	/**
	 * A local-draft avatar id (an unpublished avatar under test in the offline preview) is never a catalog id, so no
	 * request may carry one in its path or body. Logged, and answered local_draft_not_sent.
	 */
	bool RefuseLocalDraft(const FString& Verb, const FString& Path, const FString& Body, FMantleApiResult& OutResult)
	{
		if (!MantleCosmetic::ContainsLocalDraftId(Path) && !MantleCosmetic::ContainsLocalDraftId(Body))
		{
			return false;
		}
		int32 QueryStart = INDEX_NONE;
		const FString Target = Path.FindChar(TEXT('?'), QueryStart) ? Path.Left(QueryStart) : Path;
		UE_LOG(LogMantle, Error, TEXT("MANTLE_COSMETIC refused %s %s: it names a local-draft avatar item, which only exists in this offline preview and is never sent to the backend"),
			*Verb, *Target);
		OutResult = FMantleApiResult();
		OutResult.Code = TEXT("local_draft_not_sent");
		OutResult.Message = TEXT("A draft avatar is only for testing locally. It cannot be saved or sent online.");
		return true;
	}

	/** The async form: the callback runs on the game thread, after the caller's call returns, as a failed request's would. */
	void RefusePreviewRequestAsync(const FString& Verb, const FString& Path, FMantleApiCallback OnDone)
	{
		const FMantleApiResult Result = RefusePreviewRequest(Verb, Path);
		AsyncTask(ENamedThreads::GameThread, [OnDone, Result]() { OnDone.ExecuteIfBound(Result); });
	}

	/** Pulls the API's error code and message out of a failure body, when it sent one. */
	void ReadApiError(const FString& Body, FString& OutCode, FString& OutMessage)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return;
		}

		Object->TryGetStringField(TEXT("code"), OutCode);
		Object->TryGetStringField(TEXT("message"), OutMessage);
	}

	FString FriendlyFailure(int32 StatusCode, const FString& ApiMessage)
	{
		if (!ApiMessage.IsEmpty())
		{
			return ApiMessage;
		}

		switch (StatusCode)
		{
		case 401:
			return TEXT("Your Mantle sign-in expired or was not accepted. Sign in again.");
		case 404:
			return TEXT("Mantle could not find that place.");
		case 409:
			return TEXT("Save your changes before publishing.");
		case 413:
			return TEXT("That place is too large to publish.");
		case 0:
			return TEXT("Could not reach Mantle. Check your internet connection and try again.");
		default:
			return FString::Printf(TEXT("Mantle returned an unexpected error (%d)."), StatusCode);
		}
	}

	/** Builds a request with the address, token and headers every call shares. */
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> BuildRequest(
		const FString& Verb, const FString& Path, const FString& Body,
		const FString& Token = FMantleConnection::ResolveAuthToken(),
		const FString& ContentType = TEXT("application/json"))
	{
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(FMantleApiClient::BaseUrlResolver() + Path);
		Request->SetVerb(Verb);
		Request->SetHeader(TEXT("Authorization"),
			TEXT("Bearer ") + Token);
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));

		// The launch store's display name (Steam persona, Epic display name), for a sign-in
		// token that carries none. Only the signed-in client ever sets one; the editor, a
		// developer-auth run and a dedicated server never do, so they never send it.
		if (const FString DisplayName = FMantleConnection::ResolveDisplayNameHeader(); !DisplayName.IsEmpty())
		{
			Request->SetHeader(FMantleConnection::DisplayNameHeader, DisplayName);
		}

		if (!Body.IsEmpty())
		{
			Request->SetHeader(TEXT("Content-Type"), ContentType);
			Request->SetContentAsString(Body);
		}

		return Request;
	}

	/** Defined below; declared here because the server-token helper calls it first. */
	FMantleApiResult ReadResponse(
		const FString& Verb, const FString& Path, const FHttpRequestPtr& Request,
		const FHttpResponsePtr& Response);

	/** Sends a POST under /server/ authenticated with the dedicated-server bearer token instead of the creator/player one BuildRequest defaults to. */
	void SendServerTokenRequest(const FString& Path, const FString& Body, FMantleApiCallback OnDone)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (FMantleApiClient::ServerTokenRequestForTesting)
		{
			FMantleApiClient::ServerTokenRequestForTesting(Path, Body, OnDone);
			return;
		}
#endif
		if (MantlePreviewLaunch::IsRequested())
		{
			RefusePreviewRequestAsync(TEXT("POST"), Path, OnDone);
			return;
		}
		FMantleApiResult Refused;
		if (RefuseLocalDraft(TEXT("POST"), Path, Body, Refused))
		{
			AsyncTask(ENamedThreads::GameThread, [OnDone, Refused]() { OnDone.ExecuteIfBound(Refused); });
			return;
		}

		const FString Token = FMantleConnection::ResolveServerToken();
		if (Token.IsEmpty())
		{
			FMantleApiResult Result;
			Result.Message = TEXT("This dedicated server has no MANTLE_SERVER_TOKEN.");
			OnDone.ExecuteIfBound(Result);
			return;
		}

		const FString Verb = TEXT("POST");
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = BuildRequest(Verb, Path, Body, Token);
		Request->SetTimeout(FMantleApiClient::TimeoutSeconds);
		Request->OnProcessRequestComplete().BindLambda(
			[OnDone, Verb, Path](FHttpRequestPtr Finished, FHttpResponsePtr Response, bool bConnected)
			{
				FMantleApiResult Result;
				if (bConnected)
				{
					Result = ReadResponse(Verb, Path, Finished, Response);
				}
				else
				{
					Result.Message = FString::Printf(TEXT("Could not reach the Mantle %s endpoint."), *Path);
				}
				OnDone.ExecuteIfBound(Result);
			});
		if (!Request->ProcessRequest())
		{
			FMantleApiResult Result;
			Result.Message = FString::Printf(TEXT("Could not start the request to %s."), *Path);
			OnDone.ExecuteIfBound(Result);
		}
	}

	/** The bearer token a request was sent with, read back off the request itself. */
	FString SentToken(const FHttpRequestPtr& Request)
	{
		if (!Request.IsValid())
		{
			return FString();
		}
		FString Header = Request->GetHeader(TEXT("Authorization"));
		Header.RemoveFromStart(TEXT("Bearer "));
		return Header;
	}

	/** Turns a finished request into a result. One place, so both call paths report alike. */
	FMantleApiResult ReadResponse(
		const FString& Verb, const FString& Path, const FHttpRequestPtr& Request,
		const FHttpResponsePtr& Response)
	{
		FMantleApiResult Result;

		if (!Response.IsValid())
		{
			Result.Message = TEXT("Could not reach Mantle. Check your internet connection and try again.");
			return Result;
		}

		Result.StatusCode = Response->GetResponseCode();
		Result.Body = Response->GetContentAsString();
		Result.bSuccess = Result.StatusCode >= 200 && Result.StatusCode < 300;

		if (!Result.bSuccess)
		{
			ReadApiError(Result.Body, Result.Code, Result.Message);
			Result.Message = FriendlyFailure(Result.StatusCode, Result.Message);
			Result.RetryAfterSeconds = FMantleApiClient::ParseRetryAfter(Response->GetHeader(TEXT("Retry-After")), FDateTime::UtcNow());

			if (Result.Code.Equals(TEXT("compatibility_failed"), ESearchCase::CaseSensitive))
			{
				// The report lists every check, passed ones first, so the first 400 characters of
				// the body rarely reach the one that failed (#572): each failed check gets a line.
				UE_LOG(LogMantle, Warning, TEXT("%s %s -> %d %s"), *Verb, *Path, Result.StatusCode, *Result.Message);
				for (const FMantleCompatibilityFailure& Failure : FMantleApiClient::ParseCompatibilityFailures(Result.Body))
				{
					UE_LOG(LogMantle, Warning, TEXT("  Failed check %s: %s"), *Failure.Name, *Failure.ToLine());
				}
			}
			else
			{
				UE_LOG(LogMantle, Warning, TEXT("%s %s -> %d %s"),
					*Verb, *Path, Result.StatusCode, *Result.Body.Left(400));
			}

			if (Result.StatusCode == 401)
			{
				// The token itself was refused, not this particular call. Whoever owns
				// sign-in decides what to do about that; every caller reporting "sign in
				// again" in its own words would leave the player with no way to actually do so.
				FMantleConnection::OnSessionRejected().Broadcast(Result.Message, SentToken(Request));
			}
		}

		return Result;
	}
}

FMantleApiResult FMantleApiClient::Send(
	const FString& Verb,
	const FString& Path,
	const FString& Body,
	float TimeoutOverrideSeconds,
	const FString& ContentType)
{
	if (MantlePreviewLaunch::IsRequested())
	{
		return RefusePreviewRequest(Verb, Path);
	}

	FMantleApiResult Result;
	if (RefuseLocalDraft(Verb, Path, Body, Result))
	{
		return Result;
	}

	const float Budget = TimeoutOverrideSeconds > 0.0f ? TimeoutOverrideSeconds : TimeoutSeconds;

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = BuildRequest(
		Verb, Path, Body, FMantleConnection::ResolveAuthToken(), ContentType);

	bool bFinished = false;
	Request->OnProcessRequestComplete().BindLambda(
		[&bFinished](FHttpRequestPtr, FHttpResponsePtr, bool) { bFinished = true; });

	if (!Request->ProcessRequest())
	{
		Result.Message = TEXT("Could not start the request to Mantle.");
		return Result;
	}

	// The editor is not ticking while this runs, so the HTTP manager is driven directly.
	const double Deadline = FPlatformTime::Seconds() + Budget;
	while (!bFinished && FPlatformTime::Seconds() < Deadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.016f);
		FPlatformProcess::Sleep(0.005f);
	}

	if (!bFinished)
	{
		Request->CancelRequest();
		Result.Message = FString::Printf(
			TEXT("Mantle did not respond within %.0f seconds."), Budget);
		return Result;
	}

	return ReadResponse(Verb, Path, Request, Request->GetResponse());
}

namespace
{
void DispatchMantleAsyncRequest(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request,
	const FString& Verb, const FString& Path, FMantleApiCallback OnDone)
{
	if (MantlePreviewLaunch::IsRequested())
	{
		RefusePreviewRequestAsync(Verb, Path, OnDone);
		return;
	}
	FMantleApiResult Refused;
	const TArray<uint8>& Content = Request->GetContent();
	const FUTF8ToTCHAR ContentText(reinterpret_cast<const ANSICHAR*>(Content.GetData()), Content.Num());
	if (RefuseLocalDraft(Verb, Path, FString(ContentText.Length(), ContentText.Get()), Refused))
	{
		AsyncTask(ENamedThreads::GameThread, [OnDone, Refused]() { OnDone.ExecuteIfBound(Refused); });
		return;
	}
	// Verb and Path are copied into the lambda because the caller's strings may be gone by
	// the time this runs — the whole point of not blocking is that the caller moves on.
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone, Verb, Path](FHttpRequestPtr Finished, FHttpResponsePtr Response, bool bConnected)
		{
			FMantleApiResult Result;
			if (bConnected)
			{
				Result = ReadResponse(Verb, Path, Finished, Response);
			}
			else
			{
				Result.Message = TEXT("Could not reach Mantle. Check your internet connection and try again.");
				UE_LOG(LogMantle, Warning, TEXT("%s %s -> no response"), *Verb, *Path);
			}

			OnDone.ExecuteIfBound(Result);
		});

	if (!Request->ProcessRequest())
	{
		FMantleApiResult Result;
		Result.Message = TEXT("Could not start the request to Mantle.");
		OnDone.ExecuteIfBound(Result);
	}
}
}

void FMantleApiClient::SendAsync(
	const FString& Verb, const FString& Path, const FString& Body, FMantleApiCallback OnDone,
	float TimeoutOverrideSeconds)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = BuildRequest(Verb, Path, Body);
	Request->SetTimeout(TimeoutOverrideSeconds > 0.0f ? TimeoutOverrideSeconds : TimeoutSeconds);
	DispatchMantleAsyncRequest(Request, Verb, Path, OnDone);
}

void FMantleApiClient::SendWithHeadersAsync(
	const FString& Verb, const FString& Path, const FString& Body, const TMap<FString, FString>& ExtraHeaders,
	FMantleApiCallback OnDone, float TimeoutOverrideSeconds)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = BuildRequest(Verb, Path, Body);
	for (const TPair<FString, FString>& Header : ExtraHeaders)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}
	Request->SetTimeout(TimeoutOverrideSeconds > 0.0f ? TimeoutOverrideSeconds : TimeoutSeconds);
	DispatchMantleAsyncRequest(Request, Verb, Path, OnDone);
}

void FMantleApiClient::SendBytesAsync(
	const FString& Verb, const FString& Path, const TArray<uint8>& Bytes,
	const FString& ContentType, FMantleApiCallback OnDone, float TimeoutOverrideSeconds)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = BuildRequest(Verb, Path, FString());
	Request->SetHeader(TEXT("Content-Type"), ContentType);
	Request->SetContent(Bytes);
	Request->SetTimeout(TimeoutOverrideSeconds > 0.0f ? TimeoutOverrideSeconds : TimeoutSeconds);
	DispatchMantleAsyncRequest(Request, Verb, Path, OnDone);
}

FMantleApiResult FMantleApiClient::SaveDraft(const FString& PlaceId, const FString& PlaceJson)
{
	return Send(TEXT("PUT"), FString::Printf(TEXT("/places/%s/draft"), *PlaceId), PlaceJson);
}

FMantleApiResult FMantleApiClient::Publish(const FString& PlaceId, const FString& Notes)
{
	FString Escaped = Notes;
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));

	return Send(TEXT("POST"), FString::Printf(TEXT("/places/%s/publish"), *PlaceId),
		FString::Printf(TEXT("{\"notes\":\"%s\"}"), *Escaped));
}

FMantleApiResult FMantleApiClient::UploadScript(const FString& Source, const FString& Language)
{
	return Send(
		TEXT("POST"),
		FString::Printf(TEXT("/scripts?language=%s"), *Language),
		Source,
		0.0f,
		TEXT("text/plain; charset=utf-8"));
}

FMantleApiResult FMantleApiClient::GetScriptSource(const FString& SourceId)
{
	return Send(TEXT("GET"), FString::Printf(TEXT("/scripts/%s/source"), *SourceId), FString());
}

namespace
{
void FetchMantleScriptSource(const FString& SourceId, bool bServer, FMantleApiCallback OnDone)
{
	if (MantlePreviewLaunch::IsRequested())
	{
		// A preview sends nothing out.
		RefusePreviewRequestAsync(TEXT("GET"), FString::Printf(TEXT("%s/scripts/source"), bServer ? TEXT("/server") : TEXT("")), OnDone);
		return;
	}
	const FString Token = bServer ? FMantleConnection::ResolveServerToken() : FMantleConnection::ResolveAuthToken();
	if (bServer && Token.IsEmpty())
	{
		FMantleApiResult Result;
		Result.Code = TEXT("invalid_server_token");
		Result.Message = TEXT("This dedicated server has no MANTLE_SERVER_TOKEN for script content.");
		OnDone.ExecuteIfBound(Result);
		return; // Never fall back to an available player credential.
	}
	const FString Path = FString::Printf(TEXT("%s/scripts/%s/source"), bServer ? TEXT("/server") : TEXT(""), *SourceId);
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = BuildRequest(TEXT("GET"), Path, FString(), Token);
	Request->SetTimeout(FMantleApiClient::TimeoutSeconds);
	DispatchMantleAsyncRequest(Request, TEXT("GET"), Path, OnDone);
}
}

void FMantleApiClient::GetScriptSourceAsync(const FString& SourceId, FMantleApiCallback OnDone)
{
	FetchMantleScriptSource(SourceId, false, OnDone);
}

void FMantleApiClient::GetServerScriptSourceAsync(const FString& SourceId, FMantleApiCallback OnDone)
{
	FetchMantleScriptSource(SourceId, true, OnDone);
}

void FMantleApiClient::GetLiveAsync(const FString& PlaceId, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("GET"), FString::Printf(TEXT("/places/%s/live"), *PlaceId), FString(), OnDone);
}

void FMantleApiClient::GetRevisionAsync(
	const FString& PlaceId, int32 RevisionNumber, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("GET"),
		FString::Printf(TEXT("/places/%s/revisions/%d"), *PlaceId, RevisionNumber), FString(), OnDone);
}

FMantleApiResult FMantleApiClient::ListMine()
{
	return Send(TEXT("GET"), TEXT("/places/mine"), FString());
}

FString FMantleApiClient::MakeAssetUploadPath(const FMantleAssetUploadOptions& Options)
{
	FString Path = TEXT("/assets?kind=") + FGenericPlatformHttp::UrlEncode(Options.Kind);
	const TCHAR* const QueryNames[] = { TEXT("part"), TEXT("role"), TEXT("source") };
	const FString* const QueryValues[] = { &Options.Part, &Options.Role, &Options.Source };
	for (int32 Query = 0; Query < 3; ++Query)
	{
		if (!QueryValues[Query]->IsEmpty())
		{
			Path += FString::Printf(TEXT("&%s=%s"), QueryNames[Query], *FGenericPlatformHttp::UrlEncode(*QueryValues[Query]));
		}
	}
	return Path;
}

FString FMantleApiClient::MakeDiffAssetsBody(TConstArrayView<FString> Hashes)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteArrayStart(TEXT("hashes"));
	for (const FString& Hash : Hashes)
	{
		Writer->WriteValue(Hash);
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

namespace MantleApiClientUploadPrivate
{
	bool ReadAssetObject(const TSharedPtr<FJsonObject>& Object, FMantleUploadedAsset& OutAsset)
	{
		OutAsset = FMantleUploadedAsset();
		if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), OutAsset.Id) || OutAsset.Id.IsEmpty())
		{
			return false;
		}
		Object->TryGetStringField(TEXT("hash"), OutAsset.Hash);
		Object->TryGetStringField(TEXT("kind"), OutAsset.Kind);
		Object->TryGetStringField(TEXT("status"), OutAsset.Status);
		Object->TryGetStringField(TEXT("cookState"), OutAsset.CookState);
		Object->TryGetStringField(TEXT("moderationState"), OutAsset.ModerationState);
		Object->TryGetBoolField(TEXT("created"), OutAsset.bCreated);
		const TSharedPtr<FJsonObject>* Options = nullptr;
		if (Object->TryGetObjectField(TEXT("cookOptions"), Options) && Options && Options->IsValid())
		{
			(*Options)->TryGetStringField(TEXT("part"), OutAsset.CookOptions.Part);
			(*Options)->TryGetStringField(TEXT("role"), OutAsset.CookOptions.Role);
			(*Options)->TryGetStringField(TEXT("source"), OutAsset.CookOptions.Source);
		}
		Object->TryGetStringField(TEXT("url"), OutAsset.Url);
		Object->TryGetStringField(TEXT("cookedHash"), OutAsset.CookedHash);
		Object->TryGetStringField(TEXT("cookError"), OutAsset.CookError);
		double CookedBytes = 0.0;
		if (Object->TryGetNumberField(TEXT("cookedBytes"), CookedBytes) && CookedBytes >= 0.0 && CookedBytes < 9.0e15)
		{
			OutAsset.CookedBytes = static_cast<int64>(CookedBytes);
		}
		const TSharedPtr<FJsonObject>* Info = nullptr;
		const TSharedPtr<FJsonObject>* Textures = nullptr;
		if (Object->TryGetObjectField(TEXT("cookInfo"), Info) && Info && Info->IsValid()
			&& (*Info)->TryGetObjectField(TEXT("textures"), Textures) && Textures && Textures->IsValid())
		{
			for (const auto& Pair : (*Textures)->Values)
			{
				FString TextureId;
				if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String && Pair.Value->TryGetString(TextureId))
				{
					OutAsset.MaterialTextures.Add(FString(Pair.Key.ToView()), TextureId);
				}
			}
		}
		return true;
	}
}

bool FMantleApiClient::ParseUploadedAsset(const FString& Json, FMantleUploadedAsset& OutAsset)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object))
	{
		OutAsset = FMantleUploadedAsset();
		return false;
	}
	return MantleApiClientUploadPrivate::ReadAssetObject(Object, OutAsset);
}

bool FMantleApiClient::ParseDiffAssets(const FString& Json, TArray<FMantleUploadedAsset>& OutKnown, TArray<FString>& OutUnknown)
{
	OutKnown.Reset();
	OutUnknown.Reset();
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Known = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Unknown = nullptr;
	if (!Object->TryGetArrayField(TEXT("known"), Known) || !Object->TryGetArrayField(TEXT("unknown"), Unknown))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Known)
	{
		FMantleUploadedAsset Asset;
		if (!Value.IsValid() || !MantleApiClientUploadPrivate::ReadAssetObject(Value->AsObject(), Asset))
		{
			return false;
		}
		OutKnown.Add(MoveTemp(Asset));
	}
	for (const TSharedPtr<FJsonValue>& Value : *Unknown)
	{
		FString Hash;
		if (!Value.IsValid() || !Value->TryGetString(Hash))
		{
			return false;
		}
		OutUnknown.Add(MoveTemp(Hash));
	}
	return true;
}

float FMantleApiClient::ParseRetryAfter(const FString& Header, const FDateTime& Now)
{
	const FString Value = Header.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		return 0.0f;
	}
	constexpr double MaxSeconds = 24.0 * 60.0 * 60.0;
	if (Value.IsNumeric())
	{
		const double Seconds = FCString::Atod(*Value);
		return Seconds > 0.0 ? static_cast<float>(FMath::Min(Seconds, MaxSeconds)) : 0.0f;
	}
	FDateTime When;
	if (FDateTime::ParseHttpDate(Value, When))
	{
		const double Seconds = (When - Now).GetTotalSeconds();
		return Seconds > 0.0 ? static_cast<float>(FMath::Min(Seconds, MaxSeconds)) : 0.0f;
	}
	return 0.0f;
}

void FMantleApiClient::DiffAssetsAsync(const TArray<FString>& Hashes, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("POST"), TEXT("/assets/diff"), MakeDiffAssetsBody(Hashes), OnDone);
}

void FMantleApiClient::UploadAssetAsync(const FMantleAssetUploadOptions& Options, const TArray<uint8>& Bytes, FMantleApiCallback OnDone)
{
	SendBytesAsync(TEXT("POST"), MakeAssetUploadPath(Options), Bytes, TEXT("application/octet-stream"), OnDone, UploadTimeoutSeconds);
}

void FMantleApiClient::GetAssetAsync(const FString& AssetId, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("GET"), TEXT("/assets/") + FGenericPlatformHttp::UrlEncode(AssetId), FString(), OnDone);
}

FString FMantleApiClient::MakeResolveAssetsBody(TConstArrayView<FString> Ids)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteArrayStart(TEXT("ids"));
	for (const FString& Id : Ids)
	{
		Writer->WriteValue(Id);
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

bool FMantleApiClient::ParseResolvedAssets(const FString& Json, TArray<FMantleUploadedAsset>& OutAssets, TArray<FString>& OutMissing)
{
	OutAssets.Reset();
	OutMissing.Reset();
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!Object->TryGetArrayField(TEXT("assets"), Assets))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Assets)
	{
		FMantleUploadedAsset Asset;
		if (!Value.IsValid() || Value->Type != EJson::Object || !MantleApiClientUploadPrivate::ReadAssetObject(Value->AsObject(), Asset))
		{
			OutAssets.Reset();
			return false;
		}
		OutAssets.Add(MoveTemp(Asset));
	}
	// missing is optional: an answer that found everything may leave it out.
	const TArray<TSharedPtr<FJsonValue>>* Missing = nullptr;
	if (Object->TryGetArrayField(TEXT("missing"), Missing))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Missing)
		{
			FString Id;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Id))
			{
				OutAssets.Reset();
				OutMissing.Reset();
				return false;
			}
			OutMissing.Add(MoveTemp(Id));
		}
	}
	return true;
}

bool FMantleApiClient::IsMissingRoute(const FMantleApiResult& Response)
{
	return !Response.bSuccess && (Response.StatusCode == 404 || Response.StatusCode == 405) && Response.Code.IsEmpty();
}

void FMantleApiClient::ResolveAssetsAsync(const TArray<FString>& Ids, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("POST"), TEXT("/assets/resolve"), MakeResolveAssetsBody(Ids), OnDone);
}

void FMantleApiClient::ResolveServerAssetsAsync(const TArray<FString>& Ids, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/assets/resolve"), MakeResolveAssetsBody(Ids), OnDone);
}

FString FMantleApiClient::MakeResolveStorageBody(TConstArrayView<FMantleStorageRef> Entries)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteArrayStart(TEXT("entries"));
	for (const FMantleStorageRef& Entry : Entries)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("item"), Entry.Item);
		Writer->WriteValue(TEXT("version"), Entry.Version);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

void FMantleApiClient::ResolveStorageAsync(const FString& PlaceId, const TArray<FMantleStorageRef>& Entries, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("POST"), TEXT("/places/") + FGenericPlatformHttp::UrlEncode(PlaceId) + TEXT("/storage/resolve"),
		MakeResolveStorageBody(Entries), OnDone);
}

void FMantleApiClient::ResolveServerStorageAsync(const TArray<FMantleStorageRef>& Entries, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/storage/resolve"), MakeResolveStorageBody(Entries), OnDone);
}

void FMantleApiClient::GetServerAvatarBodyMetricsAsync(const TArray<FString>& ItemIds, FMantleApiCallback OnDone)
{
	// The same {"ids": [...]} body as a resolve.
	SendServerTokenRequest(TEXT("/server/avatar/metrics"), MakeResolveAssetsBody(ItemIds), OnDone);
}

void FMantleApiClient::CreateAvatarItemAsync(const FString& ItemJson, const FString& IdempotencyKey, FMantleApiCallback OnDone)
{
	const FString Verb = TEXT("POST");
	const FString Path = TEXT("/avatar/items");
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = BuildRequest(Verb, Path, ItemJson);
	if (!IdempotencyKey.IsEmpty())
	{
		Request->SetHeader(IdempotencyKeyHeader, IdempotencyKey);
	}
	Request->SetTimeout(TimeoutSeconds);
	DispatchMantleAsyncRequest(Request, Verb, Path, OnDone);
}

void FMantleApiClient::GetAvatarCreationAsync(const FString& ItemId, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("GET"), TEXT("/me/avatar/creations/") + FGenericPlatformHttp::UrlEncode(ItemId), FString(), OnDone);
}

namespace MantleApiClientLibraryPrivate
{
	/** A string field, or empty when it is absent or not a string (a null thumbnailUrl is empty). */
	FString ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		Object->TryGetStringField(FString(Field), Value);
		return Value;
	}

	/** The value as an object, or null when it is anything else (AsObject on another type logs an error). */
	TSharedPtr<FJsonObject> AsObjectOrNull(const TSharedPtr<FJsonValue>& Value)
	{
		return Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : TSharedPtr<FJsonObject>();
	}

	/** A script version's scripts[] (#838): every object entry with a source; anything else is skipped. */
	void ReadScriptFiles(const TSharedPtr<FJsonObject>& VersionObject, TArray<FMantleLibraryScriptFile>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!VersionObject->TryGetArrayField(TEXT("scripts"), Files))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Files)
		{
			const TSharedPtr<FJsonObject> FileObject = AsObjectOrNull(Value);
			if (!FileObject.IsValid())
			{
				continue;
			}
			FMantleLibraryScriptFile File;
			File.Path = ReadString(FileObject, TEXT("path"));
			File.Side = ReadString(FileObject, TEXT("side"));
			File.Language = ReadString(FileObject, TEXT("language"));
			File.Source = ReadString(FileObject, TEXT("source"));
			if (!File.Source.IsEmpty() || !File.Path.IsEmpty())
			{
				Out.Add(MoveTemp(File));
			}
		}
	}

	/** A manifest parameter's default as text: a string as is, a number or bool written out, anything else empty. */
	FString ReadDefaultText(const TSharedPtr<FJsonObject>& Object)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(TEXT("default"));
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			return (FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)) && FMath::Abs(Number) < 1.0e15)
				? FString::Printf(TEXT("%lld"), static_cast<long long>(FMath::RoundToDouble(Number)))
				: FString::SanitizeFloat(Number);
		}
		default:
			return FString();
		}
	}

	/** One option list of a script version's manifest (#838): "parameters" or (stage 6) "settings". */
	void ReadManifestOptions(const TSharedPtr<FJsonObject>& ManifestObject, const TCHAR* Field, TArray<FMantleLibraryScriptParameter>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if (!ManifestObject->TryGetArrayField(Field, Options) || !Options)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Options)
		{
			const TSharedPtr<FJsonObject> ParameterObject = AsObjectOrNull(Value);
			if (!ParameterObject.IsValid())
			{
				continue;
			}
			FMantleLibraryScriptParameter Parameter;
			Parameter.Name = ReadString(ParameterObject, TEXT("name"));
			if (Parameter.Name.IsEmpty())
			{
				continue;
			}
			Parameter.Type = ReadString(ParameterObject, TEXT("type"));
			Parameter.Default = ReadDefaultText(ParameterObject);
			Parameter.Help = ReadString(ParameterObject, TEXT("help"));
			ParameterObject->TryGetBoolField(TEXT("required"), Parameter.bRequired);
			double Bound = 0.0;
			if (ParameterObject->TryGetNumberField(TEXT("min"), Bound) && FMath::IsFinite(Bound))
			{
				Parameter.bHasMin = true;
				Parameter.Min = Bound;
			}
			if (ParameterObject->TryGetNumberField(TEXT("max"), Bound) && FMath::IsFinite(Bound))
			{
				Parameter.bHasMax = true;
				Parameter.Max = Bound;
			}
			const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
			if (ParameterObject->TryGetArrayField(TEXT("options"), Choices) && Choices)
			{
				for (const TSharedPtr<FJsonValue>& Choice : *Choices)
				{
					FString Text;
					if (Choice.IsValid() && Choice->Type == EJson::String && Choice->TryGetString(Text))
					{
						Parameter.Options.Add(MoveTemp(Text));
					}
				}
			}
			Out.Add(MoveTemp(Parameter));
		}
	}

	/** A script version's manifest (#838): {name, description, parameters: [option], settings: [option]}. */
	void ReadManifest(const TSharedPtr<FJsonObject>& ManifestObject, FMantleLibraryScriptManifest& Out)
	{
		Out = FMantleLibraryScriptManifest();
		Out.Name = ReadString(ManifestObject, TEXT("name"));
		Out.Description = ReadString(ManifestObject, TEXT("description"));
		ReadManifestOptions(ManifestObject, TEXT("parameters"), Out.Parameters);
		ReadManifestOptions(ManifestObject, TEXT("settings"), Out.Settings);
	}

	bool ReadCard(const TSharedPtr<FJsonObject>& Object, FMantleLibraryCard& Out, FString& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = TEXT("Mantle sent a library item that is not an object.");
			return false;
		}
		Out = FMantleLibraryCard();
		Out.Id = ReadString(Object, TEXT("id"));
		if (Out.Id.IsEmpty())
		{
			OutError = TEXT("Mantle sent a library item with no id.");
			return false;
		}
		Out.Kind = ReadString(Object, TEXT("kind"));
		Out.Name = ReadString(Object, TEXT("name"));
		Out.Description = ReadString(Object, TEXT("description"));
		Out.Visibility = ReadString(Object, TEXT("visibility"));
		Out.Status = ReadString(Object, TEXT("status"));
		Out.ThumbnailUrl = ReadString(Object, TEXT("thumbnailUrl"));
		Out.CreatedAt = ReadString(Object, TEXT("createdAt"));
		Out.UpdatedAt = ReadString(Object, TEXT("updatedAt"));
		double LatestVersion = 0.0;
		if (Object->TryGetNumberField(TEXT("latestVersion"), LatestVersion) && LatestVersion > 0.0)
		{
			Out.LatestVersion = static_cast<int32>(FMath::Min(LatestVersion, 1.0e9));
		}
		Object->TryGetBoolField(TEXT("owned"), Out.bOwned);
		Object->TryGetBoolField(TEXT("acquired"), Out.bAcquired);
		double Price = 0.0;
		if (Object->TryGetNumberField(TEXT("price"), Price) && Price > 0.0)
		{
			Out.Price = static_cast<int32>(FMath::Min(Price, 1.0e9));
		}
		Out.Source = ReadString(Object, TEXT("source"));
		const TSharedPtr<FJsonObject>* Creator = nullptr;
		if (Object->TryGetObjectField(TEXT("creator"), Creator) && Creator && Creator->IsValid())
		{
			Out.CreatorId = ReadString(*Creator, TEXT("id"));
			Out.CreatorName = ReadString(*Creator, TEXT("displayName"));
			Out.CreatorHandle = ReadString(*Creator, TEXT("handle"));
			(*Creator)->TryGetBoolField(TEXT("official"), Out.bCreatorOfficial);
		}
		const TArray<TSharedPtr<FJsonValue>>* Versions = nullptr;
		if (Object->TryGetArrayField(TEXT("versions"), Versions))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Versions)
			{
				const TSharedPtr<FJsonObject> VersionObject = AsObjectOrNull(Value);
				if (!VersionObject.IsValid())
				{
					continue;
				}
				FMantleLibraryVersion Version;
				double Number = 0.0;
				if (VersionObject->TryGetNumberField(TEXT("version"), Number) && Number > 0.0)
				{
					Version.Version = static_cast<int32>(FMath::Min(Number, 1.0e9));
				}
				Version.CreatedAt = ReadString(VersionObject, TEXT("createdAt"));
				Version.PrimaryAssetId = ReadString(VersionObject, TEXT("primaryAssetId"));
				const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
				if (VersionObject->TryGetArrayField(TEXT("dependencyAssetIds"), Dependencies))
				{
					for (const TSharedPtr<FJsonValue>& Dependency : *Dependencies)
					{
						FString DependencyId;
						if (Dependency.IsValid() && Dependency->TryGetString(DependencyId))
						{
							Version.DependencyAssetIds.Add(MoveTemp(DependencyId));
						}
					}
				}
				ReadScriptFiles(VersionObject, Version.Scripts);
				const TSharedPtr<FJsonObject>* Manifest = nullptr;
				if (VersionObject->TryGetObjectField(TEXT("manifest"), Manifest) && Manifest && Manifest->IsValid())
				{
					ReadManifest(*Manifest, Version.Manifest);
				}
				Out.Versions.Add(MoveTemp(Version));
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Places = nullptr;
		if (Object->TryGetArrayField(TEXT("storedByMyPlaces"), Places))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Places)
			{
				const TSharedPtr<FJsonObject> PlaceObject = AsObjectOrNull(Value);
				if (!PlaceObject.IsValid())
				{
					continue;
				}
				FMantleLibraryPlaceRef Place;
				Place.Id = ReadString(PlaceObject, TEXT("id"));
				Place.Name = ReadString(PlaceObject, TEXT("name"));
				if (!Place.Id.IsEmpty() || !Place.Name.IsEmpty())
				{
					Out.StoredByMyPlaces.Add(MoveTemp(Place));
				}
			}
		}
		return true;
	}

	void AppendQuery(FString& Path, const TCHAR* Key, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return;
		}
		Path += (Path.Contains(TEXT("?")) ? TEXT("&") : TEXT("?"));
		Path += Key;
		Path += TEXT("=");
		Path += FGenericPlatformHttp::UrlEncode(Value);
	}
}

FString FMantleApiClient::MakeLibraryListPath(const FMantleLibraryQuery& Query, const bool bMine, const FString& Cursor)
{
	using namespace MantleApiClientLibraryPrivate;
	FString Path = bMine ? TEXT("/library/items/mine") : TEXT("/library/items");
	AppendQuery(Path, TEXT("kind"), Query.Kind);
	AppendQuery(Path, TEXT("q"), Query.Search.TrimStartAndEnd());
	if (Query.Limit > 0)
	{
		AppendQuery(Path, TEXT("limit"), FString::FromInt(FMath::Min(Query.Limit, 50)));
	}
	AppendQuery(Path, TEXT("cursor"), Cursor);
	return Path;
}

void FMantleApiClient::ListLibraryItemsAsync(const FMantleLibraryQuery& Query, const bool bMine, const FString& Cursor, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("GET"), MakeLibraryListPath(Query, bMine, Cursor), FString(), OnDone);
}

void FMantleApiClient::GetLibraryItemAsync(const FString& ItemId, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("GET"), TEXT("/library/items/") + FGenericPlatformHttp::UrlEncode(ItemId), FString(), OnDone);
}

void FMantleApiClient::AcquireLibraryItemAsync(const FString& ItemId, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("POST"), TEXT("/library/items/") + FGenericPlatformHttp::UrlEncode(ItemId) + TEXT("/get"), FString(), OnDone);
}

void FMantleApiClient::RemoveLibraryItemAsync(const FString& ItemId, FMantleApiCallback OnDone)
{
	SendAsync(TEXT("DELETE"), TEXT("/library/items/") + FGenericPlatformHttp::UrlEncode(ItemId) + TEXT("/get"), FString(), OnDone);
}

bool FMantleApiClient::ParseLibraryCards(const FString& Json, TArray<FMantleLibraryCard>& OutCards, FString& OutNextCursor, FString& OutError)
{
	OutCards.Reset();
	OutNextCursor.Reset();
	OutError.Reset();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("Mantle's library answer could not be read.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(TEXT("items"), Items))
	{
		OutError = TEXT("Mantle's library answer has no list of items.");
		return false;
	}
	TArray<FMantleLibraryCard> Cards;
	for (const TSharedPtr<FJsonValue>& Value : *Items)
	{
		FMantleLibraryCard Card;
		if (!MantleApiClientLibraryPrivate::ReadCard(MantleApiClientLibraryPrivate::AsObjectOrNull(Value), Card, OutError))
		{
			return false;
		}
		Cards.Add(MoveTemp(Card));
	}
	OutCards = MoveTemp(Cards);
	Root->TryGetStringField(TEXT("nextCursor"), OutNextCursor);
	return true;
}

bool FMantleApiClient::ParseLibraryItem(const FString& Json, FMantleLibraryCard& OutCard, FString& OutError)
{
	OutCard = FMantleLibraryCard();
	OutError.Reset();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("Mantle's library answer could not be read.");
		return false;
	}
	return MantleApiClientLibraryPrivate::ReadCard(Root, OutCard, OutError);
}

void FMantleApiClient::GetServerBootstrapAsync(FMantleApiCallback OnDone)
{
	if (MantlePreviewLaunch::IsRequested())
	{
		RefusePreviewRequestAsync(TEXT("GET"), TEXT("/sessions/bootstrap"), OnDone);
		return;
	}
	const FString Token = FMantleConnection::ResolveServerToken();
	if (Token.IsEmpty())
	{
		FMantleApiResult Result;
		Result.Message = TEXT("This dedicated server has no MANTLE_SERVER_TOKEN.");
		OnDone.ExecuteIfBound(Result);
		return;
	}

	const FString Verb = TEXT("GET");
	const FString Path = TEXT("/sessions/bootstrap");
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		BuildRequest(Verb, Path, FString(), Token);
	Request->SetTimeout(TimeoutSeconds);
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone, Verb, Path](FHttpRequestPtr Finished, FHttpResponsePtr Response, bool bConnected)
		{
			FMantleApiResult Result;
			if (bConnected)
			{
				Result = ReadResponse(Verb, Path, Finished, Response);
			}
			else
			{
				Result.Message = TEXT("Could not reach the Mantle server bootstrap endpoint.");
			}
			OnDone.ExecuteIfBound(Result);
		});
	if (!Request->ProcessRequest())
	{
		FMantleApiResult Result;
		Result.Message = TEXT("Could not start the dedicated-server bootstrap request.");
		OnDone.ExecuteIfBound(Result);
	}
}

FString FMantleApiClient::ProjectBuild()
{
	FString Version;
	if (GConfig)
	{
		GConfig->GetString(TEXT("Mantle"), TEXT("ReleaseBuild"), Version, GGameIni);
	}
	return Version.TrimStartAndEnd();
}

FString FMantleApiClient::BuildVerifyJoinCredentialBody(const FString& Credential, const FString& ServerBuild)
{
	auto Escape = [](FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Value.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return Value;
	};
	return ServerBuild.IsEmpty()
		? FString::Printf(TEXT("{\"credential\":\"%s\"}"), *Escape(Credential))
		: FString::Printf(TEXT("{\"credential\":\"%s\",\"serverBuild\":\"%s\"}"),
			*Escape(Credential), *Escape(ServerBuild));
}

void FMantleApiClient::VerifyJoinCredentialAsync(const FString& Credential, FMantleApiCallback OnDone)
{
	if (MantlePreviewLaunch::IsRequested())
	{
		RefusePreviewRequestAsync(TEXT("POST"), TEXT("/server/sessions/verify"), OnDone);
		return;
	}
	const FString Token = FMantleConnection::ResolveServerToken();
	if (Token.IsEmpty())
	{
		FMantleApiResult Result;
		Result.Message = TEXT("This dedicated server has no MANTLE_SERVER_TOKEN.");
		OnDone.ExecuteIfBound(Result);
		return;
	}

	const FString Body = BuildVerifyJoinCredentialBody(Credential, ProjectBuild());

	const FString Verb = TEXT("POST");
	const FString Path = TEXT("/server/sessions/verify");
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		BuildRequest(Verb, Path, Body, Token);
	Request->SetTimeout(TimeoutSeconds);
	Request->OnProcessRequestComplete().BindLambda(
		[OnDone, Verb, Path](FHttpRequestPtr Finished, FHttpResponsePtr Response, bool bConnected)
		{
			FMantleApiResult Result;
			if (bConnected)
			{
				Result = ReadResponse(Verb, Path, Finished, Response);
			}
			else
			{
				Result.Message = TEXT("Could not reach the Mantle join-credential verify endpoint.");
			}
			OnDone.ExecuteIfBound(Result);
		});
	if (!Request->ProcessRequest())
	{
		FMantleApiResult Result;
		Result.Message = TEXT("Could not start the join-credential verify request.");
		OnDone.ExecuteIfBound(Result);
	}
}

namespace
{
	/** scope/playerId/key never contain characters JSON needs escaped: scope is one of the two
	    literals below, playerId is a plain UUID, and key matches the backend's own
	    ^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$ (BACKEND.md) -- unlike ValueEnvelopeJson, none of
	    these need FString::ReplaceInline treatment before going straight into the body. */
	FString DataStoreRequestBody(const FString& Scope, const FString& PlayerId, const FString& Key)
	{
		return FString::Printf(TEXT("{\"scope\":\"%s\",\"playerId\":\"%s\",\"key\":\"%s\""), *Scope, *PlayerId, *Key);
	}
}

void FMantleApiClient::DataStoreReadAsync(
	const FString& Scope, const FString& PlayerId, const FString& Key, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/datastore/read"), DataStoreRequestBody(Scope, PlayerId, Key) + TEXT("}"), OnDone);
}

void FMantleApiClient::DataStoreListAsync(
	const FString& Scope, const FString& PlayerId, const FString& Prefix, const FString& Cursor, int32 Limit,
	FMantleApiCallback OnDone)
{
	// The cursor is the backend's own base64url token, so it needs no escaping either. It is left
	// out when empty, which keeps the first page's body the one every server sent before cursors.
	FString Body = FString::Printf(
		TEXT("{\"scope\":\"%s\",\"playerId\":\"%s\",\"prefix\":\"%s\",\"limit\":%d"), *Scope, *PlayerId, *Prefix, Limit);
	if (!Cursor.IsEmpty())
	{
		Body += FString::Printf(TEXT(",\"cursor\":\"%s\""), *Cursor);
	}
	Body += TEXT("}");
	SendServerTokenRequest(TEXT("/server/datastore/list"), Body, OnDone);
}

void FMantleApiClient::DataStoreWriteAsync(
	const FString& Scope, const FString& PlayerId, const FString& Key, const FString& ValueEnvelopeJson,
	TOptional<int64> ExpectedVersion, FMantleApiCallback OnDone)
{
	FString Body = DataStoreRequestBody(Scope, PlayerId, Key) + FString::Printf(TEXT(",\"value\":%s"), *ValueEnvelopeJson);
	if (ExpectedVersion.IsSet())
	{
		Body += FString::Printf(TEXT(",\"expectedVersion\":%lld"), ExpectedVersion.GetValue());
	}
	Body += TEXT("}");
	SendServerTokenRequest(TEXT("/server/datastore/write"), Body, OnDone);
}

void FMantleApiClient::DataStoreDeleteAsync(
	const FString& Scope, const FString& PlayerId, const FString& Key, TOptional<int64> ExpectedVersion,
	FMantleApiCallback OnDone)
{
	FString Body = DataStoreRequestBody(Scope, PlayerId, Key);
	if (ExpectedVersion.IsSet())
	{
		Body += FString::Printf(TEXT(",\"expectedVersion\":%lld"), ExpectedVersion.GetValue());
	}
	Body += TEXT("}");
	SendServerTokenRequest(TEXT("/server/datastore/delete"), Body, OnDone);
}

void FMantleApiClient::DataStoreOrderedAsync(const FString& Operation, const FString& BodyJson, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(FString(TEXT("/server/datastore/ordered/")) + Operation, BodyJson, OnDone);
}

FString FMantleApiClient::BuildServerChatMessagesBody(TOptional<int64> After)
{
	return After.IsSet() ? FString::Printf(TEXT("{\"after\":%lld}"), After.GetValue()) : FString(TEXT("{}"));
}

FString FMantleApiClient::BuildServerChatSystemBody(const FString& Text, const FString& PlayerId,
	const TArray<FString>& PlayerIds)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("text"), Text);
	if (!PlayerId.IsEmpty())
	{
		Writer->WriteValue(TEXT("playerId"), PlayerId);
	}
	if (!PlayerIds.IsEmpty())
	{
		Writer->WriteArrayStart(TEXT("playerIds"));
		for (const FString& Id : PlayerIds)
		{
			Writer->WriteValue(Id);
		}
		Writer->WriteArrayEnd();
	}
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

void FMantleApiClient::ServerChatMessagesAsync(TOptional<int64> After, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/chat/messages"), BuildServerChatMessagesBody(After), OnDone);
}

void FMantleApiClient::ServerChatSystemAsync(const FString& Text, const FString& PlayerId, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/chat/system"), BuildServerChatSystemBody(Text, PlayerId), OnDone);
}

void FMantleApiClient::ServerChatSystemToAsync(const FString& Text, const TArray<FString>& PlayerIds, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/chat/system"), BuildServerChatSystemBody(Text, FString(), PlayerIds), OnDone);
}

void FMantleApiClient::ServerHttpSummaryAsync(const FString& BodyJson, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/http/summary"), BodyJson, OnDone);
}

void FMantleApiClient::ServerChatIngestAsync(const FString& BodyJson, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/chat/messages/ingest"), BodyJson, OnDone);
}

FString FMantleApiClient::BuildServerSeatsHeartbeatBody(const TArray<FString>& UserIds)
{
	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteArrayStart(TEXT("userIds"));
	for (const FString& UserId : UserIds)
	{
		Writer->WriteValue(UserId);
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

void FMantleApiClient::ServerSeatsHeartbeatAsync(const TArray<FString>& UserIds, FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/seats/heartbeat"), BuildServerSeatsHeartbeatBody(UserIds), OnDone);
}

void FMantleApiClient::ServerPlaceReadyAsync(FMantleApiCallback OnDone)
{
	SendServerTokenRequest(TEXT("/server/sessions/place-ready"), TEXT("{}"), OnDone);
}

FString FMantleApiClient::ParseVerifiedUserId(const FMantleApiResult& Response)
{
	if (!Response.bSuccess)
	{
		return FString();
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response.Body);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return FString();
	}

	FString UserId;
	Object->TryGetStringField(TEXT("userId"), UserId);
	return UserId;
}

FString FMantleApiClient::ParseVerifiedDisplayName(const FMantleApiResult& Response)
{
	if (!Response.bSuccess)
	{
		return FString();
	}

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response.Body);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return FString();
	}

	FString DisplayName;
	Object->TryGetStringField(TEXT("displayName"), DisplayName);
	return DisplayName;
}

FString FMantleCompatibilityFailure::ToLine() const
{
	FString Line = Problem;
	if (!Remedy.IsEmpty())
	{
		Line += TEXT(" ") + Remedy;
	}
	if (!Path.IsEmpty())
	{
		Line += FString::Printf(TEXT(" (place file: %s)"), *Path);
	}
	return Line;
}

namespace
{
	/** "Script 3" for a check at /scripts/2/..., counting from one; "A script" otherwise. */
	FString MantleCompatibilityScriptWord(const FString& Path)
	{
		const FString Prefix = TEXT("/scripts/");
		if (Path.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			FString Index = Path.Mid(Prefix.Len());
			int32 Slash = INDEX_NONE;
			if (Index.FindChar(TEXT('/'), Slash))
			{
				Index.LeftInline(Slash);
			}
			if (!Index.IsEmpty() && Index.IsNumeric())
			{
				return FString::Printf(TEXT("Script %d"), FCString::Atoi(*Index) + 1);
			}
		}
		return TEXT("A script");
	}
}

TArray<FMantleCompatibilityFailure> FMantleApiClient::ParseCompatibilityFailures(const FString& Body)
{
	TArray<FMantleCompatibilityFailure> Failures;

	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return Failures;
	}
	const TSharedPtr<FJsonObject>* Report = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
	if (!Object->TryGetObjectField(TEXT("report"), Report) || !Report || !Report->IsValid()
		|| !(*Report)->TryGetArrayField(TEXT("checks"), Checks) || !Checks)
	{
		return Failures;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Checks)
	{
		const TSharedPtr<FJsonObject>* Check = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Check) || !Check || !Check->IsValid())
		{
			continue;
		}
		bool bPassed = true;
		if (!(*Check)->TryGetBoolField(TEXT("passed"), bPassed) || bPassed)
		{
			continue;
		}

		FMantleCompatibilityFailure Failure;
		FString Message;
		(*Check)->TryGetStringField(TEXT("name"), Failure.Name);
		(*Check)->TryGetStringField(TEXT("path"), Failure.Path);
		(*Check)->TryGetStringField(TEXT("message"), Message);
		Message.TrimStartAndEndInline();

		// The checks whose message is only an instruction ("Use schemaVersion 1 ...") get the
		// problem said first, and the API's message reads as the remedy. Names are ids
		// (backend/internal/compatibility, Run), so they compare case-sensitively.
		auto Is = [&Failure](const TCHAR* Name) { return Failure.Name.Equals(Name, ESearchCase::CaseSensitive); };
		FString Problem;
		if (Is(TEXT("schema-version")))
		{
			Problem = TEXT("The place file's version is not one players' games can read.");
		}
		else if (Is(TEXT("metadata")))
		{
			Problem = TEXT("The place has no name.");
		}
		else if (Is(TEXT("server-capacity")))
		{
			Problem = TEXT("The place's maximum player count is out of range.");
		}
		else if (Is(TEXT("script-language")))
		{
			Problem = MantleCompatibilityScriptWord(Failure.Path) + TEXT(" is in a language Mantle doesn't run.");
		}
		else if (Is(TEXT("script-world")))
		{
			Problem = MantleCompatibilityScriptWord(Failure.Path) + TEXT(" doesn't say which side it runs on.");
		}
		else if (Is(TEXT("network-high-latency")))
		{
			Problem = TEXT("The place file is too large.");
		}

		if (!Problem.IsEmpty())
		{
			Failure.Problem = MoveTemp(Problem);
			Failure.Remedy = Message;
		}
		else
		{
			// Every other check's message says what failed in its first sentence (the script API
			// version, or the entity type, asset kind, key or property the released game can't
			// read yet) and what to do after it.
			const int32 End = Message.Find(TEXT(". "), ESearchCase::CaseSensitive);
			if (End == INDEX_NONE)
			{
				Failure.Problem = Message;
			}
			else
			{
				Failure.Problem = Message.Left(End + 1);
				Failure.Remedy = Message.Mid(End + 2).TrimStart();
			}
			if (Failure.Problem.IsEmpty())
			{
				Failure.Problem = FString::Printf(TEXT("The '%s' check failed."), *Failure.Name);
			}
		}
		Failures.Add(MoveTemp(Failure));
	}
	return Failures;
}

bool FMantleApiClient::PumpUntil(TFunctionRef<bool()> Condition, float BudgetSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + BudgetSeconds;

	while (!Condition() && FPlatformTime::Seconds() < Deadline)
	{
		FHttpModule::Get().GetHttpManager().Tick(0.016f);
		FPlatformProcess::Sleep(0.005f);
	}

	return Condition();
}
