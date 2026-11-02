// Copyright Joshua Eger. All Rights Reserved.
#include "Misc/AutomationTest.h"
#include "MantleSnapSockets.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MantleSnapSocketsTestsPrivate
{
	MantleSnapSockets::FMeshSocket Make(const TCHAR* Name, const TCHAR* Tag, const FVector& Location, const FRotator& Rotation = FRotator::ZeroRotator)
	{
		MantleSnapSockets::FMeshSocket Socket;
		Socket.Name = FName(Name);
		Socket.Tag = Tag;
		Socket.Location = Location;
		Socket.Rotation = Rotation;
		return Socket;
	}
}

// Placing mode: the mesh sockets named Snap_<kind> become the place file's sockets (mantleplatform/mantle#184).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleSnapSocketsConvertTest, "Mantle.Placing.MeshSockets.Convert", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMantleSnapSocketsConvertTest::RunTest(const FString& Parameters)
{
	using namespace MantleSnapSocketsTestsPrivate;
	TArray<MantleSnapSockets::FMeshSocket> Sockets;
	Sockets.Add(Make(TEXT("Snap_wall_bottom"), TEXT("floor_edge, floor_corner"), FVector(0.0, 0.0, -10.0), FRotator(0.0, 90.0, 0.0)));
	Sockets.Add(Make(TEXT("Snap_wall_bottom__left"), TEXT(""), FVector(100.0, 0.0, -10.0)));
	Sockets.Add(Make(TEXT("Handle"), TEXT("ignored"), FVector::ZeroVector)); // not a snap socket
	Sockets.Add(Make(TEXT("Snap_bad-name"), TEXT(""), FVector::ZeroVector));
	Sockets.Add(Make(TEXT("Snap_far"), TEXT(""), FVector(1.0e6, 0.0, 0.0)));
	Sockets.Add(Make(TEXT("Snap_tag"), TEXT("good, not good!"), FVector::ZeroVector));

	TArray<FMantlePlaceSocket> Out;
	TArray<FString> Problems;
	MantleSnapSockets::Convert(Sockets, Out, Problems);

	TestEqual(TEXT("two sockets are kept"), Out.Num(), 2);
	if (Out.Num() == 2)
	{
		TestEqual(TEXT("name"), Out[0].Name, FString(TEXT("Snap_wall_bottom")));
		TestEqual(TEXT("kind is the name after Snap_"), Out[0].Kind, FString(TEXT("wall_bottom")));
		TestEqual(TEXT("the tag lists two kinds"), Out[0].Accepts.Num(), 2);
		TestTrue(TEXT("position"), Out[0].Position.Equals(FVector(0.0, 0.0, -10.0)));
		TestTrue(TEXT("rotation"), Out[0].Rotation.Equals(FRotator(0.0, 90.0, 0.0)));
		TestEqual(TEXT("a second socket of a kind: Snap_<kind>__<anything>"), Out[1].Kind, FString(TEXT("wall_bottom")));
		TestEqual(TEXT("with no tag it accepts nothing"), Out[1].Accepts.Num(), 0);
	}
	TestEqual(TEXT("three sockets are explained"), Problems.Num(), 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
