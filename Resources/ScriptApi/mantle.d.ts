// Mantle scripting API for JavaScript/TypeScript. GENERATED from the engine's API registry; do not edit.
// Referenced by tsconfig.json so VS Code type-checks Content/**/Scripts/**/*.ts against this file.
export {};

declare global {

interface Signal<T extends any[] = any[]> {
	connect(handler: (...args: T) => void): Connection;
	/** Fire a custom event from Events.get. */
	fire(...args: T): void;
	once(handler: (...args: T) => void): Connection;
	wait(): Promise<T extends [infer Only] ? Only : T>;
}

class Vector3 {
	constructor(x?: number, y?: number, z?: number);
	readonly x: number;
	readonly y: number;
	readonly z: number;
	readonly magnitude: number;
	readonly unit: Vector3;
	add(other: Vector3 | number): Vector3;
	sub(other: Vector3 | number): Vector3;
	mul(other: Vector3 | number): Vector3;
	div(other: Vector3 | number): Vector3;
	dot(other: Vector3): number;
	cross(other: Vector3): Vector3;
	lerp(goal: Vector3, alpha: number): Vector3;
	distance(other: Vector3): number;
	equals(other: Vector3): boolean;
	toString(): string;
	static readonly zero: Vector3;
	static readonly one: Vector3;
	static readonly xAxis: Vector3;
	static readonly yAxis: Vector3;
	static readonly zAxis: Vector3;
}

/** A thing in the world: any actor in the level. */
interface Entity {
	/** Whether a physics entity is held where it is (true) or simulating (false); the place's physics.simulate says how it starts. Set it on the server to drop or pin it. Every other entity is always anchored, and setting it on one errors. */
	anchored: boolean;
	/** How fast the entity spins, in degrees per second about the world axes (X, Y, Z). Setting it (server only) replaces a physics entity's spin; setting it on an Anchored entity, or one with no physics block, errors. Anything that isn't simulating reads (0, 0, 0). */
	angularVelocity: Vector3;
	/** Whether players and objects bump into it. */
	canCollide: boolean;
	/** How many seconds into its clip a SkeletalMesh is now, the same on every screen; nil when it plays no clip, while the clip is still downloading, and always on a dedicated server (it never loads clips). Read-only. Errors when the entity is not a SkeletalMesh. */
	readonly clipTime: number | undefined;
	/** The collision layer the object is on ("Default" until you set it). Set it to the name of a layer made with Collision:CreateLayer, or to "Default" to take it off its layer. Only the objects of your place can be put on a layer, not players, vehicles or NPCs. Server only. (server only) */
	collisionLayer: string;
	/** A mesh's tint as "#RRGGBB", multiplied into its materials; "#FFFFFF" is none. Setting it on the server changes it for everyone, players who join later included; on a client it only changes that client's view. Only materials uploaded with the place can be tinted: errors on an entity with none of those, or that is not a mesh. */
	color: string;
	/** False once the entity has been destroyed. */
	readonly exists: boolean;
	/** The light's colour as "#RRGGBB", the same format as Vehicles:SetPaintColor. Replicates like LightEnabled. */
	lightColor: string;
	/** Whether the light shines. Setting it on the server changes it for everyone, players who join later included; on a client it only changes that client's view. Errors if the entity is not a light. */
	lightEnabled: boolean;
	/** A spot light's fully lit cone, in degrees, clamped to 0..90. Errors on other lights. Replicates like LightEnabled. */
	lightInnerCone: number;
	/** How bright the light is, in its own units: a place light keeps the units it was made with, a Lights:Spawn light uses candelas, and a directional light uses lux. Clamped to 0..100000 (0..200 for a directional light). Replicates like LightEnabled. */
	lightIntensity: number;
	/** "Point", "Spot" or "Directional" when the entity is a light, nil for anything else. Read-only. */
	readonly lightKind: string | undefined;
	/** A spot light's outer cone, where its light fades to nothing, in degrees, clamped to 0..90. Errors on other lights. Replicates like LightEnabled. */
	lightOuterCone: number;
	/** How far a point or spot light reaches, in centimetres, clamped to 1..100000. Errors on a directional light. Replicates like LightEnabled. */
	lightRange: number;
	/** A physics entity's mass in kg: the place's physics.mass, else computed from its collision. 0 for an entity with no physics block. Read-only. */
	readonly mass: number;
	/** The actor's label. */
	readonly name: string;
	/** A physics entity's surface: "default", "rubber" (bouncy), "ice" (slippery), "metal", "wood" or "stone"; the place's physics.material says how it starts. Set it on the server, anchored or not, and every player sees the change. nil for an entity with no physics block, and setting it on one errors, as does any other name. */
	physicsMaterial: string | undefined;
	/** World location in centimetres. Setting it on the server replicates to everyone; on a client it only changes that client's view. Setting it (or Rotation or Scale) on an entity the place did not mark dynamic is a script error; clones are always dynamic. */
	position: Vector3;
	/** Rotation in degrees as (pitch, yaw, roll). */
	rotation: Vector3;
	/** Scale (1, 1, 1 is normal size). */
	scale: Vector3;
	/** The id of a sound emitter's sound, to pass to Sounds:Play or Sounds:PlayMusic; nil for any other entity. */
	readonly soundId: string | undefined;
	/** Sound emitters only: the emitter's volume, 0 to 2. Setting it from a server script changes it for every player. */
	soundVolume: number;
	/** How see-through a mesh is, from 0 (solid) to 1 (invisible); values outside are clamped. Replicates like Color. Only materials uploaded with the place can be made see-through: errors on an entity with none of those. */
	transparency: number;
	/** How fast the entity moves, in cm/s. Setting it (server only) replaces a physics entity's velocity, and does nothing while it is Anchored; setting it on an entity with no physics block errors. */
	velocity: Vector3;
	/** Whether it is drawn. Hidden entities still collide unless CanCollide is false. */
	visible: boolean;
	/** An attribute on this entity changed. */
	readonly attributeChanged: Signal<[name: string, value: any]>;
	/** A physics entity started touching something (server only): another entity, a player's character, or nil for anything else. Fires once when a contact starts, not while it lasts, and at most a few dozen times a tick across the place. Only physics entities fire it. */
	readonly collided: Signal<[other: Entity | undefined]>;
	/** A player's character stopped touching this entity. */
	readonly touchEnded: Signal<[player: Player]>;
	/** A player's character started touching this entity. */
	readonly touched: Signal<[player: Player]>;
	/** Give the entity a tag (fires World:OnTagged handlers). (server only) */
	addTag(tag: string): void;
	/** Spin a physics entity: an angular impulse in kg*cm^2/s about the world axes, in degrees. Does nothing while it is Anchored. Errors on an entity with no physics block. (server only) */
	applyAngularImpulse(impulse: Vector3): void;
	/** Push a physics entity: impulse is in kg*cm/s along the world axes, so its velocity changes by impulse / Mass. Does nothing while it is Anchored. Errors on an entity with no physics block. (server only) */
	applyImpulse(impulse: Vector3): void;
	/** Make a copy (same look, tags and place). On the server the copy appears for everyone; on a client only for that player. Tip: keep a hidden template and clone it. A physics entity, a Constraint or an NPC can't be cloned (it errors, and says how to make another). */
	clone(): Entity;
	/** Remove the entity. On the server it disappears for everyone; on a client only for that player. */
	destroy(): void;
	/** A custom value stored on the entity, or nil. */
	getAttribute(name: string): any;
	/** The material asset id a mesh shows in a slot (0 when left out), or nil when the slot shows the mesh's own material. */
	getMaterial(slot?: number | undefined): string | undefined;
	/** All of the entity's tags. */
	getTags(): (string)[];
	/** Whether the entity has this tag. */
	hasTag(tag: string): boolean;
	/** Call handler with the attribute's value now and every time it changes. */
	observeAttribute(name: string, handler: (value: any) => void): Connection;
	/** Play one of the place's AnimClip assets on a SkeletalMesh, starting now. Leave clipAssetId out (or nil) to restart the clip it plays now, or its own. params: Loop (true by default; false holds the last frame), PlayRate (clip seconds per second, 1 by default, clamped to 0..10) and StartOffset (seconds into the clip to start at, 0 by default, clamped to 0..3600). On the server it plays for everyone, players who join later included, in step on every screen; on a client it only changes that client's view. Errors when the entity is not a SkeletalMesh, the id is not an AnimClip in the place's assets, or the clip was authored on another skeleton (on a dedicated server, which never loads clips, that last one shows on each player's screen as the mesh's resting pose instead). */
	playClip(clipAssetId?: string | undefined, params?: { loop?: boolean | undefined; playRate?: number | undefined; startOffset?: number | undefined } | undefined): void;
	/** Effects only: start emitting. A looping effect already playing keeps going; one that does not loop bursts for a second again. On the server it plays for everyone, players who join later included; on a client it only changes that client's view. Errors when the entity is not an Effect. */
	playEffect(): void;
	/** Sound emitters only: play the emitter's sound again from the beginning (looping if the emitter loops). From a server script every player hears it. */
	playSound(): void;
	/** Remove a tag (fires World:OnUntagged handlers). (server only) */
	removeTag(tag: string): void;
	/** Store a custom value on the entity (numbers, strings, booleans, Vector3, tables). Server values replicate. */
	setAttribute(name: string, value: any): void;
	/** Effects only: tint the particles, "#RRGGBB"; the next particles take it. Replicates like PlayEffect. Errors when the entity is not an Effect or the colour is not "#RRGGBB". */
	setEffectColor(color: string): void;
	/** Effects only: how many particles spawn a second, clamped to 0..200; 0 emits nothing while it still plays. Replicates like PlayEffect. Errors when the entity is not an Effect. */
	setEffectRate(rate: number): void;
	/** Show another of the place's Material assets on this mesh: in one material slot (counting from 0), or in every slot when slot is left out. "" puts the place's own material back. Replicates like Color. Errors when the id is not a Material in the place's assets, the slot doesn't exist, or the entity is not a mesh. */
	setMaterial(assetId: string, slot?: number | undefined): void;
	/** Stop a SkeletalMesh's clip: it shows its resting pose until the next PlayClip. Replicates like PlayClip. Errors when the entity is not a SkeletalMesh. */
	stopClip(): void;
	/** Effects only: stop emitting; the particles already out die out. Replicates like PlayEffect. Errors when the entity is not an Effect. */
	stopEffect(): void;
	/** Sound emitters only: stop the emitter's sound, fading it out over fadeOut seconds (0 by default). It stays silent until PlaySound, from a server script for players who join later too. */
	stopSound(fadeOut?: number | undefined): void;
	/** Smoothly move/rotate/scale to the goal. Since script API 1.8 the goal can also fade a light's LightColor or LightIntensity, or a mesh's Color; those errors like setting them would. style: Linear, Sine, Quad, Cubic, Quart, Quint, Exponential, Circular, Back, Bounce, Elastic. direction: In, Out, InOut. Returns a Completed signal. A Position, Rotation or Scale goal on an entity the place did not mark dynamic is a script error, like setting them. */
	tween(goal: { position?: Vector3 | undefined; rotation?: Vector3 | undefined; scale?: Vector3 | undefined; lightColor?: string | undefined; lightIntensity?: number | undefined; color?: string | undefined }, seconds: number, style?: string | undefined, direction?: string | undefined): Signal;
}

/** A connected player. */
interface Player {
	/** How much of every hit the character shrugs off, as a percent from 0 (none, the default) to 100 (all of it). Armor works before the Shield and health. nil while the player has no character. Setting it (server only) clamps to 0..100 and lasts for the rest of the session, through respawns. */
	armor: number | undefined;
	/** The item id of the avatar (body) the player has on, or nil before their look has arrived. Read-only. (server only) */
	readonly avatar: string | undefined;
	/** The seconds of air the character has left, from 0 to MaxBreath (it counts in small steps, the same on every machine, and moves only while BreathEnabled is true); nil while the player has no character. Setting it (server only) clamps to 0..MaxBreath and restarts the wait before it refills. Every new character starts full. */
	breath: number | undefined;
	/** Whether the player can run out of air underwater (false unless a script turns it on, so swimming is free in places that never use it); nil while the player has no character. With it on, Breath drains one second a second while IsUnderwater, refills at the surface after BreathRegenDelay, and once it is gone the character takes DrownDamage every second until it surfaces or dies (Died then says "Drown"). Turning it on starts the breath full. Setting it (server only) lasts for the rest of the session, through respawns. */
	breathEnabled: boolean | undefined;
	/** How many seconds of breath come back each second at the surface (10 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..10000 and lasts for the rest of the session. */
	breathRegen: number | undefined;
	/** How many seconds after the head comes out of the water before breath starts coming back (0.5 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..60 and lasts for the rest of the session. */
	breathRegenDelay: number | undefined;
	/** The player's body in the world, or nil while respawning. */
	readonly character: Entity | undefined;
	/** Whether the player can crouch or slide (true unless a script changes it); nil while the player has no character. Setting it (server only) to false stands a crouching player up and the crouch key does nothing until it is true again. It lasts for the rest of the session, through respawns. */
	crouchEnabled: boolean | undefined;
	/** How fast the character moves while crouched, in centimetres per second (225 unless a script changes it; it scales with WalkSpeed until you set it). nil while the player has no character. Setting it (server only) clamps to 0..1500 and lasts for the rest of the session, through respawns. */
	crouchSpeed: number | undefined;
	/** How much health a character out of breath loses each second, dealt once a second and tagged "Drown" (10 unless a script changes it). It goes past Armor and Shield, and nobody is credited for the kill. 0 lets a character stay under forever once its breath is gone. nil while the player has no character. Setting it (server only) clamps to 0..10000 and lasts for the rest of the session. Only counts while BreathEnabled is true. */
	drownDamage: number | undefined;
	/** The character's health, from 0 to MaxHealth; nil while the player has no character. Weapons and TakeDamage lower this same value. Setting it (server only) clamps to 0..MaxHealth, and 0 kills the character with no killer. Setting it on a dead player, or one driving a vehicle, does nothing: use Respawn to bring a player back. */
	health: number | undefined;
	/** Whether the character is crouched right now (by the key, a slide or ForceCrouch). nil while the player has no character. Read-only. */
	readonly isCrouched: boolean | undefined;
	/** Whether the character is limp on the ground right now: from Ragdoll, or a hard hit, a blast or being thrown off a vehicle knocked it down. nil while the player has no character. Read-only. */
	readonly isRagdolled: boolean | undefined;
	/** Whether the character is sprinting right now: holding the sprint key and moving forward fast enough. nil while the player has no character. Read-only. */
	readonly isSprinting: boolean | undefined;
	/** Whether the character is swimming in water right now (wading in the shallows is not swimming). nil while the player has no character. Read-only. */
	readonly isSwimming: boolean | undefined;
	/** Whether the character is swimming with its head under the surface right now: the test breath uses. nil while the player has no character. Read-only. */
	readonly isUnderwater: boolean | undefined;
	/** How high the character's jump rises, in centimetres (about 90 under normal gravity unless a script changes it). nil while the player has no character. Setting it (server only) clamps to 0..1000 and lasts for the rest of the session, through respawns; 0 stops ordinary jumps. */
	jumpHeight: number | undefined;
	/** How much stamina a jump costs (0 unless a script changes it, so jumping is free); with a cost, a character with less stamina than that cannot jump. nil while the player has no character. Setting it (server only) clamps to 0..10000 and lasts for the rest of the session. Only counts while StaminaEnabled is true. */
	jumpStaminaCost: number | undefined;
	/** How many seconds the character can stay underwater on a full breath (20 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 1..3600 and lasts for the rest of the session, through respawns. Lowering it below Breath lowers Breath too; raising it does not refill. */
	maxBreath: number | undefined;
	/** The most health the character can have (100 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 1..10000 and lasts for the rest of the session: every respawn starts with full health at this maximum. Lowering it below Health lowers Health too; raising it does not heal. */
	maxHealth: number | undefined;
	/** The size of the shield pool (0, no shield, unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..10000 and lasts for the rest of the session. A shield above the new size drops to it; raising an existing shield does not fill the pool (set Shield for that), but giving a shield to a player who had none starts it full. */
	maxShield: number | undefined;
	/** The most stamina the character can have (100 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 1..10000 and lasts for the rest of the session, through respawns. Lowering it below Stamina lowers Stamina too; raising it does not refill. */
	maxStamina: number | undefined;
	/** The player's Epic display name. */
	readonly name: string;
	/** Where the player's character is. */
	readonly position: Vector3 | undefined;
	/** Seconds the character must go without being hurt before health and shield start coming back (5 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..60 and lasts for the rest of the session. */
	regenDelay: number | undefined;
	/** Health the character gets back per second once RegenDelay seconds have passed since it was last hurt (0, no regeneration, unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..1000 and lasts for the rest of the session. */
	regenRate: number | undefined;
	/** The character's shield: a pool that soaks up damage (after Armor) before health is touched. 0 to MaxShield; nil while the player has no character. Setting it (server only) changes the pool now and clamps to 0..MaxShield; every respawn starts with it full. */
	shield: number | undefined;
	/** Shield the character gets back per second after the same RegenDelay (0, none, unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..1000 and lasts for the rest of the session. */
	shieldRegenRate: number | undefined;
	/** Whether the player can sprint (true unless a script changes it); nil while the player has no character. Setting it (server only) to false stops a sprint at once and the sprint key does nothing until it is true again. It lasts for the rest of the session, through respawns. */
	sprintEnabled: boolean | undefined;
	/** How fast the character sprints, in centimetres per second (700 unless a script changes it; it scales with WalkSpeed until you set it). nil while the player has no character. Setting it (server only) clamps to 0..1500 and lasts for the rest of the session, through respawns. */
	sprintSpeed: number | undefined;
	/** The stamina the character has left, from 0 to MaxStamina (it counts in small steps, the same on every machine, and moves only while StaminaEnabled is true); nil while the player has no character. Setting it (server only) clamps to 0..MaxStamina. Every new character starts full. */
	stamina: number | undefined;
	/** How much stamina sprinting uses each second (20 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..10000 and lasts for the rest of the session. */
	staminaDrain: number | undefined;
	/** Whether the player has a stamina pool (false unless a script turns it on, so places that never use it play as before); nil while the player has no character. With it on, sprinting drains Stamina, Stamina refills after a short rest, a sprint stops at zero and cannot restart until a tenth of MaxStamina has come back, and jumping can cost JumpStaminaCost. Turning it on starts the pool full. Setting it (server only) lasts for the rest of the session, through respawns. */
	staminaEnabled: boolean | undefined;
	/** How much stamina comes back each second once the character has rested (15 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..10000 and lasts for the rest of the session. */
	staminaRegen: number | undefined;
	/** How many seconds after sprinting (or a jump that cost stamina) before stamina starts coming back (1 unless a script changes it); nil while the player has no character. Setting it (server only) clamps to 0..60 and lasts for the rest of the session. */
	staminaRegenDelay: number | undefined;
	/** The name of the team the player is on, or nil for none. Setting it (server only) puts the player on that team, which must exist (Teams:Create); set nil to take them off. Fires Teams.PlayerTeamChanged. Team membership replicates, so client scripts can read it, and it lasts while the player is in the session. */
	team: string | undefined;
	/** The player's verified Mantle account id (a plain UUID). */
	readonly userId: string;
	/** How fast the character runs, in centimetres per second (500 unless a script changes it); walking, sprinting and crouching scale with it. nil while the player has no character. Setting it (server only) clamps to 0..1500 and lasts for the rest of the session, through respawns; 0 stops the character moving. */
	walkSpeed: number | undefined;
	/** What the player is wearing: slot name ("hair", "top", "footwear", "acc_hat", ...) to item id. Empty when nothing is worn or their look has not arrived. Read-only; a fresh table on every read. (server only) */
	readonly wearing: Record<string, string>;
	/** The character's Breath changed: holding it underwater, getting it back at the surface or a script setting it. It reports steps of about half a percent of MaxBreath, not every frame. Fires on the server and on every client while BreathEnabled is true. */
	readonly breathChanged: Signal<[player: Player, old: number, new: number]>;
	/** The character's Breath is full again after it had used some. Fires on the server and on every client. */
	readonly breathRestored: Signal<[player: Player]>;
	/** The character crouched (the key, a slide or ForceCrouch). Fires on the server and on every client. */
	readonly crouchStarted: Signal<[player: Player]>;
	/** The character stood up from a crouch. Fires on the server and on every client. */
	readonly crouchStopped: Signal<[player: Player]>;
	/** The character was hurt, on the server. amount is what got through Armor (the Shield and health took it between them), so a hit the shield soaked up still counts; a killing hit fires Damaged first and then Died. damageType is the hit's tag ("Bullet", "Melee", "Explosion", ..., or your own) and attacker the player who dealt it, or nil. Not fired for a hit a held guard blocked, friendly fire that was switched off, or Health set directly. */
	readonly damaged: Signal<[player: Player, amount: number, damageType: string, attacker: Player | undefined]>;
	/** The player's character died, on the server and on every client. killer is the player whose damage finished it (a weapon, or TakeDamage with a source); nil for falling out of the world, Kill, Health set to 0, or their own damage. damageType is the tag of the killing hit ("Bullet", "Melee", "Explosion", ..., or your own); "Generic" when nothing named one. */
	readonly died: Signal<[player: Player, killer: Player | undefined, damageType: string]>;
	/** The character's health changed: damage, healing, Health set, or a respawn's refill. Fires on the server and on every client. */
	readonly healthChanged: Signal<[player: Player, old: number, new: number]>;
	/** The character's Breath reached 0: from now on it takes DrownDamage every second until it surfaces. Fires on the server and on every client. */
	readonly outOfBreath: Signal<[player: Player]>;
	/** The character was knocked down (Ragdoll, a hard hit, a blast, or being thrown off a vehicle). Fires on the server and on every client. */
	readonly ragdolled: Signal<[player: Player]>;
	/** A knocked-down character stood back up (its time ended or Recover was called). It does not fire if the character died while down: Died does. Fires on the server and on every client. */
	readonly recovered: Signal<[player: Player]>;
	/** The player got a fresh character after a death or a Respawn call, on the server. Not fired for the first character a player gets when they join. */
	readonly respawned: Signal<[player: Player]>;
	/** The character started sprinting. Fires on the server and on every client. */
	readonly sprintStarted: Signal<[player: Player]>;
	/** The character stopped sprinting: the key let go, stamina ran out, SprintEnabled turned off, or it stopped moving. Fires on the server and on every client. */
	readonly sprintStopped: Signal<[player: Player]>;
	/** The character's Stamina changed: sprinting, resting, a jump's cost or a script setting it. It reports steps of about half a percent of MaxStamina, not every frame. Fires on the server and on every client while StaminaEnabled is true. */
	readonly staminaChanged: Signal<[player: Player, old: number, new: number]>;
	/** Hold the player crouched (true) or standing (false) whatever they press, until ReleaseCrouch. A player held crouched can still walk (sprinting is off while crouched); one held standing cannot crouch or slide. CrouchEnabled false wins over a crouch hold. Returns false when the player has no character on foot. The hold ends with the character: a respawn gives the key back. (server only) */
	forceCrouch(crouched: boolean): boolean;
	/** Raise the character's health by amount, up to MaxHealth. Never brings a dead character back (use Respawn). Returns false when the player has no character on foot, is dead, or is already at full health. (server only) */
	heal(amount: number): boolean;
	/** Remove the player from the session with a message. (server only) */
	kick(reason?: string | undefined): void;
	/** Kill the character outright: Died fires with no killer, and the place's respawn rules take over as for any other death. Returns false when the player has no character on foot or is already dead. (server only) */
	kill(): boolean;
	/** Knock the character down: it goes limp for everyone, the player cannot move or act, and when the time is up (or Recover is called) it stands back up where it lies with a get-up animation. seconds is how long to stay down, 0.1 to 300; leave it out to stay down until Recover (at most five minutes). velocity is an optional shove in centimetres per second; without it the character keeps the speed it had. Ragdolled fires now and Recovered when it gets up. Returns false when the player has no character on foot, is dead, is sitting in a vehicle or is already knocked down. (server only) */
	ragdoll(seconds?: number | undefined, velocity?: Vector3 | undefined): boolean;
	/** Stand a knocked-down character back up now, wherever it lies; Recovered fires when it is up. Safe to call at any time. Returns false when the character was not knocked down. (server only) */
	recover(): boolean;
	/** Give the player the crouch key back after ForceCrouch. Returns false when the player has no character on foot. (server only) */
	releaseCrouch(): boolean;
	/** Give the player a fresh character with full health right now, alive or dead: at position when given, otherwise at the spawn point the place's respawn mode picks. The old character (or a corpse waiting for its respawn) is removed. Returns false when the player is driving a vehicle (eject them first) or cannot be spawned. (server only) */
	respawn(position?: Vector3 | undefined): boolean;
	/** Hurt the character through the same path weapons use, so the game's damage rules apply: friendly fire, Armor, then Shield, then health. source is the player to credit: if this hit kills, Died names them as the killer. damageType tags the hit for Damaged and Died ("Generic" when left out); use a built-in such as "Bullet", "Melee", "Explosion", "Fall", "Vehicle" or "Area", or any word of your own up to 32 characters, such as "Poison". Returns false when the player has no character on foot (respawning or driving), is already dead, or nothing got through (friendly fire off, or Armor 100). (server only) */
	takeDamage(amount: number, source?: Player | undefined, damageType?: string | undefined): boolean;
	/** Move the player's character instantly. Always lands exactly at position, even inside solid geometry; returns false only when the player has no character to move right now. (server only) */
	teleport(position: Vector3): boolean;
}

/** Returned by Signal:Connect; call Disconnect to stop listening. */
interface Connection {
	/** False once disconnected. */
	readonly connected: boolean;
	/** Stop calling the handler. */
	disconnect(): void;
}

/** A named network event between server and clients. */
interface Remote {
	/** The remote's name. */
	readonly name: string;
	/** The server fired this remote to this player (client only). */
	readonly onClientEvent: Signal<[...args: any[]]>;
	/** A client fired this remote (server only). */
	readonly onServerEvent: Signal<[player: Player, ...args: any[]]>;
	/** Send arguments to every player's OnClientEvent handlers. (server only) */
	fireAllClients(...args: any[]): void;
	/** Send arguments to one player's OnClientEvent handlers. (server only) */
	fireClient(player: Player, ...args: any[]): void;
	/** Send arguments to the server's OnServerEvent handlers. (client only) */
	fireServer(...args: any[]): void;
}

/** One joint between physics entities, from Constraints:Create or Constraints:FromEntity (server only). */
interface Constraint {
	/** The joint broke: something pulled it harder than its BreakForce. Fires once, on the server; the entities move freely from then on. */
	readonly broken: Signal<[constraint: Constraint]>;
	/** Remove the joint for good: the entities move freely again, and Broken never fires. A joint a script made also goes when every script restarts. (server only) */
	destroy(): void;
}

/** One CrowdSpawner's ambient crowd, from Crowds:Get (server only). */
interface Crowd {
	/** How much of the spawner's crowd walks, from 0 (nobody) to 1 (the full count the place set, the default). 0.5 of a crowd of 40 is 20 people. (server only) */
	density: number;
	/** Whether the crowd walks at all. Starts as the spawner's own enabled setting; false removes every one of its people. (server only) */
	enabled: boolean;
	/** True freezes every one of the crowd's people where they stand, the same moment for everyone; false lets them walk on from there. (server only) */
	paused: boolean;
	/** The whole number every player's crowd is made from, 0 to 2147483647. Starts as the spawner's own seed; another seed is another crowd: as many people, on the same lanes, but other people in other places. (server only) */
	seed: number;
}

/** A character NPCs:Spawn made. The server walks and animates it; every player sees it. */
interface NPC {
	/** False once the NPC has been destroyed (Destroy, Entity:Destroy, or it fell out of the world). (server only) */
	readonly exists: boolean;
	/** The NPC's health, from 0 to MaxHealth. Weapons, TakeDamage and Heal change this same value. Setting it clamps to 0..MaxHealth, and 0 kills the NPC with no killer. Setting it on a dead NPC does nothing: use Respawn to bring it back. (server only) */
	health: number;
	/** The most health the NPC can have (100 unless a script changes it). Setting it clamps to 1..10000; lowering it below Health lowers Health too, and raising it does not heal. Respawn refills health to this maximum. (server only) */
	maxHealth: number;
	/** Where the NPC stands: the middle of its body, in centimetres, as Player.Position reads a character. Read-only: use MoveTo to walk it. (server only) */
	readonly position: Vector3;
	/** Which way the NPC faces, in degrees as (pitch, yaw, roll). It turns to face the way it walks. Read-only. (server only) */
	readonly rotation: Vector3;
	/** The entity the NPC is attacking now (a player's character entity, or an NPC's entity), or nil. Read-only; set it with Attack and ClearTarget. TargetPlayer gives the player when the target is one. (server only) */
	readonly target: Entity | undefined;
	/** The player the NPC is attacking now, or nil when it has no target or the target is not a player. Read-only. (server only) */
	readonly targetPlayer: Player | undefined;
	/** How fast the NPC walks, in centimetres per second (150 unless a script changes it; a player runs at 500). Clamped to 0..1500; 0 stops it where it is. The walk clip speeds up and slows down with it. (server only) */
	walkSpeed: number;
	/** The NPC swung or fired at its target. damage is the hit's damage (the projectile's, for a ranged NPC) and damageType its tag. Fires at the moment of the attack, on the server only. Use it to play an attack clip or a sound. */
	readonly attacked: Signal<[npc: NPC, entity: Entity | undefined, player: Player | undefined, damage: number, damageType: string]>;
	/** The NPC died: its health reached 0. killer is the player whose damage finished it (a weapon, or TakeDamage with a source); nil for Kill, Health set to 0, or damage nobody is credited with. A dead NPC stops, falls limp (or plays the clip a Died handler starts with PlayClip or PlayAssetClip, holding its last frame) and stays where it fell, still counting toward the 50, until Respawn or Destroy. Fires on the server only. */
	readonly died: Signal<[npc: NPC, killer: Player | undefined]>;
	/** A drive DriveTo or DriveAlong started ended: result is "arrived" when the car stopped within ArriveRadius of the last point, "stuck" when it could not get further (it reversed and tried again, then gave up), or "cancelled" (StopDriving, a new drive, ExitVehicle, the NPC dying or being destroyed, or the car going away). Fires on the server only, once per drive. */
	readonly driveFinished: Signal<[result: string]>;
	/** The NPC's health changed: damage, healing, Health or MaxHealth set, or a respawn's refill. Fires on the server only. */
	readonly healthChanged: Signal<[npc: NPC, old: number, new: number]>;
	/** A MoveTo walk ended: success is true when the NPC arrived, false when it could not get there. Fires on the server only, once per walk; a walk replaced by another MoveTo, or ended by Stop or Destroy, never fires. */
	readonly moveCompleted: Signal<[success: boolean]>;
	/** The NPC's attack target changed: Attack started, its aggro range picked someone, or the target died, left or was cleared. entity and player describe the new target as Raycast results do (player only when it is a player), and are both nil when the NPC no longer has one. Fires on the server only. */
	readonly targetChanged: Signal<[npc: NPC, entity: Entity | undefined, player: Player | undefined]>;
	/** Make target the NPC's enemy: it walks up to it and hurts it every Interval until it dies, goes away or you call ClearTarget. TargetChanged fires now, and Attacked fires at every swing or shot. It replaces whatever the NPC was doing. Melee by default: options Range (how close it gets before it swings, centimetres, 150 by default; 50 to 500), Damage (10 by default), DamageType ("Melee" by default) and Interval (seconds between swings, 1 by default; 0.1 to 60). The damage goes through the weapons' path, so armour, shield and the Damaged and Died signals apply to a player it hits; a kill credits nobody (the NPC is not a player), and friendly fire does not protect against an NPC. Chase (true by default): false makes it stand still and attack only what comes into Range. Give Projectile (the options of Projectiles:Fire, minus Position, Velocity, Target and Attacker) to make it a ranged attacker: it stops within Range (2000 by default, up to 5000) with a clear line to the target and fires that projectile at it, aimed for gravity, every Interval; Damage and DamageType then come from the projectile. Once a player target dies the NPC stops and TargetChanged fires with no target; attack them again after they respawn, or let SetAggroRange do it. Example: zombie:Attack(player, {Damage = 15, Interval = 1.2}) or turret:Attack(player, {Range = 3000, Interval = 0.5, Projectile = {Speed = 4000, Damage = 8, Look = "Ball"}}) (server only) */
	attack(target: Entity | Player | NPC, options?: { range?: number | undefined; damage?: number | undefined; damageType?: string | undefined; interval?: number | undefined; chase?: boolean | undefined; projectile?: { speed?: number | undefined; gravityScale?: number | undefined; lifetime?: number | undefined; look?: string | undefined; color?: string | undefined; scale?: number | undefined; damage?: number | undefined; damageType?: string | undefined; explosion?: { radius?: number | undefined; damage?: number | undefined; innerRadius?: number | undefined; falloff?: number | undefined; damageType?: string | undefined; impulse?: number | undefined; ragdoll?: boolean | undefined; ragdollSeconds?: number | undefined; lineOfSight?: boolean | undefined; effect?: string | undefined; sound?: boolean | undefined } | undefined; explodeOnExpire?: boolean | undefined; tag?: string | undefined } | undefined }): void;
	/** Call the NPC off: it stops attacking or following and stands where it is, and TargetChanged fires with no target. Returns false when it had nothing to stop. An aggro range (SetAggroRange) stays on and may pick a new target. (server only) */
	clearTarget(): boolean;
	/** Remove the NPC for everyone. Its walk ends without MoveCompleted, and it no longer counts toward the 50. (server only) */
	destroy(): void;
	/** As DriveTo, through points in order (one to 64 positions): the car drives each leg in turn, passing the points on the way without stopping, and ends at the last, which is where it arrives. ArriveRadius is the last point's radius. (server only) */
	driveAlong(vehicle: Entity, points: (Vector3)[], options?: { maxSpeed?: number | undefined; arriveRadius?: number | undefined } | undefined): void;
	/** Put the NPC in the driver's seat of vehicle and drive it to position: the NPC sits in the car for every player, which drives straight at the point, and DriveFinished fires when the drive ends. The NPC must be alive and within 4.5 m of the car with a clear line to the seat; it is the car's driver, so no player gets the Drive prompt or can take the seat, and the car errors if it already has a driver or the NPC is driving another car (StopDriving or ExitVehicle first). It replaces a drive the NPC is on in the same car (that one finishes as "cancelled"). options: MaxSpeed (centimetres per second, 1400 by default, about 50 km/h; clamped to 100..3000) and ArriveRadius (centimetres, 600 by default; clamped to 100..5000). The NPC drives straight at the point over open ground: there are no roads to follow, and it does not steer round walls (a car that can't get on reverses and tries again, then gives up as "stuck"). Cars and trucks only, not motorcycles or aircraft. Arrived, the NPC stays in the seat with the car braked until ExitVehicle. (server only) */
	driveTo(vehicle: Entity, position: Vector3, options?: { maxSpeed?: number | undefined; arriveRadius?: number | undefined } | undefined): void;
	/** Get the NPC out of the car it is driving, standing beside it, and free the seat. A drive in progress ends as "cancelled". Returns false when the NPC is not in a car, the car is still moving faster than a walk (3 m/s; StopDriving, then try again once it has stopped) or there is nowhere to stand beside it. (server only) */
	exitVehicle(): boolean;
	/** Walk after target and keep about distance (centimetres) away from it, 200 by default; 50 to 5000. The NPC follows a player through respawns, going where they go as the server's navigation mesh allows, and stands still once it is close enough. It replaces whatever the NPC was doing (a MoveTo, an earlier Follow or an Attack) and carries on until StopFollowing, ClearTarget, a MoveTo, or the NPC or the target going away; a MoveTo you give it yourself takes over and ends the follow. The walk does not fire MoveCompleted. Example: guard:Follow(player, 300) (server only) */
	follow(target: Entity | Player | NPC, distance?: number | undefined): void;
	/** The NPC as an Entity: what a raycast that hits it reports. Tag it or give it attributes like any entity. (server only) */
	getEntity(): Entity;
	/** Raise the NPC's health by amount, up to MaxHealth. Never brings a dead NPC back (use Respawn). Returns false when the NPC is dead or already at full health. (server only) */
	heal(amount: number): boolean;
	/** Kill the NPC outright: Died fires with no killer. Returns false when it is already dead. (server only) */
	kill(): boolean;
	/** Walk to position along the ground, around walls and obstacles, replacing any walk in progress. Returns false at once when position is more than 500 m away or the NPC is dead; otherwise true, and MoveCompleted fires when the walk ends: true when the NPC arrived, false when there is no way there (no walkable ground, a blocked or too long path). The first walk in a place waits for its navigation to build, which can take a few seconds. (server only) */
	moveTo(position: Vector3): boolean;
	/** Play one of the place's AnimClip assets on the NPC, starting now, on every player's screen and in step, players who join later included. params: Loop (false by default: the clip plays once and the NPC goes back to walking and standing on its own; true keeps it playing until StopClip or the next PlayClip), PlayRate (clip seconds per second, 1 by default, clamped to 0..10) and StartOffset (seconds into the clip to start at, 0 by default, clamped to 0..3600). The clip must be authored on the skeleton of the NPC's body (the avatars' MetaHuman body). Errors when the id is not an AnimClip in the place's assets or, where the server draws the NPC, the clip is on another skeleton; a dedicated server never loads clips, so it cannot check the skeleton: there a clip on another skeleton shows on each player's screen as the NPC walking and standing on its own instead, with no error for the script. (server only) */
	playAssetClip(clipAssetId: string, params?: { loop?: boolean | undefined; playRate?: number | undefined; startOffset?: number | undefined } | undefined): void;
	/** Play one of the NPC clips on every player's screen: "walk", "idle", "turn_left" or "turn_right", or (script API 1.23) one of the place's AnimClip asset ids at normal speed (PlayAssetClip also sets its speed and start). It plays once and the NPC goes back to walking and standing on its own, or with loop true keeps playing until StopClip or the next PlayClip. Players who join later see it too. A clip on another skeleton than the NPC's body can't be checked on a dedicated server, which never loads clips; each player's screen then shows the NPC walking and standing on its own instead. (server only) */
	playClip(clip: string, loop?: boolean | undefined): void;
	/** Bring the NPC back with full health, dead or alive: standing at position (its feet) when given, otherwise where NPCs:Spawn first put it. Its clip stops, and a walk in progress ends without MoveCompleted. Returns false when it could not be put there. (server only) */
	respawn(position?: Vector3 | undefined): boolean;
	/** Let the NPC pick its own enemies: whenever it has no target and a living player comes within range (centimetres, up to 5000) it attacks the nearest one, as if you had called Attack with options (the same table as Attack's). 0 turns it off. It looks a few times a second, a ranged attacker ignores players it has no clear line to, and an NPC drops a target that gets more than twice range away. Players only; attack other NPCs with Attack. Example: zombie:SetAggroRange(1500, {Damage = 12}) (server only) */
	setAggroRange(range: number, options?: { range?: number | undefined; damage?: number | undefined; damageType?: string | undefined; interval?: number | undefined; chase?: boolean | undefined; projectile?: { speed?: number | undefined; gravityScale?: number | undefined; lifetime?: number | undefined; look?: string | undefined; color?: string | undefined; scale?: number | undefined; damage?: number | undefined; damageType?: string | undefined; explosion?: { radius?: number | undefined; damage?: number | undefined; innerRadius?: number | undefined; falloff?: number | undefined; damageType?: string | undefined; impulse?: number | undefined; ragdoll?: boolean | undefined; ragdollSeconds?: number | undefined; lineOfSight?: boolean | undefined; effect?: string | undefined; sound?: boolean | undefined } | undefined; explodeOnExpire?: boolean | undefined; tag?: string | undefined } | undefined }): void;
	/** Stop walking where the NPC is. The walk it abandons never fires MoveCompleted. (server only) */
	stop(): void;
	/** End the clip PlayClip started: the NPC walks and stands on its own again. (server only) */
	stopClip(): void;
	/** End the drive the NPC is on: the car brakes to a stop with the NPC still in the seat, and DriveFinished fires with "cancelled". Does nothing when the NPC is not driving. (server only) */
	stopDriving(): void;
	/** End the follow Follow started: the NPC stops where it is. Returns false when the NPC was not following. An Attack is ended with ClearTarget. (server only) */
	stopFollowing(): boolean;
	/** Hurt the NPC through the same path weapons use. source is the player to credit: if this hit kills, Died names them as the killer. Returns false when the NPC is already dead or the damage was refused. (server only) */
	takeDamage(amount: number, source?: Player | undefined): boolean;
}

/** A named store of whole-number values for this place, read back sorted: a leaderboard. Get one with DataStore:GetOrderedDataStore (server only). Every method returns a Signal that fires once with a result table {ok, error?, ...}: call :Wait() on it straight away, or :Once(handler). */
interface OrderedDataStore {
	/** Read key's value: the result's value, nil when the store has no such key. (server only) */
	getAsync(key: string): Signal<[{ ok: boolean; error?: string | undefined; value?: number | undefined }]>;
	/** One page of the store in value order, highest first (lowest first when ascending): the result's entries is a list of {key, value}, up to pageSize (default 50, at most 100), ties in key order. Pass the result's nextCursor back, with the same ascending, for the next page; it is nil on the last one. (server only) */
	getSortedAsync(ascending: boolean, pageSize?: number | undefined, cursor?: string | undefined): Signal<[{ ok: boolean; error?: string | undefined; entries?: ({ key: string; value: number })[] | undefined; nextCursor?: string | undefined }]>;
	/** Add delta (default 1; may be negative) to key's value, a new key starting from 0. One step on the backend, so no server's increment is lost to another's. The result's value is the new total. (server only) */
	incrementAsync(key: string, delta?: number | undefined): Signal<[{ ok: boolean; error?: string | undefined; value?: number | undefined }]>;
	/** Delete key from the store. Removing a key the store doesn't have succeeds. (server only) */
	removeAsync(key: string): Signal<[{ ok: boolean; error?: string | undefined }]>;
	/** Store a whole number (up to 2^53 - 1 either way) under key. The result's value is what was stored. (server only) */
	setAsync(key: string, value: number): Signal<[{ ok: boolean; error?: string | undefined; value?: number | undefined }]>;
}

/** One sound Sounds:Play started. A server script's handle reaches every player who hears the sound. */
interface SoundHandle {
	/** Whether the sound is still playing (or still downloading, to play when it arrives). False once it is stopped, or a one-shot has finished. A dedicated server never downloads sounds, so there a one-shot counts as playing for up to a minute unless it is stopped. */
	isPlaying(): boolean;
	/** Change the sound's volume, 0 to 2. From a server script, players hear the change within a tenth of a second. */
	setVolume(volume: number): void;
	/** Stop the sound, fading it out over fadeOut seconds (0, the default, stops it at once). Stopping a sound that has already finished does nothing. */
	stop(fadeOut?: number | undefined): void;
}

/** A UI document from the place's UI/ folder, open on the local player's screen (UI:OpenDocument). It belongs to the script that opened it and closes when that script stops. */
interface UiDocument {
	/** True until the document closes: your Close, Back on a modal document, a budget it went over, or its script stopping. (client only) */
	readonly isOpen: boolean;
	/** The document closed by itself: reason is "back" (the player pressed Back on a modal document), "load failed" (its markup or styles could not load), or the budget it went over ("element budget" or "layout time"). Never fires for your own Close, or when your script stops or the place unloads. */
	readonly closed: Signal<[reason: string]>;
	/** The document called emit(name, ...) from a data-event-* attribute; args holds the values it passed. Example: shop.Event:Connect(function(name, args) if name == "buy" then buy(args[1]) end end) */
	readonly event: Signal<[name: string, args: (any)[]]>;
	/** Close the document. Closed does not fire for your own Close, and closing twice is fine. Example: shop:Close() (client only) */
	close(): void;
	/** Set a value the document shows through {{key}}, data-for, data-if and data-attr-*: a number, string, boolean, Vector3 (read as .x, .y and .z), or a table of them. An entity, a player or any other object reference is refused: send its fields instead. The key is a name of letters, digits and _ (up to 64). After the document closes it still checks the key and value, then does nothing. Example: shop:SetData("coins", 120) (client only) */
	setData(key: string, value: any): void;
	/** Switch how the document takes input: "modal", "overlay" or "hud" (see UI:OpenDocument). Does nothing once it has closed. Example: shop:SetMode("hud") (client only) */
	setMode(mode: string): void;
}

/** One node in a script's retained UI tree, mounted into the local player's HUD. */
interface UiNode {
	/** This node's id, stable for its lifetime. */
	readonly id: number;
	/** This node's current text, after any SetLocalizedText resolution. */
	readonly text: string;
	/** This node was activated: a mouse click, Enter, Space, or a focused gamepad Accept on a button, toggle or prompt. Only reaches handlers connected by the script that created this node -- nothing else ever holds its handle. */
	readonly activated: Signal<[id: number, action: string]>;
	/** Make this top-level node float over an entity, a player or an NPC in the world, on this player's screen only: a nameplate, a health bar, a marker. Build the node and its children as usual, then attach the top one; it stops being part of the screen's layout and follows the target, bottom-centre on the point above it. It is only drawn, never clicked. Attaching it again changes the options. Returns false if the node is not a top-level node of this script (call SetParent(nil) first) or the script already has 32 nodes floating; the nodes still count toward the node budget (256). If the target is destroyed, or a player has no character, the node hides. options: Bone (a bone or socket of the target's skeletal mesh, such as "head"; without it, or when the target has no such bone, the node sits over the top of the target), Offset (a Vector3 in centimetres, in world axes, added to that point, such as Vector3.new(0, 0, 30)), ScreenOffsetX and ScreenOffsetY (pixels, x right and y down), MaxDistance (centimetres from the camera, default 3000, at most 50000: farther is hidden), FadeDistance (centimetres: it fades out over the last FadeDistance before MaxDistance; default 0 cuts off at MaxDistance), HideWhenOccluded (hide while world geometry is between the camera and the point; one cheap check a tenth of a second; default false: it draws over everything), HideForSelf (hide when the target is the local player's own character, for a nameplate you do not want over yourself), ScaleWithDistance (default false: constant size on screen; true makes it smaller with distance), MinScale and MaxScale (0.05 to 4, defaults 0.4 and 1; the limits when it scales), ScaleDistance (centimetres where it draws at scale 1, default 1000), OffScreen ("Hide", the default, or "Clamp" to keep it on the edge of the screen pointing towards the target, for a marker). Example: nameplate:AttachToWorld(player, {Bone = "head", Offset = Vector3.new(0, 0, 30), MaxDistance = 2500, FadeDistance = 500, HideForSelf = true}) (client only) */
	attachToWorld(target: Entity | Player | NPC, options?: { bone?: string | undefined; offset?: Vector3 | undefined; screenOffsetX?: number | undefined; screenOffsetY?: number | undefined; maxDistance?: number | undefined; fadeDistance?: number | undefined; hideWhenOccluded?: boolean | undefined; hideForSelf?: boolean | undefined; scaleWithDistance?: boolean | undefined; minScale?: number | undefined; maxScale?: number | undefined; scaleDistance?: number | undefined; offScreen?: string | undefined }): boolean;
	/** Remove this node and every descendant. (client only) */
	destroy(): void;
	/** Stop floating this node: it goes back to the screen's layout, as a top-level node. Returns false if it was not attached. Destroy the node instead to remove it altogether. (client only) */
	detachFromWorld(): boolean;
	/** Name the action this node's Activated carries when it is clicked, tapped or confirmed. Optional on a Button or Toggle (Activated then carries ""); a Prompt needs one. (client only) */
	setAction(action: string): boolean;
	/** Attach caption text shown underneath this node when the player has captions on. (client only) */
	setCaption(caption: string): boolean;
	/** Enable or disable this node (a disabled button/toggle/prompt cannot be activated). (client only) */
	setEnabled(enabled: boolean): boolean;
	/** Set an image node's asset (an "ast_" id from the place's asset library). (client only) */
	setImage(assetId: string): boolean;
	/** Look up key in the place's localisation table for the player's locale (falling back through the language, then the place's default locale, then fallback or key itself) and set it as this node's text. (client only) */
	setLocalizedText(key: string, fallback?: string | undefined): boolean;
	/** Reparent this node (nil detaches it back to the root). Refuses a cycle or a node from another script. (client only) */
	setParent(parent?: UiNode | undefined): boolean;
	/** Describe this node for screen readers and controller focus order: "none", "heading", "label", "button", "checkbox", "image", "list-container", "list-item", "dialog" or "status". (client only) */
	setSemantics(role: string, label?: string | undefined, hint?: string | undefined, focusOrder?: number | undefined): boolean;
	/** Replace this node's style: width, height, columns, font-size; grow (0-100, share of the parent's spare height), gap (0-256, space between children), padding (0-256, space inside the node), opacity (0-1), align ("start", "center", "end" or "stretch", within the parent). Returns false for an unknown key or an out-of-range value; a non-finite number is an error. (client only) */
	setStyle(style?: Record<string, boolean | number | string> | undefined): boolean;
	/** Set this node's literal text. (client only) */
	setText(text: string): boolean;
	/** Show or hide this node (and its subtree). (client only) */
	setVisible(visible: boolean): boolean;
}

/** Spawn the place's own meshes at runtime (server only). Change any mesh's look through its Entity. */
interface AssetsService {
	/** Spawn one of the place's StaticMesh assets (any mesh its asset list names, placed or not) that every player sees, and return it as an Entity. props sets how it starts: Rotation, Scale (a Vector3, or one number for all three), Material (one of the place's Material assets, for every slot), Color, Transparency, Collision ("Blocking" by default, "Overlap" or "None"), CastShadow (true by default) and Anchored (spawned meshes stay where scripts put them; false is refused). A mesh uploaded with the place may take a moment to appear on a player's screen while it downloads. Spawned meshes count towards the same limit as Entity:Clone; Destroy one to make room. (server only) */
	spawn(assetId: string, position: Vector3, props?: { rotation?: Vector3 | undefined; scale?: Vector3 | number | undefined; material?: string | undefined; color?: string | undefined; transparency?: number | undefined; collision?: string | undefined; castShadow?: boolean | undefined; anchored?: boolean | undefined } | undefined): Entity;
}
const Assets: AssetsService;

/** The local player's avatar: open the avatar editor over the place and play their equipped emotes. On the server, read a player's look with Player.Avatar and Player.Wearing. */
interface AvatarService {
	/** The avatar editor closed (client only), by the player or by CloseEditor. saved is true when the player saved a look while it was open; LookChanged says what changed once their account has it. */
	readonly editorClosed: Signal<[saved: boolean]>;
	/** The avatar editor opened on this client (client only). */
	readonly editorOpened: Signal<[]>;
	/** The local player's saved look changed (client only): once per slot whose item changed, when their account confirms a save, from the avatar editor or Mantle's own avatar screen. slot is "avatar" or a slot name such as "hair", "top" or "acc_hat"; oldItem and newItem are item ids, nil for nothing worn there. For effects and the place's own bookkeeping: the look is already the player's. */
	readonly lookChanged: Signal<[slot: string, oldItem: string | undefined, newItem: string | undefined]>;
	/** Close the avatar editor as its Close button would, without asking: unsaved changes are put back, and a save already sent still lands. Returns false when it was not open. (client only) */
	closeEditor(): boolean;
	/** Whether the avatar editor is open on this client, whoever opened it. (client only) */
	isEditorOpen(): boolean;
	/** Open the avatar editor over the running place, where the player changes their own look while the world goes on behind it. options.Categories limits what it offers: any of "body" (the avatar, skin tone, eye colour, height and the rest of Body), "hair" (hair, eyebrows, eyelashes, facial hair), "clothes" (top, bottom, footwear), "accessories", "emotes" or "all", or slot names such as "top" or "acc_hat". Leave it out for everything (only then are outfits offered). options.Title is a heading over the panel ("Barber"), up to 40 characters. options.FreezeCharacter (default true) holds the character still and turns the camera onto it; false leaves the player in control and the camera where it is. The player sees only items they own or that are free, and Save puts the look on their account exactly as Mantle's own avatar screen does. Esc, the pad's B or Close shut it, asking first about unsaved changes. Returns false, opening nothing, when there is no local character, or the editor or another Mantle screen is already open. An unknown category is an error. (client only) */
	openEditor(options?: { categories?: (string)[] | undefined; title?: string | undefined; freezeCharacter?: boolean | undefined } | undefined): boolean;
	/** Play one of the local player's equipped emotes, by its item id (an emote on their emote wheel). Returns false, and plays nothing, when itemId is not one of their equipped emotes, when they have no character, or when called again too soon. true means the request went to the server, which checks it again and can still refuse it (the player is moving, in the air, seated or dead). (client only) */
	playEmote(itemId: string): boolean;
}
const Avatar: AvatarService;

/** Give players weapons, take them away, and equip them (server only). */
interface BackpackService {
	/** A consumable (a potion, a medkit, food: a weapon-builder item with the Consumable hold) was used, on the server, at the moment its effect lands: halfway through its drink or use clip. It fires for every use, whether the item has a built-in effect or not, and never for a use that was cancelled (sprinting, jumping or dodging first spends nothing). toolId is the item's Tool id in the player's Backpack, docName the name the creator gave it in the builder, usesLeft how many uses it has left (0: it is gone from the Backpack), and builtInEffect whether its own heal also ran (false for an item whose effect is "none": your script is the whole effect). */
	readonly itemUsed: Signal<[player: Player, toolId: string, docName: string, usesLeft: number, builtInEffect: boolean]>;
	/** Empty the player's Backpack, leaving only fists. Returns how many Tools were removed. (server only) */
	clear(player: Player): number;
	/** Put the Tool in this Backpack slot in the player's hands. Slots count from 0, so the hotbar key 1 is slot 0. Returns false when the slot is empty or the player is dead. (server only) */
	equipSlot(player: Player, slot: number): boolean;
	/** The ids of the Tools in the player's Backpack, in slot order. Fists, which every player always has, is never listed. Empty while the player has no character (respawning). (server only) */
	getTools(player: Player): (string)[];
	/** Give the player a weapon this place stores: item is the library item id Storage:Get returns for one of the place's storage names. Returns the new Tool's stable id. Errors for anything else -- an asset id, an object path, an item the place does not store, or a vehicle. Also errors if the stored weapon has not finished preparing or the player cannot receive it. (server only) */
	grant(player: Player, item: string): string;
	/** Take the Tool with this id out of the player's Backpack (unequipping it first). Returns false when the player does not have it. Fists can never be removed. (server only) */
	remove(player: Player, toolId: string): boolean;
	/** Paint one of the player's weapons: color is "#RRGGBB", the same format as Vehicles:SetPaintColor. Every player sees the new colour. Returns false when the player does not have that Tool or it is not a weapon. (server only) */
	setWeaponColor(player: Player, toolId: string, color: string): boolean;
}
const Backpack: BackpackService;

/** The local player's view (client only): first person, a fixed camera (cut to or tweened into), a camera following an entity, the field of view and a shake. All of it goes back to Mantle's own camera when the script stops or the player respawns. */
interface CameraService {
	/** How wide the view is, in degrees, clamped to 30 to 120, or nil for Mantle's own (90). Aiming down sights still zooms in from it. (client only) */
	fieldOfView: number | undefined;
	/** "Default" (Mantle's own camera, which the player can switch between first and third person), "FirstPerson" (held in first person: the player cannot switch out), "Fixed" (set by SetFixed or TweenFixed) or "Follow" (set by Follow). Set it to "Default" or "FirstPerson"; setting "Default" also ends a fixed or following camera. First person applies while on foot. (client only) */
	mode: string;
	/** Keep the camera at offset from entity, looking at it, wherever it goes (Mode becomes "Follow"). offset is in world axes, so Vector3.new(0, -600, 300) keeps the camera 6 m along -Y and 3 m up; with relative true it turns with the entity's heading instead, so Vector3.new(-600, 0, 250) stays behind it. The player still moves their character. When the entity is destroyed the camera goes back to Mantle's; set Mode to "Default" or call Reset to give it back sooner. (client only) */
	follow(entity: Entity, offset: Vector3, relative?: boolean | undefined): void;
	/** Give the camera back to Mantle: Mode "Default", Mantle's field of view, no shake. (client only) */
	reset(): void;
	/** Hold the camera still at position, looking at lookAt (Mode becomes "Fixed"). The player still moves their character. Set Mode to "Default" or call Reset to give the camera back. (client only) */
	setFixed(position: Vector3, lookAt: Vector3): void;
	/** Shake the view, fading out over duration seconds. intensity is clamped to 0 to 1 and duration to 0 to 3; a new shake replaces one still running, and 0 stops it. Does nothing when the player turned on Reduced motion. (client only) */
	shake(intensity: number, duration: number): void;
	/** Move the camera smoothly from where the view is now to position, looking at lookAt, over seconds (0 to 60), then hold it there like SetFixed. Mode is "Fixed" from the start. style and direction ease it as they do Entity:Tween (Linear and Out unless given). Returns a signal that fires once when the camera arrives; it never fires if SetFixed, TweenFixed, Follow, Mode or Reset takes the camera first. (client only) */
	tweenFixed(position: Vector3, lookAt: Vector3, seconds: number, style?: string | undefined, direction?: string | undefined): Signal;
}
const Camera: CameraService;

/** React to what players say in chat, post lines from the place, and run private team chat and whispers. */
interface ChatService {
	/** A player said something in chat. text is what everyone saw: already filtered, so masked words stay masked. On the server it fires for every player's line, a second or so after it was sent; on a client, for each player line this player's chat box shows (so never for someone they blocked). Lines from Chat:SendSystemMessage never fire it. */
	readonly messagePosted: Signal<[player: Player, text: string]>;
	/** A player's whisper was delivered. You learn who whispered to whom, never what was said: whispers are private to their two players. Server only. Use it to log or limit whispering (Chat:SetWhisperBlocked). Example: Chat.WhisperSent:Connect(function(from, to) print(from.Name .. " whispered to " .. to.Name) end) */
	readonly whisperSent: Signal<[sender: Player, recipient: Player]>;
	/** The whisper mode set with Chat:SetWhisperMode: "Everyone", "Team" or "Nobody". (server only) */
	getWhisperMode(): string;
	/** Post a line from the place into chat, drawn apart from what players say: to everyone, or only to player when you pass one. It goes through the same chat filter as player text, so a line with words that aren't allowed is masked or never shown. At most 500 characters, and at most 10 lines a minute per server. Returns false when it cannot be sent right now: over that limit, the player has no verified account yet, or this server has no session chat (a local test run). (server only) */
	sendSystemMessage(text: string, player?: Player | undefined): boolean;
	/** Post a private line from the place to a list of players, drawn apart from what players say, and seen by nobody else: a team message, a hint to the people holding the flag. It goes through the same chat filter as everything else, and Mantle stores it so that only those players can read it back. At most 10 players and 500 characters. It is one line however many players it is for, and counts as one of the server's 10 place lines a minute (Chat:SendSystemMessage shares that limit). Players with no verified account yet are skipped. Returns false when it cannot be sent right now. Example: Chat:SendToPlayers(Teams:GetPlayers("Red"), "Your flag was taken!") (server only) */
	sendToPlayers(players: (Player)[], text: string): boolean;
	/** Make what a player types in chat reach only the players in recipients (and the player themselves): a team channel. Leave recipients out, or pass nil, to let everyone see their lines again. The chat filter still checks every line, a player's own blocks still hide them, and every line can still be reported; this only ever shows a line to fewer people. Mantle stores such a line so that only the sender and these recipients can read it back; it is still kept for moderation. At most 128 recipients. Returns false when the player is not verified yet or the list is too long. Lasts until the player leaves. Example: Chat:SetAudience(p, Teams:GetPlayers(p.Team)) -- p now talks to their team only (server only) */
	setAudience(player: Player, recipients?: (Player)[] | undefined): boolean;
	/** Stop (true) or allow (false) one player sending or receiving whispers, whatever the mode: for players who are dead, jailed or out of the round. A refused whisper tells the sender so. Returns false when the player is not verified yet. Lasts until the player leaves. Example: Chat:SetWhisperBlocked(deadPlayer, true) (server only) */
	setWhisperBlocked(player: Player, blocked: boolean): boolean;
	/** Choose who players may whisper to: "Everyone" (the default), "Team" (only to someone on their own team, see Teams) or "Nobody" (whispers are off). Players whisper by typing /w name message in the chat box (the name in quotes if it has spaces). A whisper is filtered, rate-limited, reportable and hidden by blocks like any chat line, is shown only to the sender and the recipient, and is never given to Chat.MessagePosted. A refused whisper tells the sender so. Mantle stores a whisper so that only its two players can read it back; like all chat it is kept for moderation. Example: Chat:SetWhisperMode("Team") (server only) */
	setWhisperMode(mode: string): void;
}
const Chat: ChatService;

/** Collision layers: sort the objects of your place into named layers and decide which layers bump into which. Make a layer with CreateLayer (up to 8), put an object on it with entity.CollisionLayer = "Name", then use SetLayerCollides to let two layers pass through each other. Every object starts on the "Default" layer, which collides with everything, and players stay on Default: layers sort your props and never let a player through a wall (to let teammates pass through each other use Teams.TeammatesCollide). A layer also narrows World:Raycast, Spherecast, Blockcast, GetEntitiesInRadius and GetEntitiesInBox with params.Layers and params.ExcludeLayers. Server only. */
interface CollisionService {
	/** Make a collision layer. name is 1 to 32 characters (not case-sensitive), must be new and cannot be "Default". A place can have 8 layers. A new layer collides with every layer, itself included, until you change a pair. Example: Collision:CreateLayer("Debris") (server only) */
	createLayer(name: string): void;
	/** Whether objects on layer a and objects on layer b bump into each other (true unless SetLayerCollides turned it off). "Default" collides with everything. (server only) */
	getLayerCollides(a: string, b: string): boolean;
	/** The names of the layers you made ("Default" is always there and not listed). (server only) */
	getLayers(): (string)[];
	/** Delete a layer. Objects on it go back to the Default layer. (server only) */
	removeLayer(name: string): void;
	/** Say whether objects on layer a and objects on layer b bump into each other. false lets them pass through one another (a and b can be the same layer, to make its objects pass through each other). Both layers must exist, and "Default" cannot be changed: it always collides with everything, so objects still meet players and the world. Example: Collision:SetLayerCollides("Debris", "Debris", false) (server only) */
	setLayerCollides(a: string, b: string, collides: boolean): void;
}
const Collision: CollisionService;

/** Join physics entities with welds, hinges and ropes while the place runs (server only). */
interface ConstraintsService {
	/** Join two physics entities, or one to the world, and return the joint. Type is "weld" (holds them as they are), "hinge" (EntityA turns about Axis, and only that) or "rope" (the anchors stay within Length of each other). EntityA is the physics entity the joint holds and EntityB another; leave EntityB out to join EntityA to the world. AnchorA is where on EntityA the joint is, in its own space ((0, 0, 0), its middle, by default); AnchorB where on EntityB (a point in the world without it), by default wherever AnchorA is now. A hinge's Axis is a direction in EntityA's own space ((0, 0, 1) by default) and Limits is {min, max}, the degrees it may turn from where it is now, min at most 0 and max at least 0. A rope's Length is in cm, by default how far apart the anchors are now. Pull the joint harder than BreakForce newtons and it breaks for good (Broken fires). The two entities stop colliding with each other. Players see the entities move; nothing of the joint itself is sent. A place holds at most 200 joints, its own included. (server only) */
	create(params: { type: string; entityA: Entity; entityB?: Entity | undefined; anchorA?: Vector3 | undefined; anchorB?: Vector3 | undefined; axis?: Vector3 | undefined; limits?: (number)[] | undefined; length?: number | undefined; breakForce?: number | undefined }): Constraint;
	/** The joint a place's Constraint entity made, as a Constraint, so a script can hear it break or remove it; nil for any other entity. (server only) */
	fromEntity(entity: Entity): Constraint | undefined;
}
const Constraints: ConstraintsService;

/** Control the ambient crowds a place's CrowdSpawners walk: how many, on or off, moving or frozen (server only). Hear where their people go (client only). */
interface CrowdsService {
	/** One of a CrowdSpawner's people arrived at one of its points of interest and stops there a while (client only). npc says which of the crowd's people it was: the same number is the same person on every player's screen, and they all see the arrival at about the same moment. Play a greeting, open a shop's door, count visitors. Arrivals during a clock re-sync right after joining are not reported. */
	readonly pointReached: Signal<[spawner: Entity, point: Entity, npc: number]>;
	/** The crowd of a CrowdSpawner the place placed. Every player sees what you set on it. Errors if the entity is not a CrowdSpawner (a clone of one has no crowd) or no longer exists. (server only) */
	get(spawner: Entity): Crowd;
}
const Crowds: CrowdsService;

/** Durable saved data for this place (server only). */
interface DataStoreService {
	/** This place's saved value for key, from the local cache (nil if never set, removed, or not loaded yet). (server only) */
	get(key: string): any;
	/** This player's saved value for key, from the local cache. (server only) */
	getForPlayer(player: Player, key: string): any;
	/** The ordered store with this name: whole-number values for this place, read back sorted, for leaderboards. A place can fill 32 stores. (server only) */
	getOrderedDataStore(name: string): OrderedDataStore;
	/** One page (up to 100) of this place's saved keys that start with prefix, in key order, from the local cache like Get. Pass nextCursor back for the next page; it is nil on the last one. (server only) */
	listKeys(prefix?: string | undefined, cursor?: string | undefined): { keys: (string)[]; nextCursor?: string | undefined };
	/** One page of this player's saved keys, like ListKeys. (server only) */
	listKeysForPlayer(player: Player, prefix?: string | undefined, cursor?: string | undefined): { keys: (string)[]; nextCursor?: string | undefined };
	/** Delete key from this place's saved data. Get reads nil at once; removed from the backend asynchronously, like Set. (server only) */
	removeKey(key: string): void;
	/** Delete key from this player's saved data, like RemoveKey. (server only) */
	removeKeyForPlayer(player: Player, key: string): void;
	/** Save key for this place: a boolean, number, string, or table of those (nested up to 4 deep, 64 KB encoded). Applies to the local cache immediately; saved to the backend asynchronously. (server only) */
	set(key: string, value: any): void;
	/** Save key scoped to this player, with the same values as Set. Applies to the local cache immediately; saved to the backend asynchronously. (server only) */
	setForPlayer(player: Player, key: string, value: any): void;
}
const DataStore: DataStoreService;

/** Show Mantle's built-in visual effects anywhere in the world. Play makes a puff of particles (smoke, fire, sparks, dust, glow, explosion, impact, muzzle flash, blood, splash, confetti, heal), Beam draws a short tracer line between two points, and Decal leaves a mark such as a bullet hole or a scorch on a surface. All are cosmetic: they do not hurt anyone or change the world. Example: Effects:Play("Explosion", Vector3.new(0, 5, 0)) */
interface EffectsService {
	/** Draw a short-lived glowing beam (a tracer or a zap) from one point to another, then it fades. params: Color ("#RRGGBB", the beam's own look by default), Width (how thick, 0.5 to 100 cm, 4 by default) and Duration (how long it shows, 0 to 5 seconds, 0.15 by default); numbers out of range are clamped. A line longer than 1000 m is cut short. A client or a server holds at most 16 beams at once; a new one past that replaces the oldest. Use it for tracers, lightning bolts, lasers or showing a link between two things. Example: Effects:Beam(Vector3.new(0, 2, 0), Vector3.new(0, 2, 40), {Color = "#FF3030", Duration = 0.2}). From a client script only this player sees it; from a server script every player does, but not one who joins later. A dedicated server shows nothing. At most 10 effect calls a frame per script (Play, Beam and Decal count together), and a server script at most 10 a second (bursts of 20); past either the script stops. */
	beam(from: Vector3, to: Vector3, params?: { color?: string | undefined; width?: number | undefined; duration?: number | undefined } | undefined): void;
	/** Leave a mark on a surface at position, facing along normal (the direction the surface faces, such as a raycast hit's normal; it does not need to be exactly length 1). params.Style picks the mark: "BulletHole" (a dark hole with a scuffed ring, the default), "Scorch" (a ragged soot patch) or "Splat" (a paint-like blob with droplets, blue by default). Color ("#RRGGBB") tints it, Size is how wide it is (2 to 500 cm, 30 by default) and Lifetime how many seconds it stays before fading away (0.1 to 60, 20 by default); numbers out of range are clamped. A player's machine keeps at most 64 script decals at once (fewer on a low Effects quality setting); a new one past that removes the oldest. Example: local hit = World:Raycast(origin, direction); if hit then Effects:Decal(hit.Position, hit.Normal, {Style = "Scorch", Size = 80}) end. From a client script only this player sees it; from a server script every player does, but not one who joins later. A dedicated server shows nothing. At most 10 effect calls a frame per script (Play, Beam and Decal count together), and a server script at most 10 a second (bursts of 20); past either the script stops. */
	decal(position: Vector3, normal: Vector3, params?: { style?: string | undefined; color?: string | undefined; size?: number | undefined; lifetime?: number | undefined } | undefined): void;
	/** Show a built-in particle effect at position for a moment, then it is gone. archetype is one of (any case; "Muzzle Flash" and "muzzle_flash" work too): "Smoke" (a soft grey column), "Fire" (flames), "Sparks" (a spray of hot sparks), "Dust" (a slow brown puff), "Glow" (a soft light mist), "Explosion" (a fireball with smoke and sparks), "Impact" (sparks and dust where something hits), "MuzzleFlash" (the flash at a gun's mouth), "Blood" (a small stylised red spray for a hit), "Splash" (water droplets), "Confetti" (a party burst in three colours) or "Heal" (green sparkles rising). The bursts (Explosion, Impact, MuzzleFlash, Blood, Splash, Confetti) play for a fraction of a second; the others for params.Duration seconds (1 by default, 0 to 10). params tune the look, each the archetype's own by default: Color ("#RRGGBB"; on Confetti it is the first of the three colours), Size (particle size, 1 to 1000 cm; a bigger Size makes a bigger explosion all through), Rate (particles a second, 0 to 200), Speed (0 to 5000 cm/s), Lifetime (how long each particle lives, 0.05 to 10 seconds) and Duration; numbers out of range are clamped. Particles leave along +Z, or along Rotation's up (a Vector3 of pitch, yaw and roll). On a low Effects quality setting a player sees the main layer only. Example: Effects:Play("Explosion", Vector3.new(0, 5, 0), {Size = 250, Color = "#44AAFF"}). From a client script only this player sees it; from a server script every player does, but not one who joins later. A dedicated server shows nothing. At most 10 effect calls a frame per script (Play, Beam and Decal count together), and a server script at most 10 a second (bursts of 20); past either the script stops. */
	play(archetype: string, position: Vector3, params?: { color?: string | undefined; size?: number | undefined; rate?: number | undefined; speed?: number | undefined; lifetime?: number | undefined; duration?: number | undefined; rotation?: Vector3 | undefined } | undefined): void;
}
const Effects: EffectsService;

/** The time of day, the weather, the sky, the fog and the screen's colour, the same for everyone on the server. Everyone can read them; server scripts can change them, and every player sees the change. */
interface EnvironmentService {
	/** How strongly bright things glow, from 0 (no glow) to 4, or nil for the game's own. Set it on the server. */
	bloom: number | undefined;
	/** The hour, as TimeOfDay. Set it (server) to jump the clock: 21 is night. Any number works and wraps around 24. The clock keeps running from there at TimeScale. */
	clockTime: number;
	/** How much of the sky is cloudy right now, from 0 to 1. */
	readonly cloudCover: number;
	/** A colour-grading look for the whole picture: "Warm", "Cool", "Noir", "Sepia", "Vivid" or "Faded", or nil for none. Set it on the server; ColorGradingIntensity says how strongly it shows. */
	colorGrading: string | undefined;
	/** How strongly ColorGrading shows: 1 is the full look, 0 is none of it, from 0 to 1. Set it on the server. */
	colorGradingIntensity: number;
	/** Tint the whole picture: "#RRGGBB", where "#FFFFFF" is unchanged. Set it on the server; nil sets it back to "#FFFFFF". */
	colorTint: string;
	/** The picture's contrast: 1 is unchanged, from 0 to 2. Set it on the server. */
	contrast: number;
	/** How long one day lasts, in real minutes: 30. */
	readonly dayLength: number;
	/** Brighten (above 0) or darken (below 0) the whole picture, in stops, from -4 to 4: 0 is the game's own exposure and +1 doubles the brightness. It adds to the game's own exposure; while set (not 0) it also replaces the place's own exposure bias. Set it on the server. */
	exposureBias: number;
	/** The fog's colour as "#RRGGBB", or nil for the sky's own. Set it on the server. */
	fogColor: string | undefined;
	/** How thick the fog is, times the weather's own: 1 is unchanged, 0 clears it, up to 4. Set it on the server. */
	fogDensity: number;
	/** True while the sun is below the horizon. */
	readonly isNight: boolean;
	/** How hard it is raining right now, from 0 (dry) to 1 (a storm). */
	readonly rainIntensity: number;
	/** How colourful the picture is: 1 is unchanged, 0 is black and white, up to 2. Set it on the server. */
	saturation: number;
	/** true shows the stars (even by day), false hides them, nil lets them come out at night on their own. Set it on the server. */
	starsVisible: boolean | undefined;
	/** How bright the sun (and by night the moon) is, times its own: 1 is unchanged, 0 to 4. Set it on the server. */
	sunIntensityScale: number;
	/** The hour, from 0 up to 24: 6 is sunrise, 12 is noon, 18 is sunset. Every server starts at 9. */
	readonly timeOfDay: number;
	/** How fast the clock runs: 1 is the normal 30-minute day, 0 stops it, 2 is twice as fast. Set it (server) from 0 to 60; the hour carries on from where it is. */
	timeScale: number;
	/** How dark the picture's corners are, from 0 to 1, or nil for the game's own. Set it on the server. */
	vignette: number | undefined;
	/** The weather, or the weather it is changing to: Clear, PartlyCloudy, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, DustCalm or DustStorm. */
	readonly weather: string;
	/** Lightning flashed (client only), at the same moment for everyone. The default sky already shows the bolt and plays its thunder. */
	readonly lightningStrike: Signal<[]>;
	/** A server script changed an environment property: the argument names it ("ClockTime", "TimeScale", "Weather", "CloudCover", "FogDensity", "FogColor", "StarsVisible", "SunIntensityScale", "ExposureBias", "Saturation", "Contrast", "Vignette", "ColorTint", "Bloom", "ColorGrading" or "ColorGradingIntensity"). Fires everywhere, a player who joins later included. The clock running on by itself is not a change. */
	readonly propertyChanged: Signal<[property: string]>;
	/** The weather started changing. The argument is the new weather, as in Environment.Weather. */
	readonly weatherChanged: Signal<[weather: string]>;
	/** Take away this player's own look from SetLocalLook (client only): the picture follows the server's values again. (client only) */
	clearLocalLook(): void;
	/** Stop the clock where it is: the same as setting TimeScale to 0. Set TimeScale to 1 to start it again. (server only) */
	freezeClock(): void;
	/** Let the weather change on its own again after SetWeather, blending back over blendSeconds (0 to 600, 90 when left out). (server only) */
	resumeNaturalWeather(blendSeconds?: number | undefined): void;
	/** Cover this much of the sky with cloud, from 0 to 1, whatever the weather; nil gives the weather its own clouds back. The sun dims and the stars fade under heavy cover. (server only) */
	setCloudCover(cover?: number | undefined): void;
	/** Change how the picture looks for this player only (client only). look holds any of ExposureBias (-4 to 4), Saturation (0 to 2), Contrast (0 to 2), Vignette (0 to 1), ColorTint ("#RRGGBB"), Bloom (0 to 4), ColorGrading ("Warm", "Cool", "Noir", "Sepia", "Vivid" or "Faded") and ColorGradingIntensity (0 to 1), taken as the properties of the same names take them. What look sets wins over the server's values; what it leaves out follows them. Each call replaces the last look whole, and the look goes when this script stops. Time and weather stay the same for everyone. From the server, send one player a look with a Remote's FireClient and call this in its OnClientEvent. (client only) */
	setLocalLook(look: { exposureBias?: number | undefined; saturation?: number | undefined; contrast?: number | undefined; vignette?: number | undefined; colorTint?: string | undefined; bloom?: number | undefined; colorGrading?: string | undefined; colorGradingIntensity?: number | undefined }): void;
	/** Hold the weather at Clear, PartlyCloudy, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, DustCalm or DustStorm until ResumeNaturalWeather, changing to it over blendSeconds (0 to 600, 90 when left out). A Storm brings lightning. The snow and dust weathers (LightSnow, Snow, Blizzard, DustCalm, DustStorm) never come on their own. (server only) */
	setWeather(weather: string, blendSeconds?: number | undefined): void;
}
const Environment: EnvironmentService;

/** Named events that let scripts on the same side talk to each other, in any language. */
interface EventsService {
	/** The event with this name; connect to it or Fire it. Doesn't cross the network (use Net:Remote for that). */
	get(name: string): Signal;
}
const Events: EventsService;

/** Blow things up from a server script: damage that fades with distance, a shove for loose props and players, and a visible, audible blast. The damage goes through the same path weapons use, so armour, the shield, team friendly fire and the Damaged and Died signals all apply (a death credits the attacker). Everything happens on the server; there is nothing for a client script here. */
interface ExplosionsService {
	/** Explode at position (centimetres). Everything alive or loose within Radius is hurt and pushed, and the call returns how many things were hurt. options: Radius (centimetres, 500 by default; 10 to 5000), Damage (at the centre, 100 by default; 0 to 1000000), InnerRadius (full damage out to here, 0 by default), Falloff (how damage drops from InnerRadius to 0 at Radius: 0 is full damage everywhere, 1 drops in a straight line, the default, and 2 or more drops faster), DamageType (the tag Damaged and Died report, "Explosion" by default; your own tag works), Attacker (the Player who gets the kill, and whose team friendly fire protects; their own blast hurts them too, which makes rocket jumps; leave it out for the world), Impulse (the speed in centimetres per second a loose physics prop at the centre is given, fading with distance; 800 by default, 0 for none; the weight of the prop does not matter), Ragdoll (true by default: a player in the blast is thrown down and gets up again after RagdollSeconds, 2 by default; with false only a heavy hit throws them), LineOfSight (true by default: nothing behind a wall or a hill is touched; false makes the blast reach through walls), Effect (the look of the blast: "None", "Small", "Medium" (the default), "Large" or "Dirt") and Sound (true by default). At most 30 explosions a second. A player riding in a car is shielded by its body. NPCs take the damage like players do. Example: Explosions:Create(barrel.Position, {Radius = 600, Damage = 80, Attacker = player}) (server only) */
	create(position: Vector3, options?: { radius?: number | undefined; damage?: number | undefined; innerRadius?: number | undefined; falloff?: number | undefined; damageType?: string | undefined; attacker?: Player | undefined; impulse?: number | undefined; ragdoll?: boolean | undefined; ragdollSeconds?: number | undefined; lineOfSight?: boolean | undefined; effect?: string | undefined; sound?: boolean | undefined }): number;
}
const Explosions: ExplosionsService;

/** Outbound HTTP from server scripts (server only): call your own backend, a webhook, any public web service. Players' games never make these requests. Private and internal addresses are always refused. */
interface HttpService {
	/** Whether outbound HTTP works on this server: false when the server's operator turned it off, so a place can do without it quietly. (server only) */
	isAvailable(): boolean;
	/** JSON text as a value: objects become tables with string keys, arrays become lists, null becomes nil. Raises an error when the text isn't valid JSON. */
	jsonDecode(text: string): any;
	/** A value as JSON text. Accepts nil, booleans, finite numbers, strings, and tables (a list becomes an array, anything else an object), nested up to 32 deep. Raises an error for anything else. Luau has no JSON of its own; JS and Python have JSON and json. */
	jsonEncode(value: any): string;
	/** Send an HTTP request and get a Signal that fires once with the result: call :Wait() on it straight away, or :Once(handler). options.url is required (http:// or https://, port 80, 443 or 1024 and up); method is GET (default), POST, PUT, PATCH or DELETE; headers is a table of strings; body is text (UTF-8) or bodyBase64 is binary (up to 2 MiB, not with GET); timeout is 1 to 60 seconds (default 30); followRedirects (default true) follows up to 5 redirects, each one re-checked; responseType is "text" (default) or "base64". Option keys can be spelled any way (followRedirects, follow_redirects); a key that is not on this list raises bad_request. The result is {ok, status?, headers?, body?, url?, error?, code?}: ok is true only for a 2xx answer, a 404 or 500 still has its status and body with code "http_status", and a request that failed after sending has ok = false and a code such as dns_failed, blocked_address, connect_failed, tls_failed, timeout, too_many_redirects, redirect_blocked or response_too_large. A request refused before sending raises an error that starts with its code (bad_url, blocked_address, blocked_port, bad_header, bad_request, body_too_large, too_many_requests, rate_limited, bandwidth_limited or disabled): catch it with pcall (Luau) or try (JS, Python). A server may run 16 requests at once with 128 more waiting, about 600 a minute, 16 MiB sent and 128 MiB received a minute. Mantle logs the host, port, status and sizes of each request, never its path, headers or bodies. Keep API keys out of your script text: a server script's source is private to its owner, but keys you can rotate belong in Secrets (Secrets:Get). (server only) */
	requestAsync(options: { url: string; method?: string | undefined; headers?: Record<string, string> | undefined; body?: string | undefined; bodyBase64?: string | undefined; timeout?: number | undefined; followRedirects?: boolean | undefined; responseType?: string | undefined }): Signal<[{ ok: boolean; status?: number | undefined; headers?: Record<string, string> | undefined; body?: string | undefined; url?: string | undefined; error?: string | undefined; code?: string | undefined }]>;
}
const Http: HttpService;

/** The local player's keys, mouse buttons and gamepad buttons, and their sticks, triggers and mouse wheel as axes (client only). Mantle's own controls (the menu, chat, voice, movement, the Backpack and Tool keys) can never be bound or read. */
interface InputService {
	/** Call callback when the player presses one of keys (phase "Began") and when they let go (phase "Ended"); key is the name of the key that did it. keys lists 1 to 4 names such as {"Q", "ButtonY"}: letters, "0" to "9", "F1" to "F10", "Tab", "MouseLeft", "MouseRight", "MouseMiddle", "MouseButton4", "MouseButton5", "ButtonA", "ButtonB", "ButtonX", "ButtonY", "ButtonL1" and "ButtonR1". A key Mantle uses for its own controls is an error; GetBindableKeys lists the free ones. Binding a name again replaces that action. The key still does anything Mantle does with it (F and ButtonY also enter and leave cars). Nothing is heard while Mantle's menu or chat is open. (client only) */
	bindAction(name: string, keys: (string)[], callback: (phase: string, key: string) => void): void;
	/** Call callback when one of axes moves, at most once a frame per axis; axis is the name of the one that moved. axes lists 1 to 4 of "LeftStickX", "LeftStickY", "RightStickX", "RightStickY" (-1 to 1, right and up positive, 0 inside the stick's dead zone), "LeftTrigger", "RightTrigger" (0 to 1) and "MouseWheel" (the notches scrolled that frame, up positive; heard only on frames it moves). A stick or trigger whose movement is one of Mantle's own controls is an error; GetBindableAxes lists the free ones. A trigger Mantle uses as a button (fire, aim) can still be read here: the button keeps working, and no script can bind it as a key. Binding a name again replaces that binding; axis bindings and BindAction's actions have separate names. The control still does anything Mantle does with it (the mouse wheel also steps Mantle's camera in and out). Everything reads 0 while Mantle's menu or chat is open. (client only) */
	bindAxis(name: string, axes: (string)[], callback: (value: number, axis: string) => void): void;
	/** Where axis is right now, by the names BindAxis takes, as its callbacks last heard it: "MouseWheel" reads the notches of the last frame. 0 while Mantle's menu or chat is open. An axis that is part of Mantle's own controls is an error. (client only) */
	getAxis(axis: string): number;
	/** Every axis name this place can bind: the names BindAxis takes, minus the sticks and triggers whose movement Mantle uses for its own controls. A trigger Mantle uses only as a button (fire, aim) is listed. (client only) */
	getBindableAxes(): (string)[];
	/** Every key name this place can bind: the names BindAction takes, minus Mantle's own controls. (client only) */
	getBindableKeys(): (string)[];
	/** Whether the player is holding key down right now, by the same names BindAction takes. Always false while Mantle's menu or chat is open. A key Mantle uses for its own controls is an error. (client only) */
	isKeyDown(key: string): boolean;
	/** Remove the action this script bound under name. Returns false when it has none by that name. A script's actions also go away by themselves when it stops. (client only) */
	unbindAction(name: string): boolean;
	/** Remove the axis binding this script made under name. Returns false when it has none by that name. A script's axis bindings also go away by themselves when it stops. (client only) */
	unbindAxis(name: string): boolean;
}
const Input: InputService;

/** Let players walk up to an object and act on it with a key press, and hear when they do. A place can mark an object interactable itself; here a script makes one, changes what players can do to it, and reacts. The server decides everything: change things and listen to Interacted on the server. The client events are for the local player and only for effects such as a sound or a glow. */
interface InteractionService {
	/** The local player let go of a hold or mash, or its progress drained away, or the part stopped being usable (client only; for effects). A hold or mash that finishes fires Interacted on the server instead. */
	readonly cancelled: Signal<[entity: Entity, part: string, action: string]>;
	/** A player used one of the entity's options (server only). part is the id of the part they used ("" for the object as a whole) and action is "Primary" or "Secondary". A hold or mash fires it once, when it is finished. This is the event that counts: it is checked on the server, so a player out of range, behind a wall or on cooldown never gets here. Connect to it, check which entity it is, and do what the use should do. */
	readonly interacted: Signal<[entity: Entity, player: Player, part: string, action: string]>;
	/** How far the local player's hold or mash has got, 0 to 1, as it changes (client only; for effects such as a rising sound). */
	readonly progress: Signal<[entity: Entity, part: string, action: string, progress: number]>;
	/** The local player's aim came onto a part of an interactable entity and its prompt appeared (client only; for effects, not for changing the game). */
	readonly selected: Signal<[entity: Entity, part: string]>;
	/** The local player began holding or mashing an option (client only; for effects). */
	readonly started: Signal<[entity: Entity, part: string, action: string]>;
	/** The local player's aim left a part, or it stopped being usable (client only; for effects). */
	readonly unselected: Signal<[entity: Entity, part: string]>;
	/** Make the entity not interactable any more, for everyone: no prompt, no key. SetOptions makes it interactable again with the options you give. (server only) */
	remove(entity: Entity): void;
	/** Change how the entity's prompt looks, for everyone. Only the fields you give change. Style is "Default" (the key and text of each option, stacked), "Balanced" (a panel with the Title on top and the options under it) or "Informative" (like Balanced, with the Description beside the options and an accent line); Balanced and Informative need a Title and show as Default without one. Title is up to 80 characters, Description up to 200, and AccentColor is "#RRGGBB". (server only) */
	setDisplay(entity: Entity, display: { style?: string | undefined; title?: string | undefined; description?: string | undefined; accentColor?: string | undefined }): void;
	/** Turn one option of the entity off or back on, for everyone. action is "Primary" or "Secondary", the input the option uses. Turning an option on also ends its cooldown early. An option with AfterUse "OneShot" that was used stays used. Pass a part's id for an object with several parts; leave it out for the object as a whole. (server only) */
	setEnabled(entity: Entity, action: string, enabled: boolean, part?: string): void;
	/** Make the entity interactable and set what players can do to it, for everyone. options is a list of at most two tables, one for each input: Key ("Primary" by default, or "Secondary"), Text (what the prompt says beside the key, such as "Open door"; leave it out to show the key alone), HoldSeconds (hold the key this long; 0 is a tap), PressCount (mash the key this many times instead; wins over HoldSeconds), PressDrainPerSecond (how fast a mash's progress drains, 0 to 10), AfterUse ("None", "DisableUntilEnabled", "Cooldown" or "OneShot") and CooldownSeconds (needed above 0 for "Cooldown"). Pass a part's id to change only that part of an object the place gave several parts; leave it out for the object as a whole. An empty list leaves the object with nothing to do. Example: Interaction:SetOptions(door, {{Key = "Primary", Text = "Open door"}}) (server only) */
	setOptions(entity: Entity, options: ({ key?: string | undefined; text?: string | undefined; holdSeconds?: number | undefined; pressCount?: number | undefined; pressDrainPerSecond?: number | undefined; afterUse?: string | undefined; cooldownSeconds?: number | undefined })[], part?: string): void;
	/** Set how close a player must stand to the entity, in centimetres, for the prompt to show and the key to work. Clamped to 50..5000. A place's default is 250. (server only) */
	setRange(entity: Entity, range: number): void;
	/** Make one option of the entity unusable for this many seconds (up to 3600), for everyone, as if it had just been used with AfterUse "Cooldown". action is "Primary" or "Secondary". SetEnabled ends it early. Pass a part's id for an object with several parts; leave it out for the object as a whole. (server only) */
	startCooldown(entity: Entity, action: string, seconds: number, part?: string): void;
}
const Interaction: InteractionService;

/** The built-in leaderboard panel: players press Tab (or pick Leaderboard in the game menu) to see the top 10 of one of this place's ordered stores, with each player's name and picture (server only). */
interface LeaderboardService {
	/** Take the leaderboard panel down: players no longer see it until Configure shows a board again. The store keeps its entries. (server only) */
	clear(): void;
	/** Show the ordered store with this name (as DataStore:GetOrderedDataStore takes it) on the leaderboard panel: its top 10, highest first, or lowest first when options.ascending is true. Key each entry by the player's UserId and the panel shows their name and picture; any other key shows as it is. options.title (up to 32 characters, "Leaderboard" when left out) heads the panel and options.valueLabel (up to 16, "Score" when left out) heads the value column. The panel reads the store now and every 30 seconds while anyone is playing, so it trails the store by up to 30 seconds. Call it again to switch boards; one board shows at a time. (server only) */
	configure(store: string, options?: { title?: string | undefined; valueLabel?: string | undefined; ascending?: boolean | undefined } | undefined): void;
}
const Leaderboard: LeaderboardService;

/** Spawn lights at runtime (server only). Change any light through its Entity's Light members. */
interface LightsService {
	/** Spawn a light every player sees and return it as an Entity. kind is "Point", "Spot" or "Directional". props sets how it starts: Color ("#RRGGBB", white by default), Intensity (candelas, 8 by default; lux for a directional light, 10 by default), Range (cm, 1000 by default), InnerCone and OuterCone (degrees, 0 and 44 by default), Enabled, CastShadow (off by default: shadows are costly) and Rotation (a spot light points straight down and a directional light down at 45 degrees unless you give one). Numbers out of range are clamped. At most 64 spawned lights exist at once in a place; Destroy one to make room. (server only) */
	spawn(kind: string, position: Vector3, props?: { color?: string | undefined; intensity?: number | undefined; range?: number | undefined; innerCone?: number | undefined; outerCone?: number | undefined; enabled?: boolean | undefined; castShadow?: boolean | undefined; rotation?: Vector3 | undefined } | undefined): Entity;
}
const Lights: LightsService;

/** A small round lifecycle you drive yourself: start a round, end it (by calling EndRound or when its timer runs out), and read the round number and clock. The server runs it; State, Round and TimeRemaining are readable from client scripts too, for a HUD. It does not pick winners, move players or restart anything by itself: connect to RoundStarted and RoundEnded and do what your game needs. The match is Waiting before a round and between rounds, InProgress during one, and Ended after EndMatch. */
interface MatchService {
	/** The number of the current round, or of the last one while Waiting: 0 before any round has started, 1 for the first. Reset puts it back to 0. */
	readonly round: number;
	/** "Waiting" (before the first round and between rounds), "InProgress" (a round is running) or "Ended" (EndMatch was called; Reset starts over). */
	readonly state: string;
	/** Seconds left in the running round, counting down to 0, or nil when no round is running or the round has no time limit. On a client this follows the server's clock, so it can be a fraction of a second off. */
	readonly timeRemaining: number | undefined;
	/** A round ended, on the server. winner is what EndRound or EndMatch was given (nil for none, and always nil when the time ran out). reason is "Ended" (EndRound), "Time" (the round's timer ran out) or "MatchEnded" (EndMatch or Reset cut it short). */
	readonly roundEnded: Signal<[round: number, winner: string | undefined, reason: string]>;
	/** A round began (StartRound), on the server. round is its number, starting at 1. */
	readonly roundStarted: Signal<[round: number]>;
	/** State changed, on the server. Arguments are the old state, then the new one (each "Waiting", "InProgress" or "Ended"), the same order as PlayerTeamChanged. Fires after RoundStarted or RoundEnded. */
	readonly stateChanged: Signal<[old: string, new: string]>;
	/** End the whole match. A running round ends first (RoundEnded fires with reason "MatchEnded"), then State becomes Ended. Returns false when the match has already ended. (server only) */
	endMatch(winner?: string | undefined): boolean;
	/** End the running round: RoundEnded fires with winner (a team name, a player's name, anything up to 64 characters, or nil for none) and reason "Ended", and State goes back to Waiting. Returns false when no round is running. (server only) */
	endRound(winner?: string | undefined): boolean;
	/** Start over: a running round ends first (reason "MatchEnded"), then State is Waiting and Round is 0. Teams and players are not touched. (server only) */
	reset(): void;
	/** Begin the next round: Round goes up by one, State becomes InProgress, and RoundStarted fires. With seconds (up to 86400) the round ends by itself when the time is up, with reason "Time" and no winner; without, it runs until EndRound. Returns false, changing nothing, while a round is already running or after EndMatch (call Reset first). (server only) */
	startRound(seconds?: number | undefined): boolean;
}
const Match: MatchService;

/** Spawn characters the server walks around the place and animates (server only). */
interface NPCsService {
	/** The NPC an Entity is (a World:Raycast hit, or NPC:GetEntity()), or nil when it is not a live NPC. (server only) */
	fromEntity(entity: Entity): NPC | undefined;
	/** Spawn a character standing at position (its feet), facing yaw degrees (0 faces +X), and return it as an NPC. Every player sees it. look.AvatarItemId optionally selects a published avatar body; omit it to spawn an NPC with no rendered body. SkinTone and EyeColor are avatar palette ids ("tone_05"), Height is a percent from 95 to 105. A place holds at most 50 NPCs at once; Destroy one to make room. (server only) */
	spawn(position: Vector3, look?: { avatarItemId?: string | undefined; skinTone?: string | undefined; eyeColor?: string | undefined; height?: number | undefined } | undefined, yaw?: number | undefined): NPC;
}
const NPCs: NPCsService;

/** Send messages between the server and players. */
interface NetService {
	/** Get the remote with this name (the same name on server and client refers to the same remote). */
	remote(name: string): Remote;
}
const Net: NetService;

/** Everyone in the session. */
interface PlayersService {
	/** The player on this machine. Always set in client scripts (they start once it exists); nil on a dedicated server. */
	readonly localPlayer: Player;
	/** A player joined after the handler connected. Does not replay existing players; use Players:OnPlayer to handle players already present and future joins. */
	readonly playerAdded: Signal<[player: Player]>;
	/** A player is leaving. */
	readonly playerRemoving: Signal<[player: Player]>;
	/** The player whose character this entity is right now, or nil. */
	getPlayerFromCharacter(character: Entity): Player | undefined;
	/** Players currently in the session. */
	getPlayers(): (Player)[];
	/** Call handler for every player: those already here and everyone who joins later. The usual way to set players up. */
	onPlayer(handler: (player: Player) => void): Connection;
}
const Players: PlayersService;

/** Fire projectiles from a server script: a ball, rocket, arrow or cube that flies under gravity, hits what is in its way, hurts it, and can explode. Each flies on the server and shows on every player's screen. Fire returns an id; Hit and Expired say how it ended. A projectile never hits the player or NPC that fired it, and a hit goes through the same damage path weapons use (armour, shield, friendly fire, Damaged and Died). Weapon damage, fire rate and ammo stay the game's: this is for your own gadgets, traps and turrets. At most 64 projectiles fly at once. */
interface ProjectilesService {
	/** A projectile's lifetime ran out without hitting anything, on the server. position is where it was (it has exploded there if it had ExplodeOnExpire). Cancel never fires this. */
	readonly expired: Signal<[id: number, tag: string | undefined, position: Vector3]>;
	/** A projectile hit something, on the server. id is what Fire returned and tag the Tag it was given (nil without one). entity is the thing it struck (an NPC's entity too; NPCs:FromEntity gives the NPC), player is set when that was a player's character, and both are nil when it hit plain ground or a wall. position is where. The damage and any explosion have already happened; the projectile is gone. */
	readonly hit: Signal<[id: number, tag: string | undefined, entity: Entity | undefined, player: Player | undefined, position: Vector3]>;
	/** Take a projectile out of the air: it vanishes without Hit or Expired, and without exploding. Returns false when it has already ended. (server only) */
	cancel(id: number): boolean;
	/** Launch a projectile and return its id. options: Position (where it starts) and either Velocity (a Vector3 in centimetres per second: the direction and speed) or Target (a point to hit; it is aimed at that point at Speed, 3000 by default, arcing for gravity; a target out of reach gets the flattest shot it can); GravityScale (1 by default; 0 flies dead straight, up to 10), Lifetime (seconds before it expires, 5 by default; 0.1 to 30), Look ("Ball" the default, "Rocket", "Arrow", "Cube" or "Invisible"), Color ("#RRGGBB"), Scale (0.1 to 10), Damage (to what it hits directly: 25 by default, or 0 when it has an Explosion; 0 to 1000000), DamageType ("Bullet" by default), Explosion (a table like Explosions:Create's options: it goes off where the projectile hits, and replaces the direct damage unless you also give Damage), ExplodeOnExpire (true: it goes off where it is when the lifetime ends, like a grenade; it needs an Explosion table, otherwise it explodes with the defaults), Tag (a name up to 64 characters Hit and Expired give back, so a handler knows which of its projectiles ended) and Attacker (the Player who gets the credit, whose team friendly fire protects, and who it never hits). Example: Projectiles:Fire({Position = muzzle, Velocity = aim * 2500, Look = "Rocket", Attacker = player, Explosion = {Radius = 400, Damage = 90}}) (server only) */
	fire(options: { position: Vector3; velocity?: Vector3 | undefined; target?: Vector3 | undefined; speed?: number | undefined; gravityScale?: number | undefined; lifetime?: number | undefined; look?: string | undefined; color?: string | undefined; scale?: number | undefined; damage?: number | undefined; damageType?: string | undefined; explosion?: { radius?: number | undefined; damage?: number | undefined; innerRadius?: number | undefined; falloff?: number | undefined; damageType?: string | undefined; impulse?: number | undefined; ragdoll?: boolean | undefined; ragdollSeconds?: number | undefined; lineOfSight?: boolean | undefined; effect?: string | undefined; sound?: boolean | undefined } | undefined; explodeOnExpire?: boolean | undefined; tag?: string | undefined; attacker?: Player | undefined }): number;
}
const Projectiles: ProjectilesService;

/** How a death becomes a respawn. By default a dead player respawns on their own after a short wait at a spawn point the place picks. Change the wait, or turn automatic respawning off to run rounds where the dead stay down until you bring them back with Player:Respawn. The server decides; there is nothing for a client script here. Player.Respawned fires when a player gets a fresh character. */
interface RespawnService {
	/** Whether a dead player respawns by themselves after Delay (true by default). Set it false and the dead stay dead until a script calls Player:Respawn on them. Applies to deaths from then on. (server only) */
	autoRespawn: boolean;
	/** Seconds a dead player waits before the game respawns them (5 unless the place or a script changes it). Setting it clamps to 0..600 and applies to deaths from then on, not to a respawn already counting down. (server only) */
	delay: number;
}
const Respawn: RespawnService;

/** Facts about the running script. */
interface ScriptService {
	/** True when this script runs on a client. */
	readonly isClient: boolean;
	/** True when this script runs on the server. */
	readonly isServer: boolean;
	/** Display name: the script file's name without extension. */
	readonly name: string;
	/** The name this place's storage gives the script package this script belongs to, or "" for one of the place's own scripts. Example: Script.Package is "KillBricks" for Packages/KillBricks/Server/Main.luau. */
	readonly package: string;
	/** Path relative to the Scripts folder, e.g. "Server/Rounds.luau". */
	readonly path: string;
	/** The Script Points placed for this script's package: each an entity at the point's position, tagged with the package's storage name, its parameters readable with GetAttribute (numbers and true/false already converted, as the package declares them). Points that are switched off are left out. Empty for one of the place's own scripts. A package's scripts run once, not once per point: loop over Script.Points. Example: for _, point in Script.Points do print(point:GetAttribute("speed")) end */
	readonly points: (Entity)[];
	/** The settings this place gives this script's package, set once for the whole package in Place Settings > Storage: setting name to value, already converted as the package declares them (a number, true/false, or text; a choice is its text). A setting the place leaves unset has the package's default; one with no default and no value is missing (nil). Empty for one of the place's own scripts. A fresh table on every read. Example: local roundLength = Script.Settings.RoundLength */
	readonly settings: Record<string, boolean | number | string>;
	/** "server" or "client". */
	readonly side: string;
}
const Script: ScriptService;

/** Read the API keys and webhook URLs you set for this place, so they never sit in your scripts. Server scripts on a hosted server only. */
interface SecretsService {
	/** The value you set for the secret called name (in the editor plugin's place settings), or nil when this place has none by that name or secrets are not available here (see Secrets:IsAvailable). Names are upper-case letters, digits and underscores, starting with a letter: DISCORD_WEBHOOK. The server loads them once, before any script starts, so a value is there from your script's first line; one you change in the plugin reaches servers that start after the change. Never send a value to a player (in a remote event, state, an attribute, chat or UI): anything a client gets, its player can read. Use it to call a service from the server, and keep keys you can rotate. When you test locally with -MantlePlace, values come from secrets.local.json beside the place file: {"DISCORD_WEBHOOK": "https://..."}. Keep that file out of version control. Example: local hook = Secrets:Get("DISCORD_WEBHOOK") if not hook then print("set DISCORD_WEBHOOK for this place") end (server only) */
	get(name: string): string | undefined;
	/** Whether this server loaded the place's secrets, so Secrets:Get answers from them. False in standalone or listen play of a published place, on a community server, when the server could not reach Mantle, and in a place whose scriptApi is older than 1.47 (the editor records it from your scripts); Secrets:Get then reads nil for every name. True with no secrets set. Example: if not Secrets:IsAvailable() then print("no secrets here: posting to Discord is off") end (server only) */
	isAvailable(): boolean;
}
const Secrets: SecretsService;

/** Facts about the server this session runs on (server only). */
interface ServerService {
	/** The id of the private server this session belongs to, or nil for a public session. (server only) */
	readonly privateServerId: string | undefined;
	/** The Mantle account id (a plain UUID) of the private server's owner, or nil for a public session. (server only) */
	readonly privateServerOwnerId: string | undefined;
}
const Server: ServerService;

/** Play the sounds uploaded with this place: effects and loops, flat or placed in the world, and one music track per player. */
interface SoundsService {
	/** Play one of the sounds uploaded with this place (a sound emitter's Entity.SoundId, or its id in the place's assets). From a client script only this player hears it; from a server script every player does, or only params.Player. Give Position or AttachTo to place it in the world, where it fades to silence at Radius cm (4000 by default); without either it plays flat, like interface sounds. Volume 0 to 2 (1), Pitch 0.5 to 2 (1), Looped (false), FadeIn seconds (0). A sound that has not finished downloading starts when it arrives. Returns a SoundHandle. */
	play(sound: string, params?: { volume?: number | undefined; pitch?: number | undefined; looped?: boolean | undefined; position?: Vector3 | undefined; attachTo?: Entity | undefined; radius?: number | undefined; fadeIn?: number | undefined; player?: Player | undefined } | undefined): SoundHandle;
	/** Play one of the place's sounds as music: from a client script this player's, from a server script every player's, or only params.Player's. Each player has one music track: a new one crossfades from the old over FadeIn seconds (1 by default), and playing the track already playing only changes its Volume (0 to 2). Loops unless Looped is false. */
	playMusic(sound: string, params?: { volume?: number | undefined; looped?: boolean | undefined; fadeIn?: number | undefined; player?: Player | undefined } | undefined): void;
	/** Stop the music, fading it out over params.FadeOut seconds (0, the default, stops it at once). From a server script: every player's music, or only params.Player's. */
	stopMusic(params?: { fadeOut?: number | undefined; player?: Player | undefined } | undefined): void;
}
const Sounds: SoundsService;

/** Shared game values the server sets and everyone can read (score, round, phase). */
interface StateService {
	/** A value changed (fires on the server and on every client). */
	readonly changed: Signal<[key: string, value: any]>;
	/** The current value, or nil. */
	get(key: string): any;
	/** Call handler with the value now and every time it changes. The easy way to keep UI in sync. */
	observe(key: string, handler: (value: any) => void): Connection;
	/** Set a value for everyone. Numbers, strings, booleans, Vector3 and tables work. (server only) */
	set(key: string, value: any): void;
}
const State: StateService;

/** The library items (vehicles, weapons and script packages) this place stores, by the names its storage gives them. */
interface StorageService {
	/** The library item id (itm_u...) of the entry this place's storage declares under name. The name is case-sensitive. Pass the id to Vehicles:Spawn (a stored vehicle) or Backpack:Grant (a stored weapon): they accept only an id the place stores. Errors when the place's storage has no entry with that name. */
	get(name: string): string;
}
const Storage: StorageService;

/** Teams for a place: define them, put players on them, and decide whether teammates can hurt each other. The server decides everything; which team a player is on replicates, so client scripts and HUDs can read it. A team is named by its name (not case-sensitive) and has a colour. Friendly fire is off by default: once a player is on a team, damage from a teammate is ignored until Teams.FriendlyFire is set to true. */
interface TeamsService {
	/** Whether players on the same team can hurt each other (false by default). When false, damage between teammates is ignored everywhere damage goes through the health system: weapons, melee, explosions and Player:TakeDamage. Hits from nobody, and a player's own hits, still count. Setting it is server only. */
	friendlyFire: boolean;
	/** Whether players on the same team bump into each other (true by default). Set it to false and teammates walk straight through one another -- handy for a crowded team base or a spawn room -- while enemies, players on no team and the world still collide as always. It works on whoever is on a team at the moment, so a player who changes team starts or stops colliding with their new mates at once. Setting it is server only. */
	teammatesCollide: boolean;
	/** A player joined, left or switched team, on the server and on every client. Arguments are the player, the old team, then the new team (team names; nil means no team). */
	readonly playerTeamChanged: Signal<[player: Player, old: string | undefined, new: string | undefined]>;
	/** Define a team. name is 1 to 32 characters and must be new. color is "#RRGGBB" (white when left out), for a HUD or a nameplate to use. Up to 16 teams. Example: Teams:Create("Red", "#E53935") (server only) */
	create(name: string, color?: string | undefined): void;
	/** The team's colour as "#RRGGBB", or nil when there is no such team. */
	getColor(name: string): string | undefined;
	/** The players on the team right now (an empty list for a team nobody is on, or one that does not exist). */
	getPlayers(name: string): (Player)[];
	/** The names of the teams, in the order they were created. */
	list(): (string)[];
	/** Delete a team. Its players are taken off it first, so PlayerTeamChanged fires for each. (server only) */
	remove(name: string): void;
}
const Teams: TeamsService;

/** Clocks for scripts. */
interface TimeService {
	/** A high-precision timer in seconds, for measuring how long something takes. */
	readonly clock: number;
	/** Seconds since the game started, synced to the server's clock on every machine. */
	readonly now: number;
}
const Time: TimeService;

/** The local player's on-screen UI: a simple HUD line/toast, and the retained node tree behind real widgets. */
interface UIService {
	/** Whether the player wants captions shown. */
	readonly captions: boolean;
	/** Whether the player asked for a high-contrast presentation. */
	readonly highContrast: boolean;
	/** The player's current locale (e.g. "fr-CA"). */
	readonly locale: string;
	/** Whether the player asked for reduced motion. */
	readonly reducedMotion: boolean;
	/** The player's platform text-scale preference (0.85-1.5). */
	readonly textScale: number;
	/** The player's remapped label for an action name (e.g. the bound key), or the action name itself if unset. (client only) */
	actionLabel(action: string): string;
	/** Create a new node: "frame", "text", "image", "button", "toggle", "list", "grid", "scroll" or "prompt". Nil means the per-script node budget (256) is exhausted. (client only) */
	create(kind: string): UiNode | undefined;
	/** Format a number for display using the player's current locale. (client only) */
	formatNumber(value: number): string;
	/** Open one of the place's UI documents (a .rml file in its UI/ folder) on this player's screen. Mode is "modal" (the default: gameplay input stops and Back closes it), "overlay" (play goes on; the mouse works on its controls) or "hud" (drawn only, never takes input). An unknown path or mode is an error, and so is opening on a server. Example: local shop = UI:OpenDocument("UI/shop.rml") (client only) */
	openDocument(path: string, mode?: string | undefined): UiDocument;
	/** Force the mouse cursor on or off regardless of whether an interactive node is visible right now. Nil goes back to automatic: shown only while a modal UI (see SetInputMode) has a Button, a Toggle, or a Prompt with an action visible and enabled; an overlay UI shows no cursor, so the mouse keeps turning the camera. SetCursorVisible(true) shows it in either mode. A gamepad-only panel can call SetCursorVisible(false) to keep working through focus/Accept without ever drawing a cursor. (client only) */
	setCursorVisible(visible?: boolean | undefined): void;
	/** Choose what an interactive node (a Button, a Toggle, or a Prompt with an action) does to gameplay input while it is on screen. "overlay" (the default): no cursor shows, so the mouse keeps turning the camera, and the keyboard and gamepad stay with the game, so the player keeps moving; press the UI's buttons with a key binding in the script, or call UI:SetCursorVisible(true) to show a cursor. "modal": the cursor shows, the UI takes keyboard and gamepad focus, and movement, attack, look and gamepad input stop until the last interactive node is gone, then come back exactly as they were; Esc still opens the game menu. One setting for all of this place's retained UI on the player's screen; a place's scripts share it, and it resets when they all stop. Any other mode is an error. Example: UI:SetInputMode("modal") (client only) */
	setInputMode(mode: string): void;
	/** Show text in a named HUD line (same slot replaces the previous text; empty text clears it). (client only) */
	setText(slot: string, text: string): void;
	/** Pop up a short message. (client only) */
	toast(text: string, seconds?: number | undefined): void;
}
const UI: UIService;

/** Spawn and seat cars: the vehicles in the place's storage (server only). */
interface VehiclesService {
	/** The car's driver started boosting. */
	readonly boostStarted: Signal<[vehicle: Entity]>;
	/** The car's driver stopped boosting: let go of the key, ran out of boost, or the car lost its fuel or health. */
	readonly boostStopped: Signal<[vehicle: Entity]>;
	/** The car lost health: a crash, Damage, or SetHealth to a lower number. amount is how much it lost and health what it has left. Only fires for a car you spawned with Vehicles:Spawn, once health is on. */
	readonly damaged: Signal<[vehicle: Entity, amount: number, health: number]>;
	/** The car's health ran out. It cannot accelerate until it is repaired (Repair), so this is the moment to eject the driver, blow it up or remove it. */
	readonly destroyed: Signal<[vehicle: Entity]>;
	/** The car's fuel changed: it burns in steps of about a hundredth of MaxFuel, not every frame, or a script set or filled it. Arguments are the car, the old fuel, then the new. */
	readonly fuelChanged: Signal<[vehicle: Entity, old: number, new: number]>;
	/** The car ran out of fuel and cannot accelerate until it is refuelled (Refuel). */
	readonly fuelEmpty: Signal<[vehicle: Entity]>;
	/** The car Vehicles:Spawn returned has been built and can be used: Seat, Lock, paint, livery, health, fuel and boost all work from here. The server builds spawned cars one per frame, so this fires a frame or more after Spawn (a burst of spawns takes that many frames). It fires once per spawn, also when the car was built at once. Connect before you Spawn or in the same script step; it only fires for a car you spawned with Vehicles:Spawn. */
	readonly ready: Signal<[vehicle: Entity]>;
	/** Take amount of health off the car (never below 0) and return the health it has left. Fires Vehicles.Damaged, and Vehicles.Destroyed if it reaches 0. Turn health on first with SetHealthEnabled. (server only) */
	damage(vehicle: Entity, amount: number): number;
	/** Put the player back on foot beside the car they are driving. Returns false when they are not driving, the car is still moving, or there is nowhere beside it to stand. (server only) */
	eject(player: Player): boolean;
	/** The boost the car has left, from 0 to MaxBoost. (server only) */
	getBoost(vehicle: Entity): number;
	/** How much boost is used each second while the driver boosts (25 unless you change it). (server only) */
	getBoostDrain(vehicle: Entity): number;
	/** How much boost comes back each second while the driver is not boosting (10 unless you change it). (server only) */
	getBoostRegen(vehicle: Entity): number;
	/** How hard the boost pushes, from 0 (nothing) to 1 (the hardest it can; about 18 m/s of extra speed every second). 0.5 unless you change it. (server only) */
	getBoostStrength(vehicle: Entity): number;
	/** The fuel in the tank, from 0 to MaxFuel. (server only) */
	getFuel(vehicle: Entity): number;
	/** How much fuel the car burns each second while it is driven and moving (1 unless you change it). (server only) */
	getFuelConsumption(vehicle: Entity): number;
	/** The car's health, from 0 to MaxHealth. It only changes while health is on. (server only) */
	getHealth(vehicle: Entity): number;
	/** The most boost the car can store (100 unless you change it). (server only) */
	getMaxBoost(vehicle: Entity): number;
	/** The tank's size (100 unless you change it). (server only) */
	getMaxFuel(vehicle: Entity): number;
	/** The most health the car can have (100 unless you change it). (server only) */
	getMaxHealth(vehicle: Entity): number;
	/** The car's speed in centimetres per second, as the simulation reports it. Errors if the entity is not a car or no longer exists. (server only) */
	getSpeed(vehicle: Entity): number;
	/** Whether the car has a boost (SetBoostEnabled). (server only) */
	isBoostEnabled(vehicle: Entity): boolean;
	/** Whether the car's driver is boosting right now. (server only) */
	isBoosting(vehicle: Entity): boolean;
	/** Whether the car has health on and none left. (server only) */
	isDestroyed(vehicle: Entity): boolean;
	/** Whether the car has a fuel tank and it is empty. (server only) */
	isFuelEmpty(vehicle: Entity): boolean;
	/** Whether the car has a fuel tank (SetFuelEnabled). (server only) */
	isFuelEnabled(vehicle: Entity): boolean;
	/** Whether the car has health (SetHealthEnabled). (server only) */
	isHealthEnabled(vehicle: Entity): boolean;
	/** Whether the car is locked. A locked car opens only for the last player who drove it: anyone else's vehicle key rattles the door, and holding the key breaks in -- the window on that side breaks, the car unlocks, and it must be hotwired before it starts. The lock stays when the driver gets out, and its driver can also lock and unlock it from the seat (L / D-pad left). Errors if the entity is not a car or no longer exists. (server only) */
	isLocked(vehicle: Entity): boolean;
	/** Lock the car. A locked car opens only for the last player who drove it: anyone else's vehicle key rattles the door, and holding the key breaks in -- the window on that side breaks, the car unlocks, and it must be hotwired before it starts. The lock stays when the driver gets out, and its driver can also lock and unlock it from the seat (L / D-pad left). Returns false when the entity is not a car. (server only) */
	lock(vehicle: Entity): boolean;
	/** Put amount of fuel in the tank (never above MaxFuel), or fill it when you leave amount out. Returns the fuel in the tank now. Example: Vehicles:Refuel(car, 20) (server only) */
	refuel(vehicle: Entity, amount?: number): number;
	/** Give the car amount of health back (never above MaxHealth), or all of it when you leave amount out. Returns the health it has now. A destroyed car drives again once it has any health. (server only) */
	repair(vehicle: Entity, amount?: number): number;
	/** Put the player in the driver's seat, through the same checks as walking up and pressing the prompt: the player must be on foot, alive, near the car with a clear line to the seat, and the seat must be empty. Returns false when any of that refuses. (server only) */
	seat(vehicle: Entity, player: Player): boolean;
	/** Set the boost left, clamped to 0..MaxBoost. (server only) */
	setBoost(vehicle: Entity, boost: number): void;
	/** Set how much boost is used each second while the driver boosts, from 0 up. (server only) */
	setBoostDrain(vehicle: Entity, perSecond: number): void;
	/** Give the car a boost (true) or take it away (false). Off for every car until you turn it on. With boost on, its driver holds Shift (B on a gamepad) to push the car forward harder. Boost drains at BoostDrain a second while it pushes and comes back at BoostRegen a second when it does not. It starts full (MaxBoost, 100 unless you change it). A destroyed car or one with no fuel cannot boost. (server only) */
	setBoostEnabled(vehicle: Entity, enabled: boolean): void;
	/** Set how much boost comes back each second, from 0 up. 0 means a used boost never comes back by itself. (server only) */
	setBoostRegen(vehicle: Entity, perSecond: number): void;
	/** Set how hard the boost pushes, from 0 (nothing) to 1 (the hardest it can). (server only) */
	setBoostStrength(vehicle: Entity, strength: number): void;
	/** Set the fuel, clamped to 0..MaxFuel. Fires Vehicles.FuelChanged, and Vehicles.FuelEmpty if it reaches 0. (server only) */
	setFuel(vehicle: Entity, fuel: number): void;
	/** Set how much fuel the car burns each second while it is driven and moving, from 0 up. 0 means it never runs dry. (server only) */
	setFuelConsumption(vehicle: Entity, perSecond: number): void;
	/** Give the car a fuel tank (true) or take it away (false). Off for every car until you turn it on. With fuel on, the car burns FuelConsumption units a second while someone drives it and it is moving, and with none left it cannot accelerate (it rolls to a stop). It starts full (MaxFuel, 100 unless you change it). (server only) */
	setFuelEnabled(vehicle: Entity, enabled: boolean): void;
	/** Set the car's health, clamped to 0..MaxHealth. Lowering it counts as damage (Vehicles.Damaged fires, and Vehicles.Destroyed if it reaches 0). (server only) */
	setHealth(vehicle: Entity, health: number): void;
	/** Give the car health (true) or take it away again (false). It is off for every car until you turn it on, so a car you never touch crashes as it always did. With health on, crashes wear the car down (a hard enough crash wrecks it in one hit), and at 0 health the car is destroyed: it cannot accelerate until you Repair it. It starts full (MaxHealth, 100 unless you change it). (server only) */
	setHealthEnabled(vehicle: Entity, enabled: boolean): void;
	/** Put one of the car's liveries on it: index counts from 0 in the liveries its vehicle was published with, and -1 takes the livery off. Every player sees the change. Returns false when the entity is not a car or the car has no livery at that index (the built-in cars have none). (server only) */
	setLivery(vehicle: Entity, index: number): boolean;
	/** Set the most boost the car can store, from 1 to 1,000,000. (server only) */
	setMaxBoost(vehicle: Entity, max: number): void;
	/** Set the tank's size, from 1 to 1,000,000. Lowering it below the fuel lowers the fuel too; raising it does not fill the tank. (server only) */
	setMaxFuel(vehicle: Entity, max: number): void;
	/** Set the most health the car can have, from 1 to 1,000,000. Lowering it below the car's health lowers the health too; raising it does not heal. (server only) */
	setMaxHealth(vehicle: Entity, max: number): void;
	/** Paint the car: color is "#RRGGBB", the same format as Backpack:SetWeaponColor. Returns false when the entity is not a car. (server only) */
	setPaintColor(vehicle: Entity, color: string): boolean;
	/** Spawn a car at position and return it as an Entity. item is the library item id of a vehicle this place stores: the string Storage:Get returns for one of the place's storage names. Any other value errors -- an asset id, an object path, a built-in car's name, an item the place does not store, or a weapon. The car is a real replicated vehicle: every player sees it, walks up to it and can drive it. Read its Position, tag it, or Destroy it like any entity. The server builds cars one per frame, so the Entity comes back at once but the car is not ready to use: connect to Vehicles.Ready, which fires with the car when it is built, before you Seat, paint or Lock it or touch its health, fuel or boost (those error until then). (server only) */
	spawn(item: string, position: Vector3): Entity;
	/** Unlock the car, so anyone can open it again. A locked car opens only for the last player who drove it: anyone else's vehicle key rattles the door, and holding the key breaks in -- the window on that side breaks, the car unlocks, and it must be hotwired before it starts. The lock stays when the driver gets out, and its driver can also lock and unlock it from the seat (L / D-pad left). Returns false when the entity is not a car. (server only) */
	unlock(vehicle: Entity): boolean;
}
const Vehicles: VehiclesService;

/** Control who hears whom on voice chat, and see who is speaking. */
interface VoiceService {
	/** A player started talking on voice chat. Fires on the server (from the player's own report) and on clients (for players they can hear). Players with voice switched off never fire it.  Example: Voice.SpeakingStarted:Connect(function(p) print(p.Name .. " is talking") end) */
	readonly speakingStarted: Signal<[player: Player]>;
	/** A player stopped talking on voice chat. Fires wherever SpeakingStarted does. */
	readonly speakingStopped: Signal<[player: Player]>;
	/** Whether your rules let the listener hear the speaker. It does not know the players' own mutes and blocks, so a true here can still be silent for that listener. (server only) */
	canHear(listener: Player, speaker: Player): boolean;
	/** Whether you have muted this player for everyone with Voice:SetMuted. It says nothing about players' own mutes, and it is false while you have only muted them for some listeners. (server only) */
	isMuted(player: Player): boolean;
	/** Whether the player is talking on voice chat right now. Each player's game tells the server whether they are talking (a yes or no, never audio), so it can lag by a fraction of a second, and a modified game could lie: use it for indicators and game play, not for anything that must be secure. A muted player still counts as speaking. On a client it covers the players this player can hear, themselves included. Example: if Voice:IsSpeaking(p) then ShowMicIcon(p) end */
	isSpeaking(player: Player): boolean;
	/** Stop (true) or allow (false) one listener hearing one speaker; everyone else still hears the speaker. Use it for rules that depend on who is listening. Returns false when either player is not verified yet, or you already hold 4096 of these pairs. Lasts until either player leaves, or Voice:Reset. Example: for _, other in Players:GetPlayers() do Voice:MuteFor(other, dead, true) end -- the living cannot hear a dead player, who still hears them. (server only) */
	muteFor(listener: Player, speaker: Player, muted: boolean): boolean;
	/** Clear voice rules: every one that names player (muted, muted for someone, heard by an audience, or in one), or every rule on the server when you leave player out. (server only) */
	reset(player?: Player | undefined): void;
	/** Make a speaker heard only by the players in listeners (and by themselves); leave listeners out, or pass nil, to let everyone hear them again. This is team voice: set each player's audience to their team. At most 128 listeners. Returns false when the speaker is not verified yet or the list is too long. Lasts until the speaker leaves, or Voice:Reset. Mutes still apply on top. Example: Voice:SetAudience(p, Teams:GetPlayers(p.Team)) -- p's team hears p; call it again when teams change. (server only) */
	setAudience(speaker: Player, listeners?: (Player)[] | undefined): boolean;
	/** Mute (true) or unmute (false) a player's voice for everyone: nobody hears them until you unmute them, though they can still talk. Use it for players who are dead, jailed or out of the round. Returns false when the player is not verified yet (moments after joining). The mute lasts until the player leaves. It only ever makes them quieter: a player who has muted or blocked someone themselves keeps them muted whatever you do. Voice goes straight between players' games, not through the server, so the server cannot hear or record it, and each listener's game applies the mute. A player running a modified game could ignore it, so use it to run a game, never to protect a secret. Example: Players:OnPlayer(function(p) Voice:SetMuted(p, true) end) -- everyone starts muted until a round begins, then Voice:SetMuted(p, false) for the players in it. (server only) */
	setMuted(player: Player, muted: boolean): boolean;
}
const Voice: VoiceService;

/** Find and watch the things in the level. */
interface WorldService {
	/** Sweep a box of this size (its full width, depth and height, at most 100 m on any side, kept square to the world's axes) from origin along direction and return the first thing it touches, in the same table as World:Raycast, or nil. Takes the same params as World:Raycast. */
	blockcast(origin: Vector3, size: Vector3, direction: Vector3, params?: { maxDistance?: number | undefined; include?: (Entity | Player)[] | undefined; exclude?: (Entity | Player)[] | undefined; tag?: string | undefined; ignorePlayers?: boolean | undefined; layers?: (string)[] | undefined; excludeLayers?: (string)[] | undefined }): { entity: Entity; player?: Player | undefined; position: Vector3; normal: Vector3; distance: number } | undefined;
	/** Every entity with this actor tag. */
	findByTag(tag: string): (Entity)[];
	/** The first entity whose name (actor label) matches, or nil. */
	findFirst(name: string): Entity | undefined;
	/** Every entity and player character touching a box of this size (its full width, depth and height, at most 100 m on any side, kept square to the world's axes) centred on center, nearest first. Takes the same params as World:GetEntitiesInRadius. */
	getEntitiesInBox(center: Vector3, size: Vector3, params?: { include?: (Entity | Player)[] | undefined; exclude?: (Entity | Player)[] | undefined; tag?: string | undefined; ignorePlayers?: boolean | undefined; maxResults?: number | undefined; layers?: (string)[] | undefined; excludeLayers?: (string)[] | undefined }): (Entity)[];
	/** Every entity and player character touching a ball of this radius (at most 50 m) around center, nearest first. Overlap-only entities (triggers) count; entities with no collision never do. A character is listed as its Entity; Players:GetPlayerFromCharacter tells whose it is. params: Include, Exclude, Tag, IgnorePlayers, Layers, ExcludeLayers (collision layers, server scripts only), and MaxResults (256 at most, and by default). Runs on this machine only; a script may make 256 spatial queries a frame. */
	getEntitiesInRadius(center: Vector3, radius: number, params?: { include?: (Entity | Player)[] | undefined; exclude?: (Entity | Player)[] | undefined; tag?: string | undefined; ignorePlayers?: boolean | undefined; maxResults?: number | undefined; layers?: (string)[] | undefined; excludeLayers?: (string)[] | undefined }): (Entity)[];
	/** Call handler for every entity that has the tag now, and for each one that gets it later. */
	onTagged(tag: string, handler: (entity: Entity) => void): Connection;
	/** Call handler when an entity loses the tag. */
	onUntagged(tag: string, handler: (entity: Entity) => void): Connection;
	/** Cast a ray from origin along direction and return the first thing it hits: a table of Entity, Player, Position, Normal and Distance, or nil. Player is set when the hit is a player's character. The ray is as long as direction, or params.MaxDistance when given, and at most 1 km. It passes through overlap-only entities (triggers) and never hits entities with no collision. params: MaxDistance, Include (only these entities or players count), Exclude, Tag (only entities with this tag count), IgnorePlayers, Layers (only entities on these collision layers count; server scripts only) and ExcludeLayers (look through entities on these layers). Runs on this machine only; a script may make 256 spatial queries a frame. */
	raycast(origin: Vector3, direction: Vector3, params?: { maxDistance?: number | undefined; include?: (Entity | Player)[] | undefined; exclude?: (Entity | Player)[] | undefined; tag?: string | undefined; ignorePlayers?: boolean | undefined; layers?: (string)[] | undefined; excludeLayers?: (string)[] | undefined }): { entity: Entity; player?: Player | undefined; position: Vector3; normal: Vector3; distance: number } | undefined;
	/** Sweep a ball of this radius (at most 50 m) from origin along direction and return the first thing it touches, in the same table as World:Raycast, or nil. Distance is how far the ball's centre travelled. Takes the same params as World:Raycast. */
	spherecast(origin: Vector3, radius: number, direction: Vector3, params?: { maxDistance?: number | undefined; include?: (Entity | Player)[] | undefined; exclude?: (Entity | Player)[] | undefined; tag?: string | undefined; ignorePlayers?: boolean | undefined; layers?: (string)[] | undefined; excludeLayers?: (string)[] | undefined }): { entity: Entity; player?: Player | undefined; position: Vector3; normal: Vector3; distance: number } | undefined;
}
const World: WorldService;

const task: {
	wait(seconds?: number): Promise<number>;
	spawn(fn: (...args: any[]) => any, ...args: any[]): void;
	delay(seconds: number, fn: (...args: any[]) => any, ...args: any[]): number;
	defer(fn: (...args: any[]) => any, ...args: any[]): number;
	cancel(id: number): void;
};

function setTimeout(fn: (...args: any[]) => void, ms?: number, ...args: any[]): number;
function setInterval(fn: (...args: any[]) => void, ms?: number, ...args: any[]): number;
function clearTimeout(id: number): void;
function clearInterval(id: number): void;

const console: {
	log(...args: any[]): void;
	info(...args: any[]): void;
	warn(...args: any[]): void;
	error(...args: any[]): void;
};

} // declare global
