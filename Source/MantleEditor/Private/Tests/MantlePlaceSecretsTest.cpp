// Copyright Joshua Eger. All Rights Reserved.
#include "MantleApiClient.h"
#include "MantlePlaceSecrets.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// GhzGarage/mantle#1011: the place secrets checks, error wording and local test file. Pure: no calls to the backend.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantlePlaceSecretsNamesTest, "Mantle.PlaceSecrets.Names",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMantlePlaceSecretsNamesTest::RunTest(const FString& Parameters)
{
	FString Why;
	TestEqual(TEXT("typed names are upper-cased and trimmed"), MantlePlaceSecrets::NormalizeName(TEXT("  api_key2 ")), FString(TEXT("API_KEY2")));
	TestTrue(TEXT("plain name"), MantlePlaceSecrets::CheckName(TEXT("API_KEY"), Why));
	TestTrue(TEXT("one letter"), MantlePlaceSecrets::CheckName(TEXT("A"), Why));
	TestTrue(TEXT("64 characters"), MantlePlaceSecrets::CheckName(TEXT("A") + FString::ChrN(63, TEXT('B')), Why));
	TestFalse(TEXT("65 characters"), MantlePlaceSecrets::CheckName(TEXT("A") + FString::ChrN(64, TEXT('B')), Why));
	TestFalse(TEXT("empty"), MantlePlaceSecrets::CheckName(FString(), Why));
	TestFalse(TEXT("starts with a digit"), MantlePlaceSecrets::CheckName(TEXT("1KEY"), Why));
	TestFalse(TEXT("starts with underscore"), MantlePlaceSecrets::CheckName(TEXT("_KEY"), Why));
	TestFalse(TEXT("lower case"), MantlePlaceSecrets::CheckName(TEXT("Key"), Why));
	TestFalse(TEXT("a dash"), MantlePlaceSecrets::CheckName(TEXT("MY-KEY"), Why));
	TestFalse(TEXT("a space"), MantlePlaceSecrets::CheckName(TEXT("MY KEY"), Why));
	TestFalse(TEXT("a slash"), MantlePlaceSecrets::CheckName(TEXT("MY/KEY"), Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantlePlaceSecretsValuesTest, "Mantle.PlaceSecrets.Values",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMantlePlaceSecretsValuesTest::RunTest(const FString& Parameters)
{
	FString Why;
	const int32 Max = MantlePlaceSecrets::DefaultMaxValueBytes;
	TestFalse(TEXT("empty"), MantlePlaceSecrets::CheckValue(FString(), Max, Why));
	TestTrue(TEXT("one byte"), MantlePlaceSecrets::CheckValue(TEXT("x"), Max, Why));
	TestTrue(TEXT("exactly the limit"), MantlePlaceSecrets::CheckValue(FString::ChrN(Max, TEXT('a')), Max, Why));
	TestFalse(TEXT("one over"), MantlePlaceSecrets::CheckValue(FString::ChrN(Max + 1, TEXT('a')), Max, Why));
	// Bytes, not characters: 4096 two-byte characters fit, 4097 do not, though both are well under 8192 characters.
	TestTrue(TEXT("4096 two-byte characters"), MantlePlaceSecrets::CheckValue(FString::ChrN(Max / 2, 0x00E9), Max, Why));
	TestFalse(TEXT("4097 two-byte characters"), MantlePlaceSecrets::CheckValue(FString::ChrN(Max / 2 + 1, 0x00E9), Max, Why));
	TestTrue(TEXT("a PEM keeps its line endings"), MantlePlaceSecrets::CheckValue(TEXT("-----BEGIN KEY-----\r\nabc\r\n-----END KEY-----\n\t"), Max, Why));
	TestFalse(TEXT("a NUL"), MantlePlaceSecrets::CheckValue(FString(TEXT("a")) + FString::ChrN(1, 0x01) + TEXT("b"), Max, Why));
	TestFalse(TEXT("an escape"), MantlePlaceSecrets::CheckValue(FString(TEXT("a")) + FString::ChrN(1, 0x1B), Max, Why));
	TestFalse(TEXT("a delete"), MantlePlaceSecrets::CheckValue(FString(TEXT("a")) + FString::ChrN(1, 0x7F), Max, Why));

	TestTrue(TEXT("a new name under the limit"), MantlePlaceSecrets::CheckCount(49, 50, false, Why));
	TestFalse(TEXT("a new name at the limit"), MantlePlaceSecrets::CheckCount(50, 50, false, Why));
	TestTrue(TEXT("a replacement at the limit"), MantlePlaceSecrets::CheckCount(50, 50, true, Why));
	TestEqual(TEXT("count text"), MantlePlaceSecrets::CountText(3, 50), FString(TEXT("3 / 50")));

	// The body is exactly {"value": string}, with the value escaped.
	TestEqual(TEXT("set body"), MantlePlaceSecrets::MakeSetBody(TEXT("a\"b\nc")), FString(TEXT("{\"value\":\"a\\\"b\\nc\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantlePlaceSecretsErrorsTest, "Mantle.PlaceSecrets.Errors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMantlePlaceSecretsErrorsTest::RunTest(const FString& Parameters)
{
	auto Failure = [](int32 Status, const TCHAR* Code, const TCHAR* Message)
	{
		FMantleApiResult Result;
		Result.StatusCode = Status;
		Result.Code = Code;
		Result.Message = Message;
		return Result;
	};
	// The backend's own words are shown as sent, for every code in its contract.
	struct FCase { int32 Status; const TCHAR* Code; };
	const FCase Cases[] = {
		{401, TEXT("missing_token")}, {401, TEXT("invalid_token")}, {401, TEXT("expired_token")}, {401, TEXT("account_deleted")},
		{403, TEXT("creator_token_scope")}, {404, TEXT("not_found")}, {404, TEXT("secret_not_found")},
		{400, TEXT("invalid_secret_name")}, {400, TEXT("invalid_secret_value")}, {400, TEXT("invalid_request")},
		{409, TEXT("too_many_secrets")}, {500, TEXT("server_error")},
	};
	for (const FCase& Case : Cases)
	{
		const FMantleApiResult Result = Failure(Case.Status, Case.Code, TEXT("Words the backend wrote for the creator."));
		TestEqual(FString::Printf(TEXT("%s shows the backend message"), Case.Code), MantlePlaceSecrets::DescribeFailure(Result), FString(TEXT("Words the backend wrote for the creator.")));
		TestFalse(FString::Printf(TEXT("%s does not turn the store off"), Case.Code), MantlePlaceSecrets::IsStoreOff(Result));
		TestFalse(FString::Printf(TEXT("%s has a message even with no backend words"), Case.Code), MantlePlaceSecrets::DescribeFailure(Failure(Case.Status, Case.Code, TEXT(""))).IsEmpty());
	}
	const FMantleApiResult Off = Failure(503, TEXT("secrets_unavailable"), TEXT("backend words"));
	TestTrue(TEXT("503 secrets_unavailable turns the panel off"), MantlePlaceSecrets::IsStoreOff(Off));
	TestEqual(TEXT("and says so plainly"), MantlePlaceSecrets::DescribeFailure(Off), MantlePlaceSecrets::StoreOffMessage());
	TestFalse(TEXT("another 503 does not"), MantlePlaceSecrets::IsStoreOff(Failure(503, TEXT("busy"), TEXT("x"))));
	TestTrue(TEXT("no connection says so"), MantlePlaceSecrets::DescribeFailure(Failure(0, TEXT(""), TEXT(""))).Contains(TEXT("Could not reach")));
	TestFalse(TEXT("a level with no place is told so"), MantlePlaceSecrets::NoPlaceMessage().IsEmpty());

	MantlePlaceSecrets::FList List;
	TestTrue(TEXT("a list parses"), MantlePlaceSecrets::ParseList(
		TEXT("{\"secrets\":[{\"name\":\"API_KEY\",\"createdAt\":\"2026-10-21T19:00:00Z\",\"updatedAt\":\"2026-10-21T19:05:00Z\"}],\"limits\":{\"maxSecrets\":50,\"maxValueBytes\":8192}}"), List));
	TestEqual(TEXT("one entry"), List.Entries.Num(), 1);
	TestEqual(TEXT("its name"), List.Entries[0].Name, FString(TEXT("API_KEY")));
	TestEqual(TEXT("its time"), List.Entries[0].UpdatedAt, FString(TEXT("2026-10-21T19:05:00Z")));
	TestEqual(TEXT("the limits"), List.MaxSecrets, 50);
	TestFalse(TEXT("not a list"), MantlePlaceSecrets::ParseList(TEXT("{\"nope\":1}"), List));
	TestFalse(TEXT("not json"), MantlePlaceSecrets::ParseList(TEXT("<html>"), List));

	TestEqual(TEXT("list path"), MantlePlaceSecrets::ListPath(TEXT("plc_abc")), FString(TEXT("/places/plc_abc/secrets")));
	TestEqual(TEXT("item path"), MantlePlaceSecrets::ItemPath(TEXT("plc_abc"), TEXT("API_KEY")), FString(TEXT("/places/plc_abc/secrets/API_KEY")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantlePlaceSecretsLocalFileTest, "Mantle.PlaceSecrets.LocalFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMantlePlaceSecretsLocalFileTest::RunTest(const FString& Parameters)
{
	// .gitignore: added once, and never twice.
	FString Ignore = TEXT("Saved/\nIntermediate/");
	TestFalse(TEXT("not ignored yet"), MantlePlaceSecrets::AddToGitIgnore(Ignore));
	TestTrue(TEXT("now ignored"), Ignore.Contains(TEXT("\nsecrets.local.json\n")));
	TestTrue(TEXT("the existing lines stay"), Ignore.StartsWith(TEXT("Saved/\nIntermediate/\n")));
	const FString Once = Ignore;
	TestTrue(TEXT("already ignored"), MantlePlaceSecrets::AddToGitIgnore(Ignore));
	TestEqual(TEXT("and left alone"), Ignore, Once);
	FString Existing = TEXT("**/secrets.local.json\n");
	TestTrue(TEXT("an existing wildcard line counts"), MantlePlaceSecrets::AddToGitIgnore(Existing));

	// Bundle copy and cleanup in a scratch folder.
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("MantlePlaceSecretsTest"));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	const FString Bundle = Root / TEXT("bundle");
	const FString Source = Root / TEXT("source") / MantlePlaceSecrets::LocalFileName();
	IFileManager::Get().MakeDirectory(*Bundle, true);
	FString Why;
	TestTrue(TEXT("no source: nothing to copy, not an error"), MantlePlaceSecrets::CopyToBundle(Source, Bundle, Why));
	TestFalse(TEXT("and no copy appears"), IFileManager::Get().FileExists(*(Bundle / MantlePlaceSecrets::LocalFileName())));

	FFileHelper::SaveStringToFile(TEXT("{\"TEST_KEY\":\"test-value\"}"), *Source);
	TestTrue(TEXT("copy"), MantlePlaceSecrets::CopyToBundle(Source, Bundle, Why));
	FString Copied;
	FFileHelper::LoadFileToString(Copied, *(Bundle / MantlePlaceSecrets::LocalFileName()));
	TestEqual(TEXT("the copy matches"), Copied, FString(TEXT("{\"TEST_KEY\":\"test-value\"}")));
	TestTrue(TEXT("the source stays"), IFileManager::Get().FileExists(*Source));

	MantlePlaceSecrets::RemoveFromBundle(Bundle);
	TestFalse(TEXT("the copy is deleted"), IFileManager::Get().FileExists(*(Bundle / MantlePlaceSecrets::LocalFileName())));
	TestTrue(TEXT("the source still stays"), IFileManager::Get().FileExists(*Source));
	MantlePlaceSecrets::RemoveFromBundle(Bundle);
	MantlePlaceSecrets::RemoveFromBundle(FString());

	TestTrue(TEXT("no level, no path"), MantlePlaceSecrets::LocalFilePathFor(FString()).IsEmpty());
	TestTrue(TEXT("a level's path ends with the file"), MantlePlaceSecrets::LocalFilePathFor(TEXT("MyPlace")).EndsWith(TEXT("MyPlace/secrets.local.json")));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif
