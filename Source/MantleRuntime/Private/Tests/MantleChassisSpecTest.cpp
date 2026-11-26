// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleChassisDerive.h"
#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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
		double Center[3];
	};

	/** The document each preset's builder produced, captured from its code, that FMantleChassisSpec must reproduce. */
	struct FGoldenChassis
	{
		const TCHAR* Id;
		/** FMantleVehicleChassisBuilder::MakeDocument's text, as JSON (key order is not compared). */
		const TCHAR* Document;
		/** The gray body's vertices and triangles: the box's 24 and 12, and each wheel's 74 and 48 (a plane's six more boxes: a half-wing each, the tailplane, the fin and two propeller blades; a jet has a nozzle and an intake each side in place of the blades, one box more). */
		int32 Vertices;
		int32 Triangles;
		/** The body's bounds, cm: the box and the wheels. */
		double Min[3];
		double Max[3];
		/** The skeleton: a root, then each wheel's bone (a steered centerline wheel hangs under its fork bone), with the reference pose's location. */
		TArray<FGoldenBone> Bones;
		/** Each wheel's center, in wheel order. */
		TArray<FGoldenWheel> Wheels;
	};

	const TArray<FGoldenChassis>& Golden()
	{
		static const TArray<FGoldenChassis> Table = {
	{
		// A tracked chassis's road wheels are drawn, seven a side on numbered bones: its body is the hull, the fourteen road wheels, the blade in front and
		// the two tracks (a second section), its collision the hull alone. Its driver's seat has no steering wheel (a bulldozer is steered with levers).
		TEXT("bulldozer"),
		TEXT(R"MCS({"mass":22130,"maxSpeed":325,"dragCoefficient":1.0,"frontalArea":8,"engine":{"maxTorque":1050,"maxRPM":2000,"idleRPM":750,"torqueCurve":[[0,0.6],[750,0.75],[1400,1],[1850,0.79],[2000,0.7]],"tractionControl":false,"engineBraking":0.3},"transmission":{"gearRatios":[3.25,2.39,1.80,1.27,1.0],"reverseRatio":1.27,"finalDrive":7.75,"changeUpRPM":1800,"changeDownRPM":1100,"shiftTime":0.5},"suspension":{"maxRaise":8,"maxDrop":5,"springRate":2000,"springPreload":500,"damping":4.1},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":1210,"handbrakeTorque":2000,"abs":false},"tires":{"friction":1.18,"rollingResistance":0.04},"camera":{"armLength":900,"height":380},"schemaVersion":1,"wheels":[{"bone":"wheel_a1_l","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a1_r","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a2_l","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a2_r","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a3_l","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a3_r","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a4_l","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a4_r","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a5_l","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a5_r","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a6_l","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a6_r","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a7_l","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70},{"bone":"wheel_a7_r","steering":false,"driven":true,"handbrake":true,"radius":13,"width":70}],"collision":[{"center":[0.0,0.0,140.0],"extent":[200.0,130.0,100.0]}],"centerOfMass":[25,0,90],"seat":{"location":[-50,-40,225]}})MCS"),
		1132, 720,
		{ -200.0, -165.0, 0.0 }, { 275.0, 165.0, 240.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_a1_l"), 0, { 138.5, -100.0, 13.0 } },
			{ TEXT("wheel_a1_r"), 0, { 138.5, 100.0, 13.0 } },
			{ TEXT("wheel_a2_l"), 0, { 92.3333333, -100.0, 13.0 } },
			{ TEXT("wheel_a2_r"), 0, { 92.3333333, 100.0, 13.0 } },
			{ TEXT("wheel_a3_l"), 0, { 46.1666667, -100.0, 13.0 } },
			{ TEXT("wheel_a3_r"), 0, { 46.1666667, 100.0, 13.0 } },
			{ TEXT("wheel_a4_l"), 0, { 0.0, -100.0, 13.0 } },
			{ TEXT("wheel_a4_r"), 0, { 0.0, 100.0, 13.0 } },
			{ TEXT("wheel_a5_l"), 0, { -46.1666667, -100.0, 13.0 } },
			{ TEXT("wheel_a5_r"), 0, { -46.1666667, 100.0, 13.0 } },
			{ TEXT("wheel_a6_l"), 0, { -92.3333333, -100.0, 13.0 } },
			{ TEXT("wheel_a6_r"), 0, { -92.3333333, 100.0, 13.0 } },
			{ TEXT("wheel_a7_l"), 0, { -138.5, -100.0, 13.0 } },
			{ TEXT("wheel_a7_r"), 0, { -138.5, 100.0, 13.0 } }
		},
		{
			{ 138.5, -100.0, 13.0 },
			{ 138.5, 100.0, 13.0 },
			{ 92.3333333, -100.0, 13.0 },
			{ 92.3333333, 100.0, 13.0 },
			{ 46.1666667, -100.0, 13.0 },
			{ 46.1666667, 100.0, 13.0 },
			{ 0.0, -100.0, 13.0 },
			{ 0.0, 100.0, 13.0 },
			{ -46.1666667, -100.0, 13.0 },
			{ -46.1666667, 100.0, 13.0 },
			{ -92.3333333, -100.0, 13.0 },
			{ -92.3333333, 100.0, 13.0 },
			{ -138.5, -100.0, 13.0 },
			{ -138.5, 100.0, 13.0 }
		}
	},
	{
		TEXT("compact"),
		TEXT(R"MCS({"mass":1396,"maxSpeed":5556,"dragCoefficient":0.29,"frontalArea":2.2,"engine":{"maxTorque":240,"maxRPM":6500,"idleRPM":750,"torqueCurve":[[0,0.55],[1000,0.75],[1700,1],[4500,1],[6000,0.89],[6500,0.8]],"tractionControl":true,"engineBraking":0.064},"transmission":{"gearRatios":[2.53,1.867,1.378,1.017,0.751,0.554,0.41],"reverseRatio":2.0,"finalDrive":4.32,"changeUpRPM":6000,"changeDownRPM":1800,"shiftTime":0.05},"suspension":{"maxRaise":10,"maxDrop":16,"springRate":135,"springPreload":50,"damping":0.51},"steering":{"maxAngle":38,"ackermann":true},"brakes":{"brakeTorque":4000,"handbrakeTorque":5500,"abs":true,"frontShare":0.65},"tires":{"friction":1.29},"camera":{"armLength":400,"height":150},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":true,"handbrake":false,"radius":31.7,"width":23},{"bone":"wheel_fr","steering":true,"driven":true,"handbrake":false,"radius":31.7,"width":23},{"bone":"wheel_rl","steering":false,"driven":false,"handbrake":true,"radius":31.7,"width":23},{"bone":"wheel_rr","steering":false,"driven":false,"handbrake":true,"radius":31.7,"width":23}],"collision":[{"center":[0.0,0.0,46.0],"extent":[227.5,90.0,33.0]}],"centerOfMass":[30,0,46],"seat":{"location":[0,-36,74],"steeringWheel":{"location":[46.8,-36,77],"rotation":[-20.0,0.0,0.0],"radius":18}},"passengerSeat":{"location":[0,36,74]}})MCS"),
		320, 204,
		{ -227.5, -90.0, 0.0 }, { 227.5, 90.0, 79.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 136.8, -77.5, 31.7 } },
			{ TEXT("wheel_fr"), 0, { 136.8, 77.5, 31.7 } },
			{ TEXT("wheel_rl"), 0, { -136.8, -77.5, 31.7 } },
			{ TEXT("wheel_rr"), 0, { -136.8, 77.5, 31.7 } }
		},
		{
			{ 136.8, -77.5, 31.7 },
			{ 136.8, 77.5, 31.7 },
			{ -136.8, -77.5, 31.7 },
			{ -136.8, 77.5, 31.7 }
		}
	},
	{
		TEXT("drift"),
		TEXT(R"MCS({"mass":1293,"maxSpeed":6250,"dragCoefficient":0.28,"frontalArea":1.9,"engine":{"maxTorque":250,"maxRPM":7400,"idleRPM":800,"torqueCurve":[[0,0.6],[1000,0.7],[2500,0.9],[3700,1],[6000,0.96],[7000,0.93],[7400,0.85]],"tractionControl":false,"engineBraking":0.044},"transmission":{"gearRatios":[3.54,2.06,1.4,1.0,0.71,0.58],"reverseRatio":3.17,"finalDrive":3.9,"changeUpRPM":7000,"changeDownRPM":2500,"shiftTime":0.3},"suspension":{"maxRaise":8,"maxDrop":10,"springRate":240,"springPreload":70,"damping":0.52},"steering":{"maxAngle":55,"ackermann":true},"brakes":{"brakeTorque":4500,"handbrakeTorque":8000,"abs":false,"frontShare":0.75},"tires":{"friction":1.47},"camera":{"armLength":400,"height":140},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":31.5,"width":22},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":31.5,"width":22},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":31.5,"width":22},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":31.5,"width":22}],"collision":[{"center":[0.0,0.0,41.0],"extent":[213.0,89.0,28.0]}],"centerOfMass":[8,0,40],"seat":{"location":[-25,-35,56],"steeringWheel":{"location":[21.8,-35,59],"rotation":[-20.0,0.0,0.0],"radius":18}},"passengerSeat":{"location":[-25,35,56]}})MCS"),
		320, 204,
		{ -213.0, -89.0, 0.0 }, { 213.0, 89.0, 69.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 128.75, -77.0, 31.5 } },
			{ TEXT("wheel_fr"), 0, { 128.75, 77.0, 31.5 } },
			{ TEXT("wheel_rl"), 0, { -128.75, -77.0, 31.5 } },
			{ TEXT("wheel_rr"), 0, { -128.75, 77.0, 31.5 } }
		},
		{
			{ 128.75, -77.0, 31.5 },
			{ 128.75, 77.0, 31.5 },
			{ -128.75, -77.0, 31.5 },
			{ -128.75, 77.0, 31.5 }
		}
	},
	{
		TEXT("muscle"),
		TEXT(R"MCS({"mass":1915,"maxSpeed":7861,"dragCoefficient":0.35,"frontalArea":2.3,"engine":{"maxTorque":644,"maxRPM":6400,"idleRPM":700,"torqueCurve":[[0,0.6],[1000,0.72],[1500,0.8],[2500,0.9],[4200,1],[6100,0.879],[6400,0.82]],"tractionControl":true,"engineBraking":0.1},"transmission":{"gearRatios":[4.71,3.14,2.1,1.67,1.29,1.0,0.84,0.67],"reverseRatio":3.53,"finalDrive":3.056,"changeUpRPM":6100,"changeDownRPM":2000,"shiftTime":0.3},"suspension":{"maxRaise":10,"maxDrop":16,"springRate":186,"springPreload":60,"damping":0.69},"steering":{"maxAngle":37,"ackermann":true},"brakes":{"brakeTorque":7200,"handbrakeTorque":7000,"abs":true,"frontShare":0.65},"tires":{"friction":1.4},"camera":{"armLength":440,"height":150},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":36,"width":25},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":36,"width":25},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":36,"width":25},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":36,"width":25}],"collision":[{"center":[0.0,0.0,52.0],"extent":[251.0,96.0,36.0]}],"centerOfMass":[13,0,50],"seat":{"location":[-20,-40,74],"steeringWheel":{"location":[26.8,-40,77],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[-20,40,74]}})MCS"),
		320, 204,
		{ -251.0, -96.0, 0.0 }, { 251.0, 96.0, 88.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 147.5, -81.0, 36.0 } },
			{ TEXT("wheel_fr"), 0, { 147.5, 81.0, 36.0 } },
			{ TEXT("wheel_rl"), 0, { -147.5, -81.0, 36.0 } },
			{ TEXT("wheel_rr"), 0, { -147.5, 81.0, 36.0 } }
		},
		{
			{ 147.5, -81.0, 36.0 },
			{ 147.5, 81.0, 36.0 },
			{ -147.5, -81.0, 36.0 },
			{ -147.5, 81.0, 36.0 }
		}
	},
	{
		TEXT("supercar"),
		TEXT(R"MCS({"mass":1553,"maxSpeed":9028,"dragCoefficient":0.39,"frontalArea":1.9,"engine":{"maxTorque":600,"maxRPM":8500,"idleRPM":1000,"torqueCurve":[[0,0.5],[1000,0.6],[3000,0.8],[6500,1],[8000,0.937],[8500,0.85]],"tractionControl":true,"engineBraking":0.021},"transmission":{"gearRatios":[3.13,2.43,1.88,1.5,1.21,0.98,0.79],"reverseRatio":2.9,"finalDrive":4.11,"changeUpRPM":8000,"changeDownRPM":3000,"shiftTime":0.1},"suspension":{"maxRaise":8,"maxDrop":10,"springRate":289,"springPreload":80,"damping":0.63},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":7100,"handbrakeTorque":7000,"abs":true,"frontShare":0.65},"tires":{"friction":1.58},"camera":{"armLength":400,"height":130},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":true,"handbrake":false,"radius":34,"width":30},{"bone":"wheel_fr","steering":true,"driven":true,"handbrake":false,"radius":34,"width":30},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":34,"width":30},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":34,"width":30}],"collision":[{"center":[0.0,0.0,37.0],"extent":[226.0,96.5,26.0]}],"centerOfMass":[-18,0,38],"seat":{"location":[-10,-38,50],"steeringWheel":{"location":[36.8,-38,53],"rotation":[-20.0,0.0,0.0],"radius":17}},"passengerSeat":{"location":[-10,38,50]}})MCS"),
		320, 204,
		{ -226.0, -98.0, 0.0 }, { 226.0, 98.0, 68.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 131.0, -83.0, 34.0 } },
			{ TEXT("wheel_fr"), 0, { 131.0, 83.0, 34.0 } },
			{ TEXT("wheel_rl"), 0, { -131.0, -83.0, 34.0 } },
			{ TEXT("wheel_rr"), 0, { -131.0, 83.0, 34.0 } }
		},
		{
			{ 131.0, -83.0, 34.0 },
			{ 131.0, 83.0, 34.0 },
			{ -131.0, -83.0, 34.0 },
			{ -131.0, 83.0, 34.0 }
		}
	},
	{
		// A helicopter's wheels are not drawn: its body is sixteen boxes (the cabin, the mast, the boom, the fin, two skids, a leg at each skid end, two
		// cross-tubes, and two blades on each rotor), its collision the cabin, the boom and the fin.
		TEXT("helicopter"),
		TEXT(R"MCS({"mass":2193,"maxSpeed":8300,"dragCoefficient":0.4,"frontalArea":2.5,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":6,"maxDrop":6,"springRate":800,"springPreload":250,"damping":0.9},"steering":{"maxAngle":10,"ackermann":false},"brakes":{"brakeTorque":3000,"handbrakeTorque":6000},"tires":{"friction":1.47},"camera":{"armLength":1600,"height":250},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":20,"width":10}],"collision":[{"center":[40.0,0.0,165.0],"extent":[210.0,75.0,95.0]},{"center":[-430.0,0.0,215.0],"extent":[260.0,20.0,22.5]},{"center":[-655.0,0.0,292.5],"extent":[35.0,4.375,55.0]}],"centerOfMass":[0,0,140],"seat":{"location":[130,0,243]}})MCS"),
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
		// The light helicopter (#380, a Robinson R44): the same sixteen boxes on a smaller cabin, boom, fin and rotors.
		TEXT("lighthelicopter"),
		TEXT(R"MCS({"mass":1059,"maxSpeed":7700,"dragCoefficient":0.4,"frontalArea":1.9,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":6,"maxDrop":6,"springRate":400,"springPreload":120,"damping":0.9},"steering":{"maxAngle":10,"ackermann":false},"brakes":{"brakeTorque":3000,"handbrakeTorque":6000},"tires":{"friction":1.47},"camera":{"armLength":1300,"height":220},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":20,"width":10},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":20,"width":10}],"collision":[{"center":[40.0,0.0,130.0],"extent":[150.0,64.0,75.0]},{"center":[-369.0,0.0,150.0],"extent":[259.0,15.0,17.5]},{"center":[-600.5,0.0,207.5],"extent":[27.5,3.4375,40.0]}],"centerOfMass":[0,0,110],"seat":{"location":[70,0,190]}})MCS"),
		384, 192,
		{ -674.0, -503.0, 5.0 }, { 503.0, 503.0, 303.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 110.0, -109.0, 20.0 } },
			{ TEXT("wheel_fr"), 0, { 110.0, 109.0, 20.0 } },
			{ TEXT("wheel_rl"), 0, { -110.0, -109.0, 20.0 } },
			{ TEXT("wheel_rr"), 0, { -110.0, 109.0, 20.0 } },
			{ TEXT("main_rotor"), 0, { 0.0, 0.0, 300.0 } },
			{ TEXT("tail_rotor"), 0, { -600.5, -25.0, 150.0 } }
		},
		{
			{ 110.0, -109.0, 20.0 },
			{ 110.0, 109.0, 20.0 },
			{ -110.0, -109.0, 20.0 },
			{ -110.0, 109.0, 20.0 }
		}
	},
	{
		// A hovercraft's wheels are where its pads lift and are not drawn: its body is four boxes (the hull, the skirt under it, the fan's pod behind it
		// and the fin on its top), its collision the hull alone.
		TEXT("hovercraft"),
		TEXT(R"MCS({"mass":600,"maxSpeed":2600,"dragCoefficient":0.5,"frontalArea":2,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":10,"maxDrop":10,"springRate":400,"springPreload":100,"damping":0.5},"steering":{"maxAngle":10,"ackermann":false},"brakes":{"brakeTorque":1500,"handbrakeTorque":3000},"tires":{"friction":2},"camera":{"armLength":500,"height":170},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10}],"collision":[{"center":[0.0,0.0,50.0],"extent":[200.0,110.0,30.0]}],"centerOfMass":[0,0,40],"seat":{"location":[30,-40,86],"steeringWheel":{"location":[76.8,-40,89],"rotation":[-20.0,0.0,0.0],"radius":18}},"passengerSeat":{"location":[30,40,86]}})MCS"),
		96, 48,
		{ -240.0, -120.0, 8.0 }, { 210.0, 120.0, 180.0 },
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
		// A boat's (#15) wheels are hidden and drive nothing (the document's reader needs a steered and a driven one): its body is the hull's box and its
		// drive's pod, its collision the hull alone.
		TEXT("boat"),
		TEXT(R"MCS({"mass":1516,"maxSpeed":2100,"dragCoefficient":0.5,"frontalArea":2.5,"engine":{"maxTorque":300,"maxRPM":6000,"idleRPM":700,"torqueCurve":[[0,0.6],[3000,1],[6000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":5000,"changeDownRPM":2000},"suspension":{"maxRaise":10,"maxDrop":10,"springRate":400,"springPreload":100,"damping":0.5},"steering":{"maxAngle":10,"ackermann":false},"brakes":{"brakeTorque":1500,"handbrakeTorque":3000},"tires":{"friction":2},"camera":{"armLength":650,"height":200},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10}],"collision":[{"center":[0.0,0.0,60.0],"extent":[309.5,122.0,50.0]}],"centerOfMass":[-40,0,60],"seat":{"location":[20,-45,75],"steeringWheel":{"location":[62,-45,95],"rotation":[-20.0,0.0,0.0],"radius":18}},"passengerSeat":{"location":[20,45,75]}})MCS"),
		48, 24,
		{ -354.4, -122.0, -44.4 }, { 309.5, 122.0, 110.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 200.0, -90.0, 20.0 } },
			{ TEXT("wheel_fr"), 0, { 200.0, 90.0, 20.0 } },
			{ TEXT("wheel_rl"), 0, { -200.0, -90.0, 20.0 } },
			{ TEXT("wheel_rr"), 0, { -200.0, 90.0, 20.0 } }
		},
		{
			{ 200.0, -90.0, 20.0 },
			{ 200.0, 90.0, 20.0 },
			{ -200.0, -90.0, 20.0 },
			{ -200.0, 90.0, 20.0 }
		}
	},
	{
		// A jet ski (#15): a boat's hidden wheels, its hull, its jet's pod past the stern, and the handlebar bone at the steering wheel hub.
		TEXT("jetski"),
		TEXT(R"MCS({"mass":400,"maxSpeed":3100,"dragCoefficient":0.5,"frontalArea":1.2,"engine":{"maxTorque":300,"maxRPM":8000,"idleRPM":1700,"torqueCurve":[[0,0.59999999999999998],[4000,1],[8000,0.80000000000000004]]},"transmission":{"gearRatios":[1],"reverseRatio":1,"finalDrive":1,"changeUpRPM":7000,"changeDownRPM":3000},"suspension":{"maxRaise":10,"maxDrop":10,"springRate":400,"springPreload":100,"damping":0.5},"steering":{"maxAngle":10,"ackermann":false},"brakes":{"brakeTorque":1500,"handbrakeTorque":3000},"tires":{"friction":2},"camera":{"armLength":450,"height":160},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":20,"width":10}],"collision":[{"center":[0,0,45],"extent":[166,62.5,35]}],"centerOfMass":[-30,0,40],"seat":{"location":[-30,0,88],"steeringWheel":{"location":[55,0,100],"rotation":[-65,0,0],"radius":10,"bone":"handlebar"}},"passengerSeat":{"location":[-75,0,93]}})MCS"),
		96, 48,
		{ -172.5, -62.5, -22.5 }, { 166, 62.5, 105.5 },
		{
			{ TEXT("root"), -1, { 0, 0, 0 } },
			{ TEXT("wheel_fl"), 0, { 110, -40, 20 } },
			{ TEXT("wheel_fr"), 0, { 110, 40, 20 } },
			{ TEXT("wheel_rl"), 0, { -110, -40, 20 } },
			{ TEXT("wheel_rr"), 0, { -110, 40, 20 } },
			{ TEXT("handlebar"), 0, { 55, 0, 100 } }
		},
		{
			{ 110, -40, 20 },
			{ 110, 40, 20 },
			{ -110, -40, 20 },
			{ -110, 40, 20 }
		}
	},
	{
		TEXT("jet"),
		TEXT(R"MCS({"mass":10355,"maxSpeed":12000,"dragCoefficient":0.6,"frontalArea":3.1,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":10,"maxDrop":15,"springRate":2360,"springPreload":670,"damping":0.85},"steering":{"maxAngle":25,"ackermann":false},"brakes":{"brakeTorque":5000,"handbrakeTorque":5000},"tires":{"friction":1.47},"camera":{"armLength":2150,"height":150},"schemaVersion":1,"wheels":[{"bone":"wheel_f","steering":true,"driven":false,"handbrake":false,"radius":40,"width":26},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":40,"width":26},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":40,"width":26}],"collision":[{"center":[-200.0,0.0,200.0],"extent":[750.0,60.0,90.0]},{"center":[-288.75,0.0,200.0],"extent":[147.5,472.5,18.4375]},{"center":[-870.0,0.0,215.0],"extent":[70.0,250.0,8.75]},{"center":[-850.0,0.0,440.0],"extent":[100.0,12.5,150.0]}],"centerOfMass":[-200,0,205],"seat":{"location":[250,0,273]}})MCS"),
		414, 240,
		{ -1000.0, -472.5, 0.0 }, { 550.0, 472.5, 590.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_f"), 0, { 260.0, 0.0, 40.0 } },
			{ TEXT("wheel_rl"), 0, { -260.0, -140.0, 40.0 } },
			{ TEXT("wheel_rr"), 0, { -260.0, 140.0, 40.0 } },
			{ TEXT("aileron_l"), 0, { -215.0, -236.25, 200.0 } },
			{ TEXT("aileron_r"), 0, { -215.0, 236.25, 200.0 } },
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
		TEXT(R"MCS({"mass":2400,"maxSpeed":5694,"dragCoefficient":0.34,"frontalArea":2.9,"engine":{"maxTorque":389,"maxRPM":6000,"idleRPM":800,"torqueCurve":[[0,0.6],[1000,0.72],[2500,0.9],[4100,1],[4900,0.893],[6000,0.7]],"tractionControl":true,"engineBraking":0.145},"transmission":{"gearRatios":[2.84,1.55,1.0,0.7],"reverseRatio":2.32,"finalDrive":2.981,"changeUpRPM":4900,"changeDownRPM":1800,"shiftTime":0.3},"suspension":{"maxRaise":14,"maxDrop":16,"springRate":175,"springPreload":60,"damping":0.78},"steering":{"maxAngle":32,"ackermann":true},"brakes":{"brakeTorque":5000,"handbrakeTorque":6000,"abs":true,"frontShare":0.65},"tires":{"friction":1.14},"camera":{"armLength":520,"height":180},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":32,"width":21},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":32,"width":21}],"collision":[{"center":[0.0,0.0,56.0],"extent":[320.0,95.0,36.0]}],"centerOfMass":[23,0,46],"seat":{"location":[89,-36,76],"steeringWheel":{"location":[135.8,-36,79],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[89,36,76]}})MCS"),
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
		TEXT(R"MCS({"mass":265,"maxSpeed":8620,"dragCoefficient":0.55,"frontalArea":0.535,"engine":{"displacement":599,"maxTorque":61.7,"maxRPM":16500,"idleRPM":1300,"torqueCurve":[[0,0.85],[8700,0.85],[10500,1],[14500,0.93],[16500,0.79]],"tractionControl":true,"engineBraking":0.0084},"transmission":{"gearRatios":[5.355,4.146,3.456,2.993,2.666,2.384],"reverseRatio":1.18,"finalDrive":3.126,"changeUpRPM":14500,"changeDownRPM":9000,"shiftTime":0.15},"suspension":{"maxRaise":8.1,"maxDrop":10.0,"springRate":180,"springPreload":50,"damping":0.38},"steering":{"maxAngle":40,"ackermann":false},"brakes":{"brakeTorque":370,"handbrakeTorque":185,"abs":true,"frontShare":0.75},"tires":{"friction":2.0},"camera":{"armLength":380,"height":130},"schemaVersion":1,"wheels":[{"bone":"wheel_f","steering":true,"driven":false,"handbrake":false,"radius":35,"width":30},{"bone":"wheel_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":30}],"collision":[{"center":[0.0,0.0,57.5],"extent":[90.0,20.0,22.5]}],"centerOfMass":[0,0,38],"seat":{"location":[-1.4,0,89.4]}})MCS"),
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
		TEXT(R"MCS({"mass":1082,"maxSpeed":9200,"dragCoefficient":0.13,"frontalArea":1.25,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":10,"maxDrop":15,"springRate":250,"springPreload":70,"damping":0.85},"steering":{"maxAngle":25,"ackermann":false},"brakes":{"brakeTorque":300,"handbrakeTorque":300},"tires":{"friction":1.47},"camera":{"armLength":1300,"height":150},"schemaVersion":1,"wheels":[{"bone":"wheel_f","steering":true,"driven":false,"handbrake":false,"radius":25,"width":16},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":25,"width":16},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":25,"width":16}],"collision":[{"center":[-241.0,0.0,155.0],"extent":[415.0,55.0,75.0]},{"center":[-152.75,0.0,225.0],"extent":[73.5,550.0,9.1875]},{"center":[-607.0,0.0,150.0],"extent":[44.0,170.0,5.5]},{"center":[-588.25,0.0,305.0],"extent":[56.5,7.0625,75.0]}],"centerOfMass":[-104,0,170],"seat":{"location":[-40,0,190]}})MCS"),
		390, 228,
		{ -656.0, -550.0, 0.0 }, { 183.0, 550.0, 380.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_f"), 0, { 137.0, 0.0, 25.0 } },
			{ TEXT("wheel_rl"), 0, { -137.0, -125.0, 25.0 } },
			{ TEXT("wheel_rr"), 0, { -137.0, 125.0, 25.0 } },
			{ TEXT("aileron_l"), 0, { -116.0, -275.0, 225.0 } },
			{ TEXT("aileron_r"), 0, { -116.0, 275.0, 225.0 } },
			{ TEXT("elevator"), 0, { -585.0, 0.0, 150.0 } },
			{ TEXT("rudder"), 0, { -560.0, 0.0, 305.0 } },
			{ TEXT("propeller"), 0, { 180.0, 0.0, 155.0 } }
		},
		{
			{ 137.0, 0.0, 25.0 },
			{ -137.0, -125.0, 25.0 },
			{ -137.0, 125.0, 25.0 }
		}
	},
	{
		TEXT("pickup"),
		TEXT(R"MCS({"mass":2141,"maxSpeed":4611,"dragCoefficient":0.4,"frontalArea":3,"engine":{"maxTorque":542,"maxRPM":6000,"idleRPM":800,"torqueCurve":[[0,0.55],[1000,0.7],[2750,1],[3500,1],[5000,0.854],[6000,0.7]],"tractionControl":true,"engineBraking":0.155},"transmission":{"gearRatios":[4.696,2.985,2.146,1.52,1.0,0.854,0.689,0.636],"reverseRatio":4.04,"finalDrive":3.276,"changeUpRPM":5000,"changeDownRPM":1800,"shiftTime":0.3},"suspension":{"maxRaise":20,"maxDrop":25,"springRate":205,"springPreload":100,"damping":0.72},"steering":{"maxAngle":40,"ackermann":true},"brakes":{"brakeTorque":5000,"handbrakeTorque":6000,"abs":true,"frontShare":0.65},"tires":{"friction":1.21},"camera":{"armLength":550,"height":200},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":37,"width":26},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":37,"width":26},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":37,"width":26},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":37,"width":26}],"collision":[{"center":[0.0,0.0,75.0],"extent":[270.0,97.5,39.0]}],"centerOfMass":[22,0,54],"seat":{"location":[-5,-38,100],"steeringWheel":{"location":[41.8,-38,103],"rotation":[-20.0,0.0,0.0],"radius":20}},"passengerSeat":{"location":[-5,38,100]}})MCS"),
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
		TEXT("offroad"),
		TEXT(R"MCS({"mass":2097,"maxSpeed":4917,"dragCoefficient":0.45,"frontalArea":3,"engine":{"maxTorque":353,"maxRPM":6600,"idleRPM":700,"torqueCurve":[[0,0.55],[1000,0.7],[2500,0.85],[4800,1],[6400,0.9],[6600,0.85]],"tractionControl":true,"engineBraking":0.052},"transmission":{"gearRatios":[4.71,3.13,2.1,1.67,1.28,1.0,0.84,0.67],"reverseRatio":3.3,"finalDrive":4.1,"changeUpRPM":6400,"changeDownRPM":2000,"shiftTime":0.3,"lowRange":4.0,"differentials":{"front":{"type":"selectable"},"rear":{"type":"selectable"}}},"suspension":{"maxRaise":20,"maxDrop":18,"springRate":171,"springPreload":100,"damping":0.66},"steering":{"maxAngle":41,"ackermann":true},"brakes":{"brakeTorque":6100,"handbrakeTorque":6000,"abs":true,"frontShare":0.65},"tires":{"friction":1},"camera":{"armLength":510,"height":200},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":true,"handbrake":false,"radius":41.5,"width":29},{"bone":"wheel_fr","steering":true,"driven":true,"handbrake":false,"radius":41.5,"width":29},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":41.5,"width":29},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":41.5,"width":29}],"collision":[{"center":[0.0,0.0,77.5],"extent":[239.0,94.0,37.5]}],"centerOfMass":[9,0,70],"seat":{"location":[-10,-40,102],"steeringWheel":{"location":[36.8,-40,105],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[-10,40,102]}})MCS"),
		320, 204,
		{ -239.0, -96.5, 0.0 }, { 239.0, 96.5, 115.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 150.5, -82.0, 41.5 } },
			{ TEXT("wheel_fr"), 0, { 150.5, 82.0, 41.5 } },
			{ TEXT("wheel_rl"), 0, { -150.5, -82.0, 41.5 } },
			{ TEXT("wheel_rr"), 0, { -150.5, 82.0, 41.5 } }
		},
		{
			{ 150.5, -82.0, 41.5 },
			{ 150.5, 82.0, 41.5 },
			{ -150.5, -82.0, 41.5 },
			{ -150.5, 82.0, 41.5 }
		}
	},
	{
		TEXT("atv"),
		TEXT(R"MCS({"mass":330,"maxSpeed":2500,"dragCoefficient":0.9,"frontalArea":1,"engine":{"maxTorque":52,"maxRPM":7000,"idleRPM":1200,"torqueCurve":[[0,0.6],[2500,0.8],[5500,1],[6500,0.927],[7000,0.8]],"tractionControl":false,"engineBraking":0.007},"transmission":{"gearRatios":[1.2],"reverseRatio":2.346,"finalDrive":9.34,"lowRange":2.15,"cvt":{"lowRatio":3.1,"highRatio":0.75,"shiftRPM":6500}},"suspension":{"maxRaise":10,"maxDrop":10,"springRate":72,"springPreload":20,"damping":0.19},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":660,"handbrakeTorque":600,"abs":false,"frontShare":0.6},"tires":{"friction":0.9},"camera":{"armLength":260,"height":155},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":true,"handbrake":false,"radius":32,"width":23},{"bone":"wheel_fr","steering":true,"driven":true,"handbrake":false,"radius":32,"width":23},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":32,"width":23},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":32,"width":23}],"collision":[{"center":[0.0,0.0,56.5],"extent":[105.5,55.0,27.5]}],"centerOfMass":[-6,0,42],"seat":{"location":[-20,0,79]}})MCS"),
		320, 204,
		{ -105.5, -60.0, 0.0 }, { 105.5, 60.0, 84.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 64.0, -48.5, 32.0 } },
			{ TEXT("wheel_fr"), 0, { 64.0, 48.5, 32.0 } },
			{ TEXT("wheel_rl"), 0, { -64.0, -48.5, 32.0 } },
			{ TEXT("wheel_rr"), 0, { -64.0, 48.5, 32.0 } },
			{ TEXT("handlebar"), 0, { 30.0, 0.0, 108.0 } }
		},
		{
			{ 64.0, -48.5, 32.0 },
			{ 64.0, 48.5, 32.0 },
			{ -64.0, -48.5, 32.0 },
			{ -64.0, 48.5, 32.0 }
		}
	},
	{
		// #445: the Electric preset, a rear-drive saloon with a motor (no idle, torque curve or engine braking; one gear, no shift points).
		TEXT("electric"),
		TEXT(R"MCS({"mass":1765,"maxSpeed":5588,"dragCoefficient":0.219,"frontalArea":2.22,"engine":{"drive":"electric","maxTorque":377,"maxRPM":18447,"peakPower":213,"regenBraking":0.2,"batteryCapacity":70,"tractionControl":true},"transmission":{"gearRatios":[1.0],"finalDrive":8.92},"suspension":{"maxRaise":10,"maxDrop":16,"springRate":171,"springPreload":50,"damping":0.57},"steering":{"maxAngle":40,"ackermann":true},"brakes":{"brakeTorque":4900,"handbrakeTorque":6000,"abs":true,"frontShare":0.65},"tires":{"friction":1.33},"camera":{"armLength":420,"height":160},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":33,"width":24},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":33,"width":24},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":33,"width":24},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":33,"width":24}],"collision":[{"center":[0.0,0.0,55.0],"extent":[236.0,92.5,35.0]}],"centerOfMass":[-9,0,42],"seat":{"location":[0,-36,76],"steeringWheel":{"location":[46.8,-36,79],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[0,36,76]}})MCS"),
		320, 204,
		{ -236.0, -92.5, 0.0 }, { 236.0, 92.5, 90.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 144.0, -79.0, 33.0 } },
			{ TEXT("wheel_fr"), 0, { 144.0, 79.0, 33.0 } },
			{ TEXT("wheel_rl"), 0, { -144.0, -79.0, 33.0 } },
			{ TEXT("wheel_rr"), 0, { -144.0, 79.0, 33.0 } }
		},
		{
			{ 144.0, -79.0, 33.0 },
			{ 144.0, 79.0, 33.0 },
			{ -144.0, -79.0, 33.0 },
			{ -144.0, 79.0, 33.0 }
		}
	},
	{
		TEXT("bus"),
		TEXT(R"MCS({"mass":12587,"maxSpeed":2917,"dragCoefficient":0.65,"frontalArea":7.5,"engine":{"maxTorque":1356,"maxRPM":2200,"idleRPM":650,"torqueCurve":[[0,0.6],[800,0.8],[1400,1],[2000,0.866],[2200,0.75]],"tractionControl":true,"engineBraking":2.1},"transmission":{"gearRatios":[3.51,1.91,1.43,1.0,0.74,0.64],"reverseRatio":4.8,"finalDrive":4.88,"changeUpRPM":2000,"changeDownRPM":1100,"shiftTime":0.5},"suspension":{"maxRaise":15,"maxDrop":18,"springRate":1000,"springPreload":300,"damping":3.9},"steering":{"maxAngle":46,"ackermann":true},"brakes":{"brakeTorque":7400,"handbrakeTorque":16000,"abs":true,"frontShare":0.3},"tires":{"friction":1.16},"camera":{"armLength":1235,"height":310},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":50,"width":31},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":50,"width":31},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":50,"width":31},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":50,"width":31}],"collision":[{"center":[0.0,0.0,92.5],"extent":[612.0,129.5,67.5]}],"centerOfMass":[-108,0,105],"seat":{"location":[470,-80,120],"steeringWheel":{"location":[515,-80,128],"rotation":[-20.0,0.0,0.0],"radius":25}},"passengerSeat":{"location":[470,80,120]}})MCS"),
		320, 204,
		{ -612.0, -129.5, 0.0 }, { 612.0, 129.5, 160.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_fl"), 0, { 360.5, -105.0, 50.0 } },
			{ TEXT("wheel_fr"), 0, { 360.5, 105.0, 50.0 } },
			{ TEXT("wheel_rl"), 0, { -360.5, -105.0, 50.0 } },
			{ TEXT("wheel_rr"), 0, { -360.5, 105.0, 50.0 } }
		},
		{
			{ 360.5, -105.0, 50.0 },
			{ 360.5, 105.0, 50.0 },
			{ -360.5, -105.0, 50.0 },
			{ -360.5, 105.0, 50.0 }
		}
	},
	{
		TEXT("sedan"),
		TEXT(R"MCS({"mass":1546,"maxSpeed":5944,"dragCoefficient":0.31,"frontalArea":2.6,"engine":{"maxTorque":249,"maxRPM":7000,"idleRPM":900,"torqueCurve":[[0,0.6],[1000,0.7],[2500,0.88],[4800,1],[6600,0.877],[7000,0.8]],"tractionControl":true,"engineBraking":0.08},"transmission":{"gearRatios":[5.25,3.028,1.95,1.456,1.22,1.0,0.808,0.673],"reverseRatio":4.014,"finalDrive":2.685,"changeUpRPM":6600,"changeDownRPM":2000,"shiftTime":0.3},"suspension":{"maxRaise":10,"maxDrop":16,"springRate":150,"springPreload":50,"damping":0.57},"steering":{"maxAngle":40,"ackermann":true},"brakes":{"brakeTorque":4300,"handbrakeTorque":6000,"abs":true,"frontShare":0.65},"tires":{"friction":1.25},"camera":{"armLength":420,"height":160},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":true,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_fr","steering":true,"driven":true,"handbrake":false,"radius":32,"width":21},{"bone":"wheel_rl","steering":false,"driven":false,"handbrake":true,"radius":32,"width":21},{"bone":"wheel_rr","steering":false,"driven":false,"handbrake":true,"radius":32,"width":21}],"collision":[{"center":[0.0,0.0,55.0],"extent":[235.0,91.0,35.0]}],"centerOfMass":[25,0,46],"seat":{"location":[0,-36,76],"steeringWheel":{"location":[46.8,-36,79],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[0,36,76]}})MCS"),
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
		TEXT(R"MCS({"mass":9494,"maxSpeed":2472,"dragCoefficient":0.7,"frontalArea":7,"engine":{"maxTorque":1010,"maxRPM":2400,"idleRPM":600,"torqueCurve":[[0,0.6],[800,0.75],[1500,1],[2100,0.806],[2400,0.65]],"tractionControl":true,"engineBraking":0.5},"transmission":{"gearRatios":[6.0,3.6,2.3,1.6,1.0],"reverseRatio":5.5,"finalDrive":4.43,"changeUpRPM":2100,"changeDownRPM":1100,"shiftTime":0.5},"suspension":{"maxRaise":15,"maxDrop":22,"springRate":520,"springPreload":300,"damping":1.7},"steering":{"maxAngle":30,"ackermann":true},"brakes":{"brakeTorque":10800,"handbrakeTorque":12000,"abs":true,"frontShare":0.45},"tires":{"friction":1.02},"camera":{"armLength":900,"height":320},"schemaVersion":1,"wheels":[{"bone":"wheel_a1_l","steering":true,"driven":false,"handbrake":false,"radius":46,"width":30},{"bone":"wheel_a1_r","steering":true,"driven":false,"handbrake":false,"radius":46,"width":30},{"bone":"wheel_a2_l","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30},{"bone":"wheel_a2_r","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30},{"bone":"wheel_a3_l","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30},{"bone":"wheel_a3_r","steering":false,"driven":true,"handbrake":true,"radius":46,"width":30}],"collision":[{"center":[0.0,0.0,111.0],"extent":[410.0,112.5,55.0]}],"centerOfMass":[15,0,80],"seat":{"location":[178,-42,138],"steeringWheel":{"location":[224.8,-42,141],"rotation":[-20.0,0.0,0.0],"radius":22}},"passengerSeat":{"location":[178,42,138]}})MCS"),
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
		TEXT(R"MCS({"mass":1534,"maxSpeed":8528,"dragCoefficient":0.31,"frontalArea":2.0,"engine":{"maxTorque":529,"maxRPM":7500,"idleRPM":900,"torqueCurve":[[0,0.5],[1000,0.65],[2300,1],[5000,1],[6500,0.917],[7500,0.78]],"tractionControl":true,"engineBraking":0.07},"transmission":{"gearRatios":[4.89,3.17,2.15,1.56,1.18,0.94,0.76,0.61],"reverseRatio":3.96,"finalDrive":3.22,"changeUpRPM":6500,"changeDownRPM":2500,"shiftTime":0.1},"suspension":{"maxRaise":8,"maxDrop":10,"springRate":285,"springPreload":80,"damping":0.62},"steering":{"maxAngle":38,"ackermann":true},"brakes":{"brakeTorque":6400,"handbrakeTorque":7000,"abs":true,"frontShare":0.65},"tires":{"friction":1.5},"camera":{"armLength":400,"height":140},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":34,"width":28},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":34,"width":28},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":34,"width":28},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":34,"width":28}],"collision":[{"center":[0.0,0.0,43.0],"extent":[225.0,95.0,29.0]}],"centerOfMass":[-31,0,38],"seat":{"location":[-42,-37,58],"steeringWheel":{"location":[4.8,-37,61],"rotation":[-20.0,0.0,0.0],"radius":19}},"passengerSeat":{"location":[-42,37,58]}})MCS"),
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
		// The tank: as the bulldozer, with a turret on the hull's top in place of a blade.
		TEXT("tank"),
		TEXT(R"MCS({"mass":55150,"maxSpeed":1889,"dragCoefficient":0.9,"frontalArea":9,"engine":{"maxTorque":4700,"maxRPM":2700,"idleRPM":700,"torqueCurve":[[0,0.5],[700,0.6],[1600,1],[1700,1],[2600,0.86],[2700,0.8]],"tractionControl":false,"engineBraking":0.5},"transmission":{"gearRatios":[4.3,2.6,1.6,1.0],"reverseRatio":2.2,"finalDrive":5.0,"changeUpRPM":2400,"changeDownRPM":1400,"shiftTime":0.5},"suspension":{"maxRaise":25,"maxDrop":22,"springRate":1050,"springPreload":750,"damping":4.6},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":8100,"handbrakeTorque":10000,"abs":false},"tires":{"friction":1.18,"rollingResistance":0.04},"camera":{"armLength":1300,"height":450},"schemaVersion":1,"wheels":[{"bone":"wheel_a1_l","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a1_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a2_l","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a2_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a3_l","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a3_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a4_l","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a4_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a5_l","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a5_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a6_l","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a6_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a7_l","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60},{"bone":"wheel_a7_r","steering":false,"driven":true,"handbrake":true,"radius":35,"width":60}],"collision":[{"center":[0.0,0.0,109.0],"extent":[385.0,187.5,55.0]}],"centerOfMass":[-20,0,110],"seat":{"location":[250,-70,145]}})MCS"),
		1132, 720,
		{ -385.0, -187.5, 0.0 }, { 385.0, 187.5, 244.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_a1_l"), 0, { 245.0, -155.0, 35.0 } },
			{ TEXT("wheel_a1_r"), 0, { 245.0, 155.0, 35.0 } },
			{ TEXT("wheel_a2_l"), 0, { 163.3333333, -155.0, 35.0 } },
			{ TEXT("wheel_a2_r"), 0, { 163.3333333, 155.0, 35.0 } },
			{ TEXT("wheel_a3_l"), 0, { 81.6666667, -155.0, 35.0 } },
			{ TEXT("wheel_a3_r"), 0, { 81.6666667, 155.0, 35.0 } },
			{ TEXT("wheel_a4_l"), 0, { 0.0, -155.0, 35.0 } },
			{ TEXT("wheel_a4_r"), 0, { 0.0, 155.0, 35.0 } },
			{ TEXT("wheel_a5_l"), 0, { -81.6666667, -155.0, 35.0 } },
			{ TEXT("wheel_a5_r"), 0, { -81.6666667, 155.0, 35.0 } },
			{ TEXT("wheel_a6_l"), 0, { -163.3333333, -155.0, 35.0 } },
			{ TEXT("wheel_a6_r"), 0, { -163.3333333, 155.0, 35.0 } },
			{ TEXT("wheel_a7_l"), 0, { -245.0, -155.0, 35.0 } },
			{ TEXT("wheel_a7_r"), 0, { -245.0, 155.0, 35.0 } }
		},
		{
			{ 245.0, -155.0, 35.0 },
			{ 245.0, 155.0, 35.0 },
			{ 163.3333333, -155.0, 35.0 },
			{ 163.3333333, 155.0, 35.0 },
			{ 81.6666667, -155.0, 35.0 },
			{ 81.6666667, 155.0, 35.0 },
			{ 0.0, -155.0, 35.0 },
			{ 0.0, 155.0, 35.0 },
			{ -81.6666667, -155.0, 35.0 },
			{ -81.6666667, 155.0, 35.0 },
			{ -163.3333333, -155.0, 35.0 },
			{ -163.3333333, 155.0, 35.0 },
			{ -245.0, -155.0, 35.0 },
			{ -245.0, 155.0, 35.0 }
		}
	},
	{
		TEXT("truck"),
		TEXT(R"MCS({"mass":4000,"maxSpeed":4861,"dragCoefficient":0.6,"frontalArea":5,"engine":{"maxTorque":576,"maxRPM":5000,"idleRPM":600,"torqueCurve":[[0,0.6],[1000,0.75],[2500,0.9],[3800,1],[4500,0.962],[5000,0.85]],"tractionControl":true,"engineBraking":0.25},"transmission":{"gearRatios":[4.027,2.364,1.532,1.152,0.852,0.667],"reverseRatio":3.064,"finalDrive":4.459,"changeUpRPM":4500,"changeDownRPM":1200,"shiftTime":0.5},"suspension":{"maxRaise":15,"maxDrop":22,"springRate":375,"springPreload":200,"damping":1.35},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":8600,"handbrakeTorque":8000,"abs":true,"frontShare":0.65},"tires":{"friction":1.15},"camera":{"armLength":750,"height":280},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":42,"width":28},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":42,"width":28},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":42,"width":28},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":42,"width":28}],"collision":[{"center":[0.0,0.0,96.0],"extent":[320.0,102.5,48.0]}],"centerOfMass":[7,0,61],"seat":{"location":[73.2,-42,130],"steeringWheel":{"location":[120,-42,133],"rotation":[-20.0,0.0,0.0],"radius":21}},"passengerSeat":{"location":[73.2,42,130]}})MCS"),
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
		TEXT(R"MCS({"mass":2460,"maxSpeed":5556,"dragCoefficient":0.5,"frontalArea":4,"engine":{"maxTorque":542,"maxRPM":5500,"idleRPM":750,"torqueCurve":[[0,0.6],[1000,0.8],[1500,0.95],[2500,1],[4000,1],[5000,0.814],[5500,0.7]],"tractionControl":true,"engineBraking":0.135},"transmission":{"gearRatios":[4.171,2.34,1.521,1.143,0.867,0.691],"reverseRatio":3.403,"finalDrive":3.772,"changeUpRPM":5000,"changeDownRPM":1700,"shiftTime":0.3},"suspension":{"maxRaise":15,"maxDrop":22,"springRate":220,"springPreload":150,"damping":0.8},"steering":{"maxAngle":36,"ackermann":true},"brakes":{"brakeTorque":6400,"handbrakeTorque":7000,"abs":true,"frontShare":0.65},"tires":{"friction":1.08},"camera":{"armLength":600,"height":230},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":false,"radius":36,"width":24},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":false,"radius":36,"width":24},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":36,"width":24},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":36,"width":24}],"collision":[{"center":[0.0,0.0,90.0],"extent":[260.0,100.0,60.0]}],"centerOfMass":[10,0,65],"seat":{"location":[40,-38,104],"steeringWheel":{"location":[86.8,-38,107],"rotation":[-20.0,0.0,0.0],"radius":20}},"passengerSeat":{"location":[40,38,104]}})MCS"),
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
	{
		TEXT("warbird"),
		TEXT(R"MCS({"mass":4285,"maxSpeed":12000,"dragCoefficient":0.1,"frontalArea":0.8,"engine":{"maxTorque":300,"maxRPM":3000,"idleRPM":600,"torqueCurve":[[0,0.6],[1500,1],[3000,0.8]]},"transmission":{"gearRatios":[1.0],"reverseRatio":1.0,"finalDrive":1.0,"changeUpRPM":2500,"changeDownRPM":1000},"suspension":{"maxRaise":10,"maxDrop":15,"springRate":1000,"springPreload":280,"damping":0.85},"steering":{"maxAngle":25,"ackermann":false},"brakes":{"brakeTorque":4000,"handbrakeTorque":4000},"tires":{"friction":1.47},"camera":{"armLength":1800,"height":150},"schemaVersion":1,"wheels":[{"bone":"wheel_f","steering":true,"driven":false,"handbrake":false,"radius":40,"width":28},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":false,"radius":40,"width":28},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":false,"radius":40,"width":28}],"collision":[{"center":[-335.0,0.0,200.0],"extent":[575.0,55.0,80.0]},{"center":[-204.25,0.0,252.0],"extent":[96.5,564.0,12.0625]},{"center":[-853.75,0.0,195.0],"extent":[37.5,200.0,4.6875]},{"center":[-835.0,0.0,380.0],"extent":[75.0,9.375,100.0]}],"centerOfMass":[-144,0,210],"seat":{"location":[-75,0,263]}})MCS"),
		390, 228,
		{ -910.0, -564.0, 0.0 }, { 253.0, 564.0, 480.0 },
		{
			{ TEXT("root"), -1, { 0.0, 0.0, 0.0 } },
			{ TEXT("wheel_f"), 0, { 190.0, 0.0, 40.0 } },
			{ TEXT("wheel_rl"), 0, { -190.0, -160.0, 40.0 } },
			{ TEXT("wheel_rr"), 0, { -190.0, 160.0, 40.0 } },
			{ TEXT("aileron_l"), 0, { -156.0, -282.0, 252.0 } },
			{ TEXT("aileron_r"), 0, { -156.0, 282.0, 252.0 } },
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

	// #446: the semi truck (a cab and a frame under its fifth wheel) and both trailers (a body ahead of the axles, a semitrailer's landing gear).
	{
		TEXT("semi"),
		TEXT(R"MCS({"mass":8200,"maxSpeed":2917,"dragCoefficient":0.59999999999999998,"frontalArea":10,"engine":{"maxTorque":2237,"maxRPM":2000,"idleRPM":600,"torqueCurve":[[0,0.69999999999999996],[600,0.69999999999999996],[1000,0.94999999999999996],[1100,1],[1400,1],[1625,0.89000000000000001],[2000,0.59999999999999998]],"tractionControl":true,"engineBraking":0.5},"transmission":{"gearRatios":[7.4649999999999999,4.5650000000000004,2.7949999999999999,1.71,1.0449999999999999,0.81999999999999995,0.64000000000000001,0.5],"reverseRatio":8.1950000000000003,"finalDrive":4.5599999999999996,"changeUpRPM":1800,"changeDownRPM":1000,"shiftTime":0.5},"suspension":{"maxRaise":15,"maxDrop":22,"springRate":449,"springPreload":260,"damping":1.47},"steering":{"maxAngle":45,"ackermann":true},"brakes":{"brakeTorque":14200,"handbrakeTorque":13200,"abs":true,"frontShare":0.45000000000000001},"tires":{"friction":1.1499999999999999},"camera":{"armLength":900,"height":380},"schemaVersion":1,"wheels":[{"bone":"wheel_a1_l","steering":true,"driven":false,"handbrake":false,"radius":50.700000000000003,"width":30},{"bone":"wheel_a1_r","steering":true,"driven":false,"handbrake":false,"radius":50.700000000000003,"width":30},{"bone":"wheel_a2_l","steering":false,"driven":true,"handbrake":true,"radius":50.700000000000003,"width":30},{"bone":"wheel_a2_r","steering":false,"driven":true,"handbrake":true,"radius":50.700000000000003,"width":30},{"bone":"wheel_a3_l","steering":false,"driven":true,"handbrake":true,"radius":50.700000000000003,"width":30},{"bone":"wheel_a3_r","steering":false,"driven":true,"handbrake":true,"radius":50.700000000000003,"width":30}],"collision":[{"center":[147.875,0,180],"extent":[206.125,125,120]},{"center":[-162.125,0,84.5],"extent":[103.875,125,24.5]}],"centerOfMass":[59.5,0,100],"seat":{"location":[210,-55,190],"steeringWheel":{"location":[256.80000000000001,-55,193],"rotation":[-20,0,0],"radius":25}},"passengerSeat":{"location":[210,55,190]}})MCS"),
		516, 324,
		{ -304.9500122, -125, 0 }, { 354, 125, 300 },
		{
			{ TEXT("root"), -1, { 0, 0, 0 } },
			{ TEXT("wheel_a1_l"), 0, { 254.25, -102.5, 50.7 } },
			{ TEXT("wheel_a1_r"), 0, { 254.25, 102.5, 50.7 } },
			{ TEXT("wheel_a2_l"), 0, { -122.2434, -102.5, 50.7 } },
			{ TEXT("wheel_a2_r"), 0, { -122.2434, 102.5, 50.7 } },
			{ TEXT("wheel_a3_l"), 0, { -254.25, -102.5, 50.7 } },
			{ TEXT("wheel_a3_r"), 0, { -254.25, 102.5, 50.7 } }
		},
		{
			{ 254.25, -102.5, 50.7 },
			{ 254.25, 102.5, 50.7 },
			{ -122.2434, -102.5, 50.7 },
			{ -122.2434, 102.5, 50.7 },
			{ -254.25, -102.5, 50.7 },
			{ -254.25, 102.5, 50.7 }
		}
	},
	{
		TEXT("semitrailer"),
		TEXT(R"MCS({"mass":6800,"maxSpeed":2917,"dragCoefficient":0.29999999999999999,"frontalArea":10,"engine":{"maxTorque":576,"maxRPM":5000,"idleRPM":600,"torqueCurve":[[0,0.59999999999999998],[1000,0.75],[2500,0.90000000000000002],[3800,1],[4500,0.96199999999999997],[5000,0.84999999999999998]]},"transmission":{"gearRatios":[4.0270000000000001,2.3639999999999999,1.532,1.1519999999999999,0.85199999999999998,0.66700000000000004],"reverseRatio":3.0640000000000001,"finalDrive":4.4589999999999996,"changeUpRPM":4500,"changeDownRPM":1200},"suspension":{"maxRaise":15,"maxDrop":22,"springRate":363,"springPreload":200,"damping":1.1899999999999999},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":17000,"handbrakeTorque":17000,"abs":true,"frontShare":0.5},"tires":{"friction":1.1499999999999999},"camera":{"armLength":1600,"height":450},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":true,"radius":50.700000000000003,"width":30},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":true,"radius":50.700000000000003,"width":30},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":50.700000000000003,"width":30},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":50.700000000000003,"width":30}],"collision":[{"center":[533.5,0,266],"extent":[807.5,129.5,145]}],"centerOfMass":[438,0,160],"seat":{"location":[900,-42,250],"steeringWheel":{"location":[946.79999999999995,-42,253],"rotation":[-20,0,0],"radius":21}}})MCS"),
		344, 216,
		{ -274, -129.5, 0 }, { 1341, 129.5, 411 },
		{
			{ TEXT("root"), -1, { 0, 0, 0 } },
			{ TEXT("wheel_fl"), 0, { 62, -91.5, 50.7 } },
			{ TEXT("wheel_fr"), 0, { 62, 91.5, 50.7 } },
			{ TEXT("wheel_rl"), 0, { -62, -91.5, 50.7 } },
			{ TEXT("wheel_rr"), 0, { -62, 91.5, 50.7 } }
		},
		{
			{ 62, -91.5, 50.7 },
			{ 62, 91.5, 50.7 },
			{ -62, -91.5, 50.7 },
			{ -62, 91.5, 50.7 }
		}
	},
	{
		TEXT("utilitytrailer"),
		TEXT(R"MCS({"mass":900,"maxSpeed":2917,"dragCoefficient":0.40000000000000002,"frontalArea":1.5,"engine":{"maxTorque":576,"maxRPM":5000,"idleRPM":600,"torqueCurve":[[0,0.59999999999999998],[1000,0.75],[2500,0.90000000000000002],[3800,1],[4500,0.96199999999999997],[5000,0.84999999999999998]]},"transmission":{"gearRatios":[4.0270000000000001,2.3639999999999999,1.532,1.1519999999999999,0.85199999999999998,0.66700000000000004],"reverseRatio":3.0640000000000001,"finalDrive":4.4589999999999996,"changeUpRPM":4500,"changeDownRPM":1200},"suspension":{"maxRaise":10,"maxDrop":22,"springRate":62,"springPreload":30,"damping":0.20300000000000001},"steering":{"maxAngle":35,"ackermann":true},"brakes":{"brakeTorque":2400,"handbrakeTorque":2400,"abs":true,"frontShare":0.5},"tires":{"friction":1.1499999999999999},"camera":{"armLength":600,"height":250},"schemaVersion":1,"wheels":[{"bone":"wheel_fl","steering":true,"driven":false,"handbrake":true,"radius":35,"width":20},{"bone":"wheel_fr","steering":true,"driven":false,"handbrake":true,"radius":35,"width":20},{"bone":"wheel_rl","steering":false,"driven":true,"handbrake":true,"radius":35,"width":20},{"bone":"wheel_rr","steering":false,"driven":true,"handbrake":true,"radius":35,"width":20}],"collision":[{"center":[40,0,80],"extent":[213.5,106.5,30]}],"centerOfMass":[60,0,60],"seat":{"location":[100,-42,120],"steeringWheel":{"location":[146.80000000000001,-42,123],"rotation":[-20,0,0],"radius":18}}})MCS"),
		344, 216,
		{ -173.5, -106.5, 0 }, { 348, 106.5, 110 },
		{
			{ TEXT("root"), -1, { 0, 0, 0 } },
			{ TEXT("wheel_fl"), 0, { 44.5, -90, 35 } },
			{ TEXT("wheel_fr"), 0, { 44.5, 90, 35 } },
			{ TEXT("wheel_rl"), 0, { -44.5, -90, 35 } },
			{ TEXT("wheel_rr"), 0, { -44.5, 90, 35 } }
		},
		{
			{ 44.5, -90, 35 },
			{ 44.5, 90, 35 },
			{ -44.5, -90, 35 },
			{ -44.5, 90, 35 }
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
		for (const TCHAR* Key : { TEXT("movementType"), TEXT("flight"), TEXT("helicopter"), TEXT("hover"), TEXT("boat"), TEXT("trailer"), TEXT("hitch"), TEXT("tracked") })
		{
			Doc->RemoveField(Key);
		}
		// A ground car's handling baked in: the steering's speed curve goes, and the tires' friction is the preset's again (the front's is 6/7 of it; the rear's,
		// 8/7 of it or the preset's own rear grip, is Mantle.Vehicle.Document.GroundHandlingIsBakedIn's to check).
		const TSharedPtr<FJsonObject>* Steering = nullptr;
		if (Doc->TryGetObjectField(TEXT("steering"), Steering) && Steering && Steering->IsValid())
		{
			(*Steering)->RemoveField(TEXT("speedCurve"));
		}
		const TSharedPtr<FJsonObject>* Tires = nullptr;
		double FrontFriction = 0.0;
		if (Doc->TryGetObjectField(TEXT("tires"), Tires) && Tires && Tires->IsValid() && (*Tires)->HasField(TEXT("rearFriction"))
			&& (*Tires)->TryGetNumberField(TEXT("friction"), FrontFriction))
		{
			(*Tires)->SetNumberField(TEXT("friction"), FMath::RoundToDouble(FrontFriction * 7.0 / 6.0 * 1.0e6) / 1.0e6);
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
	// A shipped preset with no entry yet: the entry it would have, logged ready to paste once its body is checked by eye.
	for (const FString& Id : Ids)
	{
		if (Golden().ContainsByPredicate([&Id](const FGoldenChassis& Entry) { return Id == Entry.Id; }))
		{
			continue;
		}
		FString Text;
		FString Error;
		FMantleChassisSpec Spec;
		FMantleVehicleAsset Asset;
		if (!FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) || !FMantleChassisSpec::Parse(Text, Spec, Error)
			|| !FMantleVehicleChassisBuilder::MakeAsset(Spec, Asset, Error) || Asset.Body.LODs.Num() != 1)
		{
			AddError(FString::Printf(TEXT("%s has no golden entry and does not build (%s)"), *Id, *Error));
			continue;
		}
		const FMantleSkinnedMeshLOD& LOD = Asset.Body.LODs[0];
		FVector3f Min(TNumericLimits<float>::Max());
		FVector3f Max(-TNumericLimits<float>::Max());
		for (const FVector3f& Point : LOD.Positions)
		{
			Min = FVector3f(FMath::Min(Min.X, Point.X), FMath::Min(Min.Y, Point.Y), FMath::Min(Min.Z, Point.Z));
			Max = FVector3f(FMath::Max(Max.X, Point.X), FMath::Max(Max.Y, Point.Y), FMath::Max(Max.Z, Point.Z));
		}
		// The document as the comparison reads it: without its movement blocks.
		FString Document;
		const TSharedPtr<FJsonValue> Without = ParseJsonWithoutMovement(FMantleChassisSpec::ComposeDocument(Spec));
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Document);
		if (Without.IsValid() && Without->Type == EJson::Object)
		{
			FJsonSerializer::Serialize(Without->AsObject().ToSharedRef(), Writer);
		}
		FString Entry = FString::Printf(TEXT("\t{\n\t\tTEXT(\"%s\"),\n\t\tTEXT(R\"MCS(%s)MCS\"),\n\t\t%d, %d,\n\t\t{ %.10g, %.10g, %.10g }, { %.10g, %.10g, %.10g },\n\t\t{\n"), *Id, *Document,
			LOD.Positions.Num(), LOD.NumTriangles(), Min.X, Min.Y, Min.Z, Max.X, Max.Y, Max.Z);
		for (int32 Index = 0; Index < Asset.Body.Bones.Num(); ++Index)
		{
			const FVector At = Asset.Body.ReferencePose.IsValidIndex(Index) ? Asset.Body.ReferencePose[Index].GetLocation() : FVector::ZeroVector;
			Entry += FString::Printf(TEXT("\t\t\t{ TEXT(\"%s\"), %d, { %.10g, %.10g, %.10g } }%s\n"), *Asset.Body.Bones[Index].Name.ToString(), Asset.Body.Bones[Index].ParentIndex,
				At.X, At.Y, At.Z, Index + 1 < Asset.Body.Bones.Num() ? TEXT(",") : TEXT(""));
		}
		Entry += TEXT("\t\t},\n\t\t{\n");
		for (int32 Index = 0; Index < Asset.WheelPositions.Num(); ++Index)
		{
			const FVector& At = Asset.WheelPositions[Index];
			Entry += FString::Printf(TEXT("\t\t\t{ %.10g, %.10g, %.10g }%s\n"), At.X, At.Y, At.Z, Index + 1 < Asset.WheelPositions.Num() ? TEXT(",") : TEXT(""));
		}
		Entry += TEXT("\t\t}\n\t},");
		AddError(FString::Printf(TEXT("%s has no golden entry; once its body is checked, this is the entry it would have:\n%s"), *Id, *Entry));
	}
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

		// The skeleton and the gray body.
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
				const double* Center = Want.Wheels[Index].Center;
				TestTrue(*FString::Printf(TEXT("%s: wheel %d's center"), *Id, Index), Asset.WheelPositions[Index].Equals(FVector(Center[0], Center[1], Center[2]), 1.0e-6));
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
			// The wheels are geometry too, never held: a tracked chassis's small, wide road wheels leave limits.json's wheels/*/radius (5 to 150 cm) and
			// width (2 to 100 cm) at the selectors' ends (the bulldozer's 13 cm wheels at 0.25, its 70 cm wide ones past 1.43), and are refused the same way.
			double WheelRadiusMin = 0.0;
			double WheelRadiusMax = 0.0;
			double WheelWidthMin = 0.0;
			double WheelWidthMax = 0.0;
			FMantleVehicleLimits::GetRange(TEXT("wheels/*/radius"), WheelRadiusMin, WheelRadiusMax);
			FMantleVehicleLimits::GetRange(TEXT("wheels/*/width"), WheelWidthMin, WheelWidthMax);
			const bool bWheelsFit = Spec.WheelRadius >= WheelRadiusMin && Spec.WheelRadius <= WheelRadiusMax && Spec.WheelWidth >= WheelWidthMin && Spec.WheelWidth <= WheelWidthMax;
			const bool bFits = (Preset.Aircraft.bSet || 0.5 * Scale * FMath::Max3(Preset.Length, Preset.Width, Preset.Height) <= 1000.0) && bWheelsFit;
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
			TestTrue(*FString::Printf(TEXT("%s: the center of mass is the derived one"), *Selector), Asset.Desc.CenterOfMass.Equals(Derived.CenterOfMass, 1.0e-6));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
