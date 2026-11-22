// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCells.h"
#include "MantleCheck.h"
#include "MantlePlaceExporter.h"
#include "MantlePlacePublishSession.h"
#include "MantleServerMemoryEstimate.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// MantleCore#189, the server plan's section 6: a place's estimated server memory, in Check and the publish window. The backend's
// TestEstimateServerMemoryFromKnownCounts holds the same counts to the same numbers.

namespace MantleServerEstimateCheckTestPrivate
{
	FMantleEntity MeshEntity(const TCHAR* Id, const EMantleCollision Collision)
	{
		FMantleStaticMeshProps Mesh;
		Mesh.Mesh = TEXT("ast_cube");
		Mesh.Collision = Collision;
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Type = EMantleEntityType::StaticMesh;
		Entity.Props.Set<FMantleStaticMeshProps>(Mesh);
		return Entity;
	}

	FMantleEntity SetEntity(const TCHAR* Id, const EMantleCollision Collision, const int32 Instances)
	{
		FMantleInstancedMeshProps Set;
		Set.Mesh.Mesh = TEXT("ast_cube");
		Set.Mesh.Collision = Collision;
		Set.Instances.Init(FTransform::Identity, Instances);
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Type = EMantleEntityType::InstancedMesh;
		Entity.Props.Set<FMantleInstancedMeshProps>(Set);
		return Entity;
	}

	FMantleAssetRef Asset(const TCHAR* Id, const EMantleAssetKind Kind, const int64 Bytes)
	{
		FMantleAssetRef Ref;
		Ref.Id = Id;
		Ref.Kind = Kind;
		Ref.Bytes = Bytes;
		return Ref;
	}

	// The calibration places' counts, as their servers logged them on the Linux builder (the server plan's 6.4, 2026-11-22). Measured: City Sample
	// 5,616 MB once ready and 6,132 MB at its peak, OrbitalBase 1,092 and 1,123.
	FMantleServerMemoryEstimate CitySample()
	{
		FMantleServerMemoryCounts Counts;
		Counts.Entities = 368619;
		Counts.Instances = 8800044;
		Counts.Solid = 1047579;
		Counts.Meshes = 11741;
		Counts.MeshBytes = 873560000;
		return FMantleServerMemoryEstimate::Of(Counts);
	}

	FMantleServerMemoryEstimate OrbitalBase()
	{
		FMantleServerMemoryCounts Counts;
		Counts.Entities = 5443;
		Counts.Instances = 76505;
		Counts.Solid = 27137;
		Counts.Meshes = 418;
		Counts.MeshBytes = 217700000;
		Counts.TerrainSamples = 4068289;
		return FMantleServerMemoryEstimate::Of(Counts);
	}

