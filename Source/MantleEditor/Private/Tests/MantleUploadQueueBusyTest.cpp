// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetUploadQueue.h"
#include "MantleApiClient.h"
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticUpload.h"
#include "MantleSha256.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Mantle's "busy with other uploads" answer: POST /assets answers 503 upload_busy with a
// Retry-After when it is already taking its limit of large uploads. The upload queue waits that long
// for the one file, without spending one of its attempts and without holding up the others; the
// cosmetic upload honours the Retry-After. Pure: a scripted transport and a fake clock, no network.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUploadQueueBusyTestPrivate
{
	struct FSentCall
	{
		EMantleUploadCall Call = EMantleUploadCall::DiffAssets;
		/** First byte of an upload's body, which names the file in these tests. */
		uint8 First = 0;
		double At = 0.0;
	};

	/** Answers when the test says so (Flush), at the test's clock; records every call with its time. */
	class FDeferredTransport : public IMantleUploadTransport
	{
	public:
		TFunction<FMantleApiResult(const FMantleUploadCallRequest&)> Answer;
		const double* Now = nullptr;
		TArray<FSentCall> Calls;

		virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) override
		{
			FSentCall& Call = Calls.AddDefaulted_GetRef();
			Call.Call = Request.Call;
			Call.First = Request.Bytes && Request.Bytes->Num() > 0 ? (*Request.Bytes)[0] : 0;
			Call.At = Now ? *Now : 0.0;
			Pending.Add({ Request, OnDone });
		}

		/** Answers everything sent, and what the answers send in turn, at the current time. */
		void Flush()
		{
			while (Pending.Num() > 0)
			{
				TArray<FPending> Batch = MoveTemp(Pending);
				Pending.Reset();
				for (FPending& Item : Batch)
				{
					Item.Done.ExecuteIfBound(Answer(Item.Request));
				}
			}
		}

		TArray<double> UploadTimes(uint8 First) const
		{
			TArray<double> Times;
			for (const FSentCall& Call : Calls)
			{
				if (Call.Call == EMantleUploadCall::UploadAsset && Call.First == First)
				{
					Times.Add(Call.At);
				}
			}
			return Times;
		}

	private:
		struct FPending
		{
			FMantleUploadCallRequest Request;
			FMantleApiCallback Done;
		};
		TArray<FPending> Pending;
	};

	FMantleApiResult Ok(const FString& Body)
	{
		FMantleApiResult Result;
		Result.bSuccess = true;
		Result.StatusCode = 200;
		Result.Body = Body;
		return Result;
	}

	FMantleApiResult Refused(int32 Status, const FString& Code, float RetryAfter)
	{
		FMantleApiResult Result;
		Result.StatusCode = Status;
		Result.Code = Code;
		Result.RetryAfterSeconds = RetryAfter;
		Result.Message = TEXT("Mantle is receiving other large uploads right now.");
		return Result;
	}

	FMantleApiResult DiffEverythingUnknown(const FMantleUploadCallRequest& Request)
	{
		FString Unknown;
		for (const FString& Hash : Request.Hashes)
		{
			Unknown += FString::Printf(TEXT("%s\"%s\""), Unknown.IsEmpty() ? TEXT("") : TEXT(","), *Hash);
		}
		return Ok(FString::Printf(TEXT("{\"known\":[],\"unknown\":[%s]}"), *Unknown));
	}

	/** An approved asset, as POST /assets answers a new upload. */
	FMantleApiResult UploadedApproved(const FMantleUploadCallRequest& Request)
	{
		const FString Hash = FMantleSha256::AssetHash(*Request.Bytes);
		const FString Id = FString::Printf(TEXT("ast_u%032x"), static_cast<int32>((*Request.Bytes)[0]));
		return Ok(FString::Printf(TEXT("{\"id\":\"%s\",\"hash\":\"%s\",\"kind\":\"%s\",\"status\":\"approved\",\"cookState\":\"passed\",")
			TEXT("\"moderationState\":\"passed\",\"created\":true}"), *Id, *Hash, *Request.Options.Kind));
	}

	FMantleQueuedUpload MakeUpload(const FString& Key, uint8 First)
	{
		FMantleQueuedUpload Upload;
		Upload.Key = Key;
		Upload.Name = Key;
		Upload.Options.Kind = TEXT("mesh");
		Upload.Bytes = { First, 7, 7, 7, 7 };
		return Upload;
	}

	struct FRig
	{
		double Now = 1000.0;
		TSharedRef<FDeferredTransport> Transport = MakeShared<FDeferredTransport>();
		TSharedRef<FMantleAssetUploadQueue> Queue;

		FRig()
			: Queue(MakeShared<FMantleAssetUploadQueue>(Transport, [this] { return Now; }))
		{
			Transport->Now = &Now;
			Queue->Settings.BusyJitter = 0.0f;
		}

		/** Ticks and answers, a step of the clock at a time, until the run ends or the steps run out. */
		void Run(float Step = 0.5f, int32 MaxSteps = 2000)
		{
			for (int32 Index = 0; Index < MaxSteps && Queue->IsRunning(); ++Index)
			{
				Queue->Tick();
				Transport->Flush();
				Now += Step;
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUploadQueueBusyWaitsWithoutAttemptsTest,
	"Mantle.Editor.UploadQueue.BusyWaitsWithoutAttempts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUploadQueueBusyWaitsWithoutAttemptsTest::RunTest(const FString& Parameters)
{
	using namespace MantleUploadQueueBusyTestPrivate;
	FRig Rig;
	int32 BusyAnswersLeft = 3;
	Rig.Transport->Answer = [&BusyAnswersLeft](const FMantleUploadCallRequest& Request)
	{
		if (Request.Call == EMantleUploadCall::DiffAssets)
		{
			return DiffEverythingUnknown(Request);
		}
		if ((*Request.Bytes)[0] == 1 && BusyAnswersLeft > 0)
		{
			--BusyAnswersLeft;
			return Refused(503, TEXT("upload_busy"), 5.0f);
		}
		return UploadedApproved(Request);
	};
	Rig.Queue->Start({ MakeUpload(TEXT("a"), 1), MakeUpload(TEXT("b"), 2) });
	Rig.Run();

	TestEqual(TEXT("the run completes"), static_cast<int32>(Rig.Queue->GetOutcome()), static_cast<int32>(EMantleUploadQueueOutcome::Completed));
	const FMantleQueuedUploadStatus* A = Rig.Queue->FindStatus(TEXT("a"));
	const FMantleQueuedUploadStatus* B = Rig.Queue->FindStatus(TEXT("b"));
	if (!TestNotNull(TEXT("a's status"), A) || !TestNotNull(TEXT("b's status"), B))
	{
		return false;
	}
	TestEqual(TEXT("a is approved"), static_cast<int32>(A->State), static_cast<int32>(EMantleQueuedUploadState::Approved));
	TestEqual(TEXT("b is approved"), static_cast<int32>(B->State), static_cast<int32>(EMantleQueuedUploadState::Approved));
	TestEqual(TEXT("three busy answers spent no attempt"), A->Attempts, 0);

	const TArray<double> TimesA = Rig.Transport->UploadTimes(1);
	const TArray<double> TimesB = Rig.Transport->UploadTimes(2);
	TestEqual(TEXT("a was sent four times: three turned away, then taken"), TimesA.Num(), 4);
	TestEqual(TEXT("b was sent once: a's wait held nobody else up"), TimesB.Num(), 1);
	for (int32 Index = 1; Index < TimesA.Num(); ++Index)
	{
		const double Gap = TimesA[Index] - TimesA[Index - 1];
		TestTrue(*FString::Printf(TEXT("a waited the server's 5 s before send %d (waited %.1f)"), Index + 1, Gap), Gap >= 5.0 && Gap < 5.6);
	}
	TestTrue(TEXT("b went out before a's second try, in the same breath as a's first"), TimesB.Num() == 1 && TimesA.Num() > 1 && TimesB[0] < TimesA[1]);
	TestEqual(TEXT("no pause on the other uploads"), Rig.Queue->GetPausedUntil(), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUploadQueueBusyWaitIsBoundedTest,
	"Mantle.Editor.UploadQueue.BusyWaitIsBounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUploadQueueBusyWaitIsBoundedTest::RunTest(const FString& Parameters)
{
	using namespace MantleUploadQueueBusyTestPrivate;
	FRig Rig;
	Rig.Queue->Settings.MaxBusyWait = 20.0f;
	Rig.Queue->Settings.RetryDelays = { 1.0f, 1.0f };
	Rig.Transport->Answer = [](const FMantleUploadCallRequest& Request)
	{
		return Request.Call == EMantleUploadCall::DiffAssets ? DiffEverythingUnknown(Request) : Refused(503, TEXT("upload_busy"), 5.0f);
	};
	Rig.Queue->Start({ MakeUpload(TEXT("a"), 1) });
	Rig.Run();

	// Four waits of 5 s fill the 20 s allowance; the fifth busy answer is an ordinary failure, which
	// spends the two attempts there are, and the seventh stops the run.
	TestEqual(TEXT("the run stops"), static_cast<int32>(Rig.Queue->GetOutcome()), static_cast<int32>(EMantleUploadQueueOutcome::Failed));
	TestEqual(TEXT("a was sent seven times"), Rig.Transport->UploadTimes(1).Num(), 7);
	const FMantleQueuedUploadStatus* A = Rig.Queue->FindStatus(TEXT("a"));
	if (!TestNotNull(TEXT("a's status"), A))
	{
		return false;
	}
	TestEqual(TEXT("only after the busy allowance did attempts count"), A->Attempts, 2);
	TestTrue(TEXT("the run says Mantle is having trouble"), Rig.Queue->GetFailure().Contains(TEXT("having trouble")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUploadQueueBusyIsRecognisedTest,
	"Mantle.Editor.UploadQueue.BusyIsRecognisedAndPlain503StillSpendsAnAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUploadQueueBusyIsRecognisedTest::RunTest(const FString& Parameters)
{
	using namespace MantleUploadQueueBusyTestPrivate;
	TestTrue(TEXT("upload_busy"), FMantleAssetUploadQueue::IsUploadBusy(Refused(503, TEXT("upload_busy"), 0.0f)));
	TestTrue(TEXT("upload_busy with a Retry-After"), FMantleAssetUploadQueue::IsUploadBusy(Refused(503, TEXT("upload_busy"), 5.0f)));
	TestTrue(TEXT("any 503 that sends a Retry-After"), FMantleAssetUploadQueue::IsUploadBusy(Refused(503, TEXT("server_error"), 7.0f)));
	TestFalse(TEXT("a bare 503"), FMantleAssetUploadQueue::IsUploadBusy(Refused(503, TEXT(""), 0.0f)));
	TestFalse(TEXT("a 500 with a Retry-After"), FMantleAssetUploadQueue::IsUploadBusy(Refused(500, TEXT("server_error"), 7.0f)));
	TestFalse(TEXT("the hourly limit has its own handling"), FMantleAssetUploadQueue::IsUploadBusy(Refused(429, TEXT("rate_limited"), 60.0f)));
	TestFalse(TEXT("a dropped connection"), FMantleAssetUploadQueue::IsUploadBusy(Refused(0, TEXT(""), 0.0f)));

	// A 503 that says nothing about when is an ordinary failure: it spends an attempt.
	{
		FRig Rig;
		int32 Failures = 1;
		Rig.Transport->Answer = [&Failures](const FMantleUploadCallRequest& Request)
		{
			if (Request.Call == EMantleUploadCall::DiffAssets)
			{
				return DiffEverythingUnknown(Request);
			}
			return Failures-- > 0 ? Refused(503, TEXT(""), 0.0f) : UploadedApproved(Request);
		};
		Rig.Queue->Start({ MakeUpload(TEXT("a"), 1) });
		Rig.Queue->Tick();
		Rig.Transport->Flush();
		Rig.Now += 0.5;
		Rig.Queue->Tick();
		Rig.Transport->Flush();
		const FMantleQueuedUploadStatus* A = Rig.Queue->FindStatus(TEXT("a"));
		if (!TestNotNull(TEXT("status"), A))
		{
			return false;
		}
		TestEqual(TEXT("a bare 503 spent an attempt"), A->Attempts, 1);
		Rig.Run();
		TestEqual(TEXT("and the retry still completes the run"), static_cast<int32>(Rig.Queue->GetOutcome()), static_cast<int32>(EMantleUploadQueueOutcome::Completed));
	}

	// A 503 that sends a Retry-After without the busy code is waited out the same way, free.
	{
		FRig Rig;
		int32 Failures = 2;
		Rig.Transport->Answer = [&Failures](const FMantleUploadCallRequest& Request)
		{
			if (Request.Call == EMantleUploadCall::DiffAssets)
			{
				return DiffEverythingUnknown(Request);
			}
			return Failures-- > 0 ? Refused(503, TEXT("server_error"), 3.0f) : UploadedApproved(Request);
		};
		Rig.Queue->Start({ MakeUpload(TEXT("a"), 1) });
		Rig.Run();
		const FMantleQueuedUploadStatus* A = Rig.Queue->FindStatus(TEXT("a"));
		if (!TestNotNull(TEXT("status"), A))
		{
			return false;
		}
		TestEqual(TEXT("a 503 with a Retry-After spent no attempt"), A->Attempts, 0);
		TestEqual(TEXT("the run completes"), static_cast<int32>(Rig.Queue->GetOutcome()), static_cast<int32>(EMantleUploadQueueOutcome::Completed));
		const TArray<double> Times = Rig.Transport->UploadTimes(1);
		TestEqual(TEXT("sent three times"), Times.Num(), 3);
		// The server asked for 3 s; never less than the minimum wait, and never less than it asked.
		TestTrue(TEXT("it waited what the server asked"), Times.Num() == 3 && Times[1] - Times[0] >= 3.0 && Times[2] - Times[1] >= 3.0);
	}

	// A Retry-After shorter than the minimum is raised to it.
	{
		FRig Rig;
		int32 Failures = 1;
		Rig.Transport->Answer = [&Failures](const FMantleUploadCallRequest& Request)
		{
			if (Request.Call == EMantleUploadCall::DiffAssets)
			{
				return DiffEverythingUnknown(Request);
			}
			return Failures-- > 0 ? Refused(503, TEXT("upload_busy"), 0.5f) : UploadedApproved(Request);
		};
		Rig.Queue->Start({ MakeUpload(TEXT("a"), 1) });
		Rig.Run(0.25f);
		const TArray<double> Times = Rig.Transport->UploadTimes(1);
		TestTrue(TEXT("a short Retry-After waited at least BusyMinWait (2 s)"), Times.Num() == 2 && Times[1] - Times[0] >= 2.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUploadQueueBusyJitterTest,
	"Mantle.Editor.UploadQueue.BusyJitterStaysWithinItsBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUploadQueueBusyJitterTest::RunTest(const FString& Parameters)
{
	using namespace MantleUploadQueueBusyTestPrivate;
	FRig Rig;
	Rig.Queue->Settings.BusyJitter = 1.0f;
	int32 BusyAnswersLeft = 8;
	Rig.Transport->Answer = [&BusyAnswersLeft](const FMantleUploadCallRequest& Request)
	{
		if (Request.Call == EMantleUploadCall::DiffAssets)
		{
			return DiffEverythingUnknown(Request);
		}
		return BusyAnswersLeft-- > 0 ? Refused(503, TEXT("upload_busy"), 5.0f) : UploadedApproved(Request);
	};
	Rig.Queue->Start({ MakeUpload(TEXT("a"), 1) });
	Rig.Run(0.05f, 4000);

	const TArray<double> Times = Rig.Transport->UploadTimes(1);
	TestEqual(TEXT("eight turned away, then taken"), Times.Num(), 9);
	bool bAnyJitter = false;
	for (int32 Index = 1; Index < Times.Num(); ++Index)
	{
		const double Gap = Times[Index] - Times[Index - 1];
		// 5 s from the server, up to 1 s of jitter, and a clock step of rounding.
		TestTrue(*FString::Printf(TEXT("wait %d (%.2f s) is between 5 and 6 s"), Index, Gap), Gap >= 4.999 && Gap <= 6.1);
		bAnyJitter |= Gap > 5.1;
	}
	TestTrue(TEXT("the waits are not all the same"), bAnyJitter);
	TestEqual(TEXT("no attempt was spent"), Rig.Queue->FindStatus(TEXT("a")) ? Rig.Queue->FindStatus(TEXT("a"))->Attempts : -1, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCosmeticUploadRetryAfter503Test,
	"Mantle.Editor.CosmeticUpload.RetryLaterHonoursRetryAfterFor503",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCosmeticUploadRetryAfter503Test::RunTest(const FString& Parameters)
{
	using namespace MantleUploadQueueBusyTestPrivate;

	// The first upload is answered 503 with the given Retry-After, the retry is taken; returns when
	// the retry was sent, measured from the first send.
	const auto SecondSendAfter = [](float RetryAfter) -> double
	{
		double Now = 500.0;
		const TSharedRef<FDeferredTransport> Transport = MakeShared<FDeferredTransport>();
		Transport->Now = &Now;
		int32 Failures = 1;
		Transport->Answer = [&Failures, RetryAfter](const FMantleUploadCallRequest& Request)
		{
			if (Request.Call == EMantleUploadCall::DiffAssets)
			{
				return DiffEverythingUnknown(Request);
			}
			if (Request.Call == EMantleUploadCall::UploadAsset)
			{
				return Failures-- > 0 ? Refused(503, RetryAfter > 0.0f ? TEXT("upload_busy") : TEXT(""), RetryAfter) : UploadedApproved(Request);
			}
			return Refused(500, TEXT("server_error"), 0.0f);
		};

		FMantleCosmeticPackage Package;
		FMantleCosmeticUploadAsset Asset;
		Asset.Key = TEXT("thumbnail");
		Asset.Label = TEXT("Thumbnail");
		Asset.Options.Kind = TEXT("texture");
		Asset.Bytes = { 1, 2, 3, 4 };
		Asset.Hash = FMantleSha256::AssetHash(Asset.Bytes);
		Package.Assets.Add(Asset);

		const TSharedRef<FMantleCosmeticUploadSession> Session = MakeShared<FMantleCosmeticUploadSession>(
			Package, Transport, FString(), [&Now] { return Now; });
		Session->Timing.RetryDelays = { 2.0f, 5.0f };
		Session->Timing.BusyJitter = 0.0f;
		Session->Start();
		for (int32 Step = 0; Step < 400 && !Session->IsFinished() && Transport->UploadTimes(1).Num() < 2; ++Step)
		{
			Session->Tick();
			Transport->Flush();
			Now += 0.5;
		}
		const TArray<double> Times = Transport->UploadTimes(1);
		return Times.Num() == 2 ? Times[1] - Times[0] : -1.0;
	};

	const double WithRetryAfter = SecondSendAfter(30.0f);
	TestTrue(*FString::Printf(TEXT("a 503 with Retry-After: 30 is retried after 30 s, not the 2 s schedule (%.1f)"), WithRetryAfter),
		WithRetryAfter >= 30.0 && WithRetryAfter < 31.0);
	const double WithoutRetryAfter = SecondSendAfter(0.0f);
	TestTrue(*FString::Printf(TEXT("a 503 with none keeps the schedule (%.1f)"), WithoutRetryAfter),
		WithoutRetryAfter >= 2.0 && WithoutRetryAfter < 3.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCosmeticUploadBusyWaitsWithoutAttemptsTest,
	"Mantle.Editor.CosmeticUpload.BusyWaitsWithoutAttemptsAndIsBounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCosmeticUploadBusyWaitsWithoutAttemptsTest::RunTest(const FString& Parameters)
{
	using namespace MantleUploadQueueBusyTestPrivate;

	struct FOutcome
	{
		TArray<double> Times;
		int32 Attempts = -1;
		bool bFinished = false;
		bool bSucceededUpload = false;
	};

	// Answers the first BusyAnswers uploads "busy, 5 s" and takes the next; runs until the session ends
	// or the upload was taken.
	const auto Run = [](int32 BusyAnswers, float MaxBusyWait) -> FOutcome
	{
		double Now = 500.0;
		const TSharedRef<FDeferredTransport> Transport = MakeShared<FDeferredTransport>();
		Transport->Now = &Now;
		int32 BusyLeft = BusyAnswers;
		bool bTaken = false;
		Transport->Answer = [&BusyLeft, &bTaken](const FMantleUploadCallRequest& Request)
		{
			if (Request.Call == EMantleUploadCall::DiffAssets)
			{
				return DiffEverythingUnknown(Request);
			}
			if (Request.Call == EMantleUploadCall::UploadAsset)
			{
				if (BusyLeft-- > 0)
				{
					return Refused(503, TEXT("upload_busy"), 5.0f);
				}
				bTaken = true;
				return UploadedApproved(Request);
			}
			return Refused(500, TEXT("server_error"), 0.0f);
		};

		FMantleCosmeticPackage Package;
		FMantleCosmeticUploadAsset Asset;
		Asset.Key = TEXT("thumbnail");
		Asset.Label = TEXT("Thumbnail");
		Asset.Options.Kind = TEXT("texture");
		Asset.Bytes = { 1, 2, 3, 4 };
		Asset.Hash = FMantleSha256::AssetHash(Asset.Bytes);
		Package.Assets.Add(Asset);

		const TSharedRef<FMantleCosmeticUploadSession> Session = MakeShared<FMantleCosmeticUploadSession>(
			Package, Transport, FString(), [&Now] { return Now; });
		Session->Timing.RetryDelays = { 1.0f, 1.0f };
		Session->Timing.BusyJitter = 0.0f;
		Session->Timing.MaxBusyWait = MaxBusyWait;
		Session->Start();
		for (int32 Step = 0; Step < 2000 && !Session->IsFinished() && !bTaken; ++Step)
		{
			Session->Tick();
			Transport->Flush();
			Now += 0.5;
		}
		FOutcome Outcome;
		Outcome.Times = Transport->UploadTimes(1);
		Outcome.Attempts = Session->GetAssets().Num() > 0 ? Session->GetAssets()[0].Attempts : -1;
		Outcome.bFinished = Session->IsFinished();
		Outcome.bSucceededUpload = bTaken;
		return Outcome;
	};

	// Three busy answers, then taken: four sends 5 s apart, and not one attempt spent.
	const FOutcome Patient = Run(3, 600.0f);
	TestTrue(TEXT("the upload was taken"), Patient.bSucceededUpload);
	TestEqual(TEXT("sent four times"), Patient.Times.Num(), 4);
	for (int32 Index = 1; Index < Patient.Times.Num(); ++Index)
	{
		const double Gap = Patient.Times[Index] - Patient.Times[Index - 1];
		TestTrue(*FString::Printf(TEXT("send %d waited the server's 5 s (%.1f)"), Index + 1, Gap), Gap >= 5.0 && Gap < 5.6);
	}
	TestEqual(TEXT("busy answers spent no attempt"), Patient.Attempts, 0);

	// Always busy: four 5 s waits fill a 20 s allowance, then the two attempts there are, then the run stops.
	const FOutcome Stuck = Run(1000, 20.0f);
	TestTrue(TEXT("the session gives up"), Stuck.bFinished && !Stuck.bSucceededUpload);
	TestEqual(TEXT("sent seven times"), Stuck.Times.Num(), 7);
	TestEqual(TEXT("only past the busy allowance did attempts count"), Stuck.Attempts, 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
