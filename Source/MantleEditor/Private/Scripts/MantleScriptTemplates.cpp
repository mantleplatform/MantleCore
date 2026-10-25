// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptTemplates.h"

#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptGraphBuilder.h"
#include "Graph/MantleScriptGraphNode.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

// ---------------------------------------------------------------------------------------------
// Enum <-> string plumbing

TArray<EMantleScriptEditorLanguage> FMantleScriptTemplates::NewScriptLanguages()
{
	return {
		EMantleScriptEditorLanguage::Luau,
		EMantleScriptEditorLanguage::TypeScript,
		EMantleScriptEditorLanguage::JavaScript,
		EMantleScriptEditorLanguage::Python,
		// "Graph" creates a UMantleScriptGraph asset instead of a text file -- see
		// CreateGraphAsset, used in place of CreateScriptFile whenever this is selected.
		EMantleScriptEditorLanguage::Graph
	};
}

TArray<EMantleScriptTemplateKind> FMantleScriptTemplates::AllTemplates()
{
	return {
		EMantleScriptTemplateKind::Empty,
		EMantleScriptTemplateKind::KillBrick,
		EMantleScriptTemplateKind::Coins,
		EMantleScriptTemplateKind::RoundTimer,
		EMantleScriptTemplateKind::Door
	};
}

TArray<EMantleScriptRowSide> FMantleScriptTemplates::AllSides()
{
	return { EMantleSide::Server, EMantleSide::Client, EMantleSide::Both };
}

FString FMantleScriptTemplates::SideFolderName(EMantleScriptRowSide Side)
{
	switch (Side)
	{
	case EMantleSide::Server: return TEXT("Server");
	case EMantleSide::Client: return TEXT("Client");
	default: return TEXT("Shared");
	}
}

FString FMantleScriptTemplates::TemplateDisplayName(EMantleScriptTemplateKind Template)
{
	switch (Template)
	{
	case EMantleScriptTemplateKind::Empty: return TEXT("Empty");
	case EMantleScriptTemplateKind::KillBrick: return TEXT("Kill brick");
	case EMantleScriptTemplateKind::Coins: return TEXT("Coins");
	case EMantleScriptTemplateKind::RoundTimer: return TEXT("Round timer + HUD (client)");
	case EMantleScriptTemplateKind::Door: return TEXT("Door");
	default: return TEXT("Empty");
	}
}

FString FMantleScriptTemplates::LanguageDisplayName(EMantleScriptEditorLanguage Language)
{
	switch (Language)
	{
	case EMantleScriptEditorLanguage::Luau: return TEXT("Luau");
	case EMantleScriptEditorLanguage::TypeScript: return TEXT("TypeScript");
	case EMantleScriptEditorLanguage::JavaScript: return TEXT("JavaScript");
	case EMantleScriptEditorLanguage::Python: return TEXT("Python");
	case EMantleScriptEditorLanguage::Graph: return TEXT("Graph");
	default: return TEXT("?");
	}
}

FString FMantleScriptTemplates::FileExtension(EMantleScriptEditorLanguage Language)
{
	switch (Language)
	{
	case EMantleScriptEditorLanguage::Luau: return TEXT("luau");
	case EMantleScriptEditorLanguage::TypeScript: return TEXT("ts");
	case EMantleScriptEditorLanguage::JavaScript: return TEXT("js");
	case EMantleScriptEditorLanguage::Python: return TEXT("py");
	default: return TEXT("txt"); // Graph: unused -- a graph is a .uasset, created by CreateGraphAsset
	}
}

bool FMantleScriptTemplates::ParseSide(const FString& Text, EMantleScriptRowSide& OutSide)
{
	for (EMantleScriptRowSide Side : AllSides())
	{
		if (SideFolderName(Side).Equals(Text, ESearchCase::IgnoreCase))
		{
			OutSide = Side;
			return true;
		}
	}
	return false;
}

bool FMantleScriptTemplates::ParseLanguage(const FString& Text, EMantleScriptEditorLanguage& OutLanguage)
{
	for (EMantleScriptEditorLanguage Language : NewScriptLanguages())
	{
		if (LanguageDisplayName(Language).Equals(Text, ESearchCase::IgnoreCase))
		{
			OutLanguage = Language;
			return true;
		}
	}
	return false;
}