	FMantleServerMemoryEstimate TooBig()
	{
		FMantleServerMemoryCounts Counts;
		Counts.Entities = 1000000;
		Counts.Instances = 20000000;
		Counts.Solid = 5000000;
		Counts.Meshes = 20000;
		Counts.MeshBytes = 2000000000;
		return FMantleServerMemoryEstimate::Of(Counts);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleServerEstimateKnownCountsTest,
	"Mantle.Check.ServerEstimate.KnownCounts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleServerEstimateKnownCountsTest::RunTest(const FString& Parameters)
{
	using namespace MantleServerEstimateCheckTestPrivate;

	const FMantleServerMemoryEstimate Empty = FMantleServerMemoryEstimate::Of(FMantleServerMemoryCounts());
	TestEqual(TEXT("an empty place is the empty server"), Empty.ReadyMB, FMantleServerMemoryEstimate::BaseMB);
	TestEqual(TEXT("and loads nothing more"), Empty.PeakMB, FMantleServerMemoryEstimate::BaseMB);

	const FMantleServerMemoryEstimate City = CitySample();
	TestEqual(TEXT("City Sample's counts, ready"), City.ReadyMB, 6326.447, 0.001);
	TestEqual(TEXT("City Sample's counts, at the load's peak"), City.PeakMB, 7046.406, 0.001);
	TestTrue(TEXT("inside the biggest hosted server's world share (6 GB once ready, 7 GB at the peak)"), City.bFitsHosted);
	TestEqual(TEXT("Check's line"), FMantleCheck::ServerLine(City),
		FString(TEXT("MANTLE_CHECK_SERVER world_gb=6.18 peak_gb=6.88 fits=hosted entities=368619 instances=8800044 solid=1047579 meshes=11741 meshBytes=873560000 terrainSamples=0")));
	TestEqual(TEXT("the creator's sentence"), City.Describe(),
		FString(TEXT("Estimated server memory: 6.2 GB (6.9 GB while loading). Fits a hosted server.")));

	const FMantleServerMemoryEstimate Orbital = OrbitalBase();
	TestEqual(TEXT("OrbitalBase's counts with its terrain, ready"), Orbital.ReadyMB, 942.237, 0.001);
	TestEqual(TEXT("and at the peak"), Orbital.PeakMB, 952.868, 0.001);
	TestTrue(TEXT("fits a hosted server"), Orbital.bFitsHosted);
	TestEqual(TEXT("the creator's sentence"), Orbital.Describe(),
		FString(TEXT("Estimated server memory: 0.9 GB (0.9 GB while loading). Fits a hosted server.")));

	const FMantleServerMemoryEstimate Big = TooBig();
	TestEqual(TEXT("a place too big for hosting, ready"), Big.ReadyMB, 21113.918, 0.001);
	TestEqual(TEXT("and at the peak"), Big.PeakMB, 23067.043, 0.001);
	TestFalse(TEXT("is community only"), Big.bFitsHosted);
	TestTrue(TEXT("Check's line says so"), FMantleCheck::ServerLine(Big).Contains(TEXT(" fits=community ")));
	TestTrue(TEXT("and the sentence names community servers"), Big.Describe().Contains(TEXT("will only run on community servers")));
	TestTrue(TEXT("and the biggest hosted server"), Big.Describe().Contains(TEXT("(8 GB)")));

	// The peak alone over the hosted share is enough.
	FMantleServerMemoryCounts PeakOnly;
	PeakOnly.Entities = 1800000;
	const FMantleServerMemoryEstimate Peak = FMantleServerMemoryEstimate::Of(PeakOnly);
	TestTrue(TEXT("ready inside the hosted share"), Peak.ReadyMB - FMantleServerMemoryEstimate::BaseMB <= FMantleServerMemoryEstimate::HostedWorldReadyMB);
	TestFalse(TEXT("but the peak over it"), Peak.bFitsHosted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleServerEstimateCountsTest,
	"Mantle.Check.ServerEstimate.CountsRootCellsAndManifest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleServerEstimateCountsTest::RunTest(const FString& Parameters)
{
	using namespace MantleServerEstimateCheckTestPrivate;

	FMantleExportResult Exported;
	FMantleEntity Group;
	Group.Id = TEXT("ent_root");
	Group.Type = EMantleEntityType::Group;
	Exported.Place.Entities.Add(Group);
	Exported.Place.Entities.Add(MeshEntity(TEXT("ent_wall"), EMantleCollision::Blocking));
	Exported.Place.Entities.Add(SetEntity(TEXT("ent_rocks"), EMantleCollision::Blocking, 3));
	Exported.Place.Entities.Add(SetEntity(TEXT("ent_grass"), EMantleCollision::None, 5));

	FMantlePlannedCell& Cell = Exported.Cells.AddDefaulted_GetRef();
	Cell.Entities.Add(MeshEntity(TEXT("ent_sign"), EMantleCollision::None));
	Cell.Entities.Add(SetEntity(TEXT("ent_trigger_posts"), EMantleCollision::Overlap, 2));

	Exported.Place.Assets.Add(Asset(TEXT("ast_cube"), EMantleAssetKind::StaticMesh, 10));
	Exported.Place.Assets.Add(Asset(TEXT("ast_cube"), EMantleAssetKind::StaticMesh, 10));
	Exported.Place.Assets.Add(Asset(TEXT("ast_u0a0b0c0d0e0f00010203040506070809"), EMantleAssetKind::StaticMesh, 10));
	Exported.Place.Assets.Add(Asset(TEXT("ast_u1a1b1c1d1e1f10111213141516171819"), EMantleAssetKind::Heightfield, 36 + 2 * 100));
	Exported.Place.Assets.Add(Asset(TEXT("ast_u2a2b2c2d2e2f20212223242526272829"), EMantleAssetKind::Heightfield, 36));
	Exported.Place.Assets.Add(Asset(TEXT("ast_mat_basic"), EMantleAssetKind::Material, 10));

	const FMantleServerMemoryEstimate Estimate = FMantleCheck::ServerEstimate(Exported, nullptr);
	TestEqual(TEXT("entities, root and cells, an instance set once"), Estimate.Counts.Entities, static_cast<int64>(6));
	TestEqual(TEXT("instances, with collision or without"), Estimate.Counts.Instances, static_cast<int64>(10));
	TestEqual(TEXT("solid: the wall, the rocks and the overlap posts, as a cell index counts it"), Estimate.Counts.Solid, static_cast<int64>(6));
	TestEqual(TEXT("each mesh once"), Estimate.Counts.Meshes, static_cast<int64>(2));
	TestEqual(TEXT("and its bytes once"), Estimate.Counts.MeshBytes, static_cast<int64>(20));
	TestEqual(TEXT("the heightfield's samples; one of unknown size counts nothing"), Estimate.Counts.TerrainSamples, static_cast<int64>(100));
	TestEqual(TEXT("the estimate is the formula's on those counts"), Estimate.ReadyMB, FMantleServerMemoryEstimate::Of(Estimate.Counts).ReadyMB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleServerEstimateWarnsOnlyTest,
	"Mantle.Check.ServerEstimate.TooBigIsAWarningOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleServerEstimateWarnsOnlyTest::RunTest(const FString& Parameters)
{
	using namespace MantleServerEstimateCheckTestPrivate;

	FMantleCheckReport Fits;
	FMantleCheck::AddServerWarning(OrbitalBase(), Fits.Issues);
	TestEqual(TEXT("a place that fits gets no finding"), Fits.Issues.Num(), 0);

	FMantleCheckReport Big;
	FMantleCheck::AddServerWarning(TooBig(), Big.Issues);
	if (TestEqual(TEXT("one that does not gets one finding"), Big.Issues.Num(), 1))
	{
		TestEqual(TEXT("a warning"), static_cast<int32>(Big.Issues[0].Severity), static_cast<int32>(EMantleIssueSeverity::Warning));
		TestTrue(TEXT("saying community servers"), Big.Issues[0].Problem.Contains(TEXT("community servers")));
		TestTrue(TEXT("and that it still publishes"), Big.Issues[0].Remedy.Contains(TEXT("still publishes")));
	}
	TestTrue(TEXT("Check still calls the place ready"), Big.IsReady());
	return true;
}

namespace MantleServerEstimateCheckTestPrivate
{
	class FNoCallsTransport : public IMantleUploadTransport
	{
	public:
		int32 Calls = 0;
		virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) override { ++Calls; }
	};

	/** A level that fits every size limit but is too big for Mantle's hosted servers. */
	class FCommunityOnlySource : public IMantlePlacePublishSource
	{
	public:
		int32 ConvertCalls = 0;
		int32 ExportCalls = 0;

		virtual FString GetLevelName() const override { return TEXT("BigCity"); }
		virtual bool Prepare(FMantleValidationResult& OutValidation, FString& OutMessage) override { return true; }
		virtual bool CheckPlaceSize(FMantlePlaceSizeCheck& OutCheck) override
		{
			OutCheck.Bytes = 1024 * 1024;
			OutCheck.WorstBytes = 1024 * 1024;
			OutCheck.ServerEstimate = TooBig();
			OutCheck.bHasServerEstimate = true;
			return true;
		}
		virtual bool CheckConvertedPlaceSize(const FMantlePlaceAssetSet& Set, FMantlePlaceSizeCheck& OutCheck) override
		{
			OutCheck.Bytes = 1024 * 1024;
			return true;
		}
		virtual int32 NumConversionSteps() const override { return 1; }
		virtual FString ConversionStepName(int32 Step) const override { return TEXT("Step"); }
		virtual void Convert(int32 Step, FMantlePlaceAssetSet& Set) override { ++ConvertCalls; }
		virtual bool CollectScripts(TArray<FMantlePlaceScriptUpload>& OutScripts, FString& OutMessage) override { return true; }
		virtual EMantlePlaceExportOutcome Export(const FMantlePlaceAssetSet& Set, const TMap<FString, FString>& ScriptSourceIds,
			const TMap<FString, FString>& ScriptAuthorSourceIds, FString& OutPlaceJson, TArray<FMantleValidationIssue>& OutIssues) override
		{
			++ExportCalls;
			OutPlaceJson = TEXT("{}");
			return EMantlePlaceExportOutcome::Exported;
		}
		virtual FString FindPlaceId() override { return TEXT("plc_00000000000000000000000000000001"); }
		virtual void RememberPlaceId(const FString& PlaceId) override {}
		virtual void ForgetPlaceId() override {}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleServerEstimatePublishGoesOnTest,
	"Mantle.Check.ServerEstimate.PublishGoesOnWhenOnlyCommunityServersFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleServerEstimatePublishGoesOnTest::RunTest(const FString& Parameters)
{
	using namespace MantleServerEstimateCheckTestPrivate;

	double Now = 1000.0;
	const TSharedRef<FCommunityOnlySource> Source = MakeShared<FCommunityOnlySource>();
	const TSharedRef<FNoCallsTransport> Transport = MakeShared<FNoCallsTransport>();
	const TSharedRef<FMantlePlacePublishSession> Session = MakeShared<FMantlePlacePublishSession>(
		Source, Transport, /*bGoLive*/ true, FString(), [&Now] { return Now; });
	Session->Start();
	for (int32 Tick = 0; Tick < 20 && Session->GetPhase() != EMantlePublishPhase::Failed && Session->GetPhase() != EMantlePublishPhase::Done; ++Tick)
	{
		Now += 1.0;
		Session->Tick();
	}

	TestNotEqual(TEXT("the publish did not stop"), static_cast<int32>(Session->GetPhase()), static_cast<int32>(EMantlePublishPhase::Failed));
	TestEqual(TEXT("its files were prepared"), Source->ConvertCalls, 1);
	const FMantlePublishResult& Result = Session->GetResult();
	TestTrue(TEXT("the window shows the estimate"), Result.ServerMemory.StartsWith(TEXT("Estimated server memory: 20.6 GB (22.5 GB while loading).")));
	TestTrue(TEXT("and that only community servers hold it"), Result.ServerMemory.Contains(TEXT("community servers")));
	TestFalse(TEXT("no error"), Result.Validation.HasErrors());
	TestTrue(TEXT("one warning says so"), Result.Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("community servers"));
		}));
	AddInfo(FString::Printf(TEXT("phase %d, %d export(s), %d call(s) to Mantle"), static_cast<int32>(Session->GetPhase()), Source->ExportCalls, Transport->Calls));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
