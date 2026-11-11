# Mantle Core

The Unreal Engine plugin for making things for [Mantle](https://mantleplatform.com). Build a place in an ordinary Unreal level, check it, play it in the Mantle game on your own PC, and publish it for players to join. The plugin also turns your vehicles, weapons, avatars, clothing and emotes into Mantle items.

- **Engine:** Unreal Engine 5.8
- **Platform:** Windows (64-bit) only
- **Full docs:** [mantleplatform.com/docs/creators/plugin](https://mantleplatform.com/docs/creators/plugin/)

## What you need

- **Unreal Engine 5.8** from the Epic Games Launcher.
- **The Mantle game**, installed from Steam or the Epic Games Store on the same PC. **Test Locally** runs your level in it, and you make your creator token in it.
- **A Mantle account**, to save and publish. Starting the Mantle game signs you in.

## Install

1. On [Mantle Core on Fab](https://www.fab.com/listings/0ab720ef-e3da-4dc7-81ee-7eafb573c6bc), add Mantle Core to your library.
2. In the Epic Games Launcher, go to **Unreal Engine → Library → Fab Library**, find Mantle Core, and choose **Install to Engine** for 5.8. It installs into the engine, so every 5.8 project can use it.
3. Open any 5.8 project (a new blank one is fine), choose **Edit → Plugins**, search for **Mantle Core** (in the "Mantle" category), check it, and restart the editor.

The plugin turns on the Epic plugins it needs, such as IK Rig, Groom, glTF Exporter and MetaHuman, all of which come with the engine.

### Updating

Close the editor, then install Mantle Core again from **Fab Library → Install to Engine** in the Epic Games Launcher. The new version replaces the old one in your engine, and your projects pick it up the next time you open them.

### Source zips

Each [release](https://github.com/mantleplatform/MantleCore/releases) has a zip of the plugin's source code, which is what Fab builds from. It has no built files, so a Launcher engine can't use it directly. Use Fab unless you build plugins from source yourself.

## After you open a project

### 1. Find the tools

With the plugin turned on, the toolbar next to **Play** has two new buttons:

- **Test Locally** plays the level you have open in the Mantle game.
- **Mantle** is a dropdown with every other Mantle tool.

The same list is under **Tools → Mantle**. Your settings are under **Editor Preferences → Plugins → Mantle**.

### 2. Start a place

Open or create a level, then choose **Mantle → Start a new place**. It adds a baseplate, a spawn point and a sun. Build from there with your own meshes, lights and materials. Fab packs work too.

- **Mantle → Place settings** sets max players, gravity and respawn.
- **Add Weapon Spawn**, **Add Item Spawn** and **Add Script Point** drop Mantle actors in front of you in the viewport.

### 3. Play it

Press **Test Locally**. The Mantle game opens with your level, and the **Mantle Preview** panel shows what's happening, including any script errors. You don't need to sign in, and nothing is uploaded. Press it again after a change, and **Stop Preview** closes it.

### 4. Check it

- **Mantle → Check this place** lists anything Mantle can't publish, and why.
- **Mantle → Check what players receive** rebuilds the level the way the game will and lists anything that won't reach players.

### 5. Sign in and publish

1. In the Mantle game, go to **Settings → Account → Creator tokens** and make a token.
2. In the editor, choose **Mantle → Sign in with creator token...** and paste it. The token is kept in memory only, until you close the editor.
3. Choose **Mantle → Save to Mantle** to upload a private draft only you can see, or **Mantle → Publish** to make it live for players to join.

The [Quick start](https://mantleplatform.com/docs/creators/quick-start/) walks through your first world step by step.

## More tools

These are all in the **Mantle** dropdown:

| Tool | What it does | Docs |
|---|---|---|
| **Build a vehicle...** | Makes a drivable vehicle from your own 3D model. | [Vehicles](https://mantleplatform.com/docs/creators/vehicles/) |
| **Build a weapon...** | Turns a 3D model into a gun, bow, blade or other weapon. | [Weapons](https://mantleplatform.com/docs/creators/weapons/) |
| **Cosmetics...** | Publishes avatars, hair, clothing, accessories and emotes. | [Avatars and emotes](https://mantleplatform.com/docs/creators/avatars-emotes/), [Clothing](https://mantleplatform.com/docs/creators/clothing/) |
| **Library...** | Finds vehicles, weapons and script packages, yours or other creators' public ones, and adds them to your place. | [Script packages](https://mantleplatform.com/docs/creators/script-packages/) |
| **Voxelize selected meshes** | Turns meshes into a block world, or smooth ground, that players can dig and build in. | [Block worlds](https://mantleplatform.com/docs/creators/block-worlds/) |

For scripting, the **Mantle Scripts** button on the toolbar (also under **Tools**) opens a panel to browse, create and check your level's scripts. See [Scripting](https://mantleplatform.com/docs/scripting/).

## Need help?

The [creator docs](https://mantleplatform.com/docs/creators/) cover each tool in full, including what "Check this place" catches and how publishing works.