bool FMantleScriptTemplates::ParseTemplate(const FString& Text, EMantleScriptTemplateKind& OutTemplate)
{
	// Accepts both the display name ("Kill brick") and a bare identifier ("KillBrick") from the
	// console.
	static const TMap<FString, EMantleScriptTemplateKind> Aliases = {
		{TEXT("empty"), EMantleScriptTemplateKind::Empty},
		{TEXT("killbrick"), EMantleScriptTemplateKind::KillBrick},
		{TEXT("kill brick"), EMantleScriptTemplateKind::KillBrick},
		{TEXT("coins"), EMantleScriptTemplateKind::Coins},
		{TEXT("coin"), EMantleScriptTemplateKind::Coins},
		{TEXT("roundtimer"), EMantleScriptTemplateKind::RoundTimer},
		{TEXT("round timer"), EMantleScriptTemplateKind::RoundTimer},
		{TEXT("round timer + hud (client)"), EMantleScriptTemplateKind::RoundTimer},
		{TEXT("hud"), EMantleScriptTemplateKind::RoundTimer},
		{TEXT("door"), EMantleScriptTemplateKind::Door},
	};
	if (const EMantleScriptTemplateKind* Found = Aliases.Find(Text.ToLower()))
	{
		OutTemplate = *Found;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Content generation
//
// Method names, casing and the Tween goal shape match the registered API exactly
// (MantleApiCore.cpp): World:FindByTag/FindFirst/OnTagged, Entity.Touched/Position/Visible/
// Tween({Position=...}, seconds, style, direction), Players:GetPlayers/OnPlayer,
// Player.Name/UserId/Position/:Teleport/:Kick, State:Get/Set/Observe + .Changed,
// Net:Remote(name) with :FireServer/:FireClient/:FireAllClients + .OnServerEvent/.OnClientEvent,
// UI:SetText/:Toast. JS/TS use the camelCase the registry's own generator produces
// (FMantleNaming::ToCamel in MantleTypeGen.cpp); Python uses the snake_case its generator would.

namespace
{
	FString EmptyContent(const FMantleNewScriptRequest& Req)
	{
		switch (Req.Language)
		{
		case EMantleScriptEditorLanguage::Luau:
			return FString::Printf(TEXT("-- %s.luau (%s)\nprint(\"%s loaded\")\n"),
				*Req.Name, *FMantleScriptTemplates::SideFolderName(Req.Side), *Req.Name);
		case EMantleScriptEditorLanguage::TypeScript:
			return FString::Printf(TEXT("// %s.ts (%s)\nconsole.log(\"%s loaded\");\n"),
				*Req.Name, *FMantleScriptTemplates::SideFolderName(Req.Side), *Req.Name);
		case EMantleScriptEditorLanguage::JavaScript:
			return FString::Printf(TEXT("// %s.js (%s)\nconsole.log(\"%s loaded\");\n"),
				*Req.Name, *FMantleScriptTemplates::SideFolderName(Req.Side), *Req.Name);
		case EMantleScriptEditorLanguage::Python:
			return FString::Printf(TEXT("# %s.py (%s)\nprint(\"%s loaded\")\n"),
				*Req.Name, *FMantleScriptTemplates::SideFolderName(Req.Side), *Req.Name);
		default:
			return FString();
		}
	}

	FString KillBrickContent(EMantleScriptEditorLanguage Language)
	{
		switch (Language)
		{
		case EMantleScriptEditorLanguage::Luau:
			return TEXT(
				"-- Tag an actor \"killbrick\" in Details > Actor > Tags. Touching it sends the player back to spawn.\n"
				"local SPAWN = Vector3.new(0, 0, 150)\n"
				"\n"
				"World:OnTagged(\"killbrick\", function(brick)\n"
				"\tbrick.Touched:Connect(function(player)\n"
				"\t\tprint(`{player.Name} touched {brick.Name}; back to spawn`)\n"
				"\t\tplayer:Teleport(SPAWN)\n"
				"\t\tNet:Remote(\"Announce\"):FireClient(player, \"Ouch! Watch out for that.\")\n"
				"\tend)\n"
				"end)\n");
		case EMantleScriptEditorLanguage::TypeScript:
			return TEXT(
				"// Tag an actor \"killbrick\" in Details > Actor > Tags. Touching it sends the player back to spawn.\n"
				"const SPAWN = new Vector3(0, 0, 150);\n"
				"\n"
				"World.onTagged(\"killbrick\", (brick: Entity) => {\n"
				"\tbrick.touched.connect((player: Player) => {\n"
				"\t\tconsole.log(`${player.name} touched ${brick.name}; back to spawn`);\n"
				"\t\tplayer.teleport(SPAWN);\n"
				"\t\tNet.remote(\"Announce\").fireClient(player, \"Ouch! Watch out for that.\");\n"
				"\t});\n"
				"});\n");
		case EMantleScriptEditorLanguage::JavaScript:
			return TEXT(
				"// Tag an actor \"killbrick\" in Details > Actor > Tags. Touching it sends the player back to spawn.\n"
				"const SPAWN = new Vector3(0, 0, 150);\n"
				"\n"
				"World.onTagged(\"killbrick\", (brick) => {\n"
				"\tbrick.touched.connect((player) => {\n"
				"\t\tconsole.log(`${player.name} touched ${brick.name}; back to spawn`);\n"
				"\t\tplayer.teleport(SPAWN);\n"
				"\t\tNet.remote(\"Announce\").fireClient(player, \"Ouch! Watch out for that.\");\n"
				"\t});\n"
				"});\n");
		case EMantleScriptEditorLanguage::Python:
			return TEXT(
				"# Tag an actor \"killbrick\" in Details > Actor > Tags. Touching it sends the player back to spawn.\n"
				"SPAWN = Vector3(0, 0, 150)\n"
				"\n"
				"def setup(brick):\n"
				"    def on_touch(player):\n"
				"        print(f\"{player.name} touched {brick.name}; back to spawn\")\n"
				"        player.teleport(SPAWN)\n"
				"        Net.remote(\"Announce\").fire_client(player, \"Ouch! Watch out for that.\")\n"
				"    brick.touched.connect(on_touch)\n"
				"\n"
				"World.on_tagged(\"killbrick\", setup)\n");
		default:
			return FString();
		}
	}

	FString CoinsContent(EMantleScriptEditorLanguage Language)
	{
		switch (Language)
		{
		case EMantleScriptEditorLanguage::Luau:
			return TEXT(
				"-- Tag an actor \"coin\" in Details > Actor > Tags. Touching it scores a point and it respawns.\n"
				"World:OnTagged(\"coin\", function(coin)\n"
				"\tlocal home = coin.Position\n"
				"\ttask.spawn(function()\n"
				"\t\twhile true do\n"
				"\t\t\tcoin:Tween({ Position = home + Vector3.new(0, 0, 40) }, 1, \"Sine\", \"InOut\"):Wait()\n"
				"\t\t\tcoin:Tween({ Position = home }, 1, \"Sine\", \"InOut\"):Wait()\n"
				"\t\tend\n"
				"\tend)\n"
				"\n"
				"\tcoin.Touched:Connect(function(player)\n"
				"\t\tif not coin.Visible then return end\n"
				"\t\tcoin.Visible = false\n"
				"\t\tlocal key = `score:{player.UserId}`\n"
				"\t\tState:Set(key, (State:Get(key) or 0) + 1)\n"
				"\t\tprint(`{player.Name} collected {coin.Name} (score {State:Get(key)})`)\n"
				"\t\ttask.wait(5)\n"
				"\t\tcoin.Visible = true\n"
				"\tend)\n"
				"end)\n");
		case EMantleScriptEditorLanguage::TypeScript:
			return TEXT(
				"// Tag an actor \"coin\" in Details > Actor > Tags. Touching it scores a point and it respawns.\n"
				"World.onTagged(\"coin\", (coin: Entity) => {\n"
				"\tconst home = coin.position;\n"
				"\ttask.spawn(async () => {\n"
				"\t\twhile (true) {\n"
				"\t\t\tawait coin.tween({ position: home.add(new Vector3(0, 0, 40)) }, 1, \"Sine\", \"InOut\").wait();\n"
				"\t\t\tawait coin.tween({ position: home }, 1, \"Sine\", \"InOut\").wait();\n"
				"\t\t}\n"
				"\t});\n"
				"\n"
				"\tcoin.touched.connect(async (player: Player) => {\n"
				"\t\tif (!coin.visible) return;\n"
				"\t\tcoin.visible = false;\n"
				"\t\tconst key = `score:${player.userId}`;\n"
				"\t\tState.set(key, (State.get(key) ?? 0) + 1);\n"
				"\t\tconsole.log(`${player.name} collected ${coin.name} (score ${State.get(key)})`);\n"
				"\t\tawait task.wait(5);\n"
				"\t\tcoin.visible = true;\n"
				"\t});\n"
				"});\n");
		case EMantleScriptEditorLanguage::JavaScript:
			return TEXT(
				"// Tag an actor \"coin\" in Details > Actor > Tags. Touching it scores a point and it respawns.\n"
				"World.onTagged(\"coin\", (coin) => {\n"
				"\tconst home = coin.position;\n"
				"\ttask.spawn(async () => {\n"
				"\t\twhile (true) {\n"
				"\t\t\tawait coin.tween({ position: home.add(new Vector3(0, 0, 40)) }, 1, \"Sine\", \"InOut\").wait();\n"
				"\t\t\tawait coin.tween({ position: home }, 1, \"Sine\", \"InOut\").wait();\n"
				"\t\t}\n"
				"\t});\n"
				"\n"
				"\tcoin.touched.connect(async (player) => {\n"
				"\t\tif (!coin.visible) return;\n"
				"\t\tcoin.visible = false;\n"
				"\t\tconst key = `score:${player.userId}`;\n"
				"\t\tState.set(key, (State.get(key) ?? 0) + 1);\n"
				"\t\tconsole.log(`${player.name} collected ${coin.name} (score ${State.get(key)})`);\n"
				"\t\tawait task.wait(5);\n"
				"\t\tcoin.visible = true;\n"
				"\t});\n"
				"});\n");
		case EMantleScriptEditorLanguage::Python:
			return TEXT(
				"# Tag an actor \"coin\" in Details > Actor > Tags. Touching it scores a point and it respawns.\n"
				"def setup(coin):\n"
				"    home = coin.position\n"
				"\n"
				"    def bob():\n"
				"        while True:\n"
				"            yield coin.tween({\"position\": home + Vector3(0, 0, 40)}, 1, \"Sine\", \"InOut\").wait()\n"
				"            yield coin.tween({\"position\": home}, 1, \"Sine\", \"InOut\").wait()\n"
				"    task.spawn(bob)\n"
				"\n"
				"    def on_touch(player):\n"
				"        if not coin.visible:\n"
				"            return\n"
				"        coin.visible = False\n"
				"        key = f\"score:{player.user_id}\"\n"
				"        State.set(key, (State.get(key) or 0) + 1)\n"
				"        print(f\"{player.name} collected {coin.name} (score {State.get(key)})\")\n"
				"        yield task.wait(5)\n"
				"        coin.visible = True\n"
				"    coin.touched.connect(on_touch)\n"
				"\n"
				"World.on_tagged(\"coin\", setup)\n");
		default:
			return FString();
		}
	}

	FString DoorContent(EMantleScriptEditorLanguage Language)
	{
		switch (Language)
		{
		case EMantleScriptEditorLanguage::Luau:
			return TEXT(
				"-- Tag an actor \"door\" in Details > Actor > Tags. It slides open while a player is near.\n"
				"local DOOR_RANGE = 450\n"
				"local door = World:FindByTag(\"door\")[1]\n"
				"if not door then\n"
				"\twarn(\"No entity tagged 'door'\")\n"
				"\treturn\n"
				"end\n"
				"\n"
				"local closed = door.Position\n"
				"local open = closed - Vector3.new(0, 0, 390)\n"
				"local isOpen = false\n"
				"\n"
				"task.spawn(function()\n"
				"\twhile true do\n"
				"\t\tlocal someoneNear = false\n"
				"\t\tfor _, player in Players:GetPlayers() do\n"
				"\t\t\tlocal position = player.Position\n"
				"\t\t\tif position and (position - closed).Magnitude < DOOR_RANGE then\n"
				"\t\t\t\tsomeoneNear = true\n"
				"\t\t\tend\n"
				"\t\tend\n"
				"\t\tif someoneNear ~= isOpen then\n"
				"\t\t\tisOpen = someoneNear\n"
				"\t\t\tdoor:Tween({ Position = if isOpen then open else closed }, 0.6, \"Quad\", \"Out\")\n"
				"\t\t\tprint(if isOpen then \"door opening\" else \"door closing\")\n"
				"\t\tend\n"
				"\t\ttask.wait(0.2)\n"
				"\tend\n"
				"end)\n");
		case EMantleScriptEditorLanguage::TypeScript:
			return TEXT(
				"// Tag an actor \"door\" in Details > Actor > Tags. It slides open while a player is near.\n"
				"const DOOR_RANGE = 450;\n"
				"const door = World.findByTag(\"door\")[0];\n"
				"if (!door) {\n"
				"\tconsole.warn(\"No entity tagged 'door'\");\n"
				"} else {\n"
				"\tconst closed = door.position;\n"
				"\tconst open = closed.subtract(new Vector3(0, 0, 390));\n"
				"\tlet isOpen = false;\n"
				"\n"
				"\ttask.spawn(async () => {\n"
				"\t\twhile (true) {\n"
				"\t\t\tlet someoneNear = false;\n"
				"\t\t\tfor (const player of Players.getPlayers()) {\n"
				"\t\t\t\tif (player.position && player.position.subtract(closed).magnitude < DOOR_RANGE) {\n"
				"\t\t\t\t\tsomeoneNear = true;\n"
				"\t\t\t\t}\n"
				"\t\t\t}\n"
				"\t\t\tif (someoneNear !== isOpen) {\n"
				"\t\t\t\tisOpen = someoneNear;\n"
				"\t\t\t\tdoor.tween({ position: isOpen ? open : closed }, 0.6, \"Quad\", \"Out\");\n"
				"\t\t\t\tconsole.log(isOpen ? \"door opening\" : \"door closing\");\n"
				"\t\t\t}\n"
				"\t\t\tawait task.wait(0.2);\n"
				"\t\t}\n"
				"\t});\n"
				"}\n");
		case EMantleScriptEditorLanguage::JavaScript:
			return TEXT(
				"// Tag an actor \"door\" in Details > Actor > Tags. It slides open while a player is near.\n"
				"const DOOR_RANGE = 450;\n"
				"const door = World.findByTag(\"door\")[0];\n"
				"if (!door) {\n"
				"\tconsole.warn(\"No entity tagged 'door'\");\n"
				"} else {\n"
				"\tconst closed = door.position;\n"
				"\tconst open = closed.subtract(new Vector3(0, 0, 390));\n"
				"\tlet isOpen = false;\n"
				"\n"
				"\ttask.spawn(async () => {\n"
				"\t\twhile (true) {\n"
				"\t\t\tlet someoneNear = false;\n"
				"\t\t\tfor (const player of Players.getPlayers()) {\n"
				"\t\t\t\tif (player.position && player.position.subtract(closed).magnitude < DOOR_RANGE) {\n"
				"\t\t\t\t\tsomeoneNear = true;\n"
				"\t\t\t\t}\n"
				"\t\t\t}\n"
				"\t\t\tif (someoneNear !== isOpen) {\n"
				"\t\t\t\tisOpen = someoneNear;\n"
				"\t\t\t\tdoor.tween({ position: isOpen ? open : closed }, 0.6, \"Quad\", \"Out\");\n"
				"\t\t\t\tconsole.log(isOpen ? \"door opening\" : \"door closing\");\n"
				"\t\t\t}\n"
				"\t\t\tawait task.wait(0.2);\n"
				"\t\t}\n"
				"\t});\n"
				"}\n");
		case EMantleScriptEditorLanguage::Python:
			return TEXT(
				"# Tag an actor \"door\" in Details > Actor > Tags. It slides open while a player is near.\n"
				"DOOR_RANGE = 450\n"
				"\n"
				"def run():\n"
				"    doors = World.find_by_tag(\"door\")\n"
				"    if not doors:\n"
				"        print(\"No entity tagged 'door'\")\n"
				"        return\n"
				"    door = doors[0]\n"
				"    closed = door.position\n"
				"    opened = closed + Vector3(0, 0, -390)\n"
				"    is_open = False\n"
				"\n"
				"    while True:\n"
				"        someone_near = False\n"
				"        for player in Players.get_players():\n"
				"            if player.position and (player.position - closed).magnitude < DOOR_RANGE:\n"
				"                someone_near = True\n"
				"        if someone_near != is_open:\n"
				"            is_open = someone_near\n"
				"            door.tween({\"position\": opened if is_open else closed}, 0.6, \"Quad\", \"Out\")\n"
				"            print(\"door opening\" if is_open else \"door closing\")\n"
				"        yield task.wait(0.2)\n"
				"\n"
				"task.spawn(run)\n");
		default:
			return FString();
		}
	}

	FString RoundTimerServerContent(EMantleScriptEditorLanguage Language)
	{
		switch (Language)
		{
		case EMantleScriptEditorLanguage::Luau:
			return TEXT(
				"-- A simple round timer everyone can see (pairs with the Client/ HUD template).\n"
				"local ROUND_SECONDS = 60\n"
				"local announce = Net:Remote(\"Announce\")\n"
				"\n"
				"Players.PlayerAdded:Connect(function(player)\n"
				"\tannounce:FireClient(player, `Welcome, {player.Name}!`)\n"
				"end)\n"
				"\n"
				"task.spawn(function()\n"
				"\tlocal round = 0\n"
				"\twhile true do\n"
				"\t\tround += 1\n"
				"\t\tState:Set(\"round\", round)\n"
				"\t\tannounce:FireAllClients(`Round {round} started`)\n"
				"\t\tfor remaining = ROUND_SECONDS, 0, -1 do\n"
				"\t\t\tState:Set(\"timeLeft\", remaining)\n"
				"\t\t\ttask.wait(1)\n"
				"\t\tend\n"
				"\tend\n"
				"end)\n");
		case EMantleScriptEditorLanguage::TypeScript:
			return TEXT(
				"// A simple round timer everyone can see (pairs with the Client/ HUD template).\n"
				"const ROUND_SECONDS = 60;\n"
				"const announce = Net.remote(\"Announce\");\n"
				"\n"
				"Players.playerAdded.connect((player: Player) => {\n"
				"\tannounce.fireClient(player, `Welcome, ${player.name}!`);\n"
				"});\n"
				"\n"
				"task.spawn(async () => {\n"
				"\tlet round = 0;\n"
				"\twhile (true) {\n"
				"\t\tround += 1;\n"
				"\t\tState.set(\"round\", round);\n"
				"\t\tannounce.fireAllClients(`Round ${round} started`);\n"
				"\t\tfor (let remaining = ROUND_SECONDS; remaining >= 0; remaining--) {\n"
				"\t\t\tState.set(\"timeLeft\", remaining);\n"
				"\t\t\tawait task.wait(1);\n"
				"\t\t}\n"
				"\t}\n"
				"});\n");
		case EMantleScriptEditorLanguage::JavaScript:
			return TEXT(
				"// A simple round timer everyone can see (pairs with the Client/ HUD template).\n"
				"const ROUND_SECONDS = 60;\n"
				"const announce = Net.remote(\"Announce\");\n"
				"\n"
				"Players.playerAdded.connect((player) => {\n"
				"\tannounce.fireClient(player, `Welcome, ${player.name}!`);\n"
				"});\n"
				"\n"
				"task.spawn(async () => {\n"
				"\tlet round = 0;\n"
				"\twhile (true) {\n"
				"\t\tround += 1;\n"
				"\t\tState.set(\"round\", round);\n"
				"\t\tannounce.fireAllClients(`Round ${round} started`);\n"
				"\t\tfor (let remaining = ROUND_SECONDS; remaining >= 0; remaining--) {\n"
				"\t\t\tState.set(\"timeLeft\", remaining);\n"
				"\t\t\tawait task.wait(1);\n"
				"\t\t}\n"
				"\t}\n"
				"});\n");
		case EMantleScriptEditorLanguage::Python:
			return TEXT(
				"# A simple round timer everyone can see (pairs with the Client/ HUD template).\n"
				"ROUND_SECONDS = 60\n"
				"announce = Net.remote(\"Announce\")\n"
				"\n"
				"def on_player_added(player):\n"
				"    announce.fire_client(player, f\"Welcome, {player.name}!\")\n"
				"Players.player_added.connect(on_player_added)\n"
				"\n"
				"def run_round():\n"
				"    round_number = 0\n"
				"    while True:\n"
				"        round_number += 1\n"
				"        State.set(\"round\", round_number)\n"
				"        announce.fire_all_clients(f\"Round {round_number} started\")\n"
				"        for remaining in range(ROUND_SECONDS, -1, -1):\n"
				"            State.set(\"timeLeft\", remaining)\n"
				"            yield task.wait(1)\n"
				"task.spawn(run_round)\n");
		default:
			return FString();
		}
	}

	FString RoundTimerHudContent(EMantleScriptEditorLanguage Language)
	{
		switch (Language)
		{
		case EMantleScriptEditorLanguage::Luau:
			return TEXT(
				"-- Shows the round and time left, and pops up announcements (pairs with the Server/ round timer template).\n"
				"State:Observe(\"round\", function(r) UI:SetText(\"round\", `Round {r or 0}`) end)\n"
				"State:Observe(\"timeLeft\", function(s) UI:SetText(\"time\", `Time left: {s or 0}s`) end)\n"
				"Net:Remote(\"Announce\").OnClientEvent:Connect(function(message) UI:Toast(message, 3) end)\n");
		case EMantleScriptEditorLanguage::TypeScript:
			return TEXT(
				"// Shows the round and time left, and pops up announcements (pairs with the Server/ round timer template).\n"
				"State.observe(\"round\", (r: number) => UI.setText(\"round\", `Round ${r ?? 0}`));\n"
				"State.observe(\"timeLeft\", (s: number) => UI.setText(\"time\", `Time left: ${s ?? 0}s`));\n"
				"Net.remote(\"Announce\").onClientEvent.connect((message: string) => UI.toast(message, 3));\n");
		case EMantleScriptEditorLanguage::JavaScript:
			return TEXT(
				"// Shows the round and time left, and pops up announcements (pairs with the Server/ round timer template).\n"
				"State.observe(\"round\", (r) => UI.setText(\"round\", `Round ${r ?? 0}`));\n"
				"State.observe(\"timeLeft\", (s) => UI.setText(\"time\", `Time left: ${s ?? 0}s`));\n"
				"Net.remote(\"Announce\").onClientEvent.connect((message) => UI.toast(message, 3));\n");
		case EMantleScriptEditorLanguage::Python:
			return TEXT(
				"# Shows the round and time left, and pops up announcements (pairs with the Server/ round timer template).\n"
				"State.observe(\"round\", lambda r: UI.set_text(\"round\", f\"Round {r or 0}\"))\n"
				"State.observe(\"timeLeft\", lambda s: UI.set_text(\"time\", f\"Time left: {s or 0}s\"))\n"
				"Net.remote(\"Announce\").on_client_event.connect(lambda msg: UI.toast(msg, 3))\n");
		default:
			return FString();
		}
	}
}

FString FMantleScriptTemplates::Generate(const FMantleNewScriptRequest& Req)
{
	switch (Req.Template)
	{
	case EMantleScriptTemplateKind::Empty:
		return EmptyContent(Req);
	case EMantleScriptTemplateKind::KillBrick:
		return KillBrickContent(Req.Language);
	case EMantleScriptTemplateKind::Coins:
		return CoinsContent(Req.Language);
	case EMantleScriptTemplateKind::Door:
		return DoorContent(Req.Language);
	case EMantleScriptTemplateKind::RoundTimer:
		// The one template that's genuinely two scripts: Server/ gets the round loop, Client/
		// (and Shared/, as the closest fit) gets the HUD that reads it back through State/Net.
		return Req.Side == EMantleSide::Server ? RoundTimerServerContent(Req.Language) : RoundTimerHudContent(Req.Language);
	default:
		return FString();
	}
}

FString FMantleScriptTemplates::CreateScriptFile(const FString& ScriptsRoot, const FMantleNewScriptRequest& Req, FString& OutError)
{
	if (Req.Language == EMantleScriptEditorLanguage::Graph)
	{
		OutError = TEXT("a Graph is a Mantle Script Graph asset, not a text file -- use CreateGraphAsset");
		return FString();
	}
	if (ScriptsRoot.IsEmpty())
	{
		OutError = TEXT("no Scripts folder to write into -- open a level under /Game first");
		return FString();
	}
	FString Name = Req.Name;
	Name.TrimStartAndEndInline();
	if (Name.IsEmpty())
	{
		OutError = TEXT("give the script a name");
		return FString();
	}
	for (TCHAR C : Name)
	{
		if (!FChar::IsAlnum(C) && C != '_' && C != '-')
		{
			OutError = FString::Printf(TEXT("\"%s\" isn't a valid file name: use letters, numbers, _ and - only"), *Name);
			return FString();
		}
	}

	const FString FolderOnDisk = ScriptsRoot / SideFolderName(Req.Side);
	const FString Extension = FileExtension(Req.Language);
	const FString FullPath = FolderOnDisk / (Name + TEXT(".") + Extension);

	if (FPaths::FileExists(FullPath))
	{
		OutError = FString::Printf(TEXT("%s already exists"), *FullPath);
		return FString();
	}
	if (!IFileManager::Get().MakeDirectory(*FolderOnDisk, /*Tree*/ true))
	{
		OutError = FString::Printf(TEXT("couldn't create folder %s"), *FolderOnDisk);
		return FString();
	}

	const FString Content = Generate(Req);
	if (Content.IsEmpty())
	{
		OutError = TEXT("no template content for that language/template combination");
		return FString();
	}
	if (!FFileHelper::SaveStringToFile(Content, *FullPath))
	{
		OutError = FString::Printf(TEXT("couldn't write %s"), *FullPath);
		return FString();
	}
	return FullPath;
}

// ---------------------------------------------------------------------------------------------
// Graph templates, built with direct FMantleScriptGraphBuilder calls. There is no JSON-to-EdGraph
// importer (every graph starts from one of these templates or an empty canvas), and building
// nodes/wires directly here keeps New Script's output on the plain
// FMantleScriptGraphBuilder code path.
namespace
{
	using FGB = FMantleScriptGraphBuilder;

	void BuildKillBrickGraph(UMantleScriptEdGraph& G)
	{
		UMantleScriptGraphNode& Tagged = FGB::AddMemberNode(G, TEXT("event"), TEXT("World.OnTagged"), FVector2D(0, 0));
		FGB::SetDefault(Tagged, TEXT("tag"), TEXT("killbrick"));
		UMantleScriptGraphNode& Touch = FGB::AddMemberNode(G, TEXT("event"), TEXT("Entity.Touched"), FVector2D(300, 0));
		FGB::Wire(Tagged, TEXT("entity"), Touch, TEXT("target"));
		UMantleScriptGraphNode& Tp = FGB::AddMemberNode(G, TEXT("call"), TEXT("Player.Teleport"), FVector2D(600, 0));
		FGB::Wire(Touch, TEXT("player"), Tp, TEXT("target"));
		FGB::SetVectorDefault(Tp, TEXT("position"), FVector(0, 0, 150));
		UMantleScriptGraphNode& Log = FGB::AddNode(G, TEXT("print"), FVector2D(900, 0), [](UMantleScriptGraphNode& N) { N.TextValue = TEXT("a player hit a killbrick"); });

		FGB::Wire(Tagged, TEXT("next"), Touch, TEXT("In"));
		FGB::Wire(Touch, TEXT("next"), Tp, TEXT("In"));
		FGB::Wire(Tp, TEXT("next"), Log, TEXT("In"));
	}

    // Compact named-node builder for the two larger gameplay recipes. This creates the same
    // real editor pins as hand placement; no fixture files or JSON loader ship with templates.
    struct FRecipeGraph
    {
        UMantleScriptEdGraph& Graph;
        TMap<FString, UMantleScriptGraphNode*> Nodes;
        explicit FRecipeGraph(UMantleScriptEdGraph& InGraph) : Graph(InGraph) {}
        UMantleScriptGraphNode& Add(const TCHAR* Id, const TCHAR* Kind, const TCHAR* Detail = TEXT(""))
        {
            const int32 Index = Nodes.Num();
            UMantleScriptGraphNode& Node = FGB::AddNode(Graph, Kind, FVector2D((Index % 6) * 320, (Index / 6) * 240), [&](UMantleScriptGraphNode& N)
            {
                N.Dotted = Detail;
                N.Op = Detail;
                N.VariableName = Detail;
                N.FunctionName = Detail;
                N.TextValue = Detail;
                if (N.NodeKind == TEXT("format")) N.NumOutputs = 1;
            });
            Nodes.Add(Id, &Node);
            return Node;
        }
        void Number(const TCHAR* Id, double Value)
        {
            UMantleScriptGraphNode& N = FGB::AddNode(Graph, TEXT("literal"), FVector2D(0, Nodes.Num() * 120), [&](UMantleScriptGraphNode& Node)
            { Node.LiteralType = TEXT("number"); Node.NumberValue = Value; });
            Nodes.Add(Id, &N);
        }
        void Vector(const TCHAR* Id, const FVector& Value)
        {
            UMantleScriptGraphNode& N = FGB::AddNode(Graph, TEXT("literal"), FVector2D(0, Nodes.Num() * 120), [&](UMantleScriptGraphNode& Node)
            { Node.LiteralType = TEXT("vector"); Node.VectorValue = Value; });
            Nodes.Add(Id, &N);
        }
        void Boolean(const TCHAR* Id, bool Value)
        {
            UMantleScriptGraphNode& N = FGB::AddNode(Graph, TEXT("literal"), FVector2D(0, Nodes.Num() * 120), [&](UMantleScriptGraphNode& Node)
            { Node.LiteralType = TEXT("bool"); Node.BoolValue = Value; });
            Nodes.Add(Id, &N);
        }
        void Wire(const TCHAR* From, const TCHAR* Output, const TCHAR* To, const TCHAR* Input)
        { FGB::Wire(*Nodes.FindChecked(From), Output, *Nodes.FindChecked(To), Input); }
        void Exec(const TCHAR* From, const TCHAR* To, const TCHAR* Port = TEXT("next"))
        { Wire(From, Port, To, TEXT("In")); }
        void Set(const TCHAR* Id, const TCHAR* Pin, const TCHAR* Value)
        { FGB::SetDefault(*Nodes.FindChecked(Id), Pin, Value); }
    };

    void BuildCoinsGraph(UMantleScriptEdGraph& G)
    {
        FMantleEdGraphFunction Bob;
        Bob.Name = TEXT("Bob");
        Bob.Params = {{TEXT("coin"), TEXT("Entity")}, {TEXT("home"), TEXT("Vector3")}};
        G.Functions.Add(Bob);
        FRecipeGraph B(G);
        B.Add(TEXT("tagged"), TEXT("event"), TEXT("World.OnTagged")); B.Set(TEXT("tagged"), TEXT("tag"), TEXT("coin"));
        B.Add(TEXT("setup"), TEXT("sequence"));
        B.Add(TEXT("touch"), TEXT("event"), TEXT("Entity.Touched")); B.Wire(TEXT("tagged"), TEXT("entity"), TEXT("touch"), TEXT("target"));
        B.Add(TEXT("visible"), TEXT("get"), TEXT("Entity.Visible")); B.Wire(TEXT("tagged"), TEXT("entity"), TEXT("visible"), TEXT("target"));
        B.Add(TEXT("guard"), TEXT("branch")); B.Wire(TEXT("visible"), TEXT("value"), TEXT("guard"), TEXT("condition"));
        B.Add(TEXT("hide"), TEXT("set"), TEXT("Entity.Visible")); B.Wire(TEXT("tagged"), TEXT("entity"), TEXT("hide"), TEXT("target")); B.Set(TEXT("hide"), TEXT("value"), TEXT("false"));
        B.Add(TEXT("playerId"), TEXT("get"), TEXT("Player.UserId")); B.Wire(TEXT("touch"), TEXT("player"), TEXT("playerId"), TEXT("target"));
        B.Add(TEXT("key"), TEXT("format"), TEXT("score:{}")); B.Wire(TEXT("playerId"), TEXT("value"), TEXT("key"), TEXT("values0"));
        B.Add(TEXT("score"), TEXT("call"), TEXT("State.Get")); B.Wire(TEXT("key"), TEXT("value"), TEXT("score"), TEXT("key"));
        B.Number(TEXT("zero"), 0);
        B.Add(TEXT("orZero"), TEXT("bool"), TEXT("or")); B.Wire(TEXT("score"), TEXT("value"), TEXT("orZero"), TEXT("a")); B.Wire(TEXT("zero"), TEXT("value"), TEXT("orZero"), TEXT("b"));
        B.Add(TEXT("increment"), TEXT("math"), TEXT("add")); B.Wire(TEXT("orZero"), TEXT("value"), TEXT("increment"), TEXT("a")); B.Set(TEXT("increment"), TEXT("b"), TEXT("1"));
        B.Add(TEXT("setScore"), TEXT("call"), TEXT("State.Set")); B.Wire(TEXT("key"), TEXT("value"), TEXT("setScore"), TEXT("key")); B.Wire(TEXT("increment"), TEXT("value"), TEXT("setScore"), TEXT("value"));
        B.Add(TEXT("wait"), TEXT("delay")); B.Set(TEXT("wait"), TEXT("seconds"), TEXT("5"));
        B.Add(TEXT("show"), TEXT("set"), TEXT("Entity.Visible")); B.Wire(TEXT("tagged"), TEXT("entity"), TEXT("show"), TEXT("target")); B.Set(TEXT("show"), TEXT("value"), TEXT("true"));
        B.Add(TEXT("home"), TEXT("get"), TEXT("Entity.Position")); B.Wire(TEXT("tagged"), TEXT("entity"), TEXT("home"), TEXT("target"));
        B.Add(TEXT("startBob"), TEXT("callfunction"), TEXT("Bob")); B.Wire(TEXT("tagged"), TEXT("entity"), TEXT("startBob"), TEXT("coin")); B.Wire(TEXT("home"), TEXT("value"), TEXT("startBob"), TEXT("home"));
        B.Add(TEXT("bob"), TEXT("function"), TEXT("Bob"));
        B.Add(TEXT("bobLoop"), TEXT("while")); B.Set(TEXT("bobLoop"), TEXT("condition"), TEXT("true"));
        B.Vector(TEXT("height"), FVector(0, 0, 40));
        B.Add(TEXT("raised"), TEXT("vectormath"), TEXT("add")); B.Wire(TEXT("bob"), TEXT("home"), TEXT("raised"), TEXT("a")); B.Wire(TEXT("height"), TEXT("value"), TEXT("raised"), TEXT("b"));
        B.Add(TEXT("upGoal"), TEXT("positiongoal")); B.Wire(TEXT("raised"), TEXT("value"), TEXT("upGoal"), TEXT("position"));
        B.Add(TEXT("homeGoal"), TEXT("positiongoal")); B.Wire(TEXT("bob"), TEXT("home"), TEXT("homeGoal"), TEXT("position"));
        B.Add(TEXT("up"), TEXT("call"), TEXT("Entity.Tween")); B.Wire(TEXT("bob"), TEXT("coin"), TEXT("up"), TEXT("target")); B.Wire(TEXT("upGoal"), TEXT("value"), TEXT("up"), TEXT("goal"));
        B.Set(TEXT("up"), TEXT("seconds"), TEXT("1")); B.Set(TEXT("up"), TEXT("style"), TEXT("Sine")); B.Set(TEXT("up"), TEXT("direction"), TEXT("InOut"));
        B.Add(TEXT("upWait"), TEXT("call"), TEXT("Signal.Wait")); B.Wire(TEXT("up"), TEXT("value"), TEXT("upWait"), TEXT("target"));
        B.Add(TEXT("down"), TEXT("call"), TEXT("Entity.Tween")); B.Wire(TEXT("bob"), TEXT("coin"), TEXT("down"), TEXT("target")); B.Wire(TEXT("homeGoal"), TEXT("value"), TEXT("down"), TEXT("goal"));
        B.Set(TEXT("down"), TEXT("seconds"), TEXT("1")); B.Set(TEXT("down"), TEXT("style"), TEXT("Sine")); B.Set(TEXT("down"), TEXT("direction"), TEXT("InOut"));
        B.Add(TEXT("downWait"), TEXT("call"), TEXT("Signal.Wait")); B.Wire(TEXT("down"), TEXT("value"), TEXT("downWait"), TEXT("target"));
        B.Exec(TEXT("tagged"), TEXT("setup")); B.Exec(TEXT("setup"), TEXT("touch"), TEXT("0")); B.Exec(TEXT("setup"), TEXT("startBob"), TEXT("1"));
        B.Exec(TEXT("touch"), TEXT("guard")); B.Exec(TEXT("guard"), TEXT("hide"), TEXT("true")); B.Exec(TEXT("hide"), TEXT("score"));
        B.Exec(TEXT("score"), TEXT("setScore")); B.Exec(TEXT("setScore"), TEXT("wait")); B.Exec(TEXT("wait"), TEXT("show"));
        B.Exec(TEXT("bob"), TEXT("bobLoop")); B.Exec(TEXT("bobLoop"), TEXT("up"), TEXT("body")); B.Exec(TEXT("up"), TEXT("upWait")); B.Exec(TEXT("upWait"), TEXT("down")); B.Exec(TEXT("down"), TEXT("downWait"));
    }

    void BuildDoorGraph(UMantleScriptEdGraph& G)
    {
        G.Variables.Add(FMantleEdGraphVariable{TEXT("DoorClosedPos"), TEXT("Vector3")});
        G.Variables.Add(FMantleEdGraphVariable{TEXT("IsOpen"), TEXT("boolean")});
        G.Variables.Add(FMantleEdGraphVariable{TEXT("Near"), TEXT("boolean")});
        FRecipeGraph B(G);
        B.Add(TEXT("begin"), TEXT("start"));
        B.Add(TEXT("find"), TEXT("call"), TEXT("World.FindFirst")); B.Set(TEXT("find"), TEXT("name"), TEXT("door"));
        B.Add(TEXT("position"), TEXT("get"), TEXT("Entity.Position")); B.Wire(TEXT("find"), TEXT("value"), TEXT("position"), TEXT("target"));
        B.Add(TEXT("capture"), TEXT("setvar"), TEXT("DoorClosedPos")); B.Wire(TEXT("position"), TEXT("value"), TEXT("capture"), TEXT("value"));
        B.Add(TEXT("closed"), TEXT("getvar"), TEXT("DoorClosedPos")); B.Add(TEXT("isOpen"), TEXT("getvar"), TEXT("IsOpen")); B.Add(TEXT("near"), TEXT("getvar"), TEXT("Near"));
        B.Add(TEXT("poll"), TEXT("while")); B.Set(TEXT("poll"), TEXT("condition"), TEXT("true"));
        B.Add(TEXT("clearNear"), TEXT("setvar"), TEXT("Near")); B.Set(TEXT("clearNear"), TEXT("value"), TEXT("false"));
        B.Add(TEXT("players"), TEXT("call"), TEXT("Players.GetPlayers"));
        B.Add(TEXT("each"), TEXT("foreach")); B.Wire(TEXT("players"), TEXT("value"), TEXT("each"), TEXT("array"));
        B.Add(TEXT("playerPos"), TEXT("get"), TEXT("Player.Position")); B.Wire(TEXT("each"), TEXT("value"), TEXT("playerPos"), TEXT("target"));
        B.Boolean(TEXT("true"), true);
        B.Add(TEXT("hasPosition"), TEXT("bool"), TEXT("and")); B.Wire(TEXT("playerPos"), TEXT("value"), TEXT("hasPosition"), TEXT("a")); B.Wire(TEXT("true"), TEXT("value"), TEXT("hasPosition"), TEXT("b"));
        B.Add(TEXT("positionGuard"), TEXT("branch")); B.Wire(TEXT("hasPosition"), TEXT("value"), TEXT("positionGuard"), TEXT("condition"));
        B.Add(TEXT("offset"), TEXT("vectormath"), TEXT("sub")); B.Wire(TEXT("playerPos"), TEXT("value"), TEXT("offset"), TEXT("a")); B.Wire(TEXT("closed"), TEXT("value"), TEXT("offset"), TEXT("b"));
        B.Add(TEXT("parts"), TEXT("breakvector3")); B.Wire(TEXT("offset"), TEXT("value"), TEXT("parts"), TEXT("value"));
        for (const TCHAR* Axis : {TEXT("x"), TEXT("y"), TEXT("z")})
        {
            const FString Square = FString(Axis) + TEXT("2");
            B.Add(*Square, TEXT("math"), TEXT("mul")); B.Wire(TEXT("parts"), Axis, *Square, TEXT("a")); B.Wire(TEXT("parts"), Axis, *Square, TEXT("b"));
        }
        B.Add(TEXT("xy2"), TEXT("math"), TEXT("add")); B.Wire(TEXT("x2"), TEXT("value"), TEXT("xy2"), TEXT("a")); B.Wire(TEXT("y2"), TEXT("value"), TEXT("xy2"), TEXT("b"));
        B.Add(TEXT("distance2"), TEXT("math"), TEXT("add")); B.Wire(TEXT("xy2"), TEXT("value"), TEXT("distance2"), TEXT("a")); B.Wire(TEXT("z2"), TEXT("value"), TEXT("distance2"), TEXT("b"));
        B.Number(TEXT("radius2"), 450 * 450);
        B.Add(TEXT("inRange"), TEXT("compare"), TEXT("lt")); B.Wire(TEXT("distance2"), TEXT("value"), TEXT("inRange"), TEXT("a")); B.Wire(TEXT("radius2"), TEXT("value"), TEXT("inRange"), TEXT("b"));
        B.Add(TEXT("rangeGuard"), TEXT("branch")); B.Wire(TEXT("inRange"), TEXT("value"), TEXT("rangeGuard"), TEXT("condition"));
        B.Add(TEXT("markNear"), TEXT("setvar"), TEXT("Near")); B.Set(TEXT("markNear"), TEXT("value"), TEXT("true"));
        B.Add(TEXT("afterScan"), TEXT("sequence"));
        B.Add(TEXT("changed"), TEXT("compare"), TEXT("ne")); B.Wire(TEXT("near"), TEXT("value"), TEXT("changed"), TEXT("a")); B.Wire(TEXT("isOpen"), TEXT("value"), TEXT("changed"), TEXT("b"));
        B.Add(TEXT("changeGuard"), TEXT("branch")); B.Wire(TEXT("changed"), TEXT("value"), TEXT("changeGuard"), TEXT("condition"));
        B.Add(TEXT("remember"), TEXT("setvar"), TEXT("IsOpen")); B.Wire(TEXT("near"), TEXT("value"), TEXT("remember"), TEXT("value"));
        B.Add(TEXT("direction"), TEXT("branch")); B.Wire(TEXT("isOpen"), TEXT("value"), TEXT("direction"), TEXT("condition"));
        B.Vector(TEXT("height"), FVector(0, 0, 390));
        B.Add(TEXT("openPos"), TEXT("vectormath"), TEXT("sub")); B.Wire(TEXT("closed"), TEXT("value"), TEXT("openPos"), TEXT("a")); B.Wire(TEXT("height"), TEXT("value"), TEXT("openPos"), TEXT("b"));
        B.Add(TEXT("openGoal"), TEXT("positiongoal")); B.Wire(TEXT("openPos"), TEXT("value"), TEXT("openGoal"), TEXT("position"));
        B.Add(TEXT("closedGoal"), TEXT("positiongoal")); B.Wire(TEXT("closed"), TEXT("value"), TEXT("closedGoal"), TEXT("position"));
        for (const TCHAR* Id : {TEXT("open"), TEXT("close")})
        {
            B.Add(Id, TEXT("call"), TEXT("Entity.Tween")); B.Wire(TEXT("find"), TEXT("value"), Id, TEXT("target"));
            B.Wire(FCString::Strcmp(Id, TEXT("open")) == 0 ? TEXT("openGoal") : TEXT("closedGoal"), TEXT("value"), Id, TEXT("goal"));
            B.Set(Id, TEXT("seconds"), TEXT("0.6")); B.Set(Id, TEXT("style"), TEXT("Linear")); B.Set(Id, TEXT("direction"), TEXT("InOut"));
        }
        B.Add(TEXT("tick"), TEXT("delay")); B.Set(TEXT("tick"), TEXT("seconds"), TEXT("0.2"));
        B.Exec(TEXT("begin"), TEXT("find")); B.Exec(TEXT("find"), TEXT("capture")); B.Exec(TEXT("capture"), TEXT("poll"));
        B.Exec(TEXT("poll"), TEXT("clearNear"), TEXT("body")); B.Exec(TEXT("clearNear"), TEXT("players")); B.Exec(TEXT("players"), TEXT("each"));
        B.Exec(TEXT("each"), TEXT("positionGuard"), TEXT("body")); B.Exec(TEXT("positionGuard"), TEXT("rangeGuard"), TEXT("true")); B.Exec(TEXT("rangeGuard"), TEXT("markNear"), TEXT("true"));
        B.Exec(TEXT("each"), TEXT("afterScan"), TEXT("done")); B.Exec(TEXT("afterScan"), TEXT("changeGuard"), TEXT("0")); B.Exec(TEXT("afterScan"), TEXT("tick"), TEXT("1"));
        B.Exec(TEXT("changeGuard"), TEXT("remember"), TEXT("true")); B.Exec(TEXT("remember"), TEXT("direction")); B.Exec(TEXT("direction"), TEXT("open"), TEXT("true")); B.Exec(TEXT("direction"), TEXT("close"), TEXT("false"));
    }
	void BuildRoundTimerServerGraph(UMantleScriptEdGraph& G)
	{
		G.Variables.Add(FMantleEdGraphVariable{TEXT("Round"), TEXT("number")});

		UMantleScriptGraphNode& Begin = FGB::AddNode(G, TEXT("start"), FVector2D(0, 0));
		UMantleScriptGraphNode& Joined = FGB::AddMemberNode(G, TEXT("event"), TEXT("Players.PlayerAdded"), FVector2D(0, 300));
		UMantleScriptGraphNode& WelcomeRemote = FGB::AddMemberNode(G, TEXT("call"), TEXT("Net.Remote"), FVector2D(300, 300));
		FGB::SetDefault(WelcomeRemote, TEXT("name"), TEXT("Announce"));
		UMantleScriptGraphNode& FireWelcome = FGB::AddMemberNode(G, TEXT("call"), TEXT("Remote.FireClient"), FVector2D(600, 300));
		FGB::Wire(WelcomeRemote, TEXT("value"), FireWelcome, TEXT("target"));
		FGB::Wire(Joined, TEXT("player"), FireWelcome, TEXT("player"));

		UMantleScriptGraphNode& LoopForever = FGB::AddNode(G, TEXT("while"), FVector2D(300, 0));
		FGB::SetDefault(LoopForever, TEXT("condition"), TEXT("true"));

		UMantleScriptGraphNode& GetRound = FGB::AddNode(G, TEXT("getvar"), FVector2D(600, -200), [](UMantleScriptGraphNode& N) { N.VariableName = TEXT("Round"); });
		UMantleScriptGraphNode& NextRound = FGB::AddNode(G, TEXT("math"), FVector2D(900, -200), [](UMantleScriptGraphNode& N) { N.Op = TEXT("add"); });
		FGB::Wire(GetRound, TEXT("value"), NextRound, TEXT("a"));
		FGB::SetDefault(NextRound, TEXT("b"), TEXT("1"));
		UMantleScriptGraphNode& SetRound = FGB::AddNode(G, TEXT("setvar"), FVector2D(600, 100), [](UMantleScriptGraphNode& N) { N.VariableName = TEXT("Round"); });
		FGB::Wire(NextRound, TEXT("value"), SetRound, TEXT("value"));
		UMantleScriptGraphNode& PublishRound = FGB::AddMemberNode(G, TEXT("call"), TEXT("State.Set"), FVector2D(900, 100));
		FGB::SetDefault(PublishRound, TEXT("key"), TEXT("round"));
		FGB::Wire(GetRound, TEXT("value"), PublishRound, TEXT("value"));
		UMantleScriptGraphNode& AnnounceRemote = FGB::AddMemberNode(G, TEXT("call"), TEXT("Net.Remote"), FVector2D(1200, 100));
		FGB::SetDefault(AnnounceRemote, TEXT("name"), TEXT("Announce"));
		UMantleScriptGraphNode& AnnounceAll = FGB::AddMemberNode(G, TEXT("call"), TEXT("Remote.FireAllClients"), FVector2D(1500, 100));
		FGB::Wire(AnnounceRemote, TEXT("value"), AnnounceAll, TEXT("target"));

		UMantleScriptGraphNode& Countdown = FGB::AddNode(G, TEXT("forloop"), FVector2D(1800, 100));
		FGB::SetDefault(Countdown, TEXT("first"), TEXT("30"));
		FGB::SetDefault(Countdown, TEXT("last"), TEXT("0"));
		FGB::SetDefault(Countdown, TEXT("step"), TEXT("-1"));
		UMantleScriptGraphNode& PublishTime = FGB::AddMemberNode(G, TEXT("call"), TEXT("State.Set"), FVector2D(2100, 100));
		FGB::SetDefault(PublishTime, TEXT("key"), TEXT("timeLeft"));
		FGB::Wire(Countdown, TEXT("index"), PublishTime, TEXT("value"));
		UMantleScriptGraphNode& Tick = FGB::AddNode(G, TEXT("delay"), FVector2D(2400, 100));
		FGB::SetDefault(Tick, TEXT("seconds"), TEXT("1"));

		FGB::Wire(Begin, TEXT("next"), LoopForever, TEXT("In"));
		FGB::Wire(Joined, TEXT("next"), WelcomeRemote, TEXT("In"));
		FGB::Wire(WelcomeRemote, TEXT("next"), FireWelcome, TEXT("In"));
		FGB::Wire(LoopForever, TEXT("body"), SetRound, TEXT("In"));
		FGB::Wire(SetRound, TEXT("next"), PublishRound, TEXT("In"));
		FGB::Wire(PublishRound, TEXT("next"), AnnounceRemote, TEXT("In"));
		FGB::Wire(AnnounceRemote, TEXT("next"), AnnounceAll, TEXT("In"));
		FGB::Wire(AnnounceAll, TEXT("next"), Countdown, TEXT("In"));
		FGB::Wire(Countdown, TEXT("body"), PublishTime, TEXT("In"));
		FGB::Wire(PublishTime, TEXT("next"), Tick, TEXT("In"));
	}

	void BuildRoundTimerHudGraph(UMantleScriptEdGraph& G)
	{
		UMantleScriptGraphNode& Begin = FGB::AddNode(G, TEXT("start"), FVector2D(0, 0));
		UMantleScriptGraphNode& Ready = FGB::AddNode(G, TEXT("print"), FVector2D(300, 0), [](UMantleScriptGraphNode& N) { N.TextValue = TEXT("HUD ready"); });
		UMantleScriptGraphNode& AnnounceRemote = FGB::AddMemberNode(G, TEXT("call"), TEXT("Net.Remote"), FVector2D(600, 0));
		FGB::SetDefault(AnnounceRemote, TEXT("name"), TEXT("Announce"));
		UMantleScriptGraphNode& OnAnnounce = FGB::AddMemberNode(G, TEXT("event"), TEXT("Remote.OnClientEvent"), FVector2D(900, 0),
			[](UMantleScriptGraphNode& N) { N.ExtraOutputNames = {TEXT("message")}; });
		FGB::Wire(AnnounceRemote, TEXT("value"), OnAnnounce, TEXT("target"));
		UMantleScriptGraphNode& Toast = FGB::AddMemberNode(G, TEXT("call"), TEXT("UI.Toast"), FVector2D(1200, 0));
		FGB::Wire(OnAnnounce, TEXT("message"), Toast, TEXT("text"));
		FGB::SetDefault(Toast, TEXT("seconds"), TEXT("3"));

		UMantleScriptGraphNode& Me = FGB::AddMemberNode(G, TEXT("get"), TEXT("Players.LocalPlayer"), FVector2D(0, 300));
		UMantleScriptGraphNode& MyId = FGB::AddMemberNode(G, TEXT("get"), TEXT("Player.UserId"), FVector2D(300, 300));
		FGB::Wire(Me, TEXT("value"), MyId, TEXT("target"));
		UMantleScriptGraphNode& ScoreKey = FGB::AddNode(G, TEXT("format"), FVector2D(600, 300), [](UMantleScriptGraphNode& N) { N.TextValue = TEXT("score:{}"); N.NumOutputs = 1; });
		FGB::Wire(MyId, TEXT("value"), ScoreKey, TEXT("values0"));

		UMantleScriptGraphNode& ObsScore = FGB::AddMemberNode(G, TEXT("event"), TEXT("State.Observe"), FVector2D(900, 300));
		FGB::Wire(ScoreKey, TEXT("value"), ObsScore, TEXT("key"));
		UMantleScriptGraphNode& ScoreText = FGB::AddNode(G, TEXT("format"), FVector2D(1200, 300), [](UMantleScriptGraphNode& N) { N.TextValue = TEXT("Coins: {}"); N.NumOutputs = 1; });
		FGB::Wire(ObsScore, TEXT("value"), ScoreText, TEXT("values0"));
		UMantleScriptGraphNode& SetScoreText = FGB::AddMemberNode(G, TEXT("call"), TEXT("UI.SetText"), FVector2D(1500, 300));
		FGB::SetDefault(SetScoreText, TEXT("slot"), TEXT("score"));
		FGB::Wire(ScoreText, TEXT("value"), SetScoreText, TEXT("text"));

		UMantleScriptGraphNode& ObsRound = FGB::AddMemberNode(G, TEXT("event"), TEXT("State.Observe"), FVector2D(900, 600));
		FGB::SetDefault(ObsRound, TEXT("key"), TEXT("round"));
		UMantleScriptGraphNode& RoundText = FGB::AddNode(G, TEXT("format"), FVector2D(1200, 600), [](UMantleScriptGraphNode& N) { N.TextValue = TEXT("Round {}"); N.NumOutputs = 1; });
		FGB::Wire(ObsRound, TEXT("value"), RoundText, TEXT("values0"));
		UMantleScriptGraphNode& SetRoundText = FGB::AddMemberNode(G, TEXT("call"), TEXT("UI.SetText"), FVector2D(1500, 600));
		FGB::SetDefault(SetRoundText, TEXT("slot"), TEXT("round"));
		FGB::Wire(RoundText, TEXT("value"), SetRoundText, TEXT("text"));

		UMantleScriptGraphNode& ObsTime = FGB::AddMemberNode(G, TEXT("event"), TEXT("State.Observe"), FVector2D(900, 900));
		FGB::SetDefault(ObsTime, TEXT("key"), TEXT("timeLeft"));
		UMantleScriptGraphNode& TimeText = FGB::AddNode(G, TEXT("format"), FVector2D(1200, 900), [](UMantleScriptGraphNode& N) { N.TextValue = TEXT("Time left: {}s"); N.NumOutputs = 1; });
		FGB::Wire(ObsTime, TEXT("value"), TimeText, TEXT("values0"));
		UMantleScriptGraphNode& SetTimeText = FGB::AddMemberNode(G, TEXT("call"), TEXT("UI.SetText"), FVector2D(1500, 900));
		FGB::SetDefault(SetTimeText, TEXT("slot"), TEXT("time"));
		FGB::Wire(TimeText, TEXT("value"), SetTimeText, TEXT("text"));

		FGB::Wire(Begin, TEXT("next"), Ready, TEXT("In"));
		FGB::Wire(Ready, TEXT("next"), AnnounceRemote, TEXT("In"));
		FGB::Wire(AnnounceRemote, TEXT("next"), OnAnnounce, TEXT("In"));
		FGB::Wire(OnAnnounce, TEXT("next"), Toast, TEXT("In"));
		FGB::Wire(ObsScore, TEXT("next"), SetScoreText, TEXT("In"));
		FGB::Wire(ObsRound, TEXT("next"), SetRoundText, TEXT("In"));
		FGB::Wire(ObsTime, TEXT("next"), SetTimeText, TEXT("In"));
	}

	void BuildGraphTemplate(UMantleScriptEdGraph& G, EMantleScriptTemplateKind Template, EMantleScriptRowSide Side)
	{
		switch (Template)
		{
		case EMantleScriptTemplateKind::KillBrick: BuildKillBrickGraph(G); return;
		case EMantleScriptTemplateKind::Coins: BuildCoinsGraph(G); return;
		case EMantleScriptTemplateKind::Door: BuildDoorGraph(G); return;
		case EMantleScriptTemplateKind::RoundTimer:
			if (Side == EMantleSide::Server) BuildRoundTimerServerGraph(G);
			else BuildRoundTimerHudGraph(G);
			return;
		case EMantleScriptTemplateKind::Empty:
		default:
			FGB::AddNode(G, TEXT("start"), FVector2D(0, 0));
			return;
		}
	}
}

void FMantleScriptTemplates::PopulateGraph(UMantleScriptEdGraph& Graph, EMantleScriptTemplateKind Template, EMantleScriptRowSide Side)
{
    BuildGraphTemplate(Graph, Template, Side);
}

FString FMantleScriptTemplates::CreateGraphAsset(const FString& ScriptsPackageRoot, const FMantleNewScriptRequest& Req, FString& OutError)
{
	if (ScriptsPackageRoot.IsEmpty())
	{
		OutError = TEXT("no Scripts folder to write into -- open a level under /Game first");
		return FString();
	}
	if (Req.Side != EMantleSide::Server && Req.Side != EMantleSide::Client)
	{
		// A graph compiles to one side's Luau; unlike a plain script it can't require()-only
		// (there is no "Shared" concept for a compiled graph -- see FMantleScriptGraphSerializer::
		// ClassifySide). Caught here so New Script explains it immediately rather than the graph
		// silently landing in Scripts/Shared and only failing much later, on first compile.
		OutError = TEXT("a graph must be Server or Client -- there's no Shared graph");
		return FString();
	}
	FString Name = Req.Name;
	Name.TrimStartAndEndInline();
	if (Name.IsEmpty())
	{
		OutError = TEXT("give the script a name");
		return FString();
	}
	for (TCHAR C : Name)
	{
		if (!FChar::IsAlnum(C) && C != '_' && C != '-')
		{
			OutError = FString::Printf(TEXT("\"%s\" isn't a valid file name: use letters, numbers, _ and - only"), *Name);
			return FString();
		}
	}

	const FString PackagePath = ScriptsPackageRoot / SideFolderName(Req.Side) / Name;
	if (FPackageName::DoesPackageExist(PackagePath))
	{
		OutError = FString::Printf(TEXT("%s already exists"), *PackagePath);
		return FString();
	}

	UPackage* Package = CreatePackage(*PackagePath);
	UMantleScriptGraph* Graph = NewObject<UMantleScriptGraph>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
	Graph->EnsureEdGraph();
	UMantleScriptEdGraph* EdGraph = CastChecked<UMantleScriptEdGraph>(Graph->EdGraph);
	PopulateGraph(*EdGraph, Req.Template, Req.Side);

	FAssetRegistryModule::AssetCreated(Graph);
	Package->MarkPackageDirty();

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FString FileToSave = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Graph, *FileToSave, SaveArgs))
	{
		OutError = FString::Printf(TEXT("couldn't save %s"), *FileToSave);
		return FString();
	}

	return PackagePath;
}
