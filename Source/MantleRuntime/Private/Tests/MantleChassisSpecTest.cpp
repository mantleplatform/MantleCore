// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleChassisDerive.h"
#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The parametric chassis spec: the preset's chassis as FMantleChassisSpec composes the same document and builds the same skeleton and
// body as the preset's builder document does, and a selector names one chassis on every machine.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisSpecTestPrivate
{
	struct FGoldenBone
	{
		const TCHAR* Name;
		int32 Parent;
		double Pose[3];
	};

	struct FGoldenWheel
	{
		double Centre[3];
	};

	/** The document each preset's builder produced, captured from its code, that FMantleChassisSpec must reproduce. */
	struct FGoldenChassis
	{
		const TCHAR* Id;
		/** FMantleVehicleChassisBuilder::MakeDocument's text, as JSON (key order is not compared). */
		const TCHAR* Document;
		/** The grey body's vertices and triangles: the box's 24 and 12, and each wheel's 74 and 48 (a plane's six more boxes: a half-wing each, the tailplane, the fin and two propeller blades; a jet has a nozzle and an intake each side in place of the blades, one box more). */
		int32 Vertices;
		int32 Triangles;
		/** The body's bounds, cm: the box and the wheels. */
		double Min[3];
		double Max[3];
		/** The skeleton: a root, then each wheel's bone (a steered centreline wheel hangs under its fork bone), with the reference pose's location. */
		TArray<FGoldenBone> Bones;
		/** Each wheel's centre, in wheel order. */
		TArray<FGoldenWheel> Wheels;
	};

	const TArray<FGoldenChassis>& Golden()
	{
		static const TArray<FGoldenChassis> Table = {
	{
		// A helicopter's wheels are not drawn: its body is sixteen boxes (the cabin, the mast, the boom, the fin, two skids, a leg at each skid end, two
		// cross-tubes, and two blades on each rotor), its collision the cabin, the boom and the fin.
		TEXT("helicopter"),
		TEXT(R"MCS({"mass":2500,"maxSpeed":7000,"dragCoefficient":0.4,"frontalArea":2.5,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":6,"maxDrop":6,"springRate":800,"springPreload":250,"damping":0.9},"steering":{"maxAngle":10,"ackermann":false},"brakes":{"brakeTorque":3000,"handbrakeTorque":6000},"tires":{"friction":2},"camera":{"armLength":1600,"height":250},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":20,"width":10}],"collision":[{"center":[40.0,0.0,165.0],"extent":[210.0,75.0,95.0]},{"center":[-430.0,0.0,215.0],"extent":[260.0,20.0,22.5]},{"center":[-655.0,0.0,292.5],"extent":[35.0,4.375,55.0]}],"centerOfMass":[0,0,140],"seat":{"location":[130,0,243]}})MCS"),
		384, 192,
		{ -735.0, -535.0, 5.0 }, { 535.0, 535.0, 353.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 120.0, -110.0, 20.0 } },
			{ TEXT("wheel_fr"), 0, { 120.0, 110.0, 20.0 } },
			{ TEXT("wheel_rl"), 0, { -120.0, -110.0, 20.0 } },
			{ TEXT("wheel_rr"), 0, { -120.0, 110.0, 20.0 } },
			{ TEXT("main_rotor"), 0, { 0.0, 0.0, 350.0 } },
			{ TEXT("tail_rotor"), 0, { -655.0, -30.0, 215.0 } }
		},
		{
			{ 120.0, -110.0, 20.0 },
			{ 120.0, 110.0, 20.0 },
			{ -120.0, -110.0, 20.0 },
			{ -120.0, 110.0, 20.0 }
		}
	},
	{
		// A hovercraft's wheels are where its pads lift and are not drawn: its body is four boxes (the hull, the skirt under it, the fan's pod behind it
		// and the fin on its top), its collision the hull alone.
		TEXT("hovercraft"),
		TEXT(R"MCS({"mass":600,"maxSpeed":2500,"dragCoefficient":0.5,"frontalArea":2,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":10,"maxDrop":10,"springRate":400,"springPreload":100,"damping":0.5},"steering":{"maxAngle":10,"ackermann":false},"brakes":{"brakeTorque":1500,"handbrakeTorque":3000},"tires":{"friction":2},"camera":{"armLength":500,"height":170},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10}],"collision":[{"center":[0.0,0.0,60.0],"extent":[200.0,110.0,30.0]}],"centerOfMass":[0,0,50],"seat":{"location":[30,-40,96],"steeringWheel":{"location":[76.8,-40,99],"rotation":[-20.0,0.0,0.0],"radius":18}},"passengerSeat":{"location":[30,40,96]}})MCS"),
		96, 48,
		{ -240.0, -120.0, 8.0 }, { 210.0, 120.0, 190.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 160.0, -88.0, 20.0 } },
			{ TEXT("wheel_fr"), 0, { 160.0, 88.0, 20.0 } },
			{ TEXT("wheel_rl"), 0, { -160.0, -88.0, 20.0 } },
			{ TEXT("wheel_rr"), 0, { -160.0, 88.0, 20.0 } }
		},
		{
			{ 160.0, -88.0, 20.0 },
			{ 160.0, 88.0, 20.0 },
			{ -160.0, -88.0, 20.0 },
			{ -160.0, 88.0, 20.0 }
		}
	},
	{
		TEXT("jet"),
		TEXT(R"MCS({"mass":9000,"maxSpeed":12000,"dragCoefficient":0.25,"frontalArea":2.16,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":10,"maxDrop":15,"springRate":2050,"springPreload":580,"damping":0.85},"steering":{"maxAngle":25,"ackermann":false},"brakes":{"brakeTorque":5000,"handbrakeTorque":5000},"tires":{"friction":2},"camera":{"armLength":2150,"height":150},"schemaVersion":1,"wheels":[{"bone":"wheel_f","steering":true,"driven":false,"handbrake":false,"radius":40,"width":26},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":40,"width":26},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":40,"width":26}],"collision":[{"center":[-200.0,0.0,200.0],"extent":[750.0,60.0,90.0]},{"center":[-290.0,0.0,200.0],"extent":[150.0,490.0,18.75]},{"center":[-870.0,0.0,215.0],"extent":[70.0,250.0,8.75]},{"center":[-850.0,0.0,440.0],"extent":[100.0,12.5,150.0]}],"centerOfMass":[-200,0,205],"seat":{"location":[250,0,273]}})MCS"),
		414, 240,
		{ -1000.0, -490.0, 0.0 }, { 550.0, 490.0, 590.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_f"), 0, { 260.0, 0.0, 40.0 } },
			{ TEXT("wheel_rl"), 0, { -260.0, -140.0, 40.0 } },
			{ TEXT("wheel_rr"), 0, { -260.0, 140.0, 40.0 } },
			{ TEXT("aileron_l"), 0, { -215.0, -245.0, 200.0 } },
			{ TEXT("aileron_r"), 0, { -215.0, 245.0, 200.0 } },
			{ TEXT("elevator"), 0, { -835.0, 0.0, 215.0 } },
			{ TEXT("rudder"), 0, { -800.0, 0.0, 440.0 } },
			{ TEXT("nozzle"), 0, { -1000.0, 0.0, 200.0 } }
		},
		{
			{ 260.0, 0.0, 40.0 },
			{ -260.0, -140.0, 40.0 },
			{ -260.0, 140.0, 40.0 }
		}
	},
	{
		TEXT("limousine"),
		TEXT(R"MCS({"mass":2400,"maxSpeed":4000,"dragCoefficient":0.34,"frontalArea":2.9,"engine":{"maxTorque":800,"maxRPM":6000,"idleRPM":800,"torqueCurve":[[0,0.55],[1800,0.85],[3500,1],[6000,0.8]]},"transmission":{"gearRatios":[4.0,2.4,1.6,1.2,0.95],"reverseRatio":3.9,"finalDrive":3.0,"changeUpRPM":5100,"changeDownRPM":1800},"suspension":{"maxRaise":14,"maxDrop":14,"springRate":200,"springPreload":60,"damping":0.45},"steering":{"maxAngle":32,"ackermann":true},"brakes":{"brakeTorque":3500,"handbrakeTorque":6000},"tires":{"friction":3.0},"camera":{"armLength":520,"height":180},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":32,"width":21},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":32,"width":21}],"collision":[{"center":[0.0,0.0,56.0],"extent":[320.0,95.0,36.0]}],"centerOfMass":[0,0,46],"seat":{"location":[89,-36,76],"steeringWheel":{"location":[135.8,-36,79],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[89,36,76]}})MCS"),
		320, 204,
		{ -320.0, -95.0, 0.0 }, { 320.0, 95.0, 92.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 225.0, -82.5, 32.0 } },
			{ TEXT("wheel_fr"), 0, { 225.0, 82.5, 32.0 } },
			{ TEXT("wheel_rl"), 0, { -225.0, -82.5, 32.0 } },
			{ TEXT("wheel_rr"), 0, { -225.0, 82.5, 32.0 } }
		},
		{
			{ 225.0, -82.5, 32.0 },
			{ 225.0, 82.5, 32.0 },
			{ -225.0, -82.5, 32.0 },
			{ -225.0, 82.5, 32.0 }
		}
	},
	{
		TEXT("motorcycle"),
		TEXT(R"MCS({"mass":350,"maxSpeed":4000,"dragCoefficient":0.31,"frontalArea":1.72,"engine":{"maxTorque":549,"maxRPM":7000,"idleRPM":900,"torqueCurve":[[0,0.5],[2000,0.8],[4500,1],[7000,0.8]]},"transmission":{"gearRatios":[4.25,2.52,1.66,1.22,1.0],"reverseRatio":4.04,"finalDrive":2.81,"changeUpRPM":6000,"changeDownRPM":2000},"suspension":{"maxRaise":8.1,"maxDrop":8.1,"springRate":307,"springPreload":50,"damping":0.5},"steering":{"maxAngle":40,"ackermann":false},"brakes":{"brakeTorque":3662,"handbrakeTorque":1220},"tires":{"friction":4},"camera":{"armLength":380,"height":130},"schemaVersion":1,"wheels":[{"bone":"wheel_f","steering":true,"driven":false,"handbrake":false,"radius":35,"width":30},{"bone":"wheel_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":30}],"collision":[{"center":[0.0,0.0,57.5],"extent":[90.0,20.0,22.5]}],"centerOfMass":[0,0,38],"seat":{"location":[-1.4,0,89.4]}})MCS"),
		172, 108,
		{ -105.0, -20.0, 0.0 }, { 105.0, 20.0, 80.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("fork_f"), 0, { 70.0, 0.0, 35.0 } },
			{ TEXT("wheel_f"), 1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_r"), 0, { -70.0, 0.0, 35.0 } }
		},
		{
			{ 70.0, 0.0, 35.0 },
			{ -70.0, 0.0, 35.0 }
		}
	},
	{
		TEXT("plane"),
		TEXT(R"MCS({"mass":7000,"maxSpeed":12000,"dragCoefficient":0.3,"frontalArea":1.76,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":10,"maxDrop":15,"springRate":1600,"springPreload":450,"damping":0.85},"steering":{"maxAngle":25,"ackermann":false},"brakes":{"brakeTorque":4000,"handbrakeTorque":4000},"tires":{"friction":2},"camera":{"armLength":1800,"height":150},"schemaVersion":1,"wheels":[{"bone":"wheel_f","steering":true,"driven":false,"handbrake":false,"radius":40,"width":28},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":40,"width":28},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":40,"width":28}],"collision":[{"center":[-335.0,0.0,200.0],"extent":[575.0,55.0,80.0]},{"center":[-190.5,0.0,252.0],"extent":[69.0,725.0,8.625]},{"center":[-853.75,0.0,195.0],"extent":[37.5,200.0,4.6875]},{"center":[-835.0,0.0,380.0],"extent":[75.0,9.375,100.0]}],"centerOfMass":[-144,0,210],"seat":{"location":[-75,0,263]}})MCS"),
		390, 228,
		{ -910.0, -725.0, 0.0 }, { 253.0, 725.0, 480.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_f"), 0, { 190.0, 0.0, 40.0 } },
			{ TEXT("wheel_rl"), 0, { -190.0, -160.0, 40.0 } },
			{ TEXT("wheel_rr"), 0, { -190.0, 160.0, 40.0 } },
			{ TEXT("aileron_l"), 0, { -156.0, -362.5, 252.0 } },
			{ TEXT("aileron_r"), 0, { -156.0, 362.5, 252.0 } },
			{ TEXT("elevator"), 0, { -835.0, 0.0, 195.0 } },
			{ TEXT("rudder"), 0, { -797.5, 0.0, 380.0 } },
			{ TEXT("propeller"), 0, { 250.0, 0.0, 200.0 } }
		},
		{
			{ 190.0, 0.0, 40.0 },
			{ -190.0, -160.0, 40.0 },
			{ -190.0, 160.0, 40.0 }
		}
	},
	{
		TEXT("pickup"),
		TEXT(R"MCS({"mass":2000,"maxSpeed":3800,"dragCoefficient":0.4,"frontalArea":3,"engine":{"maxTorque":800,"maxRPM":6000,"idleRPM":800,"torqueCurve":[[0,0.6],[1500,0.9],[3500,1],[6000,0.75]]},"transmission":{"gearRatios":[4.25,2.52,1.66,1.22,1.0],"reverseRatio":4.04,"finalDrive":3.4,"changeUpRPM":5200,"changeDownRPM":1800},"suspension":{"maxRaise":20,"maxDrop":20,"springRate":300,"springPreload":100,"damping":0.5},"steering":{"maxAngle":40,"ackermann":true},"brakes":{"brakeTorque":3500,"handbrakeTorque":6000},"tires":{"friction":3.5},"camera":{"armLength":550,"height":200},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":true,"handbrake":false,"radius":37,"width":26},{"bone":"wheel_fr","steering":true,"driven":true,"handbrake":false,"radius":37,"width":26},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":37,"width":26},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":37,"width":26}],"collision":[{"center":[0.0,0.0,75.0],"extent":[270.0,97.5,39.0]}],"centerOfMass":[0,0,54],"seat":{"location":[-5,-38,100],"steeringWheel":{"location":[41.8,-38,103],"rotation":[-20.0,0.0,0.0],"radius":20}},"passengerSeat":{"location":[-5,38,100]}})MCS"),
		320, 204,
		{ -270.0, -97.5, 0.0 }, { 270.0, 97.5, 114.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 155.0, -80.0, 37.0 } },
			{ TEXT("wheel_fr"), 0, { 155.0, 80.0, 37.0 } },
			{ TEXT("wheel_rl"), 0, { -155.0, -80.0, 37.0 } },
			{ TEXT("wheel_rr"), 0, { -155.0, 80.0, 37.0 } }
		},
		{
			{ 155.0, -80.0, 37.0 },
			{ 155.0, 80.0, 37.0 },
			{ -155.0, -80.0, 37.0 },
			{ -155.0, 80.0, 37.0 }
		}
	},
	{
		TEXT("sedan"),
		TEXT(R"MCS({"mass":1500,"maxSpeed":4500,"dragCoefficient":0.31,"frontalArea":2.6,"engine":{"maxTorque":675,"maxRPM":7000,"idleRPM":900,"torqueCurve":[[0,0.5],[2000,0.8],[4500,1],[7000,0.8]]},"transmission":{"gearRatios":[4.25,2.52,1.66,1.22,1.0],"reverseRatio":4.04,"finalDrive":2.81,"changeUpRPM":6000,"changeDownRPM":2000},"suspension":{"maxRaise":10,"maxDrop":10,"springRate":250,"springPreload":50,"damping":0.5},"steering":{"maxAngle":40,"ackermann":true},"brakes":{"brakeTorque":3000,"handbrakeTorque":6000},"tires":{"friction":3.5},"camera":{"armLength":420,"height":160},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":32,"width":21},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":32,"width":21}],"collision":[{"center":[0.0,0.0,55.0],"extent":[235.0,91.0,35.0]}],"centerOfMass":[0,0,46],"seat":{"location":[0,-36,76],"steeringWheel":{"location":[46.8,-36,79],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[0,36,76]}})MCS"),
		320, 204,
		{ -235.0, -93.0, 0.0 }, { 235.0, 93.0, 90.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 136.5, -82.5, 32.0 } },
			{ TEXT("wheel_fr"), 0, { 136.5, 82.5, 32.0 } },
			{ TEXT("wheel_rl"), 0, { -136.5, -82.5, 32.0 } },
			{ TEXT("wheel_rr"), 0, { -136.5, 82.5, 32.0 } }
		},
		{
			{ 136.5, -82.5, 32.0 },
			{ 136.5, 82.5, 32.0 },
			{ -136.5, -82.5, 32.0 },
			{ -136.5, 82.5, 32.0 }
		}
	},
	{
		TEXT("sixwheeler"),
		TEXT(R"MCS({"mass":9000,"maxSpeed":2600,"dragCoefficient":0.7,"frontalArea":7,"engine":{"maxTorque":3500,"maxRPM":3200,"idleRPM":600,"torqueCurve":[[0,0.6],[1000,0.9],[1800,1],[3200,0.7]]},"transmission":{"gearRatios":[6.0,3.6,2.3,1.6,1.0],"reverseRatio":5.5,"finalDrive":4.8,"changeUpRPM":2700,"changeDownRPM":1100},"suspension":{"maxRaise":15,"maxDrop":15,"springRate":1000,"springPreload":300,"damping":0.8},"steering":{"maxAngle":30,"ackermann":true},"brakes":{"brakeTorque":9000,"handbrakeTorque":12000},"tires":{"friction":3.5},"camera":{"armLength":900,"height":320},"schemaVersion":1,"wheels":[{"bone":"wheel_a1_l","steering":true,"driven":false,"handbrake":false,"radius":46,"width":30},{"bone":"wheel_a1_r","steering":true,"driven":false,"handbrake":false,"radius":46,"width":30},{"bone":"wheel_a2_l","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30},{"bone":"wheel_a2_r","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30},{"bone":"wheel_a3_l","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30},{"bone":"wheel_a3_r","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30}],"collision":[{"center":[0.0,0.0,111.0],"extent":[410.0,112.5,55.0]}],"centerOfMass":[-45,0,80],"seat":{"location":[178,-42,138],"steeringWheel":{"location":[224.8,-42,141],"rotation":[-20.0,0.0,0.0],"radius":22}},"passengerSeat":{"location":[178,42,138]}})MCS"),
		468, 300,
		{ -410.0, -112.5, 0.0 }, { 410.0, 112.5, 166.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_a1_l"), 0, { 280.0, -90.0, 46.0 } },
			{ TEXT("wheel_a1_r"), 0, { 280.0, 90.0, 46.0 } },
			{ TEXT("wheel_a2_l"), 0, { -140.0, -90.0, 46.0 } },
			{ TEXT("wheel_a2_r"), 0, { -140.0, 90.0, 46.0 } },
			{ TEXT("wheel_a3_l"), 0, { -280.0, -90.0, 46.0 } },
			{ TEXT("wheel_a3_r"), 0, { -280.0, 90.0, 46.0 } }
		},
		{
			{ 280.0, -90.0, 46.0 },
			{ 280.0, 90.0, 46.0 },
			{ -140.0, -90.0, 46.0 },
			{ -140.0, 90.0, 46.0 },
			{ -280.0, -90.0, 46.0 },
			{ -280.0, 90.0, 46.0 }
		}
	},
	{
		TEXT("sports"),
		TEXT(R"MCS({"mass":1400,"maxSpeed":6000,"dragCoefficient":0.32,"frontalArea":2.0,"engine":{"maxTorque":900,"maxRPM":8000,"idleRPM":900,"torqueCurve":[[0,0.5],[2500,0.85],[5500,1],[8000,0.8]]},"transmission":{"gearRatios":[4.1,2.6,1.8,1.35,1.05,0.85],"reverseRatio":3.8,"finalDrive":3.0,"changeUpRPM":7000,"changeDownRPM":2500},"suspension":{"maxRaise":8,"maxDrop":8,"springRate":400,"springPreload":80,"damping":0.6},"steering":{"maxAngle":38,"ackermann":true},"brakes":{"brakeTorque":4500,"handbrakeTorque":7000},"tires":{"friction":4.0},"camera":{"armLength":400,"height":140},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":34,"width":28},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":34,"width":28},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":34,"width":28},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":34,"width":28}],"collision":[{"center":[0.0,0.0,43.0],"extent":[225.0,95.0,29.0]}],"centerOfMass":[0,0,38],"seat":{"location":[-42,-37,58],"steeringWheel":{"location":[4.8,-37,61],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[-42,37,58]}})MCS"),
		320, 204,
		{ -225.0, -98.0, 0.0 }, { 225.0, 98.0, 72.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 131.0, -84.0, 34.0 } },
			{ TEXT("wheel_fr"), 0, { 131.0, 84.0, 34.0 } },
			{ TEXT("wheel_rl"), 0, { -131.0, -84.0, 34.0 } },
			{ TEXT("wheel_rr"), 0, { -131.0, 84.0, 34.0 } }
		},
		{
			{ 131.0, -84.0, 34.0 },
			{ 131.0, 84.0, 34.0 },
			{ -131.0, -84.0, 34.0 },
			{ -131.0, 84.0, 34.0 }
		}
	},
	{
		TEXT("truck"),
		TEXT(R"MCS({"mass":4000,"maxSpeed":3000,"dragCoefficient":0.6,"frontalArea":5,"engine":{"maxTorque":2000,"maxRPM":3500,"idleRPM":600,"torqueCurve":[[0,0.6],[1000,0.9],[2000,1],[3500,0.75]]},"transmission":{"gearRatios":[5.5,3.2,2,1.4,1.0],"reverseRatio":5,"finalDrive":4.1,"changeUpRPM":3000,"changeDownRPM":1200},"suspension":{"maxRaise":15,"maxDrop":15,"springRate":800,"springPreload":200,"damping":0.75},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":6000,"handbrakeTorque":8000},"tires":{"friction":3.5},"camera":{"armLength":750,"height":280},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":42,"width":28},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":42,"width":28},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":42,"width":28},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":42,"width":28}],"collision":[{"center":[0.0,0.0,96.0],"extent":[320.0,102.5,48.0]}],"centerOfMass":[0,0,61],"seat":{"location":[73.2,-42,130],"steeringWheel":{"location":[120,-42,133],"rotation":[-20.0,0.0,0.0],"radius":21}},"passengerSeat":{"location":[73.2,42,130]}})MCS"),
		320, 204,
		{ -320.0, -102.5, 0.0 }, { 320.0, 102.5, 144.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 175.0, -84.0, 42.0 } },
			{ TEXT("wheel_fr"), 0, { 175.0, 84.0, 42.0 } },
			{ TEXT("wheel_rl"), 0, { -175.0, -84.0, 42.0 } },
			{ TEXT("wheel_rr"), 0, { -175.0, 84.0, 42.0 } }
		},
		{
			{ 175.0, -84.0, 42.0 },
			{ 175.0, 84.0, 42.0 },
			{ -175.0, -84.0, 42.0 },
			{ -175.0, 84.0, 42.0 }
		}
	},
	{
		TEXT("van"),
		TEXT(R"MCS({"mass":2500,"maxSpeed":3600,"dragCoefficient":0.5,"frontalArea":4,"engine":{"maxTorque":1000,"maxRPM":5500,"idleRPM":750,"torqueCurve":[[0,0.6],[1500,0.9],[3000,1],[5500,0.75]]},"transmission":{"gearRatios":[4.6,2.8,1.8,1.3,1.0],"reverseRatio":4.4,"finalDrive":3.7,"changeUpRPM":4700,"changeDownRPM":1700},"suspension":{"maxRaise":15,"maxDrop":15,"springRate":400,"springPreload":150,"damping":0.65},"steering":{"maxAngle":36,"ackermann":true},"brakes":{"brakeTorque":4500,"handbrakeTorque":7000},"tires":{"friction":3.2},"camera":{"armLength":600,"height":230},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":36,"width":24},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":36,"width":24},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":36,"width":24},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":36,"width":24}],"collision":[{"center":[0.0,0.0,90.0],"extent":[260.0,100.0,60.0]}],"centerOfMass":[0,0,65],"seat":{"location":[40,-38,104],"steeringWheel":{"location":[86.8,-38,107],"rotation":[-20.0,0.0,0.0],"radius":20}},"passengerSeat":{"location":[40,38,104]}})MCS"),
		320, 204,
		{ -260.0, -100.0, 0.0 }, { 260.0, 100.0, 150.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 165.0, -82.5, 36.0 } },
			{ TEXT("wheel_fr"), 0, { 165.0, 82.5, 36.0 } },
			{ TEXT("wheel_rl"), 0, { -165.0, -82.5, 36.0 } },
			{ TEXT("wheel_rr"), 0, { -165.0, 82.5, 36.0 } }
		},
		{
			{ 165.0, -82.5, 36.0 },
			{ 165.0, 82.5, 36.0 },
			{ -165.0, -82.5, 36.0 },
			{ -165.0, 82.5, 36.0 }
		}
	},

		};
		return Table;
	}

	/** Where two JSON values first differ ("/engine/maxTorque"), or empty when they are equal. Numbers are compared exactly. */
	FString FirstDifference(const TSharedPtr<FJsonValue>& Got, const TSharedPtr<FJsonValue>& Want, const FString& Path)
	{
		if (!Got.IsValid() || !Want.IsValid() || Got->Type != Want->Type)
		{
			return Path.IsEmpty() ? FString(TEXT("/")) : Path;
		}
		if (Got->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> A = Got->AsObject();
			const TSharedPtr<FJsonObject> B = Want->AsObject();
			for (const auto& Pair : A->Values)
			{
				const FString Key(Pair.Key.ToView());
				const FString Difference = FirstDifference(Pair.Value, B->TryGetField(Key), Path + TEXT("/") + Key);
				if (!Difference.IsEmpty())
				{
					return Difference;
				}
			}
			for (const auto& Pair : B->Values)
			{
				const FString Key(Pair.Key.ToView());
				if (!A->HasField(Key))
				{
					return Path + TEXT("/") + Key;
				}
			}
			return FString();
		}
		if (Got->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& A = Got->AsArray();
			const TArray<TSharedPtr<FJsonValue>>& B = Want->AsArray();
			for (int32 Index = 0; Index < FMath::Max(A.Num(), B.Num()); ++Index)
			{
				const FString Difference = FirstDifference(A.IsValidIndex(Index) ? A[Index] : nullptr, B.IsValidIndex(Index) ? B[Index] : nullptr,
					Path + FString::Printf(TEXT("/%d"), Index));
				if (!Difference.IsEmpty())
				{
					return Difference;
				}
			}
			return FString();
		}
		return FJsonValue::CompareEqual(*Got, *Want) ? FString() : Path;
	}

	TSharedPtr<FJsonValue> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonValue> Value;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value) ? Value : nullptr;
	}

	/**
	 * The document without its movement keys: the golden texts have none, and the movement keys are held to their own
	 * numbers by Mantle.Vehicle.Document.*, so what is compared here is the rest.
	 */
	TSharedPtr<FJsonValue> ParseJsonWithoutMovement(const FString& Text)
	{
		const TSharedPtr<FJsonValue> Value = ParseJson(Text);
		const TSharedPtr<FJsonObject> Doc = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		if (!Doc.IsValid())
		{
			return Value;
		}
		for (const TCHAR* Key : { TEXT("movementType"), TEXT("flight"), TEXT("helicopter"), TEXT("hover") })
		{
			Doc->RemoveField(Key);
		}
		// A ground car's handling baked in: the steering's speed curve goes, and the tyres' friction is the preset's again (the front's is 6/7 of it, the rear's 8/7).
		const TSharedPtr<FJsonObject>* Steering = nullptr;
		if (Doc->TryGetObjectField(TEXT("steering"), Steering) && Steering && Steering->IsValid())
		{
			(*Steering)->RemoveField(TEXT("speedCurve"));
		}
		const TSharedPtr<FJsonObject>* Tires = nullptr;
		double RearFriction = 0.0;
		if (Doc->TryGetObjectField(TEXT("tires"), Tires) && Tires && Tires->IsValid() && (*Tires)->TryGetNumberField(TEXT("rearFriction"), RearFriction))
		{
			(*Tires)->SetNumberField(TEXT("friction"), FMath::RoundToDouble(RearFriction * 7.0 / 8.0 * 1.0e6) / 1.0e6);
			(*Tires)->RemoveField(TEXT("rearFriction"));
		}
		const TPair<const TCHAR*, const TCHAR*> Extras[] = {
			{ TEXT("seat"), TEXT("kind") }, { TEXT("seat"), TEXT("exitSideDistance") }, { TEXT("seat"), TEXT("rider") },
			{ TEXT("passengerSeat"), TEXT("kind") }, { TEXT("camera"), TEXT("boomLocation") } };
		for (const TPair<const TCHAR*, const TCHAR*>& Extra : Extras)
		{
			const TSharedPtr<FJsonObject>* Parent = nullptr;
			if (Doc->TryGetObjectField(Extra.Key, Parent) && Parent && Parent->IsValid())
			{
				(*Parent)->RemoveField(Extra.Value);
			}
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisSpecComposesLikeBeforeTest,
	"Mantle.Vehicle.Chassis.SpecComposesLikeBefore",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisSpecComposesLikeBeforeTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisSpecTestPrivate;
	const TArray<FString> Ids = FMantleVehicleChassisBuilder::PresetIds();
	TestEqual(TEXT("every shipped preset has a golden entry"), Ids.Num(), Golden().Num());
	for (const FGoldenChassis& Want : Golden())
	{
		const FString Id = Want.Id;
		TestTrue(*FString::Printf(TEXT("%s: it is a shipped preset"), *Id), Ids.Contains(Id));
		FString Text;
		FString Error;
		FMantleChassisSpec Spec;
		if (!TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), *Id, *Error),
				FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) && FMantleChassisSpec::Parse(Text, Spec, Error)))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s: the spec is the preset's own"), *Id), Spec.Id, Id);
		TestTrue(*FString::Printf(TEXT("%s: at scale 1 with its hand-tuned handling"), *Id), Spec.Scale == 1.0 && !Spec.Derived.IsSet());

		// The document: every key and number as before.
		const FString Document = FMantleChassisSpec::ComposeDocument(Spec);
		const FString Difference = FirstDifference(ParseJsonWithoutMovement(Document), ParseJson(Want.Document), FString());
		TestTrue(*FString::Printf(TEXT("%s: the composed document is the one the builder made (differs at %s)"), *Id, *Difference), Difference.IsEmpty());
		TestEqual(*FString::Printf(TEXT("%s: composing again gives the same text (the spec is not changed)"), *Id), FMantleChassisSpec::ComposeDocument(Spec), Document);
		FString FromText;
		FMantleVehicleChassisBuilder::MakeDocument(Text, FromText, Error);
		TestEqual(*FString::Printf(TEXT("%s: MakeDocument on the text is the spec's document"), *Id), FromText, Document);

		// The skeleton and the grey body.
		FMantleVehicleAsset Asset;
		FMantleVehicleAsset FromTextAsset;
		if (!TestTrue(*FString::Printf(TEXT("%s: it makes an asset from the spec and from the text (%s)"), *Id, *Error),
				FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error) && FMantleVehicleChassisBuilder::MakeAsset(Text, FromTextAsset, Error)))
		{
			continue;
		}
		if (TestEqual(*FString::Printf(TEXT("%s: the bones"), *Id), Asset.Body.Bones.Num(), Want.Bones.Num())
			&& TestEqual(*FString::Printf(TEXT("%s: the poses"), *Id), Asset.Body.ReferencePose.Num(), Want.Bones.Num()))
		{
			for (int32 Index = 0; Index < Want.Bones.Num(); ++Index)
			{
				const FGoldenBone& Bone = Want.Bones[Index];
				TestEqual(*FString::Printf(TEXT("%s: bone %d's name"), *Id, Index), Asset.Body.Bones[Index].Name, FName(Bone.Name));
				TestEqual(*FString::Printf(TEXT("%s: bone %d's parent"), *Id, Index), Asset.Body.Bones[Index].ParentIndex, Bone.Parent);
				TestTrue(*FString::Printf(TEXT("%s: bone %d's pose"), *Id, Index),
					Asset.Body.ReferencePose[Index].GetLocation().Equals(FVector(Bone.Pose[0], Bone.Pose[1], Bone.Pose[2]), 1.0e-6));
			}
		}
		if (TestEqual(*FString::Printf(TEXT("%s: the wheel positions"), *Id), Asset.WheelPositions.Num(), Want.Wheels.Num()))
		{
			for (int32 Index = 0; Index < Want.Wheels.Num(); ++Index)
			{
				const double* Centre = Want.Wheels[Index].Centre;
				TestTrue(*FString::Printf(TEXT("%s: wheel %d's centre"), *Id, Index), Asset.WheelPositions[Index].Equals(FVector(Centre[0], Centre[1], Centre[2]), 1.0e-6));
			}
		}
		if (TestTrue(*FString::Printf(TEXT("%s: the body has a LOD"), *Id), Asset.Body.LODs.Num() == 1 && FromTextAsset.Body.LODs.Num() == 1))
		{
			const FMantleSkinnedMeshLOD& LOD = Asset.Body.LODs[0];
			TestEqual(*FString::Printf(TEXT("%s: the vertices"), *Id), LOD.Positions.Num(), Want.Vertices);
			TestEqual(*FString::Printf(TEXT("%s: the triangles"), *Id), LOD.NumTriangles(), Want.Triangles);
			FVector3f Min(TNumericLimits<float>::Max());
			FVector3f Max(-TNumericLimits<float>::Max());
			for (const FVector3f& Point : LOD.Positions)
			{
				Min = FVector3f(FMath::Min(Min.X, Point.X), FMath::Min(Min.Y, Point.Y), FMath::Min(Min.Z, Point.Z));
				Max = FVector3f(FMath::Max(Max.X, Point.X), FMath::Max(Max.Y, Point.Y), FMath::Max(Max.Z, Point.Z));
			}
			TestTrue(*FString::Printf(TEXT("%s: the body's bounds"), *Id),
				FVector(Min).Equals(FVector(Want.Min[0], Want.Min[1], Want.Min[2]), 1.0e-3) && FVector(Max).Equals(FVector(Want.Max[0], Want.Max[1], Want.Max[2]), 1.0e-3));
			const FMantleSkinnedMeshLOD& Other = FromTextAsset.Body.LODs[0];
			TestTrue(*FString::Printf(TEXT("%s: the text and the spec make the same body"), *Id),
				Other.Positions == LOD.Positions && Other.Indices == LOD.Indices && Other.InfluenceBones == LOD.InfluenceBones);
		}
		TestEqual(*FString::Printf(TEXT("%s: the text and the spec make the same vehicle"), *Id), FromTextAsset.Desc.Mass, Asset.Desc.Mass);
	}

	// A preset's text without a type or a derive block, or with a type its layout contradicts, is not a preset.
	FString Sedan;
	FString Error;
	if (TestTrue(TEXT("the sedan's text reads"), FMantleVehicleChassisBuilder::LoadPresetText(TEXT("sedan"), Sedan, Error)))
	{
		FMantleChassisSpec Spec;
		FString Cut = Sedan.Replace(TEXT("\"type\""), TEXT("\"kind\""));
		TestFalse(TEXT("a preset with no type is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
		TestTrue(TEXT("and says so"), Error.Contains(TEXT("type")));
		Cut = Sedan.Replace(TEXT("\"derive\""), TEXT("\"derived\""));
		TestFalse(TEXT("a preset with no derive block is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
		TestTrue(TEXT("and says so"), Error.Contains(TEXT("derive")));
		Cut = Sedan.Replace(TEXT("\"type\": \"car\""), TEXT("\"type\": \"motorcycle\""));
		TestFalse(TEXT("a car that says it is a motorcycle is refused"), FMantleChassisSpec::Parse(Cut, Spec, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisSelectorsTest,
	"Mantle.Vehicle.Chassis.SelectorsAreStrict",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisSelectorsTest::RunTest(const FString& Parameters)
{
	// Replicated as a string, parsed on every machine: the same text is the same chassis, and only a preset id and a plain number get through.
	struct FValid
	{
		const TCHAR* Selector;
		const TCHAR* Id;
		int32 Milli;
		const TCHAR* Canonical;
	};
	const FValid Valid[] = {
		{ TEXT("sedan"), TEXT("sedan"), 1000, TEXT("sedan") },
		{ TEXT("sedan@1"), TEXT("sedan"), 1000, TEXT("sedan") },
		{ TEXT("sedan@1.0"), TEXT("sedan"), 1000, TEXT("sedan") },
		{ TEXT("sedan@1.000"), TEXT("sedan"), 1000, TEXT("sedan") },
		{ TEXT("sedan@0.7"), TEXT("sedan"), 700, TEXT("sedan@0.7") },
		{ TEXT("sedan@0.70"), TEXT("sedan"), 700, TEXT("sedan@0.7") },
		{ TEXT("sedan@0.5"), TEXT("sedan"), 500, TEXT("sedan@0.5") },
		{ TEXT("sedan@1.3"), TEXT("sedan"), 1300, TEXT("sedan@1.3") },
		{ TEXT("sedan@1.6"), TEXT("sedan"), 1600, TEXT("sedan@1.6") },
		{ TEXT("sixwheeler@1.25"), TEXT("sixwheeler"), 1250, TEXT("sixwheeler@1.25") },
		{ TEXT("pickup@2"), TEXT("pickup"), 2000, TEXT("pickup@2") },
		{ TEXT("pickup@0.25"), TEXT("pickup"), 250, TEXT("pickup@0.25") },
		{ TEXT("pickup@2.5"), TEXT("pickup"), 2500, TEXT("pickup@2.5") },
		{ TEXT("van@1.234"), TEXT("van"), 1234, TEXT("van@1.234") },
		{ TEXT("sedan@01.5"), TEXT("sedan"), 1500, TEXT("sedan@1.5") },
	};
	for (const FValid& Case : Valid)
	{
		FString Id;
		double Scale = 0.0;
		FString Error;
		if (TestTrue(*FString::Printf(TEXT("'%s' is a selector (%s)"), Case.Selector, *Error), FMantleChassisSpec::ParseSelector(Case.Selector, Id, Scale, Error)))
		{
			TestEqual(*FString::Printf(TEXT("'%s': the preset"), Case.Selector), Id, FString(Case.Id));
			TestTrue(*FString::Printf(TEXT("'%s': the scale is exactly %d thousandths"), Case.Selector, Case.Milli), Scale == static_cast<double>(Case.Milli) / 1000.0);
			TestEqual(*FString::Printf(TEXT("'%s': its canonical spelling"), Case.Selector), FMantleChassisSpec::MakeSelector(Id, Scale), FString(Case.Canonical));
		}
	}
	const TCHAR* const Invalid[] = {
		TEXT(""), TEXT("@1"), TEXT("sedan@"), TEXT("sedan@.5"), TEXT("sedan@1."), TEXT("sedan@0.24"), TEXT("sedan@2.51"), TEXT("sedan@0"), TEXT("sedan@10"),
		TEXT("sedan@1.2345"), TEXT("sedan@-1"), TEXT("sedan@+1"), TEXT("sedan@1e0"), TEXT("sedan@ 1"), TEXT("sedan@1 "), TEXT("sedan@0x1"), TEXT("sedan@1,5"),
		TEXT("sedan@1@2"), TEXT("sedan@1..5"), TEXT("sedan@nan"), TEXT("Sedan"), TEXT("sedan "), TEXT("1sedan"), TEXT("../sedan"), TEXT("..\\sedan"), TEXT("a/b"),
		TEXT("sedan.json"), TEXT("sed an"), TEXT("sedan_2"), TEXT("C:\\temp\\sedan"), TEXT("sedan@0.7sedan"),
		TEXT("sedan-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa@1"),
	};
	for (const TCHAR* Selector : Invalid)
	{
		FString Id;
		double Scale = 0.0;
		FString Error;
		TestFalse(*FString::Printf(TEXT("'%s' is not a selector"), Selector), FMantleChassisSpec::ParseSelector(Selector, Id, Scale, Error));
		TestFalse(*FString::Printf(TEXT("'%s': and says why"), Selector), Error.IsEmpty());
	}

	// A selector loads the plugin's preset at that scale: the preset itself at 1, a derived, scaled one otherwise.
	FMantleChassisSpec Preset;
	FMantleChassisSpec Smaller;
	FString Error;
	if (TestTrue(TEXT("sedan and sedan@0.7 load"), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Preset, Error) && FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan@0.7"), Smaller, Error)))
	{
		TestTrue(TEXT("sedan is the preset, hand-tuned"), Preset.Id == TEXT("sedan") && Preset.Scale == 1.0 && !Preset.Derived.IsSet());
		TestTrue(TEXT("sedan@0.7 is a derived 0.7"), Smaller.Id == TEXT("sedan") && Smaller.Scale == 0.7 && Smaller.Derived.IsSet());
		TestTrue(TEXT("sedan@1.0 is the preset, hand-tuned"), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan@1.0"), Preset, Error) && !Preset.Derived.IsSet());
	}
	TestFalse(TEXT("a preset the plugin does not carry does not load"), FMantleVehicleChassisBuilder::LoadSpec(TEXT("hovertank@1"), Preset, Error));
	TestFalse(TEXT("a scale out of range does not load"), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan@9"), Preset, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisScaledMakesValidAssetsTest,
	"Mantle.Vehicle.Chassis.ScaledChassisMakesValidAssets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisScaledMakesValidAssetsTest::RunTest(const FString& Parameters)
{
	// Every preset at every scale a selector may name makes a document the reader accepts (the derived numbers are held inside limits.json)
	// and a body that validates; the wheels stand where the scaled layout says.
	const double Scales[] = { 0.25, 0.5, 0.7, 1.3, 1.6, 2.5 };
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FString Text;
		FString Error;
		FMantleChassisSpec Preset;
		FMantleVehicleAsset PresetAsset;
		if (!TestTrue(*FString::Printf(TEXT("%s: it reads (%s)"), *Id, *Error),
				FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) && FMantleChassisSpec::Parse(Text, Preset, Error)
					&& FMantleVehicleChassisBuilder::MakeAsset(Preset, PresetAsset, Error)))
		{
			continue;
		}
		for (const double Scale : Scales)
		{
			const FString Selector = FMantleChassisSpec::MakeSelector(Id, Scale);
			FMantleChassisSpec Spec;
			FMantleVehicleAsset Asset;
			if (!TestTrue(*FString::Printf(TEXT("%s: it loads (%s)"), *Selector, *Error), FMantleVehicleChassisBuilder::LoadSpec(Selector, Spec, Error)))
			{
				continue;
			}
			// A body box with a half-extent past limits.json's 1000 cm (the six-wheeler at 2.5) is the reader's to refuse; the size envelope is not
			// enforced earlier, so it must be refused in words and not half built. A plane's boxes (its fuselage,
			// wing, tailplane and fin) are held inside the document's +-1000 cm instead, since a wing of 14.5 m is a wide plane at 1.4 times its size.
			const bool bFits = Preset.Aircraft.bSet || 0.5 * Scale * FMath::Max3(Preset.Length, Preset.Width, Preset.Height) <= 1000.0;
			const bool bMade = FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error);
			if (!bFits)
			{
				TestFalse(*FString::Printf(TEXT("%s: a body that large is refused"), *Selector), bMade);
				TestFalse(*FString::Printf(TEXT("%s: and says why"), *Selector), Error.IsEmpty());
				continue;
			}
			if (!TestTrue(*FString::Printf(TEXT("%s: it makes an asset (%s)"), *Selector, *Error), bMade))
			{
				continue;
			}
			FString Why;
			TestTrue(*FString::Printf(TEXT("%s: the body validates (%s)"), *Selector, *Why), Asset.Body.Validate(Why));
			if (TestEqual(*FString::Printf(TEXT("%s: the same wheels"), *Selector), Asset.WheelPositions.Num(), PresetAsset.WheelPositions.Num()))
			{
				for (int32 Index = 0; Index < Asset.WheelPositions.Num(); ++Index)
				{
					TestTrue(*FString::Printf(TEXT("%s: wheel %d stands at %g times the preset's"), *Selector, Index, Scale),
						Asset.WheelPositions[Index].Equals(PresetAsset.WheelPositions[Index] * Scale, 1.0e-6 * FMath::Max(1.0, Scale)));
				}
			}
			TestEqual(*FString::Printf(TEXT("%s: the same bones"), *Selector), Asset.Body.Bones.Num(), PresetAsset.Body.Bones.Num());
			// The document is the derived handling.
			const FMantleChassisHandling& Derived = Spec.Derived.IsSet() ? Spec.Derived.GetValue() : Spec.Archetype;
			TestEqual(*FString::Printf(TEXT("%s: the mass"), *Selector), Asset.Desc.Mass, Derived.Mass);
			TestEqual(*FString::Printf(TEXT("%s: the spring rate"), *Selector), Asset.Desc.SpringRate, Derived.SpringRate);
			TestEqual(*FString::Printf(TEXT("%s: the engine's torque"), *Selector), Asset.Desc.MaxTorque, Derived.MaxTorque);
			TestEqual(*FString::Printf(TEXT("%s: the steering lock"), *Selector), Asset.Desc.MaxSteeringAngle, Derived.MaxSteerAngle);
			TestTrue(*FString::Printf(TEXT("%s: the centre of mass is the derived one"), *Selector), Asset.Desc.CenterOfMass.Equals(Derived.CenterOfMass, 1.0e-6));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
