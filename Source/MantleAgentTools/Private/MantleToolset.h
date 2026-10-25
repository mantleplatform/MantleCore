// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "ToolsetRegistry/ToolsetDefinition.h"

#include "MantleToolset.generated.h"

class UToolCallAsyncResultString;

/**
 * Mantle: check, preview, export, publish and follow a Mantle place from the level open in the
 * Unreal editor, and convert, check and upload cosmetics (clothing, hair, avatars, overlays, emotes) exactly as the
 * Cosmetics panel does, with the panel's own messages.
 * Place tools work on the editor's current level. Pass every parameter a tool lists, with ""
 * for one that may be empty: a call that leaves them out is refused before the tool runs.
 * Every tool returns one JSON object as text.
 * A tool that could not do its job says so in that object ("ok": false, with "refused" a short
 * code and "message" a sentence for the creator). Tools that talk to Mantle need the creator to
 * be signed in (Tools > Mantle > Sign in with creator token...); no tool accepts or returns a
 * token, so never ask the creator for one.
 */
UCLASS()
class UMantleToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Check this place: runs Mantle's validator on the open level, exactly as Tools > Mantle >
	 * Check this place does, and returns every issue. Read-only.
	 * @return JSON: {"ok", "level", "ready" (true when nothing blocks publishing), "errors",
	 * "warnings", "issues": [{"severity": "error"|"warning", "actor" (outliner label, empty for
	 * place-wide issues), "message" (what is wrong), "remedy" (what to do about it)}]}.
	 */
	UFUNCTION(meta = (AICallable))
	static FString CheckPlace();

	/**
	 * Export preview: exports the open level to Mantle's place format in memory and summarises
	 * it. Nothing is written or uploaded; meshes and materials from the project carry provisional
	 * ids until a publish uploads them. Read-only.
	 * @return JSON: {"ok", "level", "exported" (false when the level cannot be expressed; see
	 * "errors"), "placeJsonBytes", "entities": {"total", "byType": {type: count}}, "assets":
	 * {"total", "bytes", "byKind": {kind: count}}, "scripts", "scriptApi": {"major", "minor",
	 * "analysed"}, "errors": [{"path", "message"}]}.
	 */
	UFUNCTION(meta = (AICallable))
	static FString ExportPreview();

	/**
	 * Publish the open level to Mantle and make it live for players, exactly as Tools > Mantle >
	 * Publish does: check, upload what Mantle does not have yet, wait for processing, publish. It
	 * runs in the background under the Mantle Publish panel, and this call completes when it ends.
	 * This is a public action: players can join the result.
	 * This should ONLY be called after getting explicit direction or permission from the user,
	 * for this specific publish. Refused, with nothing sent, when the creator is not signed in, no
	 * level is open, or a publish is already running. New vehicles in the level become PRIVATE library
	 * items stored in the place (an agent cannot make the rights declaration a public one needs); an
	 * item the creator already made public stays public, and the creator can make others public from the panel.
	 * @param Notes Revision notes shown with this version. Always pass it; "" for none.
	 * @return JSON: {"ok", "placeId", "revision", "live", "waitingForReview", "cancelled",
	 * "message", "issues": [{"severity", "actor", "message", "remedy"}]}, or a refusal.
	 */
	UFUNCTION(meta = (AICallable))
	static UToolCallAsyncResultString* PublishPlace(const FString& Notes = TEXT(""));

	/**
	 * List the places the signed-in creator owns on Mantle (the first page). Read-only.
	 * @return JSON: {"ok", "places": [the API's place objects], "more" (true when there are more
	 * pages)}, or a refusal when not signed in or Mantle could not be reached.
	 */
	UFUNCTION(meta = (AICallable))
	static FString ListMyPlaces();

	/**
	 * Upload status of the running or last publish in this editor, per asset, as the Mantle
	 * Publish panel shows it. No network. Read-only.
	 * @param AssetName Part of an Unreal asset's name or path, or an ast_ id, to report only
	 * matching assets. Always pass it; "" reports every asset.
	 * @return JSON: {"ok", "publish": null when none has run, else {"phase" (idle, checking,
	 * converting, uploading, crediting, scripts, exporting, library, creatingPlace, saving,
	 * publishing, done, failed or cancelled; "library" is a go-live turning uploaded vehicles into
	 * library items), "running", "goLive", "progress", "status", "placeId", "revision"}, "assets": [{"name", "state" (queued,
	 * uploading, processing, approved or refused), "assetId", "bytes", "alreadyOnMantle",
	 * "inReview", "code", "message", "cookError"}]}.
	 */
	UFUNCTION(meta = (AICallable))
	static FString UploadStatus(const FString& AssetName = TEXT(""));

	/**
	 * Explain an issue: runs Check this place on the open level and returns, in full, every issue
	 * whose message, actor or remedy contains Query (issues have no codes; a phrase from a
	 * CheckPlace message is the usual query). Read-only.
	 * @param Query Part of an issue's message, its actor's label, or its remedy.
	 * @return JSON: {"ok", "query", "matches": [{"severity", "actor", "message", "remedy"}]}.
	 */
	UFUNCTION(meta = (AICallable))
	static FString ExplainIssue(const FString& Query);

	/**
	 * The script API version the open level's scripts need, as a publish records it in the place
	 * (the lowest minor that covers every script), and each script's own need. A script the scan
	 * cannot follow makes the place record the editor's newest minor; it is listed with why. Read-only.
	 * @return JSON: {"ok", "scripts", "recorded": {"major", "minor"} (both 0 without scripts),
	 * "editorMinor", "analysed", "neededFor", "perScript": [{"path", "minor", "neededFor",
	 * "analysed", "problem", "remedy"}]}.
	 */
	UFUNCTION(meta = (AICallable))
	static FString ScriptApiRequirement();

	/**
	 * Preview the place as players receive it, exactly as Tools > Mantle > Check what players
	 * receive does: exports the open level, reads the file back and rebuilds it in a scratch world
	 * (meshes and materials already on Mantle load as players get them when signed in), reports,
	 * then tears the rebuilt world down. Nothing is uploaded or changed. Refused when no level is
	 * open or a preview is already running.
	 * @return JSON: {"ok", "level", "built", "sourceEntities", "previewActors", "notUploaded",
	 * "leftOut": [lines], "summary", "issues": [{"severity", "actor", "message", "remedy"}]}.
	 */
	UFUNCTION(meta = (AICallable))
	static FString PreviewPlace();

	/**
	 * Measure an avatar's head the way an avatar upload fits its hat socket (acc_hat): the cranium (the body's and
	 * face's vertices on the head above its joint) as a box in the hat socket's frame, and the socket override an
	 * upload would send against the plugin's reference head. Read-only.
	 * @param BodyPath The avatar's body skeletal mesh.
	 * @param FacePath Its face skeletal mesh; "" for a body that carries its own head. Always pass it.
	 * @return JSON: {"ok", "vertices", "min", "max", "size" ([x, y, z] cm), "reference" (bool), "location", "scale"
	 * (the fitted socket, when there is a reference)}, or a refusal (not_found, not_measured).
	 */
	UFUNCTION(meta = (AICallable))
	static FString MeasureHead(const FString& BodyPath, const FString& FacePath = TEXT(""));

	/**
	 * Check a cosmetic: the Cosmetics panel's "Check only", with the panel's defaults (the authoring body it finds, generated
	 * LODs, thumbnail) and its messages. Exports in memory; nothing is uploaded. Read-only. It takes:
	 * - a skeletal mesh: clothing (a garment slot), hair (a hair slot), or an avatar body (slot avatar; see below);
	 * - a Chaos cloth asset: clothing only;
	 * - a MetaHuman Outfit (an imported .mhpkg outfit): clothing, or a rigid accessory such as a hat (an accessory slot);
	 * - a Groom or a hair cards static mesh: hair slots only;
	 * - a texture: an overlay (a tattoo, scar or makeup mask; an overlay slot). A scar's optional normal map and placing a
	 *   picture on the body are set in the panel;
	 * - an animation sequence: an emote (slot emote; one on the MetaHuman, UE5 or UE4 Mannequin skeleton is retargeted onto
	 *   the UEFN mannequin first, at most 10 s).
	 * An avatar uploads the body ConvertCosmetic made, as the panel's Use picks it: its converted face is found with it, and it
	 * is checked as the route it was converted on (Convert: a converted MetaHuman; As is). A character that has not been
	 * converted (any other skeletal mesh, a static mesh, a MetaHuman Creator character, a Blueprint) goes through
	 * ConvertCosmetic first.
	 * @param AssetPath The asset, e.g. /Game/Clothing/SKM_Jacket, /Game/Outfits/Survivor/OA_SurvivorOutfit, or a converted body
	 * such as /Game/Characters/Converted/Vampire/SKM_Vampire_Body.
	 * @param Slot The item's slot (a garment slot such as top, a hair slot such as hair, avatar, or, for a MetaHuman
	 * Outfit hat or other rigid piece, an accessory slot such as acc_hat: it uploads fitted to the reference head);
	 * "" takes the panel's default, the first 3D clothing slot. Always pass it.
	 * @param Name The item's name; "" takes the asset's name. Always pass it.
	 * @param Sections The sections to upload, as the panel's section ticks: material or material slot names separated
	 * by commas (e.g. "MI_Coat,MI_Shirt" for the top of a mesh holding a whole outfit); "" keeps the panel's default.
	 * Always pass it.
	 * A skeletal mesh or Chaos cloth asset garment made on a MetaHuman Creator body gets the same variants: it is wrapped as an Outfit first.
	 * With no such body it uploads in one size, and a note says so.
	 * A MetaHuman Outfit uploads as clothing with garment variants made automatically: it is resized onto Epic's standard
	 * body types (by default the medium-height underweight, normal and overweight bodies of one sex, named like
	 * "f_med_nrw"), made in memory, so nothing needs converting and no Epic sign-in is needed. The sex follows FitSex;
	 * with "" it is chosen from the Outfit's own sizes. A note in "notes" says which and why. When those bodies cannot be
	 * made, the check fails with an error, as the panel's does: it never uploads such an Outfit in one size.
	 * @param FitSex For a MetaHuman Outfit: the style of body the clothing was made on, as the panel's "Made for":
	 * "female" (women's bodies, f_...), "male" (men's bodies, m_...), or "" / "auto" to let Mantle choose from the
	 * Outfit's sizes. Ask the creator when you do not know; ignored for anything but clothing from an Outfit.
	 * @return JSON: {"ok", "asset", "class", "slot", "name", "canUpload" (no errors), "issues":
	 * [{"severity": "error"|"warning"|"info", "message", "hint"}], "notes": [lines], "files":
	 * [{"key", "label", "bytes"}]}, or a refusal (not_found, not_skeletal_mesh, unknown_slot, convert_first,
	 * outfit_not_clothing, cards_not_hair, cloth_not_clothing, accessory_needs_outfit, animation_not_emote, texture_not_overlay,
	 * unknown_section, unknown_fit_sex, bad_item_id).
	 */
	UFUNCTION(meta = (AICallable))
	static FString CheckCosmetic(const FString& AssetPath, const FString& Slot = TEXT(""), const FString& Name = TEXT(""), const FString& Sections = TEXT(""),
		const FString& FitSex = TEXT(""));

	/**
	 * Upload a cosmetic to Mantle as a new item, exactly as the Cosmetics panel's Upload to Mantle
	 * does: checks it (as CheckCosmetic), uploads the files Mantle does not have, waits for them to
	 * be processed, creates the item and follows it through screening. This call completes when
	 * that ends. The item uploads PRIVATE: an agent cannot make the rights declaration a public item needs.
	 * The creator can make it public from the Cosmetics panel, so tell them it is private.
	 * This should ONLY be called after getting explicit direction or permission from the user,
	 * for this specific upload. A MetaHuman Outfit uploads with garment variants fitted to Epic's standard body types,
	 * made automatically (as CheckCosmetic says); no converted MetaHuman is needed. Refused, with nothing sent, when the creator is not signed in, the
	 * check finds errors, or an upload started here is still running.
	 * @param AssetPath Any asset CheckCosmetic takes: a skeletal mesh, Chaos cloth asset, Outfit, Groom, cards mesh, texture (an
	 * overlay), animation sequence (an emote), or a converted avatar body (its face goes with it).
	 * @param Slot The item's slot, as for CheckCosmetic.
	 * @param Name The item's name players see; "" takes the asset's name. Always pass it.
	 * @param Sections The sections to upload, as for CheckCosmetic. Always pass it.
	 * @param ItemId "" creates a new item. An id (itm_...) publishes a new version into that existing item instead,
	 * which the signed-in creator must own; the slot must match the item's, and the new version is screened again.
	 * @param FitSex For a MetaHuman Outfit, the body it was made on, as for CheckCosmetic: "female", "male", or "" / "auto".
	 * @return JSON: {"ok", "itemId", "itemStatus", "moderationState", "mayClipInMotion",
	 * "message", "files": [{"label", "state", "assetId", "alreadyOnMantle", "message"}]}, or a
	 * refusal (with "issues" when the check found errors).
	 */
	UFUNCTION(meta = (AICallable))
	static UToolCallAsyncResultString* UploadCosmetic(const FString& AssetPath, const FString& Slot, const FString& Name = TEXT(""), const FString& Sections = TEXT(""), const FString& ItemId = TEXT(""),
		const FString& FitSex = TEXT(""));

	/**
	 * Convert a character so it can be uploaded, as the Cosmetics panel's Convert does: lists what
	 * the plugin would fix (the fix list), and with Apply writes the fixes as NEW assets in a
	 * Converted folder next to the character (the creator's own assets are never changed; an
	 * earlier conversion of the same character there is replaced), then runs the upload checks on
	 * each converted part. Nothing is uploaded. Without Apply it is read-only.
	 * Like the panel's Avatar door it says what it sees ("sees"), and for a skeletal mesh converted as an avatar it looks at
	 * what the pack holds beside it ("packReview": the body it converts, each piece of clothing in its slot, hair, and what
	 * is not used, at the panel's defaults).
	 * @param AssetPath Any character the panel's Avatar picker takes: a skeletal mesh (/Game/Characters/SKM_Hero; one with no
	 * usable skin weights is treated like a static mesh), a static mesh (a model with no skeleton, fitted to a MetaHuman
	 * body), a MetaHuman Creator character (an Epic preset or the creator's own, built as it is), a MetaHuman Blueprint or
	 * its body (/Game/CharacterAssemblies/Vampire/BP_Vampire), or such a character's folder (/Game/CharacterAssemblies/Vampire):
	 * a Blueprint yields its avatar, clothing and hair as parts.
	 * @param Slot For a single skeletal mesh, the slot it converts for (avatar, top, bottom,
	 * footwear, hair, eyebrows, eyelashes, facial_hair); "" takes its slot in its character, else
	 * avatar. Ignored for a whole character. Always pass it.
	 * @param bApply False lists the fixes only; true converts. Always pass it.
	 * The body row, as the panel shows it, is four keys per part: "Strip.<part>.FaceOnly" (on: put the face on a MetaHuman
	 * body; pre-ticked when the body is covered by clothing), "Strip.<part>.FeminineBody" / "Strip.<part>.MasculineBody" (the
	 * MetaHuman body it goes on, Women's or Men's; never pre-picked, one is needed while FaceOnly is on), and
	 * "Strip.<part>.BareSkin" (needsConfirm: the kept parts are bare skin; pre-ticked only when a bare body is clearly found,
	 * and not needed while FaceOnly is on). "bodyChoice" says what the row still needs; "canConvert" is the panel's Convert
	 * button.
	 * @param TurnOff Comma-separated fix keys (from "fixes"[].key, e.g. "Triangles.0.Mesh") to leave
	 * alone; every other fix keeps its default ("enabled"). "Strip.<part>.FaceOnly" keeps the character's own body (refused,
	 * cannot_keep_body, when it can't be kept). Turning off another Strip line changes the kept parts, so the bare-skin line
	 * starts unticked again, as in the panel. "" for none. Always pass it.
	 * @param Confirm Comma-separated keys of the fixes that need confirming ("needsConfirm", e.g.
	 * "Strip.0.BareSkin": the kept parts are bare skin) that you confirm, and body row picks ("Strip.0.FeminineBody" or
	 * "Strip.0.MasculineBody": one unpicks the other). Convert does nothing while one is unconfirmed. Only confirm what the
	 * person asking told you is true. "" for none.
	 * @param Route How it should go up, the panel's "How should it go up?": "convert" (Convert to MetaHuman, the default) or
	 * "asis" (As is: it keeps its own look; offered only while As-is uploads are allowed, and refused with the panel's
	 * reason when the panel greys it, e.g. a MetaHuman Creator character, "it's already a MetaHuman"). "" is convert.
	 * Always pass it.
	 * @return JSON: {"ok", "source", "assembly", "outputFolder", "route", "sees", "packReview": [lines], "canConvert", "bodyChoice": [lines], "parts": [{"index", "slot", "class",
	 * "name", "mesh", "original", "face", "section", "skeleton", "authoringBody", "authoringFace",
	 * "included", "clothAsset"}], "fixes": [{"key", "fixer", "kind", "part", "problem", "action",
	 * "canFix", "enabled", "needsConfirm"}], "notes", "unknownTurnOff", "unknownConfirm", "applied", and with Apply "result":
	 * {"removedPrevious", "applied", "failed", "leftAlone" (fixes with a "reason"), "created" (asset
	 * paths), "checks": [{"part", "canUpload", "standingHeightCm", "issues": [{"severity",
	 * "message", "hint"}]}], "blocked"}}, or a refusal (not_convertible, no_contract, unknown_route, as_is_greyed, cannot_keep_body).
	 */
	UFUNCTION(meta = (AICallable))
	static FString ConvertCosmetic(const FString& AssetPath, const FString& Slot, bool bApply, const FString& TurnOff = TEXT(""), const FString& Confirm = TEXT(""),
		const FString& Route = TEXT(""));

	/**
	 * Make a MetaHuman Outfit from a third-party character's clothing: the garment meshes plus that character's bare
	 * body. The body goes through Convert's MetaHuman steps only (strip to bare skin, MetaHuman's A-pose, conform; no
	 * build, no Epic sign-in); each garment is posed and scaled the way the body was, takes the MetaHuman's skin weights,
	 * and becomes an Outfit whose authored size is that MetaHuman. Check or upload each Outfit with CheckCosmetic /
	 * UploadCosmetic (slot top, bottom, ...): it is resized onto Epic's standard bodies like any MetaHuman Outfit. Writes
	 * NEW assets under <body folder>/Converted/<body>_Outfit (the creator's assets are never changed; a body made a
	 * MetaHuman there before is replaced). Nothing is uploaded. Takes minutes (the conform).
	 * @param BodyPath The character's bare body skeletal mesh (the whole body with its head, no clothing), e.g.
	 * /Game/Pack/SK_Hero_Nude. A body with clothing on it works when Strip can remove the clothing slots.
	 * @param GarmentPaths One or more of that character's clothing skeletal meshes, comma-separated.
	 * @param TurnOff Comma-separated Convert fix keys to leave alone, as ConvertCosmetic (e.g. "Strip.0.Slot3" keeps that
	 * slot, for a skin slot whose material isn't named like skin). "" for none. Always pass it.
	 * @param Confirm Comma-separated fix keys to tick, as ConvertCosmetic: "Strip.0.BareSkin" when the person asking says the
	 * kept parts are bare skin. Only confirm what they told you is true. "" for none. Always pass it.
	 * @return JSON: {"ok", "body", "outputFolder", "character", "authoringBody", "inputScale", "offsetCm", "bodyGapCm" (how far
	 * the MetaHuman's skin is from the original body's on average), "outfits": [{"garment", "outfit" (the Outfit's path;
	 * empty when it failed), "error", "vertices", "farVertices", "meanSkinCm"}], "notes", "convert" (ConvertCosmetic's fix
	 * list and result)}, or a refusal (not_found, no_garment, no_contract, no_skeleton, not_convertible, convert_failed
	 * with "convert": e.g. the bare-skin line not confirmed, no_metahuman, pose_failed).
	 */
	UFUNCTION(meta = (AICallable))
	static FString MakeOutfitFromGarment(const FString& BodyPath, const FString& GarmentPaths, const FString& TurnOff = TEXT(""), const FString& Confirm = TEXT(""));
};
