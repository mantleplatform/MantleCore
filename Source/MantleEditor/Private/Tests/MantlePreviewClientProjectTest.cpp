// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewClient.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// Test Locally against another project (a mantle checkout's client): which project is chosen, which exe runs it, and
// the dev-editor arguments. No client is started.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewClientProjectOverrideTest,
	"Mantle.Editor.PreviewClient.ProjectOverride",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewClientProjectOverrideTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("nothing set: no override"), MantlePreviewClient::ProjectOverride(TEXT("-log"), FString()), FString());
	TestEqual(TEXT("the setting alone"), MantlePreviewClient::ProjectOverride(TEXT("-log"), TEXT("  D:/Mantle/Client.uproject ")), FString(TEXT("D:/Mantle/Client.uproject")));
	TestEqual(TEXT("the command line wins over the setting"),
		MantlePreviewClient::ProjectOverride(TEXT("-log -MantlePreviewProject=D:/Other/Other.uproject"), TEXT("D:/Mantle/Client.uproject")), FString(TEXT("D:/Other/Other.uproject")));
	TestEqual(TEXT("a quoted command-line path with a space"),
		MantlePreviewClient::ProjectOverride(TEXT("-MantlePreviewProject=\"D:/My Maps/Client.uproject\""), FString()), FString(TEXT("D:/My Maps/Client.uproject")));
	TestEqual(TEXT("an empty command-line value falls back to the setting"),
		MantlePreviewClient::ProjectOverride(TEXT("-MantlePreviewProject="), TEXT("D:/Mantle/Client.uproject")), FString(TEXT("D:/Mantle/Client.uproject")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewClientProjectExecutableTest,
	"Mantle.Editor.PreviewClient.ProjectExecutable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewClientProjectExecutableTest::RunTest(const FString& Parameters)
{
	FString Exe, Error;
	TestTrue(TEXT("no project: the configured exe is kept"), MantlePreviewClient::ResolveExecutable(TEXT("D:/Mantle/Mantle-Win64-Shipping.exe"), FString(), Exe, Error));
	TestEqual(TEXT("as is"), Exe, FString(TEXT("D:/Mantle/Mantle-Win64-Shipping.exe")));

	TestTrue(TEXT("a project with no exe runs this editor's UnrealEditor.exe"), MantlePreviewClient::ResolveExecutable(FString(), TEXT("D:/Mantle/Client.uproject"), Exe, Error));
	TestEqual(TEXT("UnrealEditor.exe"), FPaths::GetCleanFilename(Exe), FString(TEXT("UnrealEditor.exe")));

	TestTrue(TEXT("a project with UnrealEditor.exe keeps that editor"), MantlePreviewClient::ResolveExecutable(TEXT("D:/UE/UnrealEditor.exe"), TEXT("D:/Mantle/Client.uproject"), Exe, Error));
	TestEqual(TEXT("the chosen editor"), Exe, FString(TEXT("D:/UE/UnrealEditor.exe")));

	TestFalse(TEXT("a project with a packaged client is refused"), MantlePreviewClient::ResolveExecutable(TEXT("D:/Mantle/Mantle-Win64-Shipping.exe"), TEXT("D:/Mantle/Client.uproject"), Exe, Error));
	TestTrue(TEXT("and says which to clear"), Error.Contains(TEXT("Preview Client Executable")) && Error.Contains(TEXT("Client.uproject")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewClientDevEditorTest,
	"Mantle.Editor.PreviewClient.DevEditorProject",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewClientDevEditorTest::RunTest(const FString& Parameters)
{
	FMantlePreviewClientInfo Info;
	FString Error;

	const FString Missing = FPaths::ProjectSavedDir() / TEXT("Mantle/PreviewClientTest") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".uproject"));
	TestFalse(TEXT("a project file that is missing is refused"), MantlePreviewClient::DescribeDevEditor(TEXT("D:/UE/UnrealEditor.exe"), Missing, Info, Error));
	TestTrue(TEXT("and named in the error"), Error.Contains(TEXT("Preview Client Project does not exist")) && Error.Contains(FPaths::GetCleanFilename(Missing)));

	const FString Folder = FPaths::ProjectSavedDir() / TEXT("Mantle/PreviewClientTest") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString NotProject = Folder / TEXT("Client.txt");
	const FString Project = Folder / TEXT("My Client.uproject");
	FFileHelper::SaveStringToFile(TEXT("{}"), *NotProject);
	FFileHelper::SaveStringToFile(TEXT("{}"), *Project);

	TestFalse(TEXT("a file that is not a .uproject is refused"), MantlePreviewClient::DescribeDevEditor(TEXT("D:/UE/UnrealEditor.exe"), NotProject, Info, Error));
	TestTrue(TEXT("and says so"), Error.Contains(TEXT(".uproject")));

	if (TestTrue(TEXT("an existing .uproject is accepted"), MantlePreviewClient::DescribeDevEditor(TEXT("D:/UE/UnrealEditor.exe"), Project, Info, Error)))
	{
		TestEqual(TEXT("it is a dev-editor client"), Info.Source, FString(TEXT("dev-editor")));
		TestEqual(TEXT("running that project, not the open one"), Info.PrefixArgs.Num(), 2);
		if (Info.PrefixArgs.Num() == 2)
		{
			TestEqual(TEXT("the other project first"), FPaths::GetCleanFilename(Info.PrefixArgs[0]), FString(TEXT("My Client.uproject")));
			TestEqual(TEXT("then -game"), Info.PrefixArgs[1], FString(TEXT("-game")));
		}
		TestEqual(TEXT("the panel can name it"), Info.Project, Info.PrefixArgs.IsValidIndex(0) ? Info.PrefixArgs[0] : FString());
		TestEqual(TEXT("with the editor exe"), FPaths::GetCleanFilename(Info.Executable), FString(TEXT("UnrealEditor.exe")));
	}

	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

#endif
