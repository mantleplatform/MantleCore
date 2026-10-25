// Copyright Joshua Eger. All Rights Reserved.
#include "MantleApiClient.h"
#include "MantleLibraryPanel.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceStorage.h"
#include "MantleVehicleSpawnPoint.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The editor Library panel's testable halves: reading the library routes' answers, and the rules for
// turning a library item into a place storage entry. The Slate panel itself is a manual check.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleLibraryTestPrivate
{
	const TCHAR* const ItemTruck = TEXT("itm_u0123456789abcdef0123456789abcdef");
	const TCHAR* const ItemBus = TEXT("itm_u11111111111111111111111111111111");

	/** JSON written with single quotes, so the cases stay readable. */
	FString Json(const TCHAR* Text)
	{
		FString Result(Text);
		Result.ReplaceInline(TEXT("'"), TEXT("\""));
		return Result;
	}

	FMantleStorageEntry MakeEntry(const FString& Name, const FString& Item, const int32 Version)
	{
		FMantleStorageEntry Entry;
		Entry.Name = Name;
		Entry.Item = Item;
		Entry.Version = Version;
		return Entry;
	}

	/** The n-th distinct, valid library item id. */
	FString ItemId(const int32 Number)
	{
		return FString::Printf(TEXT("itm_u%032x"), Number);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryParsesCardsTest,
	"Mantle.Library.Api.ParsesCardsAndCursor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryParsesCardsTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	const FString Body = Json(TEXT(
		"{'items':["
		"{'id':'itm_u0123456789abcdef0123456789abcdef','kind':'vehicle','name':'Sports Car','description':'Fast.',"
		"'visibility':'private','status':'active','creator':{'id':'usr_1','displayName':'Alice','handle':'alice','official':false},"
		"'latestVersion':3,'owned':true,'acquired':false,'thumbnailUrl':'https://cdn.example/thumb.png',"
		"'createdAt':'2026-10-01T10:00:00Z','updatedAt':'2026-10-02T10:00:00Z'},"
		"{'id':'itm_u11111111111111111111111111111111','kind':'weapon','name':'Rifle','description':'',"
		"'visibility':'public','status':'active','creator':{'id':'usr_2','displayName':'Mantle','handle':'mantle','official':true},"
		"'latestVersion':1,'owned':false,'acquired':true,'thumbnailUrl':null,"
		"'createdAt':'2026-10-01T10:00:00Z','updatedAt':'2026-10-01T10:00:00Z'}],"
		"'nextCursor':'abc123'}"));

	TArray<FMantleLibraryCard> Cards;
	FString Next;
	FString Error;
	if (!TestTrue(TEXT("a list answer is read"), FMantleApiClient::ParseLibraryCards(Body, Cards, Next, Error)) || !TestEqual(TEXT("two cards"), Cards.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the cursor"), Next, FString(TEXT("abc123")));
	const FMantleLibraryCard& Car = Cards[0];
	TestEqual(TEXT("id"), Car.Id, FString(ItemTruck));
	TestEqual(TEXT("kind"), Car.Kind, FString(TEXT("vehicle")));
	TestEqual(TEXT("name"), Car.Name, FString(TEXT("Sports Car")));
	TestEqual(TEXT("creator name"), Car.CreatorName, FString(TEXT("Alice")));
	TestEqual(TEXT("latest version"), Car.LatestVersion, 3);
	TestTrue(TEXT("private"), Car.IsPrivate());
	TestTrue(TEXT("owned"), Car.bOwned);
	TestTrue(TEXT("an owned item can be used"), Car.CanUse());
	TestEqual(TEXT("thumbnail"), Car.ThumbnailUrl, FString(TEXT("https://cdn.example/thumb.png")));
	const FMantleLibraryCard& Rifle = Cards[1];
	TestFalse(TEXT("public is not private"), Rifle.IsPrivate());
	TestTrue(TEXT("official creator"), Rifle.bCreatorOfficial);
	TestTrue(TEXT("an acquired item can be used"), Rifle.CanUse());

	// The last page has no cursor.
	const FString LastPage = Json(TEXT("{'items':[]}"));
	TestTrue(TEXT("an empty page is read"), FMantleApiClient::ParseLibraryCards(LastPage, Cards, Next, Error));
	TestEqual(TEXT("no cards"), Cards.Num(), 0);
	TestTrue(TEXT("no cursor"), Next.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryNullThumbnailTest,
	"Mantle.Library.Api.NullThumbnailIsEmpty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryNullThumbnailTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	// A card with a null thumbnail and with none at all both read as "no image", and keep their other fields.
	const FString Body = Json(TEXT(
		"{'items':[{'id':'itm_u0123456789abcdef0123456789abcdef','name':'A','thumbnailUrl':null,'latestVersion':1},"
		"{'id':'itm_u11111111111111111111111111111111'}]}"));
	TArray<FMantleLibraryCard> Cards;
	FString Next;
	FString Error;
	if (!TestTrue(TEXT("read"), FMantleApiClient::ParseLibraryCards(Body, Cards, Next, Error)) || !TestEqual(TEXT("two cards"), Cards.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("null thumbnail is empty"), Cards[0].ThumbnailUrl.IsEmpty());
	TestEqual(TEXT("the other fields still read"), Cards[0].Name, FString(TEXT("A")));
	TestTrue(TEXT("a missing thumbnail is empty"), Cards[1].ThumbnailUrl.IsEmpty());
	TestEqual(TEXT("a missing version is 0"), Cards[1].LatestVersion, 0);
	TestFalse(TEXT("a card with no flags is neither owned nor acquired"), Cards[1].CanUse());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryMalformedAnswerTest,
	"Mantle.Library.Api.RejectsMalformedAnswers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryMalformedAnswerTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	const TCHAR* const Bad[] = {
		TEXT(""),
		TEXT("not json"),
		TEXT("[]"),
		TEXT("{}"),
		TEXT("{'items':'nope'}"),
		TEXT("{'items':[1]}"),
		TEXT("{'items':[{'name':'no id'}]}"),
		TEXT("{'items':[{'id':'itm_u0123456789abcdef0123456789abcdef'},{'id':''}]}"),
	};
	for (const TCHAR* Text : Bad)
	{
		TArray<FMantleLibraryCard> Cards;
		Cards.Add(FMantleLibraryCard());
		FString Next = TEXT("stale");
		FString Error;
		const FString Body = Json(Text);
		TestFalse(FString::Printf(TEXT("'%s' is refused"), *Body), FMantleApiClient::ParseLibraryCards(Body, Cards, Next, Error));
		TestFalse(TEXT("with a message"), Error.IsEmpty());
		TestEqual(TEXT("and no cards kept"), Cards.Num(), 0);
		TestTrue(TEXT("and no cursor kept"), Next.IsEmpty());
	}

	// One item with its versions.
	FMantleLibraryCard Card;
	FString Error;
	const FString Detail = Json(TEXT(
		"{'id':'itm_u0123456789abcdef0123456789abcdef','kind':'vehicle','name':'Car','latestVersion':2,"
		"'versions':[{'version':1,'createdAt':'2026-10-01T10:00:00Z','primaryAssetId':'ast_u11111111111111111111111111111111','dependencyAssetIds':[]},"
		"{'version':2,'primaryAssetId':'ast_u22222222222222222222222222222222','dependencyAssetIds':['ast_u33333333333333333333333333333333']}]}"));
	if (TestTrue(TEXT("a detail answer is read"), FMantleApiClient::ParseLibraryItem(Detail, Card, Error)) && TestEqual(TEXT("two versions"), Card.Versions.Num(), 2))
	{
		TestEqual(TEXT("version number"), Card.Versions[1].Version, 2);
		TestEqual(TEXT("its primary asset"), Card.Versions[1].PrimaryAssetId, FString(TEXT("ast_u22222222222222222222222222222222")));
		TestEqual(TEXT("its dependency"), Card.Versions[1].DependencyAssetIds.Num(), 1);
	}
	TestFalse(TEXT("a detail answer needs an id"), FMantleApiClient::ParseLibraryItem(Json(TEXT("{'name':'x'}")), Card, Error));
	TestFalse(TEXT("a detail answer must be JSON"), FMantleApiClient::ParseLibraryItem(TEXT("<html>"), Card, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryListPathTest,
	"Mantle.Library.Api.ListPathEncodesTheQuery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryListPathTest::RunTest(const FString& Parameters)
{
	FMantleLibraryQuery Query;
	TestEqual(TEXT("all items, first page"), FMantleApiClient::MakeLibraryListPath(Query, false, FString()), FString(TEXT("/library/items?limit=24")));
	TestEqual(TEXT("the caller's own library"), FMantleApiClient::MakeLibraryListPath(Query, true, FString()), FString(TEXT("/library/items/mine?limit=24")));

	Query.Kind = TEXT("vehicle");
	Query.Search = TEXT("  red & fast  ");
	Query.Limit = 500;
	const FString Path = FMantleApiClient::MakeLibraryListPath(Query, false, TEXT("abc"));
	TestTrue(TEXT("starts with the route"), Path.StartsWith(TEXT("/library/items?")));
	TestTrue(TEXT("the kind"), Path.Contains(TEXT("kind=vehicle")));
	TestTrue(TEXT("a trimmed search with its space and ampersand encoded"), Path.Contains(TEXT("q=red%20%26%20fast&")));
	TestTrue(TEXT("the limit clamped to the API's 50"), Path.Contains(TEXT("limit=50")));
	TestTrue(TEXT("the cursor"), Path.EndsWith(TEXT("cursor=abc")));
	TestFalse(TEXT("no raw space"), Path.Contains(TEXT(" ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryStorageNameTest,
	"Mantle.Library.Storage.NamesAreLegalAndUnique",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryStorageNameTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	const TCHAR* const Cases[][2] = {
		{ TEXT("Sports Car"), TEXT("Sports_Car") },
		{ TEXT("  --Hello,  World!!  "), TEXT("Hello_World") },
		{ TEXT("A__B"), TEXT("A_B") },
		{ TEXT("2 Fast"), TEXT("Item_2_Fast") },
		{ TEXT("Caf\u00e9 Racer"), TEXT("Caf_Racer") },
		{ TEXT(""), TEXT("Item") },
		{ TEXT("!!!"), TEXT("Item") },
		{ TEXT("Truck"), TEXT("Truck") },
	};
	for (const TCHAR* const (&Case)[2] : Cases)
	{
		const FString Name = FMantleLibraryStorage::SanitizeName(Case[0]);
		TestEqual(FString::Printf(TEXT("'%s' becomes a storage name"), Case[0]), Name, FString(Case[1]));
		TestTrue(TEXT("and it is legal"), FMantlePlaceLimits::IsStorageName(Name));
	}

	// A long name is cut to the limit and still legal.
	const FString Long = FMantleLibraryStorage::SanitizeName(FString::ChrN(100, TEXT('a')));
	TestEqual(TEXT("cut to 64 characters"), Long.Len(), FMantleLibraryStorage::MaxNameLength);
	TestTrue(TEXT("and legal"), FMantlePlaceLimits::IsStorageName(Long));

	// Uniqueness ignores case, so the add never offers 'Truck' beside 'truck'.
	TArray<FMantleStorageEntry> Entries;
	Entries.Add(MakeEntry(TEXT("Truck"), ItemTruck, 1));
	Entries.Add(MakeEntry(TEXT("truck_2"), ItemBus, 1));
	TestEqual(TEXT("a free name is kept"), FMantleLibraryStorage::MakeUniqueName(TEXT("Bus"), Entries), FString(TEXT("Bus")));
	TestEqual(TEXT("a taken name gets a number, skipping 'truck_2' in another case"), FMantleLibraryStorage::MakeUniqueName(TEXT("Truck"), Entries), FString(TEXT("Truck_3")));
	const FString Full64 = FString::ChrN(64, TEXT('a'));
	Entries.Add(MakeEntry(Full64, ItemId(5), 1));
	const FString Suffixed = FMantleLibraryStorage::MakeUniqueName(Full64, Entries);
	TestEqual(TEXT("a numbered name stays within 64 characters"), Suffixed.Len(), 64);
	TestTrue(TEXT("and is legal"), FMantlePlaceLimits::IsStorageName(Suffixed));
	TestFalse(TEXT("and differs from the taken one"), Suffixed.Equals(Full64, ESearchCase::IgnoreCase));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryStorageAddTest,
	"Mantle.Library.Storage.AddNamesTheItemAndPinsTheVersion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryStorageAddTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	TArray<FMantleStorageEntry> Entries;
	const FMantleLibraryAddPlan First = FMantleLibraryStorage::PlanAdd(Entries, ItemTruck, TEXT("Sports Car"), 3, TEXT("active"));
	if (!TestTrue(TEXT("an item is added to an empty place"), First.Result == EMantleLibraryAdd::Added))
	{
		AddError(First.Message);
		return false;
	}
	TestEqual(TEXT("under its sanitized name"), First.Entry.Name, FString(TEXT("Sports_Car")));
	TestEqual(TEXT("its id"), First.Entry.Item, FString(ItemTruck));
	TestEqual(TEXT("pinned at the given version"), First.Entry.Version, 3);
	Entries.Add(First.Entry);

	// A different item with the same name gets a numbered one, and the result is a valid storage.
	const FMantleLibraryAddPlan Second = FMantleLibraryStorage::PlanAdd(Entries, ItemBus, TEXT("Sports Car"), 1, TEXT("active"));
	if (TestTrue(TEXT("another item is added"), Second.Result == EMantleLibraryAdd::Added))
	{
		TestEqual(TEXT("under a numbered name"), Second.Entry.Name, FString(TEXT("Sports_Car_2")));
		Entries.Add(Second.Entry);
		TestEqual(TEXT("and the storage passes the place's rules"), FMantleStorage::CheckEntries(Entries).Num(), 0);
	}

	// A bad id or version is refused before anything is stored.
	TestTrue(TEXT("an id that is not an item id is refused"),
		FMantleLibraryStorage::PlanAdd(Entries, TEXT("itm_uABC"), TEXT("X"), 1, TEXT("active")).Result == EMantleLibraryAdd::Invalid);
	TestTrue(TEXT("an item with no version is refused"),
		FMantleLibraryStorage::PlanAdd(Entries, ItemId(9), TEXT("X"), 0, TEXT("active")).Result == EMantleLibraryAdd::Invalid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryStorageSecondVersionTest,
	"Mantle.Library.Storage.RefusesASecondVersionOfAnItem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryStorageSecondVersionTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	TArray<FMantleStorageEntry> Entries;
	Entries.Add(MakeEntry(TEXT("Truck"), ItemTruck, 1));

	const FMantleLibraryAddPlan Newer = FMantleLibraryStorage::PlanAdd(Entries, ItemTruck, TEXT("Truck"), 2, TEXT("active"));
	TestTrue(TEXT("a newer version of a stored item is refused"), Newer.Result == EMantleLibraryAdd::AlreadyStored);
	TestEqual(TEXT("and names the entry"), Newer.ExistingIndex, 0);
	TestTrue(TEXT("and offers the update"), Newer.Message.Contains(TEXT("Update to v2")));

	const FMantleLibraryAddPlan Same = FMantleLibraryStorage::PlanAdd(Entries, ItemTruck, TEXT("Truck"), 1, TEXT("active"));
	TestTrue(TEXT("the same version is refused too"), Same.Result == EMantleLibraryAdd::AlreadyStored);
	TestFalse(TEXT("with no update offered"), Same.Message.Contains(TEXT("Update to")));

	TestEqual(TEXT("an entry below the latest version can be updated"), FMantleLibraryStorage::FindUpdatable(Entries, ItemTruck, 2), 0);
	TestEqual(TEXT("one at the latest cannot"), FMantleLibraryStorage::FindUpdatable(Entries, ItemTruck, 1), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("one that is not stored cannot"), FMantleLibraryStorage::FindUpdatable(Entries, ItemBus, 5), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("ids compare exactly"), FMantleLibraryStorage::FindItem(Entries, FString(ItemTruck).ToUpper()), static_cast<int32>(INDEX_NONE));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryStorageCapTest,
	"Mantle.Library.Storage.RefusesPastThe32Cap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryStorageCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	TArray<FMantleStorageEntry> Entries;
	for (int32 Index = 1; Index <= FMantlePlaceLimits::MaxStorageEntries; ++Index)
	{
		const FMantleLibraryAddPlan Plan = FMantleLibraryStorage::PlanAdd(Entries, ItemId(Index), TEXT("Car"), 1, TEXT("active"));
		if (!TestTrue(FString::Printf(TEXT("item %d fits"), Index), Plan.Result == EMantleLibraryAdd::Added))
		{
			AddError(Plan.Message);
			return false;
		}
		Entries.Add(Plan.Entry);
	}
	TestEqual(TEXT("the place holds the most it can"), Entries.Num(), FMantlePlaceLimits::MaxStorageEntries);
	TestEqual(TEXT("and passes the place's rules"), FMantleStorage::CheckEntries(Entries).Num(), 0);

	const FMantleLibraryAddPlan Over = FMantleLibraryStorage::PlanAdd(Entries, ItemId(100), TEXT("Car"), 1, TEXT("active"));
	TestTrue(TEXT("one more is refused as full"), Over.Result == EMantleLibraryAdd::Full);
	TestTrue(TEXT("with the cap in the message"), Over.Message.Contains(FString::FromInt(FMantlePlaceLimits::MaxStorageEntries)));

	// An item already stored is reported as stored, even in a full place: that is what the creator should fix.
	const FMantleLibraryAddPlan Again = FMantleLibraryStorage::PlanAdd(Entries, ItemId(1), TEXT("Car"), 2, TEXT("active"));
	TestTrue(TEXT("a stored item is reported as stored"), Again.Result == EMantleLibraryAdd::AlreadyStored);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryStorageDelistedTest,
	"Mantle.Library.Storage.RefusesAddingADelistedItem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryStorageDelistedTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	TestTrue(TEXT("a delisted item still loads"), FMantleLibraryStorage::IsUsableStatus(TEXT("delisted")));
	TestFalse(TEXT("but may not be newly added"), FMantleLibraryStorage::IsAddableStatus(TEXT("delisted")));
	TestTrue(TEXT("an active item may be added"), FMantleLibraryStorage::IsAddableStatus(TEXT("active")));
	TestFalse(TEXT("an unknown status may not"), FMantleLibraryStorage::IsAddableStatus(TEXT("removed")));
	TestFalse(TEXT("nor an empty one"), FMantleLibraryStorage::IsAddableStatus(FString()));

	// A new add of a delisted item is refused, and says why.
	TArray<FMantleStorageEntry> Entries;
	const FMantleLibraryAddPlan New = FMantleLibraryStorage::PlanAdd(Entries, ItemTruck, TEXT("Truck"), 2, TEXT("delisted"));
	TestTrue(TEXT("a delisted item is not added"), New.Result == EMantleLibraryAdd::Invalid);
	TestTrue(TEXT("and the message says delisted"), New.Message.Contains(TEXT("delisted")));
	TestFalse(TEXT("with no entry to append"), New.Entry.Item.Equals(ItemTruck));

	// An entry already stored stays; a newer version of it is not offered as an update.
	Entries.Add(MakeEntry(TEXT("Truck"), ItemTruck, 1));
	const FMantleLibraryAddPlan Stored = FMantleLibraryStorage::PlanAdd(Entries, ItemTruck, TEXT("Truck"), 2, TEXT("delisted"));
	TestTrue(TEXT("a stored delisted item is reported as stored"), Stored.Result == EMantleLibraryAdd::AlreadyStored);
	TestFalse(TEXT("with no update offered"), Stored.Message.Contains(TEXT("Update to")));
	TestTrue(TEXT("and the reason named"), Stored.Message.Contains(TEXT("delisted")));
	const FMantleLibraryAddPlan Active = FMantleLibraryStorage::PlanAdd(Entries, ItemTruck, TEXT("Truck"), 2, TEXT("active"));
	TestTrue(TEXT("an active item still offers the update"), Active.Message.Contains(TEXT("Update to v2")));

	// A different item that is active is still added beside it.
	TestTrue(TEXT("another, active item is added"),
		FMantleLibraryStorage::PlanAdd(Entries, ItemBus, TEXT("Bus"), 1, TEXT("active")).Result == EMantleLibraryAdd::Added);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryImageUrlTest,
	"Mantle.Library.Images.FetchOnlyFromSafeAddresses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryImageUrlTest::RunTest(const FString& Parameters)
{
	FString Url;
	const FString Api = TEXT("https://api.example");

	// The API's own host, and the asset download hosts.
	TestTrue(TEXT("https on the API's own host is used as it is"), FMantleLibraryImages::ResolveUrl(TEXT("https://api.example/a.png"), Api, Url));
	TestEqual(TEXT("unchanged"), Url, FString(TEXT("https://api.example/a.png")));
	TestTrue(TEXT("an API path is joined to the API address"), FMantleLibraryImages::ResolveUrl(TEXT("/thumbs/a.png"), TEXT("https://api.example/"), Url));
	TestEqual(TEXT("without a double slash"), Url, FString(TEXT("https://api.example/thumbs/a.png")));
	TestTrue(TEXT("the asset gateway's own account is allowed"),
		FMantleLibraryImages::ResolveUrl(TEXT("https://mantle-edge-staging.mantle-edge-gateway.workers.dev/a/b.png?e=1&s=2"), Api, Url));
	TestTrue(TEXT("a public R2 host is allowed"), FMantleLibraryImages::ResolveUrl(TEXT("https://pub-0123abc.r2.dev/a.png"), Api, Url));
	TestTrue(TEXT("the host is compared ignoring case"), FMantleLibraryImages::IsAllowedUrl(TEXT("HTTPS://PUB-0123ABC.R2.DEV/a.png"), Api));

	// Development API over http.
	TestTrue(TEXT("http on the API's own address is a development API"), FMantleLibraryImages::ResolveUrl(TEXT("http://127.0.0.1:8080/t.png"), TEXT("http://127.0.0.1:8080"), Url));
	TestFalse(TEXT("http anywhere else is refused"), FMantleLibraryImages::ResolveUrl(TEXT("http://evil.example/t.png"), Api, Url));
	TestFalse(TEXT("http on the API's host is refused when the API itself is https"), FMantleLibraryImages::IsAllowedUrl(TEXT("http://api.example/t.png"), Api));
	TestFalse(TEXT("http on an allowed asset host is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("http://pub-0123abc.r2.dev/a.png"), Api));
	TestFalse(TEXT("a port that only starts like the API's is refused"), FMantleLibraryImages::ResolveUrl(TEXT("http://127.0.0.1:80801/t.png"), TEXT("http://127.0.0.1:8080"), Url));
	TestFalse(TEXT("another port on the API's host is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("http://127.0.0.1:9000/t.png"), TEXT("http://127.0.0.1:8080")));

	// Hosts that are not ours.
	TestFalse(TEXT("an https host nobody named is refused"), FMantleLibraryImages::ResolveUrl(TEXT("https://cdn.example/a.png"), Api, Url));
	TestFalse(TEXT("a host that only ends like an allowed one is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://evilr2.dev/a.png"), Api));
	TestFalse(TEXT("another account's workers.dev host is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://x.someone-else.workers.dev/a.png"), Api));
	TestFalse(TEXT("the bare suffix is not a host"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://.r2.dev/a.png"), Api));
	TestFalse(TEXT("an allowed host with another port is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://pub-0123abc.r2.dev:8443/a.png"), Api));
	TestFalse(TEXT("an allowed name inside user-info is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://pub-0123abc.r2.dev@evil.example/a.png"), Api));
	TestFalse(TEXT("an allowed name as a path is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://evil.example/pub-0123abc.r2.dev/a.png"), Api));
	TestFalse(TEXT("a backslash is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://evil.example\\@pub-0123abc.r2.dev/a.png"), Api));
	TestFalse(TEXT("a protocol-relative address is refused"), FMantleLibraryImages::ResolveUrl(TEXT("//evil.example/t.png"), Api, Url));
	TestFalse(TEXT("a file address is refused"), FMantleLibraryImages::ResolveUrl(TEXT("file:///C:/secret.png"), Api, Url));
	TestFalse(TEXT("an empty address is refused"), FMantleLibraryImages::ResolveUrl(TEXT("  "), Api, Url));
	TestFalse(TEXT("an API path with no API address is refused"), FMantleLibraryImages::ResolveUrl(TEXT("/thumbs/a.png"), FString(), Url));

	// Extra hosts a deployment names.
	const TArray<FString> Extra = { TEXT("assets.mantle.example"), TEXT("*.cdn.mantle.example") };
	TestTrue(TEXT("a named host is allowed"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://assets.mantle.example/a.png"), Api, Extra));
	TestTrue(TEXT("a named suffix allows its subdomains"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://eu.cdn.mantle.example/a.png"), Api, Extra));
	TestFalse(TEXT("but not the suffix itself"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://cdn.mantle.example/a.png"), Api, Extra));
	TestFalse(TEXT("a named host is exact"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://x.assets.mantle.example/a.png"), Api, Extra));
	TestFalse(TEXT("and nothing is allowed without naming it"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://assets.mantle.example/a.png"), Api));

	// What a redirect may end at is judged by the same rule.
	TestFalse(TEXT("a redirect to another host is refused"), FMantleLibraryImages::IsAllowedUrl(TEXT("https://evil.example/a.png"), Api));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryImageDimensionsTest,
	"Mantle.Library.Images.RefusesOversizedDimensionsFromTheHeader",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryImageDimensionsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a card-sized image is accepted"), FMantleLibraryImages::AreDimensionsAcceptable(640, 480));
	TestTrue(TEXT("one pixel is accepted"), FMantleLibraryImages::AreDimensionsAcceptable(1, 1));
	TestTrue(TEXT("the largest side is accepted"), FMantleLibraryImages::AreDimensionsAcceptable(FMantleLibraryImages::MaxSide, FMantleLibraryImages::MaxSide));
	TestFalse(TEXT("a width over the limit is refused"), FMantleLibraryImages::AreDimensionsAcceptable(FMantleLibraryImages::MaxSide + 1, 100));
	TestFalse(TEXT("a height over the limit is refused"), FMantleLibraryImages::AreDimensionsAcceptable(100, FMantleLibraryImages::MaxSide + 1));
	TestFalse(TEXT("a header that claims a huge image is refused"), FMantleLibraryImages::AreDimensionsAcceptable(65535, 65535));
	TestFalse(TEXT("a header beyond 32 bits is refused"), FMantleLibraryImages::AreDimensionsAcceptable(int64(1) << 33, 1));
	TestFalse(TEXT("zero is refused"), FMantleLibraryImages::AreDimensionsAcceptable(0, 100));
	TestFalse(TEXT("a negative size is refused"), FMantleLibraryImages::AreDimensionsAcceptable(100, -1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibrarySpawnLabelTest,
	"Mantle.Library.SpawnPoint.LabelNamesTheStoredItem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibrarySpawnLabelTest::RunTest(const FString& Parameters)
{
	AMantleVehicleSpawnPoint* Spawn = NewObject<AMantleVehicleSpawnPoint>(GetTransientPackage());
	if (!TestNotNull(TEXT("a spawn point"), Spawn))
	{
		return false;
	}
	TestTrue(TEXT("a spawn with no storage item has no label"), Spawn->MakeEditorLabelText().IsEmpty());
	Spawn->StorageItem = TEXT("Truck");
	const FString Label = Spawn->MakeEditorLabelText();
	TestTrue(TEXT("the label names the storage item"), Label.Contains(TEXT("\"Truck\"")));
	TestTrue(TEXT("and says it is a stored vehicle"), Label.Contains(TEXT("stored vehicle")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryTakenDownStatusTest,
	"Mantle.Library.Storage.ATakenDownItemCannotBeAddedOrUpdatedButStaysVisible",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryTakenDownStatusTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryTestPrivate;
	const FString TakenDown(TEXT("taken_down"));
	TestTrue(TEXT("taken_down is the taken-down status"), FMantleLibraryStorage::IsTakenDownStatus(TakenDown));
	TestFalse(TEXT("active is not"), FMantleLibraryStorage::IsTakenDownStatus(TEXT("active")));
	TestFalse(TEXT("delisted is not"), FMantleLibraryStorage::IsTakenDownStatus(TEXT("delisted")));
	TestFalse(TEXT("the status is case sensitive"), FMantleLibraryStorage::IsTakenDownStatus(TEXT("Taken_Down")));
	TestFalse(TEXT("a taken-down item does not load, so it is not usable"), FMantleLibraryStorage::IsUsableStatus(TakenDown));
	TestFalse(TEXT("and may not be added or re-pinned"), FMantleLibraryStorage::IsAddableStatus(TakenDown));

	// A new add is refused, with the taken-down note.
	const FMantleLibraryAddPlan Add = FMantleLibraryStorage::PlanAdd(TArray<FMantleStorageEntry>(), ItemTruck, TEXT("Truck"), 2, TakenDown);
	TestTrue(TEXT("a taken-down item is not added"), Add.Result != EMantleLibraryAdd::Added);
	TestTrue(TEXT("the reason says it was taken down"), Add.Message.Contains(TEXT("taken down")));
	TestTrue(TEXT("and carries the panel's note"), Add.Message.Contains(FMantleLibraryStorage::TakenDownNote()));

	// An entry already stored stays, and offers no update, only the note and a way out.
	TArray<FMantleStorageEntry> Stored;
	Stored.Add(MakeEntry(TEXT("Truck"), ItemTruck, 1));
	const FMantleLibraryAddPlan Again = FMantleLibraryStorage::PlanAdd(Stored, ItemTruck, TEXT("Truck"), 2, TakenDown);
	TestTrue(TEXT("a stored taken-down item is reported as already stored"), Again.Result == EMantleLibraryAdd::AlreadyStored);
	TestTrue(TEXT("its entry is found"), Again.ExistingIndex == 0);
	TestTrue(TEXT("the message says it was taken down"), Again.Message.Contains(TEXT("taken down")));
	TestFalse(TEXT("and does not offer Update"), Again.Message.Contains(TEXT("use Update")));
	TestTrue(TEXT("it points at removing it"), Again.Message.Contains(TEXT("Remove it from storage")));
	TestEqual(TEXT("planning does not change the stored entry"), Stored[0].Version, 1);

	// Active and delisted keep their rules.
	TestTrue(TEXT("an active item is still added"), FMantleLibraryStorage::PlanAdd(TArray<FMantleStorageEntry>(), ItemTruck, TEXT("Truck"), 2, TEXT("active")).Result == EMantleLibraryAdd::Added);
	TestTrue(TEXT("a delisted item is still refused as delisted"),
		FMantleLibraryStorage::PlanAdd(TArray<FMantleStorageEntry>(), ItemTruck, TEXT("Truck"), 2, TEXT("delisted")).Message.Contains(TEXT("delisted")));

	// A card with the status reads from the API.
	TArray<FMantleLibraryCard> Cards;
	FString Next;
	FString Error;
	const FString Body = Json(TEXT("{'items':[{'id':'itm_u0123456789abcdef0123456789abcdef','kind':'vehicle','name':'Truck','status':'taken_down','latestVersion':2,'owned':true}]}"));
	if (TestTrue(TEXT("a taken-down card is read"), FMantleApiClient::ParseLibraryCards(Body, Cards, Next, Error)) && TestEqual(TEXT("one card"), Cards.Num(), 1))
	{
		TestEqual(TEXT("its status is kept"), Cards[0].Status, TakenDown);
		TestTrue(TEXT("the panel treats it as taken down"), FMantleLibraryStorage::IsTakenDownStatus(Cards[0].Status));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
