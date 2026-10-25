# Mantle scripting API for Python. GENERATED from the engine's API registry; do not edit.
from typing import Any, Callable, Optional

class Vector3:
    x: float
    y: float
    z: float
    magnitude: float
    unit: 'Vector3'
    zero: 'Vector3'
    one: 'Vector3'
    x_axis: 'Vector3'
    y_axis: 'Vector3'
    z_axis: 'Vector3'
    def __init__(self, x: float = 0, y: float = 0, z: float = 0) -> None: ...
    def __add__(self, other: 'Vector3 | float') -> 'Vector3': ...
    def __sub__(self, other: 'Vector3 | float') -> 'Vector3': ...
    def __mul__(self, other: 'Vector3 | float') -> 'Vector3': ...
    def __truediv__(self, other: 'Vector3 | float') -> 'Vector3': ...
    def __neg__(self) -> 'Vector3': ...
    def dot(self, other: 'Vector3') -> float: ...
    def cross(self, other: 'Vector3') -> 'Vector3': ...
    def lerp(self, goal: 'Vector3', alpha: float) -> 'Vector3': ...
    def distance(self, other: 'Vector3') -> float: ...

class WaitToken:
    """Yield this from a generator function to wait: `yield task.wait(1)`."""
    result: Any

class Signal:
    def connect(self, handler: Callable[..., Any]) -> 'Connection': ...
    def once(self, handler: Callable[..., Any]) -> 'Connection': ...
    def wait(self) -> WaitToken: ...
    def fire(self, *args: Any) -> None: ...

class Entity:
    """A thing in the world: any actor in the level."""
    anchored: bool
    angular_velocity: Vector3
    can_collide: bool
    clip_time: Optional[float]
    collision_layer: str
    color: str
    exists: bool
    light_color: str
    light_enabled: bool
    light_inner_cone: float
    light_intensity: float
    light_kind: Optional[str]
    light_outer_cone: float
    light_range: float
    mass: float
    name: str
    physics_material: Optional[str]
    position: Vector3
    rotation: Vector3
    scale: Vector3
    sound_id: Optional[str]
    sound_volume: float
    transparency: float
    velocity: Vector3
    visible: bool
    attribute_changed: Signal
    collided: Signal
    touch_ended: Signal
    touched: Signal
    def add_tag(self, tag: str, /) -> None:
        """Give the entity a tag (fires World:OnTagged handlers)."""
        ...
    def apply_angular_impulse(self, impulse: Vector3, /) -> None:
        """Spin a physics entity: an angular impulse in kg*cm^2/s about the world axes, in degrees. Does nothing while it is Anchored. Errors on an entity with no physics block."""
        ...
    def apply_impulse(self, impulse: Vector3, /) -> None:
        """Push a physics entity: impulse is in kg*cm/s along the world axes, so its velocity changes by impulse / Mass. Does nothing while it is Anchored. Errors on an entity with no physics block."""
        ...
    def clone(self) -> Entity:
        """Make a copy (same look, tags and place). On the server the copy appears for everyone; on a client only for that player. Tip: keep a hidden template and clone it. A physics entity, a Constraint or an NPC can't be cloned (it errors, and says how to make another)."""
        ...
    def destroy(self) -> None:
        """Remove the entity. On the server it disappears for everyone; on a client only for that player."""
        ...
    def get_attribute(self, name: str, /) -> Any:
        """A custom value stored on the entity, or nil."""
        ...
    def get_material(self, slot: Optional[float] = ..., /) -> Optional[str]:
        """The material asset id a mesh shows in a slot (0 when left out), or nil when the slot shows the mesh's own material."""
        ...
    def get_tags(self) -> list[str]:
        """All of the entity's tags."""
        ...
    def has_tag(self, tag: str, /) -> bool:
        """Whether the entity has this tag."""
        ...
    def observe_attribute(self, name: str, handler: Callable[..., Any], /) -> Connection:
        """Call handler with the attribute's value now and every time it changes."""
        ...
    def play_clip(self, clipAssetId: Optional[str] = ..., params: Optional[dict[str, Any]] = ..., /) -> None:
        """Play one of the place's AnimClip assets on a SkeletalMesh, starting now. Leave clipAssetId out (or nil) to restart the clip it plays now, or its own. params: Loop (true by default; false holds the last frame), PlayRate (clip seconds per second, 1 by default, clamped to 0..10) and StartOffset (seconds into the clip to start at, 0 by default, clamped to 0..3600). On the server it plays for everyone, players who join later included, in step on every screen; on a client it only changes that client's view. Errors when the entity is not a SkeletalMesh, the id is not an AnimClip in the place's assets, or the clip was authored on another skeleton (on a dedicated server, which never loads clips, that last one shows on each player's screen as the mesh's resting pose instead)."""
        ...
    def play_effect(self) -> None:
        """Effects only: start emitting. A looping effect already playing keeps going; one that does not loop bursts for a second again. On the server it plays for everyone, players who join later included; on a client it only changes that client's view. Errors when the entity is not an Effect."""
        ...
    def play_sound(self) -> None:
        """Sound emitters only: play the emitter's sound again from the beginning (looping if the emitter loops). From a server script every player hears it."""
        ...
    def remove_tag(self, tag: str, /) -> None:
        """Remove a tag (fires World:OnUntagged handlers)."""
        ...
    def set_attribute(self, name: str, value: Any, /) -> None:
        """Store a custom value on the entity (numbers, strings, booleans, Vector3, tables). Server values replicate."""
        ...
    def set_effect_color(self, color: str, /) -> None:
        """Effects only: tint the particles, '#RRGGBB'; the next particles take it. Replicates like PlayEffect. Errors when the entity is not an Effect or the colour is not '#RRGGBB'."""
        ...
    def set_effect_rate(self, rate: float, /) -> None:
        """Effects only: how many particles spawn a second, clamped to 0..200; 0 emits nothing while it still plays. Replicates like PlayEffect. Errors when the entity is not an Effect."""
        ...
    def set_material(self, assetId: str, slot: Optional[float] = ..., /) -> None:
        """Show another of the place's Material assets on this mesh: in one material slot (counting from 0), or in every slot when slot is left out. '' puts the place's own material back. Replicates like Color. Errors when the id is not a Material in the place's assets, the slot doesn't exist, or the entity is not a mesh."""
        ...
    def stop_clip(self) -> None:
        """Stop a SkeletalMesh's clip: it shows its resting pose until the next PlayClip. Replicates like PlayClip. Errors when the entity is not a SkeletalMesh."""
        ...
    def stop_effect(self) -> None:
        """Effects only: stop emitting; the particles already out die out. Replicates like PlayEffect. Errors when the entity is not an Effect."""
        ...
    def stop_sound(self, fadeOut: Optional[float] = ..., /) -> None:
        """Sound emitters only: stop the emitter's sound, fading it out over fadeOut seconds (0 by default). It stays silent until PlaySound, from a server script for players who join later too."""
        ...
    def tween(self, goal: dict[str, Any], seconds: float, style: Optional[str] = ..., direction: Optional[str] = ..., /) -> Signal:
        """Smoothly move/rotate/scale to the goal. Since script API 1.8 the goal can also fade a light's LightColor or LightIntensity, or a mesh's Color; those errors like setting them would. style: Linear, Sine, Quad, Cubic, Quart, Quint, Exponential, Circular, Back, Bounce, Elastic. direction: In, Out, InOut. Returns a Completed signal. A Position, Rotation or Scale goal on an entity the place did not mark dynamic is a script error, like setting them."""
        ...

class Player:
    """A connected player."""
    armor: Optional[float]
    avatar: Optional[str]
    breath: Optional[float]
    breath_enabled: Optional[bool]
    breath_regen: Optional[float]
    breath_regen_delay: Optional[float]
    character: Optional[Entity]
    crouch_enabled: Optional[bool]
    crouch_speed: Optional[float]
    drown_damage: Optional[float]
    health: Optional[float]
    is_crouched: Optional[bool]
    is_ragdolled: Optional[bool]
    is_sprinting: Optional[bool]
    is_swimming: Optional[bool]
    is_underwater: Optional[bool]
    jump_height: Optional[float]
    jump_stamina_cost: Optional[float]
    max_breath: Optional[float]
    max_health: Optional[float]
    max_shield: Optional[float]
    max_stamina: Optional[float]
    name: str
    position: Optional[Vector3]
    regen_delay: Optional[float]
    regen_rate: Optional[float]
    shield: Optional[float]
    shield_regen_rate: Optional[float]
    sprint_enabled: Optional[bool]
    sprint_speed: Optional[float]
    stamina: Optional[float]
    stamina_drain: Optional[float]
    stamina_enabled: Optional[bool]
    stamina_regen: Optional[float]
    stamina_regen_delay: Optional[float]
    team: Optional[str]
    user_id: str
    walk_speed: Optional[float]
    wearing: dict[str, Any]
    breath_changed: Signal
    breath_restored: Signal
    crouch_started: Signal
    crouch_stopped: Signal
    damaged: Signal
    died: Signal
    health_changed: Signal
    out_of_breath: Signal
    ragdolled: Signal
    recovered: Signal
    respawned: Signal
    sprint_started: Signal
    sprint_stopped: Signal
    stamina_changed: Signal
    def force_crouch(self, crouched: bool, /) -> bool:
        """Hold the player crouched (true) or standing (false) whatever they press, until ReleaseCrouch. A player held crouched can still walk (sprinting is off while crouched); one held standing cannot crouch or slide. CrouchEnabled false wins over a crouch hold. Returns false when the player has no character on foot. The hold ends with the character: a respawn gives the key back."""
        ...
    def heal(self, amount: float, /) -> bool:
        """Raise the character's health by amount, up to MaxHealth. Never brings a dead character back (use Respawn). Returns false when the player has no character on foot, is dead, or is already at full health."""
        ...
    def kick(self, reason: Optional[str] = ..., /) -> None:
        """Remove the player from the session with a message."""
        ...
    def kill(self) -> bool:
        """Kill the character outright: Died fires with no killer, and the place's respawn rules take over as for any other death. Returns false when the player has no character on foot or is already dead."""
        ...
    def ragdoll(self, seconds: Optional[float] = ..., velocity: Optional[Vector3] = ..., /) -> bool:
        """Knock the character down: it goes limp for everyone, the player cannot move or act, and when the time is up (or Recover is called) it stands back up where it lies with a get-up animation. seconds is how long to stay down, 0.1 to 300; leave it out to stay down until Recover (at most five minutes). velocity is an optional shove in centimetres per second; without it the character keeps the speed it had. Ragdolled fires now and Recovered when it gets up. Returns false when the player has no character on foot, is dead, is sitting in a vehicle or is already knocked down."""
        ...
    def recover(self) -> bool:
        """Stand a knocked-down character back up now, wherever it lies; Recovered fires when it is up. Safe to call at any time. Returns false when the character was not knocked down."""
        ...
    def release_crouch(self) -> bool:
        """Give the player the crouch key back after ForceCrouch. Returns false when the player has no character on foot."""
        ...
    def respawn(self, position: Optional[Vector3] = ..., /) -> bool:
        """Give the player a fresh character with full health right now, alive or dead: at position when given, otherwise at the spawn point the place's respawn mode picks. The old character (or a corpse waiting for its respawn) is removed. Returns false when the player is driving a vehicle (eject them first) or cannot be spawned."""
        ...
    def take_damage(self, amount: float, source: Optional[Player] = ..., damageType: Optional[str] = ..., /) -> bool:
        """Hurt the character through the same path weapons use, so the game's damage rules apply: friendly fire, Armor, then Shield, then health. source is the player to credit: if this hit kills, Died names them as the killer. damageType tags the hit for Damaged and Died ('Generic' when left out); use a built-in such as 'Bullet', 'Melee', 'Explosion', 'Fall', 'Vehicle' or 'Area', or any word of your own up to 32 characters, such as 'Poison'. Returns false when the player has no character on foot (respawning or driving), is already dead, or nothing got through (friendly fire off, or Armor 100)."""
        ...
    def teleport(self, position: Vector3, /) -> bool:
        """Move the player's character instantly. Always lands exactly at position, even inside solid geometry; returns false only when the player has no character to move right now."""
        ...

class Connection:
    """Returned by Signal:Connect; call Disconnect to stop listening."""
    connected: bool
    def disconnect(self) -> None:
        """Stop calling the handler."""
        ...

class Remote:
    """A named network event between server and clients."""
    name: str
    on_client_event: Signal
    on_server_event: Signal
    def fire_all_clients(self, *args: Any) -> None:
        """Send arguments to every player's OnClientEvent handlers."""
        ...
    def fire_client(self, player: Player, /, *args: Any) -> None:
        """Send arguments to one player's OnClientEvent handlers."""
        ...
    def fire_server(self, *args: Any) -> None:
        """Send arguments to the server's OnServerEvent handlers."""
        ...

class Constraint:
    """One joint between physics entities, from Constraints:Create or Constraints:FromEntity (server only)."""
    broken: Signal
    def destroy(self) -> None:
        """Remove the joint for good: the entities move freely again, and Broken never fires. A joint a script made also goes when every script restarts."""
        ...

class Crowd:
    """One CrowdSpawner's ambient crowd, from Crowds:Get (server only)."""
    density: float
    enabled: bool
    paused: bool
    seed: float

class NPC:
    """A character NPCs:Spawn made. The server walks and animates it; every player sees it."""
    exists: bool
    health: float
    max_health: float
    position: Vector3
    rotation: Vector3
    target: Optional[Entity]
    target_player: Optional[Player]
    walk_speed: float
    attacked: Signal
    died: Signal
    drive_finished: Signal
    health_changed: Signal
    move_completed: Signal
    target_changed: Signal
    def attack(self, target: Entity | Player | NPC, options: dict[str, Any] = ..., /) -> None:
        """Make target the NPC's enemy: it walks up to it and hurts it every Interval until it dies, goes away or you call ClearTarget. TargetChanged fires now, and Attacked fires at every swing or shot. It replaces whatever the NPC was doing. Melee by default: options Range (how close it gets before it swings, centimetres, 150 by default; 50 to 500), Damage (10 by default), DamageType ('Melee' by default) and Interval (seconds between swings, 1 by default; 0.1 to 60). The damage goes through the weapons' path, so armour, shield and the Damaged and Died signals apply to a player it hits; a kill credits nobody (the NPC is not a player), and friendly fire does not protect against an NPC. Chase (true by default): false makes it stand still and attack only what comes into Range. Give Projectile (the options of Projectiles:Fire, minus Position, Velocity, Target and Attacker) to make it a ranged attacker: it stops within Range (2000 by default, up to 5000) with a clear line to the target and fires that projectile at it, aimed for gravity, every Interval; Damage and DamageType then come from the projectile. Once a player target dies the NPC stops and TargetChanged fires with no target; attack them again after they respawn, or let SetAggroRange do it. Example: zombie:Attack(player, {Damage = 15, Interval = 1.2}) or turret:Attack(player, {Range = 3000, Interval = 0.5, Projectile = {Speed = 4000, Damage = 8, Look = 'Ball'}})"""
        ...
    def clear_target(self) -> bool:
        """Call the NPC off: it stops attacking or following and stands where it is, and TargetChanged fires with no target. Returns false when it had nothing to stop. An aggro range (SetAggroRange) stays on and may pick a new target."""
        ...
    def destroy(self) -> None:
        """Remove the NPC for everyone. Its walk ends without MoveCompleted, and it no longer counts toward the 50."""
        ...
    def drive_along(self, vehicle: Entity, points: list[Vector3], options: Optional[dict[str, Any]] = ..., /) -> None:
        """As DriveTo, through points in order (one to 64 positions): the car drives each leg in turn, passing the points on the way without stopping, and ends at the last, which is where it arrives. ArriveRadius is the last point's radius."""
        ...
    def drive_to(self, vehicle: Entity, position: Vector3, options: Optional[dict[str, Any]] = ..., /) -> None:
        """Put the NPC in the driver's seat of vehicle and drive it to position: the NPC sits in the car for every player, which drives straight at the point, and DriveFinished fires when the drive ends. The NPC must be alive and within 4.5 m of the car with a clear line to the seat; it is the car's driver, so no player gets the Drive prompt or can take the seat, and the car errors if it already has a driver or the NPC is driving another car (StopDriving or ExitVehicle first). It replaces a drive the NPC is on in the same car (that one finishes as 'cancelled'). options: MaxSpeed (centimetres per second, 1400 by default, about 50 km/h; clamped to 100..3000) and ArriveRadius (centimetres, 600 by default; clamped to 100..5000). The NPC drives straight at the point over open ground: there are no roads to follow, and it does not steer round walls (a car that can't get on reverses and tries again, then gives up as 'stuck'). Cars and trucks only, not motorcycles or aircraft. Arrived, the NPC stays in the seat with the car braked until ExitVehicle."""
        ...
    def exit_vehicle(self) -> bool:
        """Get the NPC out of the car it is driving, standing beside it, and free the seat. A drive in progress ends as 'cancelled'. Returns false when the NPC is not in a car, the car is still moving faster than a walk (3 m/s; StopDriving, then try again once it has stopped) or there is nowhere to stand beside it."""
        ...
    def follow(self, target: Entity | Player | NPC, distance: Optional[float] = ..., /) -> None:
        """Walk after target and keep about distance (centimetres) away from it, 200 by default; 50 to 5000. The NPC follows a player through respawns, going where they go as the server's navigation mesh allows, and stands still once it is close enough. It replaces whatever the NPC was doing (a MoveTo, an earlier Follow or an Attack) and carries on until StopFollowing, ClearTarget, a MoveTo, or the NPC or the target going away; a MoveTo you give it yourself takes over and ends the follow. The walk does not fire MoveCompleted. Example: guard:Follow(player, 300)"""
        ...
    def get_entity(self) -> Entity:
        """The NPC as an Entity: what a raycast that hits it reports. Tag it or give it attributes like any entity."""
        ...
    def heal(self, amount: float, /) -> bool:
        """Raise the NPC's health by amount, up to MaxHealth. Never brings a dead NPC back (use Respawn). Returns false when the NPC is dead or already at full health."""
        ...
    def kill(self) -> bool:
        """Kill the NPC outright: Died fires with no killer. Returns false when it is already dead."""
        ...
    def move_to(self, position: Vector3, /) -> bool:
        """Walk to position along the ground, around walls and obstacles, replacing any walk in progress. Returns false at once when position is more than 500 m away or the NPC is dead; otherwise true, and MoveCompleted fires when the walk ends: true when the NPC arrived, false when there is no way there (no walkable ground, a blocked or too long path). The first walk in a place waits for its navigation to build, which can take a few seconds."""
        ...
    def play_asset_clip(self, clipAssetId: str, params: Optional[dict[str, Any]] = ..., /) -> None:
        """Play one of the place's AnimClip assets on the NPC, starting now, on every player's screen and in step, players who join later included. params: Loop (false by default: the clip plays once and the NPC goes back to walking and standing on its own; true keeps it playing until StopClip or the next PlayClip), PlayRate (clip seconds per second, 1 by default, clamped to 0..10) and StartOffset (seconds into the clip to start at, 0 by default, clamped to 0..3600). The clip must be authored on the skeleton of the NPC's body (the avatars' MetaHuman body). Errors when the id is not an AnimClip in the place's assets or, where the server draws the NPC, the clip is on another skeleton; a dedicated server never loads clips, so it cannot check the skeleton: there a clip on another skeleton shows on each player's screen as the NPC walking and standing on its own instead, with no error for the script."""
        ...
    def play_clip(self, clip: str, loop: Optional[bool] = ..., /) -> None:
        """Play one of the NPC clips on every player's screen: 'walk', 'idle', 'turn_left' or 'turn_right', or (script API 1.23) one of the place's AnimClip asset ids at normal speed (PlayAssetClip also sets its speed and start). It plays once and the NPC goes back to walking and standing on its own, or with loop true keeps playing until StopClip or the next PlayClip. Players who join later see it too. A clip on another skeleton than the NPC's body can't be checked on a dedicated server, which never loads clips; each player's screen then shows the NPC walking and standing on its own instead."""
        ...
    def respawn(self, position: Optional[Vector3] = ..., /) -> bool:
        """Bring the NPC back with full health, dead or alive: standing at position (its feet) when given, otherwise where NPCs:Spawn first put it. Its clip stops, and a walk in progress ends without MoveCompleted. Returns false when it could not be put there."""
        ...
    def set_aggro_range(self, range: float, options: dict[str, Any] = ..., /) -> None:
        """Let the NPC pick its own enemies: whenever it has no target and a living player comes within range (centimetres, up to 5000) it attacks the nearest one, as if you had called Attack with options (the same table as Attack's). 0 turns it off. It looks a few times a second, a ranged attacker ignores players it has no clear line to, and an NPC drops a target that gets more than twice range away. Players only; attack other NPCs with Attack. Example: zombie:SetAggroRange(1500, {Damage = 12})"""
        ...
    def stop(self) -> None:
        """Stop walking where the NPC is. The walk it abandons never fires MoveCompleted."""
        ...
    def stop_clip(self) -> None:
        """End the clip PlayClip started: the NPC walks and stands on its own again."""
        ...
    def stop_driving(self) -> None:
        """End the drive the NPC is on: the car brakes to a stop with the NPC still in the seat, and DriveFinished fires with 'cancelled'. Does nothing when the NPC is not driving."""
        ...
    def stop_following(self) -> bool:
        """End the follow Follow started: the NPC stops where it is. Returns false when the NPC was not following. An Attack is ended with ClearTarget."""
        ...
    def take_damage(self, amount: float, source: Optional[Player] = ..., /) -> bool:
        """Hurt the NPC through the same path weapons use. source is the player to credit: if this hit kills, Died names them as the killer. Returns false when the NPC is already dead or the damage was refused."""
        ...

class OrderedDataStore:
    """A named store of whole-number values for this place, read back sorted: a leaderboard. Get one with DataStore:GetOrderedDataStore (server only). Every method returns a Signal that fires once with a result table {ok, error?, ...}: call :Wait() on it straight away, or :Once(handler)."""
    def get_async(self, key: str, /) -> Signal:
        """Read key's value: the result's value, nil when the store has no such key."""
        ...
    def get_sorted_async(self, ascending: bool, pageSize: Optional[float] = ..., cursor: Optional[str] = ..., /) -> Signal:
        """One page of the store in value order, highest first (lowest first when ascending): the result's entries is a list of {key, value}, up to pageSize (default 50, at most 100), ties in key order. Pass the result's nextCursor back, with the same ascending, for the next page; it is nil on the last one."""
        ...
    def increment_async(self, key: str, delta: Optional[float] = ..., /) -> Signal:
        """Add delta (default 1; may be negative) to key's value, a new key starting from 0. One step on the backend, so no server's increment is lost to another's. The result's value is the new total."""
        ...
    def remove_async(self, key: str, /) -> Signal:
        """Delete key from the store. Removing a key the store doesn't have succeeds."""
        ...
    def set_async(self, key: str, value: float, /) -> Signal:
        """Store a whole number (up to 2^53 - 1 either way) under key. The result's value is what was stored."""
        ...

class SoundHandle:
    """One sound Sounds:Play started. A server script's handle reaches every player who hears the sound."""
    def is_playing(self) -> bool:
        """Whether the sound is still playing (or still downloading, to play when it arrives). False once it is stopped, or a one-shot has finished. A dedicated server never downloads sounds, so there a one-shot counts as playing for up to a minute unless it is stopped."""
        ...
    def set_volume(self, volume: float, /) -> None:
        """Change the sound's volume, 0 to 2. From a server script, players hear the change within a tenth of a second."""
        ...
    def stop(self, fadeOut: Optional[float] = ..., /) -> None:
        """Stop the sound, fading it out over fadeOut seconds (0, the default, stops it at once). Stopping a sound that has already finished does nothing."""
        ...

class UiDocument:
    """A UI document from the place's UI/ folder, open on the local player's screen (UI:OpenDocument). It belongs to the script that opened it and closes when that script stops."""
    is_open: bool
    closed: Signal
    event: Signal
    def close(self) -> None:
        """Close the document. Closed does not fire for your own Close, and closing twice is fine. Example: shop:Close()"""
        ...
    def set_data(self, key: str, value: Any, /) -> None:
        """Set a value the document shows through {{key}}, data-for, data-if and data-attr-*: a number, string, boolean, Vector3 (read as .x, .y and .z), or a table of them. An entity, a player or any other object reference is refused: send its fields instead. The key is a name of letters, digits and _ (up to 64). After the document closes it still checks the key and value, then does nothing. Example: shop:SetData('coins', 120)"""
        ...
    def set_mode(self, mode: str, /) -> None:
        """Switch how the document takes input: 'modal', 'overlay' or 'hud' (see UI:OpenDocument). Does nothing once it has closed. Example: shop:SetMode('hud')"""
        ...

class UiNode:
    """One node in a script's retained UI tree, mounted into the local player's HUD."""
    id: float
    text: str
    activated: Signal
    def attach_to_world(self, target: Entity | Player | NPC, options: dict[str, Any] = ..., /) -> bool:
        """Make this top-level node float over an entity, a player or an NPC in the world, on this player's screen only: a nameplate, a health bar, a marker. Build the node and its children as usual, then attach the top one; it stops being part of the screen's layout and follows the target, bottom-centre on the point above it. It is only drawn, never clicked. Attaching it again changes the options. Returns false if the node is not a top-level node of this script (call SetParent(nil) first) or the script already has 32 nodes floating; the nodes still count toward the node budget (256). If the target is destroyed, or a player has no character, the node hides. options: Bone (a bone or socket of the target's skeletal mesh, such as 'head'; without it, or when the target has no such bone, the node sits over the top of the target), Offset (a Vector3 in centimetres, in world axes, added to that point, such as Vector3.new(0, 0, 30)), ScreenOffsetX and ScreenOffsetY (pixels, x right and y down), MaxDistance (centimetres from the camera, default 3000, at most 50000: farther is hidden), FadeDistance (centimetres: it fades out over the last FadeDistance before MaxDistance; default 0 cuts off at MaxDistance), HideWhenOccluded (hide while world geometry is between the camera and the point; one cheap check a tenth of a second; default false: it draws over everything), HideForSelf (hide when the target is the local player's own character, for a nameplate you do not want over yourself), ScaleWithDistance (default false: constant size on screen; true makes it smaller with distance), MinScale and MaxScale (0.05 to 4, defaults 0.4 and 1; the limits when it scales), ScaleDistance (centimetres where it draws at scale 1, default 1000), OffScreen ('Hide', the default, or 'Clamp' to keep it on the edge of the screen pointing towards the target, for a marker). Example: nameplate:AttachToWorld(player, {Bone = 'head', Offset = Vector3.new(0, 0, 30), MaxDistance = 2500, FadeDistance = 500, HideForSelf = true})"""
        ...
    def destroy(self) -> None:
        """Remove this node and every descendant."""
        ...
    def detach_from_world(self) -> bool:
        """Stop floating this node: it goes back to the screen's layout, as a top-level node. Returns false if it was not attached. Destroy the node instead to remove it altogether."""
        ...
    def set_action(self, action: str, /) -> bool:
        """Name the action this node's Activated carries when it is clicked, tapped or confirmed. Optional on a Button or Toggle (Activated then carries ''); a Prompt needs one."""
        ...
    def set_caption(self, caption: str, /) -> bool:
        """Attach caption text shown underneath this node when the player has captions on."""
        ...
    def set_enabled(self, enabled: bool, /) -> bool:
        """Enable or disable this node (a disabled button/toggle/prompt cannot be activated)."""
        ...
    def set_image(self, assetId: str, /) -> bool:
        """Set an image node's asset (an 'ast_' id from the place's asset library)."""
        ...
    def set_localized_text(self, key: str, fallback: Optional[str] = ..., /) -> bool:
        """Look up key in the place's localisation table for the player's locale (falling back through the language, then the place's default locale, then fallback or key itself) and set it as this node's text."""
        ...
    def set_parent(self, parent: Optional[UiNode] = ..., /) -> bool:
        """Reparent this node (nil detaches it back to the root). Refuses a cycle or a node from another script."""
        ...
    def set_semantics(self, role: str, label: Optional[str] = ..., hint: Optional[str] = ..., focusOrder: Optional[float] = ..., /) -> bool:
        """Describe this node for screen readers and controller focus order: 'none', 'heading', 'label', 'button', 'checkbox', 'image', 'list-container', 'list-item', 'dialog' or 'status'."""
        ...
    def set_style(self, style: Optional[dict[str, Any]] = ..., /) -> bool:
        """Replace this node's style: width, height, columns, font-size; grow (0-100, share of the parent's spare height), gap (0-256, space between children), padding (0-256, space inside the node), opacity (0-1), align ('start', 'center', 'end' or 'stretch', within the parent). Returns false for an unknown key or an out-of-range value; a non-finite number is an error."""
        ...
    def set_text(self, text: str, /) -> bool:
        """Set this node's literal text."""
        ...
    def set_visible(self, visible: bool, /) -> bool:
        """Show or hide this node (and its subtree)."""
        ...

class _AssetsService:
    """Spawn the place's own meshes at runtime (server only). Change any mesh's look through its Entity."""
    def spawn(self, assetId: str, position: Vector3, props: Optional[dict[str, Any]] = ..., /) -> Entity:
        """Spawn one of the place's StaticMesh assets (any mesh its asset list names, placed or not) that every player sees, and return it as an Entity. props sets how it starts: Rotation, Scale (a Vector3, or one number for all three), Material (one of the place's Material assets, for every slot), Color, Transparency, Collision ('Blocking' by default, 'Overlap' or 'None'), CastShadow (true by default) and Anchored (spawned meshes stay where scripts put them; false is refused). A mesh uploaded with the place may take a moment to appear on a player's screen while it downloads. Spawned meshes count towards the same limit as Entity:Clone; Destroy one to make room."""
        ...

Assets: _AssetsService

class _AvatarService:
    """The local player's avatar: open the avatar editor over the place and play their equipped emotes. On the server, read a player's look with Player.Avatar and Player.Wearing."""
    editor_closed: Signal
    editor_opened: Signal
    look_changed: Signal
    def close_editor(self) -> bool:
        """Close the avatar editor as its Close button would, without asking: unsaved changes are put back, and a save already sent still lands. Returns false when it was not open."""
        ...
    def is_editor_open(self) -> bool:
        """Whether the avatar editor is open on this client, whoever opened it."""
        ...
    def open_editor(self, options: Optional[dict[str, Any]] = ..., /) -> bool:
        """Open the avatar editor over the running place, where the player changes their own look while the world goes on behind it. options.Categories limits what it offers: any of 'body' (the avatar, skin tone, eye colour, height and the rest of Body), 'hair' (hair, eyebrows, eyelashes, facial hair), 'clothes' (top, bottom, footwear), 'accessories', 'emotes' or 'all', or slot names such as 'top' or 'acc_hat'. Leave it out for everything (only then are outfits offered). options.Title is a heading over the panel ('Barber'), up to 40 characters. options.FreezeCharacter (default true) holds the character still and turns the camera onto it; false leaves the player in control and the camera where it is. The player sees only items they own or that are free, and Save puts the look on their account exactly as Mantle's own avatar screen does. Esc, the pad's B or Close shut it, asking first about unsaved changes. Returns false, opening nothing, when there is no local character, or the editor or another Mantle screen is already open. An unknown category is an error."""
        ...
    def play_emote(self, itemId: str, /) -> bool:
        """Play one of the local player's equipped emotes, by its item id (an emote on their emote wheel). Returns false, and plays nothing, when itemId is not one of their equipped emotes, when they have no character, or when called again too soon. true means the request went to the server, which checks it again and can still refuse it (the player is moving, in the air, seated or dead)."""
        ...

Avatar: _AvatarService

class _BackpackService:
    """Give players weapons, take them away, and equip them (server only)."""
    item_used: Signal
    def clear(self, player: Player, /) -> float:
        """Empty the player's Backpack, leaving only fists. Returns how many Tools were removed."""
        ...
    def equip_slot(self, player: Player, slot: float, /) -> bool:
        """Put the Tool in this Backpack slot in the player's hands. Slots count from 0, so the hotbar key 1 is slot 0. Returns false when the slot is empty or the player is dead."""
        ...
    def get_tools(self, player: Player, /) -> list[str]:
        """The ids of the Tools in the player's Backpack, in slot order. Fists, which every player always has, is never listed. Empty while the player has no character (respawning)."""
        ...
    def grant(self, player: Player, item: str, /) -> str:
        """Give the player a weapon this place stores: item is the library item id Storage:Get returns for one of the place's storage names. Returns the new Tool's stable id. Errors for anything else -- an asset id, an object path, an item the place does not store, or a vehicle. Also errors if the stored weapon has not finished preparing or the player cannot receive it."""
        ...
    def remove(self, player: Player, toolId: str, /) -> bool:
        """Take the Tool with this id out of the player's Backpack (unequipping it first). Returns false when the player does not have it. Fists can never be removed."""
        ...
    def set_weapon_color(self, player: Player, toolId: str, color: str, /) -> bool:
        """Paint one of the player's weapons: color is '#RRGGBB', the same format as Vehicles:SetPaintColor. Every player sees the new colour. Returns false when the player does not have that Tool or it is not a weapon."""
        ...

Backpack: _BackpackService

class _CameraService:
    """The local player's view (client only): first person, a fixed camera (cut to or tweened into), a camera following an entity, the field of view and a shake. All of it goes back to Mantle's own camera when the script stops or the player respawns."""
    field_of_view: Optional[float]
    mode: str
    def follow(self, entity: Entity, offset: Vector3, relative: Optional[bool] = ..., /) -> None:
        """Keep the camera at offset from entity, looking at it, wherever it goes (Mode becomes 'Follow'). offset is in world axes, so Vector3.new(0, -600, 300) keeps the camera 6 m along -Y and 3 m up; with relative true it turns with the entity's heading instead, so Vector3.new(-600, 0, 250) stays behind it. The player still moves their character. When the entity is destroyed the camera goes back to Mantle's; set Mode to 'Default' or call Reset to give it back sooner."""
        ...
    def reset(self) -> None:
        """Give the camera back to Mantle: Mode 'Default', Mantle's field of view, no shake."""
        ...
    def set_fixed(self, position: Vector3, lookAt: Vector3, /) -> None:
        """Hold the camera still at position, looking at lookAt (Mode becomes 'Fixed'). The player still moves their character. Set Mode to 'Default' or call Reset to give the camera back."""
        ...
    def shake(self, intensity: float, duration: float, /) -> None:
        """Shake the view, fading out over duration seconds. intensity is clamped to 0 to 1 and duration to 0 to 3; a new shake replaces one still running, and 0 stops it. Does nothing when the player turned on Reduced motion."""
        ...
    def tween_fixed(self, position: Vector3, lookAt: Vector3, seconds: float, style: Optional[str] = ..., direction: Optional[str] = ..., /) -> Signal:
        """Move the camera smoothly from where the view is now to position, looking at lookAt, over seconds (0 to 60), then hold it there like SetFixed. Mode is 'Fixed' from the start. style and direction ease it as they do Entity:Tween (Linear and Out unless given). Returns a signal that fires once when the camera arrives; it never fires if SetFixed, TweenFixed, Follow, Mode or Reset takes the camera first."""
        ...

Camera: _CameraService

class _ChatService:
    """React to what players say in chat, post lines from the place, and run private team chat and whispers."""
    message_posted: Signal
    whisper_sent: Signal
    def get_whisper_mode(self) -> str:
        """The whisper mode set with Chat:SetWhisperMode: 'Everyone', 'Team' or 'Nobody'."""
        ...
    def send_system_message(self, text: str, player: Optional[Player] = ..., /) -> bool:
        """Post a line from the place into chat, drawn apart from what players say: to everyone, or only to player when you pass one. It goes through the same chat filter as player text, so a line with words that aren't allowed is masked or never shown. At most 500 characters, and at most 10 lines a minute per server. Returns false when it cannot be sent right now: over that limit, the player has no verified account yet, or this server has no session chat (a local test run)."""
        ...
    def send_to_players(self, players: list[Player], text: str, /) -> bool:
        """Post a private line from the place to a list of players, drawn apart from what players say, and seen by nobody else: a team message, a hint to the people holding the flag. It goes through the same chat filter as everything else, and Mantle stores it so that only those players can read it back. At most 10 players and 500 characters. It is one line however many players it is for, and counts as one of the server's 10 place lines a minute (Chat:SendSystemMessage shares that limit). Players with no verified account yet are skipped. Returns false when it cannot be sent right now. Example: Chat:SendToPlayers(Teams:GetPlayers('Red'), 'Your flag was taken!')"""
        ...
    def set_audience(self, player: Player, recipients: Optional[list[Player]] = ..., /) -> bool:
        """Make what a player types in chat reach only the players in recipients (and the player themselves): a team channel. Leave recipients out, or pass nil, to let everyone see their lines again. The chat filter still checks every line, a player's own blocks still hide them, and every line can still be reported; this only ever shows a line to fewer people. Mantle stores such a line so that only the sender and these recipients can read it back; it is still kept for moderation. At most 128 recipients. Returns false when the player is not verified yet or the list is too long. Lasts until the player leaves. Example: Chat:SetAudience(p, Teams:GetPlayers(p.Team)) -- p now talks to their team only"""
        ...
    def set_whisper_blocked(self, player: Player, blocked: bool, /) -> bool:
        """Stop (true) or allow (false) one player sending or receiving whispers, whatever the mode: for players who are dead, jailed or out of the round. A refused whisper tells the sender so. Returns false when the player is not verified yet. Lasts until the player leaves. Example: Chat:SetWhisperBlocked(deadPlayer, true)"""
        ...
    def set_whisper_mode(self, mode: str, /) -> None:
        """Choose who players may whisper to: 'Everyone' (the default), 'Team' (only to someone on their own team, see Teams) or 'Nobody' (whispers are off). Players whisper by typing /w name message in the chat box (the name in quotes if it has spaces). A whisper is filtered, rate-limited, reportable and hidden by blocks like any chat line, is shown only to the sender and the recipient, and is never given to Chat.MessagePosted. A refused whisper tells the sender so. Mantle stores a whisper so that only its two players can read it back; like all chat it is kept for moderation. Example: Chat:SetWhisperMode('Team')"""
        ...

Chat: _ChatService

class _CollisionService:
    """Collision layers: sort the objects of your place into named layers and decide which layers bump into which. Make a layer with CreateLayer (up to 8), put an object on it with entity.CollisionLayer = 'Name', then use SetLayerCollides to let two layers pass through each other. Every object starts on the 'Default' layer, which collides with everything, and players stay on Default: layers sort your props and never let a player through a wall (to let teammates pass through each other use Teams.TeammatesCollide). A layer also narrows World:Raycast, Spherecast, Blockcast, GetEntitiesInRadius and GetEntitiesInBox with params.Layers and params.ExcludeLayers. Server only."""
    def create_layer(self, name: str, /) -> None:
        """Make a collision layer. name is 1 to 32 characters (not case-sensitive), must be new and cannot be 'Default'. A place can have 8 layers. A new layer collides with every layer, itself included, until you change a pair. Example: Collision:CreateLayer('Debris')"""
        ...
    def get_layer_collides(self, a: str, b: str, /) -> bool:
        """Whether objects on layer a and objects on layer b bump into each other (true unless SetLayerCollides turned it off). 'Default' collides with everything."""
        ...
    def get_layers(self) -> list[str]:
        """The names of the layers you made ('Default' is always there and not listed)."""
        ...
    def remove_layer(self, name: str, /) -> None:
        """Delete a layer. Objects on it go back to the Default layer."""
        ...
    def set_layer_collides(self, a: str, b: str, collides: bool, /) -> None:
        """Say whether objects on layer a and objects on layer b bump into each other. false lets them pass through one another (a and b can be the same layer, to make its objects pass through each other). Both layers must exist, and 'Default' cannot be changed: it always collides with everything, so objects still meet players and the world. Example: Collision:SetLayerCollides('Debris', 'Debris', false)"""
        ...

Collision: _CollisionService

class _ConstraintsService:
    """Join physics entities with welds, hinges and ropes while the place runs (server only)."""
    def create(self, params: dict[str, Any], /) -> Constraint:
        """Join two physics entities, or one to the world, and return the joint. Type is 'weld' (holds them as they are), 'hinge' (EntityA turns about Axis, and only that) or 'rope' (the anchors stay within Length of each other). EntityA is the physics entity the joint holds and EntityB another; leave EntityB out to join EntityA to the world. AnchorA is where on EntityA the joint is, in its own space ((0, 0, 0), its middle, by default); AnchorB where on EntityB (a point in the world without it), by default wherever AnchorA is now. A hinge's Axis is a direction in EntityA's own space ((0, 0, 1) by default) and Limits is {min, max}, the degrees it may turn from where it is now, min at most 0 and max at least 0. A rope's Length is in cm, by default how far apart the anchors are now. Pull the joint harder than BreakForce newtons and it breaks for good (Broken fires). The two entities stop colliding with each other. Players see the entities move; nothing of the joint itself is sent. A place holds at most 200 joints, its own included."""
        ...
    def from_entity(self, entity: Entity, /) -> Optional[Constraint]:
        """The joint a place's Constraint entity made, as a Constraint, so a script can hear it break or remove it; nil for any other entity."""
        ...

Constraints: _ConstraintsService

class _CrowdsService:
    """Control the ambient crowds a place's CrowdSpawners walk: how many, on or off, moving or frozen (server only). Hear where their people go (client only)."""
    point_reached: Signal
    def get(self, spawner: Entity, /) -> Crowd:
        """The crowd of a CrowdSpawner the place placed. Every player sees what you set on it. Errors if the entity is not a CrowdSpawner (a clone of one has no crowd) or no longer exists."""
        ...

Crowds: _CrowdsService

class _DataStoreService:
    """Durable saved data for this place (server only)."""
    def get(self, key: str, /) -> Any:
        """This place's saved value for key, from the local cache (nil if never set, removed, or not loaded yet)."""
        ...
    def get_for_player(self, player: Player, key: str, /) -> Any:
        """This player's saved value for key, from the local cache."""
        ...
    def get_ordered_data_store(self, name: str, /) -> OrderedDataStore:
        """The ordered store with this name: whole-number values for this place, read back sorted, for leaderboards. A place can fill 32 stores."""
        ...
    def list_keys(self, prefix: Optional[str] = ..., cursor: Optional[str] = ..., /) -> dict[str, Any]:
        """One page (up to 100) of this place's saved keys that start with prefix, in key order, from the local cache like Get. Pass nextCursor back for the next page; it is nil on the last one."""
        ...
    def list_keys_for_player(self, player: Player, prefix: Optional[str] = ..., cursor: Optional[str] = ..., /) -> dict[str, Any]:
        """One page of this player's saved keys, like ListKeys."""
        ...
    def remove_key(self, key: str, /) -> None:
        """Delete key from this place's saved data. Get reads nil at once; removed from the backend asynchronously, like Set."""
        ...
    def remove_key_for_player(self, player: Player, key: str, /) -> None:
        """Delete key from this player's saved data, like RemoveKey."""
        ...
    def set(self, key: str, value: Any, /) -> None:
        """Save key for this place: a boolean, number, string, or table of those (nested up to 4 deep, 64 KB encoded). Applies to the local cache immediately; saved to the backend asynchronously."""
        ...
    def set_for_player(self, player: Player, key: str, value: Any, /) -> None:
        """Save key scoped to this player, with the same values as Set. Applies to the local cache immediately; saved to the backend asynchronously."""
        ...

DataStore: _DataStoreService

class _EffectsService:
    """Show Mantle's built-in visual effects anywhere in the world. Play makes a puff of particles (smoke, fire, sparks, dust, glow, explosion, impact, muzzle flash, blood, splash, confetti, heal), Beam draws a short tracer line between two points, and Decal leaves a mark such as a bullet hole or a scorch on a surface. All are cosmetic: they do not hurt anyone or change the world. Example: Effects:Play('Explosion', Vector3.new(0, 5, 0))"""
    def beam(self, from_: Vector3, to: Vector3, params: Optional[dict[str, Any]] = ..., /) -> None:
        """Draw a short-lived glowing beam (a tracer or a zap) from one point to another, then it fades. params: Color ('#RRGGBB', the beam's own look by default), Width (how thick, 0.5 to 100 cm, 4 by default) and Duration (how long it shows, 0 to 5 seconds, 0.15 by default); numbers out of range are clamped. A line longer than 1000 m is cut short. A client or a server holds at most 16 beams at once; a new one past that replaces the oldest. Use it for tracers, lightning bolts, lasers or showing a link between two things. Example: Effects:Beam(Vector3.new(0, 2, 0), Vector3.new(0, 2, 40), {Color = '#FF3030', Duration = 0.2}). From a client script only this player sees it; from a server script every player does, but not one who joins later. A dedicated server shows nothing. At most 10 effect calls a frame per script (Play, Beam and Decal count together), and a server script at most 10 a second (bursts of 20); past either the script stops."""
        ...
    def decal(self, position: Vector3, normal: Vector3, params: Optional[dict[str, Any]] = ..., /) -> None:
        """Leave a mark on a surface at position, facing along normal (the direction the surface faces, such as a raycast hit's normal; it does not need to be exactly length 1). params.Style picks the mark: 'BulletHole' (a dark hole with a scuffed ring, the default), 'Scorch' (a ragged soot patch) or 'Splat' (a paint-like blob with droplets, blue by default). Color ('#RRGGBB') tints it, Size is how wide it is (2 to 500 cm, 30 by default) and Lifetime how many seconds it stays before fading away (0.1 to 60, 20 by default); numbers out of range are clamped. A player's machine keeps at most 64 script decals at once (fewer on a low Effects quality setting); a new one past that removes the oldest. Example: local hit = World:Raycast(origin, direction); if hit then Effects:Decal(hit.Position, hit.Normal, {Style = 'Scorch', Size = 80}) end. From a client script only this player sees it; from a server script every player does, but not one who joins later. A dedicated server shows nothing. At most 10 effect calls a frame per script (Play, Beam and Decal count together), and a server script at most 10 a second (bursts of 20); past either the script stops."""
        ...
    def play(self, archetype: str, position: Vector3, params: Optional[dict[str, Any]] = ..., /) -> None:
        """Show a built-in particle effect at position for a moment, then it is gone. archetype is one of (any case; 'Muzzle Flash' and 'muzzle_flash' work too): 'Smoke' (a soft grey column), 'Fire' (flames), 'Sparks' (a spray of hot sparks), 'Dust' (a slow brown puff), 'Glow' (a soft light mist), 'Explosion' (a fireball with smoke and sparks), 'Impact' (sparks and dust where something hits), 'MuzzleFlash' (the flash at a gun's mouth), 'Blood' (a small stylised red spray for a hit), 'Splash' (water droplets), 'Confetti' (a party burst in three colours) or 'Heal' (green sparkles rising). The bursts (Explosion, Impact, MuzzleFlash, Blood, Splash, Confetti) play for a fraction of a second; the others for params.Duration seconds (1 by default, 0 to 10). params tune the look, each the archetype's own by default: Color ('#RRGGBB'; on Confetti it is the first of the three colours), Size (particle size, 1 to 1000 cm; a bigger Size makes a bigger explosion all through), Rate (particles a second, 0 to 200), Speed (0 to 5000 cm/s), Lifetime (how long each particle lives, 0.05 to 10 seconds) and Duration; numbers out of range are clamped. Particles leave along +Z, or along Rotation's up (a Vector3 of pitch, yaw and roll). On a low Effects quality setting a player sees the main layer only. Example: Effects:Play('Explosion', Vector3.new(0, 5, 0), {Size = 250, Color = '#44AAFF'}). From a client script only this player sees it; from a server script every player does, but not one who joins later. A dedicated server shows nothing. At most 10 effect calls a frame per script (Play, Beam and Decal count together), and a server script at most 10 a second (bursts of 20); past either the script stops."""
        ...

Effects: _EffectsService

class _EnvironmentService:
    """The time of day, the weather, the sky, the fog and the screen's colour, the same for everyone on the server. Everyone can read them; server scripts can change them, and every player sees the change."""
    bloom: Optional[float]
    clock_time: float
    cloud_cover: float
    color_grading: Optional[str]
    color_grading_intensity: float
    color_tint: str
    contrast: float
    day_length: float
    exposure_bias: float
    fog_color: Optional[str]
    fog_density: float
    is_night: bool
    rain_intensity: float
    saturation: float
    stars_visible: Optional[bool]
    sun_intensity_scale: float
    time_of_day: float
    time_scale: float
    vignette: Optional[float]
    weather: str
    lightning_strike: Signal
    property_changed: Signal
    weather_changed: Signal
    def clear_local_look(self) -> None:
        """Take away this player's own look from SetLocalLook (client only): the picture follows the server's values again."""
        ...
    def freeze_clock(self) -> None:
        """Stop the clock where it is: the same as setting TimeScale to 0. Set TimeScale to 1 to start it again."""
        ...
    def resume_natural_weather(self, blendSeconds: Optional[float] = ..., /) -> None:
        """Let the weather change on its own again after SetWeather, blending back over blendSeconds (0 to 600, 90 when left out)."""
        ...
    def set_cloud_cover(self, cover: Optional[float] = ..., /) -> None:
        """Cover this much of the sky with cloud, from 0 to 1, whatever the weather; nil gives the weather its own clouds back. The sun dims and the stars fade under heavy cover."""
        ...
    def set_local_look(self, look: dict[str, Any], /) -> None:
        """Change how the picture looks for this player only (client only). look holds any of ExposureBias (-4 to 4), Saturation (0 to 2), Contrast (0 to 2), Vignette (0 to 1), ColorTint ('#RRGGBB'), Bloom (0 to 4), ColorGrading ('Warm', 'Cool', 'Noir', 'Sepia', 'Vivid' or 'Faded') and ColorGradingIntensity (0 to 1), taken as the properties of the same names take them. What look sets wins over the server's values; what it leaves out follows them. Each call replaces the last look whole, and the look goes when this script stops. Time and weather stay the same for everyone. From the server, send one player a look with a Remote's FireClient and call this in its OnClientEvent."""
        ...
    def set_weather(self, weather: str, blendSeconds: Optional[float] = ..., /) -> None:
        """Hold the weather at Clear, PartlyCloudy, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, DustCalm or DustStorm until ResumeNaturalWeather, changing to it over blendSeconds (0 to 600, 90 when left out). A Storm brings lightning. The snow and dust weathers (LightSnow, Snow, Blizzard, DustCalm, DustStorm) never come on their own."""
        ...

Environment: _EnvironmentService

class _EventsService:
    """Named events that let scripts on the same side talk to each other, in any language."""
    def get(self, name: str, /) -> Signal:
        """The event with this name; connect to it or Fire it. Doesn't cross the network (use Net:Remote for that)."""
        ...

Events: _EventsService

class _ExplosionsService:
    """Blow things up from a server script: damage that fades with distance, a shove for loose props and players, and a visible, audible blast. The damage goes through the same path weapons use, so armour, the shield, team friendly fire and the Damaged and Died signals all apply (a death credits the attacker). Everything happens on the server; there is nothing for a client script here."""
    def create(self, position: Vector3, options: dict[str, Any] = ..., /) -> float:
        """Explode at position (centimetres). Everything alive or loose within Radius is hurt and pushed, and the call returns how many things were hurt. options: Radius (centimetres, 500 by default; 10 to 5000), Damage (at the centre, 100 by default; 0 to 1000000), InnerRadius (full damage out to here, 0 by default), Falloff (how damage drops from InnerRadius to 0 at Radius: 0 is full damage everywhere, 1 drops in a straight line, the default, and 2 or more drops faster), DamageType (the tag Damaged and Died report, 'Explosion' by default; your own tag works), Attacker (the Player who gets the kill, and whose team friendly fire protects; their own blast hurts them too, which makes rocket jumps; leave it out for the world), Impulse (the speed in centimetres per second a loose physics prop at the centre is given, fading with distance; 800 by default, 0 for none; the weight of the prop does not matter), Ragdoll (true by default: a player in the blast is thrown down and gets up again after RagdollSeconds, 2 by default; with false only a heavy hit throws them), LineOfSight (true by default: nothing behind a wall or a hill is touched; false makes the blast reach through walls), Effect (the look of the blast: 'None', 'Small', 'Medium' (the default), 'Large' or 'Dirt') and Sound (true by default). At most 30 explosions a second. A player riding in a car is shielded by its body. NPCs take the damage like players do. Example: Explosions:Create(barrel.Position, {Radius = 600, Damage = 80, Attacker = player})"""
        ...

Explosions: _ExplosionsService

class _HttpService:
    """Outbound HTTP from server scripts (server only): call your own backend, a webhook, any public web service. Players' games never make these requests. Private and internal addresses are always refused."""
    def is_available(self) -> bool:
        """Whether outbound HTTP works on this server: false when the server's operator turned it off, so a place can do without it quietly."""
        ...
    def json_decode(self, text: str, /) -> Any:
        """JSON text as a value: objects become tables with string keys, arrays become lists, null becomes nil. Raises an error when the text isn't valid JSON."""
        ...
    def json_encode(self, value: Any, /) -> str:
        """A value as JSON text. Accepts nil, booleans, finite numbers, strings, and tables (a list becomes an array, anything else an object), nested up to 32 deep. Raises an error for anything else. Luau has no JSON of its own; JS and Python have JSON and json."""
        ...
    def request_async(self, options: dict[str, Any], /) -> Signal:
        """Send an HTTP request and get a Signal that fires once with the result: call :Wait() on it straight away, or :Once(handler). options.url is required (http:// or https://, port 80, 443 or 1024 and up); method is GET (default), POST, PUT, PATCH or DELETE; headers is a table of strings; body is text (UTF-8) or bodyBase64 is binary (up to 2 MiB, not with GET); timeout is 1 to 60 seconds (default 30); followRedirects (default true) follows up to 5 redirects, each one re-checked; responseType is 'text' (default) or 'base64'. Option keys can be spelled any way (followRedirects, follow_redirects); a key that is not on this list raises bad_request. The result is {ok, status?, headers?, body?, url?, error?, code?}: ok is true only for a 2xx answer, a 404 or 500 still has its status and body with code 'http_status', and a request that failed after sending has ok = false and a code such as dns_failed, blocked_address, connect_failed, tls_failed, timeout, too_many_redirects, redirect_blocked or response_too_large. A request refused before sending raises an error that starts with its code (bad_url, blocked_address, blocked_port, bad_header, bad_request, body_too_large, too_many_requests, rate_limited, bandwidth_limited or disabled): catch it with pcall (Luau) or try (JS, Python). A server may run 16 requests at once with 128 more waiting, about 600 a minute, 16 MiB sent and 128 MiB received a minute. Mantle logs the host, port, status and sizes of each request, never its path, headers or bodies. Keep API keys out of your script text: a server script's source is private to its owner, but keys you can rotate belong in Secrets (Secrets:Get)."""
        ...

Http: _HttpService

class _InputService:
    """The local player's keys, mouse buttons and gamepad buttons, and their sticks, triggers and mouse wheel as axes (client only). Mantle's own controls (the menu, chat, voice, movement, the Backpack and Tool keys) can never be bound or read."""
    def bind_action(self, name: str, keys: list[str], callback: Callable[..., Any], /) -> None:
        """Call callback when the player presses one of keys (phase 'Began') and when they let go (phase 'Ended'); key is the name of the key that did it. keys lists 1 to 4 names such as {'Q', 'ButtonY'}: letters, '0' to '9', 'F1' to 'F10', 'Tab', 'MouseLeft', 'MouseRight', 'MouseMiddle', 'MouseButton4', 'MouseButton5', 'ButtonA', 'ButtonB', 'ButtonX', 'ButtonY', 'ButtonL1' and 'ButtonR1'. A key Mantle uses for its own controls is an error; GetBindableKeys lists the free ones. Binding a name again replaces that action. The key still does anything Mantle does with it (F and ButtonY also enter and leave cars). Nothing is heard while Mantle's menu or chat is open."""
        ...
    def bind_axis(self, name: str, axes: list[str], callback: Callable[..., Any], /) -> None:
        """Call callback when one of axes moves, at most once a frame per axis; axis is the name of the one that moved. axes lists 1 to 4 of 'LeftStickX', 'LeftStickY', 'RightStickX', 'RightStickY' (-1 to 1, right and up positive, 0 inside the stick's dead zone), 'LeftTrigger', 'RightTrigger' (0 to 1) and 'MouseWheel' (the notches scrolled that frame, up positive; heard only on frames it moves). A stick or trigger whose movement is one of Mantle's own controls is an error; GetBindableAxes lists the free ones. A trigger Mantle uses as a button (fire, aim) can still be read here: the button keeps working, and no script can bind it as a key. Binding a name again replaces that binding; axis bindings and BindAction's actions have separate names. The control still does anything Mantle does with it (the mouse wheel also steps Mantle's camera in and out). Everything reads 0 while Mantle's menu or chat is open."""
        ...
    def get_axis(self, axis: str, /) -> float:
        """Where axis is right now, by the names BindAxis takes, as its callbacks last heard it: 'MouseWheel' reads the notches of the last frame. 0 while Mantle's menu or chat is open. An axis that is part of Mantle's own controls is an error."""
        ...
    def get_bindable_axes(self) -> list[str]:
        """Every axis name this place can bind: the names BindAxis takes, minus the sticks and triggers whose movement Mantle uses for its own controls. A trigger Mantle uses only as a button (fire, aim) is listed."""
        ...
    def get_bindable_keys(self) -> list[str]:
        """Every key name this place can bind: the names BindAction takes, minus Mantle's own controls."""
        ...
    def is_key_down(self, key: str, /) -> bool:
        """Whether the player is holding key down right now, by the same names BindAction takes. Always false while Mantle's menu or chat is open. A key Mantle uses for its own controls is an error."""
        ...
    def unbind_action(self, name: str, /) -> bool:
        """Remove the action this script bound under name. Returns false when it has none by that name. A script's actions also go away by themselves when it stops."""
        ...
    def unbind_axis(self, name: str, /) -> bool:
        """Remove the axis binding this script made under name. Returns false when it has none by that name. A script's axis bindings also go away by themselves when it stops."""
        ...

Input: _InputService

class _InteractionService:
    """Let players walk up to an object and act on it with a key press, and hear when they do. A place can mark an object interactable itself; here a script makes one, changes what players can do to it, and reacts. The server decides everything: change things and listen to Interacted on the server. The client events are for the local player and only for effects such as a sound or a glow."""
    cancelled: Signal
    interacted: Signal
    progress: Signal
    selected: Signal
    started: Signal
    unselected: Signal
    def remove(self, entity: Entity, /) -> None:
        """Make the entity not interactable any more, for everyone: no prompt, no key. SetOptions makes it interactable again with the options you give."""
        ...
    def set_display(self, entity: Entity, display: dict[str, Any], /) -> None:
        """Change how the entity's prompt looks, for everyone. Only the fields you give change. Style is 'Default' (the key and text of each option, stacked), 'Balanced' (a panel with the Title on top and the options under it) or 'Informative' (like Balanced, with the Description beside the options and an accent line); Balanced and Informative need a Title and show as Default without one. Title is up to 80 characters, Description up to 200, and AccentColor is '#RRGGBB'."""
        ...
    def set_enabled(self, entity: Entity, action: str, enabled: bool, part: str = ..., /) -> None:
        """Turn one option of the entity off or back on, for everyone. action is 'Primary' or 'Secondary', the input the option uses. Turning an option on also ends its cooldown early. An option with AfterUse 'OneShot' that was used stays used. Pass a part's id for an object with several parts; leave it out for the object as a whole."""
        ...
    def set_options(self, entity: Entity, options: list[dict[str, Any]], part: str = ..., /) -> None:
        """Make the entity interactable and set what players can do to it, for everyone. options is a list of at most two tables, one for each input: Key ('Primary' by default, or 'Secondary'), Text (what the prompt says beside the key, such as 'Open door'; leave it out to show the key alone), HoldSeconds (hold the key this long; 0 is a tap), PressCount (mash the key this many times instead; wins over HoldSeconds), PressDrainPerSecond (how fast a mash's progress drains, 0 to 10), AfterUse ('None', 'DisableUntilEnabled', 'Cooldown' or 'OneShot') and CooldownSeconds (needed above 0 for 'Cooldown'). Pass a part's id to change only that part of an object the place gave several parts; leave it out for the object as a whole. An empty list leaves the object with nothing to do. Example: Interaction:SetOptions(door, {{Key = 'Primary', Text = 'Open door'}})"""
        ...
    def set_range(self, entity: Entity, range: float, /) -> None:
        """Set how close a player must stand to the entity, in centimetres, for the prompt to show and the key to work. Clamped to 50..5000. A place's default is 250."""
        ...
    def start_cooldown(self, entity: Entity, action: str, seconds: float, part: str = ..., /) -> None:
        """Make one option of the entity unusable for this many seconds (up to 3600), for everyone, as if it had just been used with AfterUse 'Cooldown'. action is 'Primary' or 'Secondary'. SetEnabled ends it early. Pass a part's id for an object with several parts; leave it out for the object as a whole."""
        ...

Interaction: _InteractionService

class _LeaderboardService:
    """The built-in leaderboard panel: players press Tab (or pick Leaderboard in the game menu) to see the top 10 of one of this place's ordered stores, with each player's name and picture (server only)."""
    def clear(self) -> None:
        """Take the leaderboard panel down: players no longer see it until Configure shows a board again. The store keeps its entries."""
        ...
    def configure(self, store: str, options: Optional[dict[str, Any]] = ..., /) -> None:
        """Show the ordered store with this name (as DataStore:GetOrderedDataStore takes it) on the leaderboard panel: its top 10, highest first, or lowest first when options.ascending is true. Key each entry by the player's UserId and the panel shows their name and picture; any other key shows as it is. options.title (up to 32 characters, 'Leaderboard' when left out) heads the panel and options.valueLabel (up to 16, 'Score' when left out) heads the value column. The panel reads the store now and every 30 seconds while anyone is playing, so it trails the store by up to 30 seconds. Call it again to switch boards; one board shows at a time."""
        ...

Leaderboard: _LeaderboardService

class _LightsService:
    """Spawn lights at runtime (server only). Change any light through its Entity's Light members."""
    def spawn(self, kind: str, position: Vector3, props: Optional[dict[str, Any]] = ..., /) -> Entity:
        """Spawn a light every player sees and return it as an Entity. kind is 'Point', 'Spot' or 'Directional'. props sets how it starts: Color ('#RRGGBB', white by default), Intensity (candelas, 8 by default; lux for a directional light, 10 by default), Range (cm, 1000 by default), InnerCone and OuterCone (degrees, 0 and 44 by default), Enabled, CastShadow (off by default: shadows are costly) and Rotation (a spot light points straight down and a directional light down at 45 degrees unless you give one). Numbers out of range are clamped. At most 64 spawned lights exist at once in a place; Destroy one to make room."""
        ...

Lights: _LightsService

class _MatchService:
    """A small round lifecycle you drive yourself: start a round, end it (by calling EndRound or when its timer runs out), and read the round number and clock. The server runs it; State, Round and TimeRemaining are readable from client scripts too, for a HUD. It does not pick winners, move players or restart anything by itself: connect to RoundStarted and RoundEnded and do what your game needs. The match is Waiting before a round and between rounds, InProgress during one, and Ended after EndMatch."""
    round: float
    state: str
    time_remaining: Optional[float]
    round_ended: Signal
    round_started: Signal
    state_changed: Signal
    def end_match(self, winner: Optional[str] = ..., /) -> bool:
        """End the whole match. A running round ends first (RoundEnded fires with reason 'MatchEnded'), then State becomes Ended. Returns false when the match has already ended."""
        ...
    def end_round(self, winner: Optional[str] = ..., /) -> bool:
        """End the running round: RoundEnded fires with winner (a team name, a player's name, anything up to 64 characters, or nil for none) and reason 'Ended', and State goes back to Waiting. Returns false when no round is running."""
        ...
    def reset(self) -> None:
        """Start over: a running round ends first (reason 'MatchEnded'), then State is Waiting and Round is 0. Teams and players are not touched."""
        ...
    def start_round(self, seconds: Optional[float] = ..., /) -> bool:
        """Begin the next round: Round goes up by one, State becomes InProgress, and RoundStarted fires. With seconds (up to 86400) the round ends by itself when the time is up, with reason 'Time' and no winner; without, it runs until EndRound. Returns false, changing nothing, while a round is already running or after EndMatch (call Reset first)."""
        ...

Match: _MatchService

class _NPCsService:
    """Spawn characters the server walks around the place and animates (server only)."""
    def from_entity(self, entity: Entity, /) -> Optional[NPC]:
        """The NPC an Entity is (a World:Raycast hit, or NPC:GetEntity()), or nil when it is not a live NPC."""
        ...
    def spawn(self, position: Vector3, look: Optional[dict[str, Any]] = ..., yaw: Optional[float] = ..., /) -> NPC:
        """Spawn a character standing at position (its feet), facing yaw degrees (0 faces +X), and return it as an NPC. Every player sees it. look.AvatarItemId optionally selects a published avatar body; omit it to spawn an NPC with no rendered body. SkinTone and EyeColor are avatar palette ids ('tone_05'), Height is a percent from 95 to 105. A place holds at most 50 NPCs at once; Destroy one to make room."""
        ...

NPCs: _NPCsService

class _NetService:
    """Send messages between the server and players."""
    def remote(self, name: str, /) -> Remote:
        """Get the remote with this name (the same name on server and client refers to the same remote)."""
        ...

Net: _NetService

class _PlayersService:
    """Everyone in the session."""
    local_player: Player
    player_added: Signal
    player_removing: Signal
    def get_player_from_character(self, character: Entity, /) -> Optional[Player]:
        """The player whose character this entity is right now, or nil."""
        ...
    def get_players(self) -> list[Player]:
        """Players currently in the session."""
        ...
    def on_player(self, handler: Callable[..., Any], /) -> Connection:
        """Call handler for every player: those already here and everyone who joins later. The usual way to set players up."""
        ...

Players: _PlayersService

class _ProjectilesService:
    """Fire projectiles from a server script: a ball, rocket, arrow or cube that flies under gravity, hits what is in its way, hurts it, and can explode. Each flies on the server and shows on every player's screen. Fire returns an id; Hit and Expired say how it ended. A projectile never hits the player or NPC that fired it, and a hit goes through the same damage path weapons use (armour, shield, friendly fire, Damaged and Died). Weapon damage, fire rate and ammo stay the game's: this is for your own gadgets, traps and turrets. At most 64 projectiles fly at once."""
    expired: Signal
    hit: Signal
    def cancel(self, id: float, /) -> bool:
        """Take a projectile out of the air: it vanishes without Hit or Expired, and without exploding. Returns false when it has already ended."""
        ...
    def fire(self, options: dict[str, Any], /) -> float:
        """Launch a projectile and return its id. options: Position (where it starts) and either Velocity (a Vector3 in centimetres per second: the direction and speed) or Target (a point to hit; it is aimed at that point at Speed, 3000 by default, arcing for gravity; a target out of reach gets the flattest shot it can); GravityScale (1 by default; 0 flies dead straight, up to 10), Lifetime (seconds before it expires, 5 by default; 0.1 to 30), Look ('Ball' the default, 'Rocket', 'Arrow', 'Cube' or 'Invisible'), Color ('#RRGGBB'), Scale (0.1 to 10), Damage (to what it hits directly: 25 by default, or 0 when it has an Explosion; 0 to 1000000), DamageType ('Bullet' by default), Explosion (a table like Explosions:Create's options: it goes off where the projectile hits, and replaces the direct damage unless you also give Damage), ExplodeOnExpire (true: it goes off where it is when the lifetime ends, like a grenade; it needs an Explosion table, otherwise it explodes with the defaults), Tag (a name up to 64 characters Hit and Expired give back, so a handler knows which of its projectiles ended) and Attacker (the Player who gets the credit, whose team friendly fire protects, and who it never hits). Example: Projectiles:Fire({Position = muzzle, Velocity = aim * 2500, Look = 'Rocket', Attacker = player, Explosion = {Radius = 400, Damage = 90}})"""
        ...

Projectiles: _ProjectilesService

class _RespawnService:
    """How a death becomes a respawn. By default a dead player respawns on their own after a short wait at a spawn point the place picks. Change the wait, or turn automatic respawning off to run rounds where the dead stay down until you bring them back with Player:Respawn. The server decides; there is nothing for a client script here. Player.Respawned fires when a player gets a fresh character."""
    auto_respawn: bool
    delay: float

Respawn: _RespawnService

class _ScriptService:
    """Facts about the running script."""
    is_client: bool
    is_server: bool
    name: str
    package: str
    path: str
    points: list[Entity]
    settings: dict[str, Any]
    side: str

Script: _ScriptService

class _SecretsService:
    """Read the API keys and webhook URLs you set for this place, so they never sit in your scripts. Server scripts on a hosted server only."""
    def get(self, name: str, /) -> Optional[str]:
        """The value you set for the secret called name (in the editor plugin's place settings), or nil when this place has none by that name or secrets are not available here (see Secrets:IsAvailable). Names are upper-case letters, digits and underscores, starting with a letter: DISCORD_WEBHOOK. The server loads them once, before any script starts, so a value is there from your script's first line; one you change in the plugin reaches servers that start after the change. Never send a value to a player (in a remote event, state, an attribute, chat or UI): anything a client gets, its player can read. Use it to call a service from the server, and keep keys you can rotate. When you test locally with -MantlePlace, values come from secrets.local.json beside the place file: {'DISCORD_WEBHOOK': 'https://...'}. Keep that file out of version control. Example: local hook = Secrets:Get('DISCORD_WEBHOOK') if not hook then print('set DISCORD_WEBHOOK for this place') end"""
        ...
    def is_available(self) -> bool:
        """Whether this server loaded the place's secrets, so Secrets:Get answers from them. False in standalone or listen play of a published place, on a community server, when the server could not reach Mantle, and in a place whose scriptApi is older than 1.47 (the editor records it from your scripts); Secrets:Get then reads nil for every name. True with no secrets set. Example: if not Secrets:IsAvailable() then print('no secrets here: posting to Discord is off') end"""
        ...

Secrets: _SecretsService

class _ServerService:
    """Facts about the server this session runs on (server only)."""
    private_server_id: Optional[str]
    private_server_owner_id: Optional[str]

Server: _ServerService

class _SoundsService:
    """Play the sounds uploaded with this place: effects and loops, flat or placed in the world, and one music track per player."""
    def play(self, sound: str, params: Optional[dict[str, Any]] = ..., /) -> SoundHandle:
        """Play one of the sounds uploaded with this place (a sound emitter's Entity.SoundId, or its id in the place's assets). From a client script only this player hears it; from a server script every player does, or only params.Player. Give Position or AttachTo to place it in the world, where it fades to silence at Radius cm (4000 by default); without either it plays flat, like interface sounds. Volume 0 to 2 (1), Pitch 0.5 to 2 (1), Looped (false), FadeIn seconds (0). A sound that has not finished downloading starts when it arrives. Returns a SoundHandle."""
        ...
    def play_music(self, sound: str, params: Optional[dict[str, Any]] = ..., /) -> None:
        """Play one of the place's sounds as music: from a client script this player's, from a server script every player's, or only params.Player's. Each player has one music track: a new one crossfades from the old over FadeIn seconds (1 by default), and playing the track already playing only changes its Volume (0 to 2). Loops unless Looped is false."""
        ...
    def stop_music(self, params: Optional[dict[str, Any]] = ..., /) -> None:
        """Stop the music, fading it out over params.FadeOut seconds (0, the default, stops it at once). From a server script: every player's music, or only params.Player's."""
        ...

Sounds: _SoundsService

class _StateService:
    """Shared game values the server sets and everyone can read (score, round, phase)."""
    changed: Signal
    def get(self, key: str, /) -> Any:
        """The current value, or nil."""
        ...
    def observe(self, key: str, handler: Callable[..., Any], /) -> Connection:
        """Call handler with the value now and every time it changes. The easy way to keep UI in sync."""
        ...
    def set(self, key: str, value: Any, /) -> None:
        """Set a value for everyone. Numbers, strings, booleans, Vector3 and tables work."""
        ...

State: _StateService

class _StorageService:
    """The library items (vehicles, weapons and script packages) this place stores, by the names its storage gives them."""
    def get(self, name: str, /) -> str:
        """The library item id (itm_u...) of the entry this place's storage declares under name. The name is case-sensitive. Pass the id to Vehicles:Spawn (a stored vehicle) or Backpack:Grant (a stored weapon): they accept only an id the place stores. Errors when the place's storage has no entry with that name."""
        ...

Storage: _StorageService

class _TeamsService:
    """Teams for a place: define them, put players on them, and decide whether teammates can hurt each other. The server decides everything; which team a player is on replicates, so client scripts and HUDs can read it. A team is named by its name (not case-sensitive) and has a colour. Friendly fire is off by default: once a player is on a team, damage from a teammate is ignored until Teams.FriendlyFire is set to true."""
    friendly_fire: bool
    teammates_collide: bool
    player_team_changed: Signal
    def create(self, name: str, color: Optional[str] = ..., /) -> None:
        """Define a team. name is 1 to 32 characters and must be new. color is '#RRGGBB' (white when left out), for a HUD or a nameplate to use. Up to 16 teams. Example: Teams:Create('Red', '#E53935')"""
        ...
    def get_color(self, name: str, /) -> Optional[str]:
        """The team's colour as '#RRGGBB', or nil when there is no such team."""
        ...
    def get_players(self, name: str, /) -> list[Player]:
        """The players on the team right now (an empty list for a team nobody is on, or one that does not exist)."""
        ...
    def list(self) -> list[str]:
        """The names of the teams, in the order they were created."""
        ...
    def remove(self, name: str, /) -> None:
        """Delete a team. Its players are taken off it first, so PlayerTeamChanged fires for each."""
        ...

Teams: _TeamsService

class _TimeService:
    """Clocks for scripts."""
    clock: float
    now: float

Time: _TimeService

class _UIService:
    """The local player's on-screen UI: a simple HUD line/toast, and the retained node tree behind real widgets."""
    captions: bool
    high_contrast: bool
    locale: str
    reduced_motion: bool
    text_scale: float
    def action_label(self, action: str, /) -> str:
        """The player's remapped label for an action name (e.g. the bound key), or the action name itself if unset."""
        ...
    def create(self, kind: str, /) -> Optional[UiNode]:
        """Create a new node: 'frame', 'text', 'image', 'button', 'toggle', 'list', 'grid', 'scroll' or 'prompt'. Nil means the per-script node budget (256) is exhausted."""
        ...
    def format_number(self, value: float, /) -> str:
        """Format a number for display using the player's current locale."""
        ...
    def open_document(self, path: str, mode: Optional[str] = ..., /) -> UiDocument:
        """Open one of the place's UI documents (a .rml file in its UI/ folder) on this player's screen. Mode is 'modal' (the default: gameplay input stops and Back closes it), 'overlay' (play goes on; the mouse works on its controls) or 'hud' (drawn only, never takes input). An unknown path or mode is an error, and so is opening on a server. Example: local shop = UI:OpenDocument('UI/shop.rml')"""
        ...
    def set_cursor_visible(self, visible: Optional[bool] = ..., /) -> None:
        """Force the mouse cursor on or off regardless of whether an interactive node is visible right now. Nil goes back to automatic: shown only while a modal UI (see SetInputMode) has a Button, a Toggle, or a Prompt with an action visible and enabled; an overlay UI shows no cursor, so the mouse keeps turning the camera. SetCursorVisible(true) shows it in either mode. A gamepad-only panel can call SetCursorVisible(false) to keep working through focus/Accept without ever drawing a cursor."""
        ...
    def set_input_mode(self, mode: str, /) -> None:
        """Choose what an interactive node (a Button, a Toggle, or a Prompt with an action) does to gameplay input while it is on screen. 'overlay' (the default): no cursor shows, so the mouse keeps turning the camera, and the keyboard and gamepad stay with the game, so the player keeps moving; press the UI's buttons with a key binding in the script, or call UI:SetCursorVisible(true) to show a cursor. 'modal': the cursor shows, the UI takes keyboard and gamepad focus, and movement, attack, look and gamepad input stop until the last interactive node is gone, then come back exactly as they were; Esc still opens the game menu. One setting for all of this place's retained UI on the player's screen; a place's scripts share it, and it resets when they all stop. Any other mode is an error. Example: UI:SetInputMode('modal')"""
        ...
    def set_text(self, slot: str, text: str, /) -> None:
        """Show text in a named HUD line (same slot replaces the previous text; empty text clears it)."""
        ...
    def toast(self, text: str, seconds: Optional[float] = ..., /) -> None:
        """Pop up a short message."""
        ...

UI: _UIService

class _VehiclesService:
    """Spawn and seat cars: the vehicles in the place's storage (server only)."""
    boost_started: Signal
    boost_stopped: Signal
    damaged: Signal
    destroyed: Signal
    fuel_changed: Signal
    fuel_empty: Signal
    ready: Signal
    def damage(self, vehicle: Entity, amount: float, /) -> float:
        """Take amount of health off the car (never below 0) and return the health it has left. Fires Vehicles.Damaged, and Vehicles.Destroyed if it reaches 0. Turn health on first with SetHealthEnabled."""
        ...
    def eject(self, player: Player, /) -> bool:
        """Put the player back on foot beside the car they are driving. Returns false when they are not driving, the car is still moving, or there is nowhere beside it to stand."""
        ...
    def get_boost(self, vehicle: Entity, /) -> float:
        """The boost the car has left, from 0 to MaxBoost."""
        ...
    def get_boost_drain(self, vehicle: Entity, /) -> float:
        """How much boost is used each second while the driver boosts (25 unless you change it)."""
        ...
    def get_boost_regen(self, vehicle: Entity, /) -> float:
        """How much boost comes back each second while the driver is not boosting (10 unless you change it)."""
        ...
    def get_boost_strength(self, vehicle: Entity, /) -> float:
        """How hard the boost pushes, from 0 (nothing) to 1 (the hardest it can; about 18 m/s of extra speed every second). 0.5 unless you change it."""
        ...
    def get_fuel(self, vehicle: Entity, /) -> float:
        """The fuel in the tank, from 0 to MaxFuel."""
        ...
    def get_fuel_consumption(self, vehicle: Entity, /) -> float:
        """How much fuel the car burns each second while it is driven and moving (1 unless you change it)."""
        ...
    def get_health(self, vehicle: Entity, /) -> float:
        """The car's health, from 0 to MaxHealth. It only changes while health is on."""
        ...
    def get_max_boost(self, vehicle: Entity, /) -> float:
        """The most boost the car can store (100 unless you change it)."""
        ...
    def get_max_fuel(self, vehicle: Entity, /) -> float:
        """The tank's size (100 unless you change it)."""
        ...
    def get_max_health(self, vehicle: Entity, /) -> float:
        """The most health the car can have (100 unless you change it)."""
        ...
    def get_speed(self, vehicle: Entity, /) -> float:
        """The car's speed in centimetres per second, as the simulation reports it. Errors if the entity is not a car or no longer exists."""
        ...
    def is_boost_enabled(self, vehicle: Entity, /) -> bool:
        """Whether the car has a boost (SetBoostEnabled)."""
        ...
    def is_boosting(self, vehicle: Entity, /) -> bool:
        """Whether the car's driver is boosting right now."""
        ...
    def is_destroyed(self, vehicle: Entity, /) -> bool:
        """Whether the car has health on and none left."""
        ...
    def is_fuel_empty(self, vehicle: Entity, /) -> bool:
        """Whether the car has a fuel tank and it is empty."""
        ...
    def is_fuel_enabled(self, vehicle: Entity, /) -> bool:
        """Whether the car has a fuel tank (SetFuelEnabled)."""
        ...
    def is_health_enabled(self, vehicle: Entity, /) -> bool:
        """Whether the car has health (SetHealthEnabled)."""
        ...
    def is_locked(self, vehicle: Entity, /) -> bool:
        """Whether the car is locked. A locked car opens only for the last player who drove it: anyone else's vehicle key rattles the door, and holding the key breaks in -- the window on that side breaks, the car unlocks, and it must be hotwired before it starts. The lock stays when the driver gets out, and its driver can also lock and unlock it from the seat (L / D-pad left). Errors if the entity is not a car or no longer exists."""
        ...
    def lock(self, vehicle: Entity, /) -> bool:
        """Lock the car. A locked car opens only for the last player who drove it: anyone else's vehicle key rattles the door, and holding the key breaks in -- the window on that side breaks, the car unlocks, and it must be hotwired before it starts. The lock stays when the driver gets out, and its driver can also lock and unlock it from the seat (L / D-pad left). Returns false when the entity is not a car."""
        ...
    def refuel(self, vehicle: Entity, amount: float = ..., /) -> float:
        """Put amount of fuel in the tank (never above MaxFuel), or fill it when you leave amount out. Returns the fuel in the tank now. Example: Vehicles:Refuel(car, 20)"""
        ...
    def repair(self, vehicle: Entity, amount: float = ..., /) -> float:
        """Give the car amount of health back (never above MaxHealth), or all of it when you leave amount out. Returns the health it has now. A destroyed car drives again once it has any health."""
        ...
    def seat(self, vehicle: Entity, player: Player, /) -> bool:
        """Put the player in the driver's seat, through the same checks as walking up and pressing the prompt: the player must be on foot, alive, near the car with a clear line to the seat, and the seat must be empty. Returns false when any of that refuses."""
        ...
    def set_boost(self, vehicle: Entity, boost: float, /) -> None:
        """Set the boost left, clamped to 0..MaxBoost."""
        ...
    def set_boost_drain(self, vehicle: Entity, perSecond: float, /) -> None:
        """Set how much boost is used each second while the driver boosts, from 0 up."""
        ...
    def set_boost_enabled(self, vehicle: Entity, enabled: bool, /) -> None:
        """Give the car a boost (true) or take it away (false). Off for every car until you turn it on. With boost on, its driver holds Shift (B on a gamepad) to push the car forward harder. Boost drains at BoostDrain a second while it pushes and comes back at BoostRegen a second when it does not. It starts full (MaxBoost, 100 unless you change it). A destroyed car or one with no fuel cannot boost."""
        ...
    def set_boost_regen(self, vehicle: Entity, perSecond: float, /) -> None:
        """Set how much boost comes back each second, from 0 up. 0 means a used boost never comes back by itself."""
        ...
    def set_boost_strength(self, vehicle: Entity, strength: float, /) -> None:
        """Set how hard the boost pushes, from 0 (nothing) to 1 (the hardest it can)."""
        ...
    def set_fuel(self, vehicle: Entity, fuel: float, /) -> None:
        """Set the fuel, clamped to 0..MaxFuel. Fires Vehicles.FuelChanged, and Vehicles.FuelEmpty if it reaches 0."""
        ...
    def set_fuel_consumption(self, vehicle: Entity, perSecond: float, /) -> None:
        """Set how much fuel the car burns each second while it is driven and moving, from 0 up. 0 means it never runs dry."""
        ...
    def set_fuel_enabled(self, vehicle: Entity, enabled: bool, /) -> None:
        """Give the car a fuel tank (true) or take it away (false). Off for every car until you turn it on. With fuel on, the car burns FuelConsumption units a second while someone drives it and it is moving, and with none left it cannot accelerate (it rolls to a stop). It starts full (MaxFuel, 100 unless you change it)."""
        ...
    def set_health(self, vehicle: Entity, health: float, /) -> None:
        """Set the car's health, clamped to 0..MaxHealth. Lowering it counts as damage (Vehicles.Damaged fires, and Vehicles.Destroyed if it reaches 0)."""
        ...
    def set_health_enabled(self, vehicle: Entity, enabled: bool, /) -> None:
        """Give the car health (true) or take it away again (false). It is off for every car until you turn it on, so a car you never touch crashes as it always did. With health on, crashes wear the car down (a hard enough crash wrecks it in one hit), and at 0 health the car is destroyed: it cannot accelerate until you Repair it. It starts full (MaxHealth, 100 unless you change it)."""
        ...
    def set_livery(self, vehicle: Entity, index: float, /) -> bool:
        """Put one of the car's liveries on it: index counts from 0 in the liveries its vehicle was published with, and -1 takes the livery off. Every player sees the change. Returns false when the entity is not a car or the car has no livery at that index (the built-in cars have none)."""
        ...
    def set_max_boost(self, vehicle: Entity, max: float, /) -> None:
        """Set the most boost the car can store, from 1 to 1,000,000."""
        ...
    def set_max_fuel(self, vehicle: Entity, max: float, /) -> None:
        """Set the tank's size, from 1 to 1,000,000. Lowering it below the fuel lowers the fuel too; raising it does not fill the tank."""
        ...
    def set_max_health(self, vehicle: Entity, max: float, /) -> None:
        """Set the most health the car can have, from 1 to 1,000,000. Lowering it below the car's health lowers the health too; raising it does not heal."""
        ...
    def set_paint_color(self, vehicle: Entity, color: str, /) -> bool:
        """Paint the car: color is '#RRGGBB', the same format as Backpack:SetWeaponColor. Returns false when the entity is not a car."""
        ...
    def spawn(self, item: str, position: Vector3, /) -> Entity:
        """Spawn a car at position and return it as an Entity. item is the library item id of a vehicle this place stores: the string Storage:Get returns for one of the place's storage names. Any other value errors -- an asset id, an object path, a built-in car's name, an item the place does not store, or a weapon. The car is a real replicated vehicle: every player sees it, walks up to it and can drive it. Read its Position, tag it, or Destroy it like any entity. The server builds cars one per frame, so the Entity comes back at once but the car is not ready to use: connect to Vehicles.Ready, which fires with the car when it is built, before you Seat, paint or Lock it or touch its health, fuel or boost (those error until then)."""
        ...
    def unlock(self, vehicle: Entity, /) -> bool:
        """Unlock the car, so anyone can open it again. A locked car opens only for the last player who drove it: anyone else's vehicle key rattles the door, and holding the key breaks in -- the window on that side breaks, the car unlocks, and it must be hotwired before it starts. The lock stays when the driver gets out, and its driver can also lock and unlock it from the seat (L / D-pad left). Returns false when the entity is not a car."""
        ...

Vehicles: _VehiclesService

class _VoiceService:
    """Control who hears whom on voice chat, and see who is speaking."""
    speaking_started: Signal
    speaking_stopped: Signal
    def can_hear(self, listener: Player, speaker: Player, /) -> bool:
        """Whether your rules let the listener hear the speaker. It does not know the players' own mutes and blocks, so a true here can still be silent for that listener."""
        ...
    def is_muted(self, player: Player, /) -> bool:
        """Whether you have muted this player for everyone with Voice:SetMuted. It says nothing about players' own mutes, and it is false while you have only muted them for some listeners."""
        ...
    def is_speaking(self, player: Player, /) -> bool:
        """Whether the player is talking on voice chat right now. Each player's game tells the server whether they are talking (a yes or no, never audio), so it can lag by a fraction of a second, and a modified game could lie: use it for indicators and game play, not for anything that must be secure. A muted player still counts as speaking. On a client it covers the players this player can hear, themselves included. Example: if Voice:IsSpeaking(p) then ShowMicIcon(p) end"""
        ...
    def mute_for(self, listener: Player, speaker: Player, muted: bool, /) -> bool:
        """Stop (true) or allow (false) one listener hearing one speaker; everyone else still hears the speaker. Use it for rules that depend on who is listening. Returns false when either player is not verified yet, or you already hold 4096 of these pairs. Lasts until either player leaves, or Voice:Reset. Example: for _, other in Players:GetPlayers() do Voice:MuteFor(other, dead, true) end -- the living cannot hear a dead player, who still hears them."""
        ...
    def reset(self, player: Optional[Player] = ..., /) -> None:
        """Clear voice rules: every one that names player (muted, muted for someone, heard by an audience, or in one), or every rule on the server when you leave player out."""
        ...
    def set_audience(self, speaker: Player, listeners: Optional[list[Player]] = ..., /) -> bool:
        """Make a speaker heard only by the players in listeners (and by themselves); leave listeners out, or pass nil, to let everyone hear them again. This is team voice: set each player's audience to their team. At most 128 listeners. Returns false when the speaker is not verified yet or the list is too long. Lasts until the speaker leaves, or Voice:Reset. Mutes still apply on top. Example: Voice:SetAudience(p, Teams:GetPlayers(p.Team)) -- p's team hears p; call it again when teams change."""
        ...
    def set_muted(self, player: Player, muted: bool, /) -> bool:
        """Mute (true) or unmute (false) a player's voice for everyone: nobody hears them until you unmute them, though they can still talk. Use it for players who are dead, jailed or out of the round. Returns false when the player is not verified yet (moments after joining). The mute lasts until the player leaves. It only ever makes them quieter: a player who has muted or blocked someone themselves keeps them muted whatever you do. Voice goes straight between players' games, not through the server, so the server cannot hear or record it, and each listener's game applies the mute. A player running a modified game could ignore it, so use it to run a game, never to protect a secret. Example: Players:OnPlayer(function(p) Voice:SetMuted(p, true) end) -- everyone starts muted until a round begins, then Voice:SetMuted(p, false) for the players in it."""
        ...

Voice: _VoiceService

class _WorldService:
    """Find and watch the things in the level."""
    def blockcast(self, origin: Vector3, size: Vector3, direction: Vector3, params: dict[str, Any] = ..., /) -> Optional[dict[str, Any]]:
        """Sweep a box of this size (its full width, depth and height, at most 100 m on any side, kept square to the world's axes) from origin along direction and return the first thing it touches, in the same table as World:Raycast, or nil. Takes the same params as World:Raycast."""
        ...
    def find_by_tag(self, tag: str, /) -> list[Entity]:
        """Every entity with this actor tag."""
        ...
    def find_first(self, name: str, /) -> Optional[Entity]:
        """The first entity whose name (actor label) matches, or nil."""
        ...
    def get_entities_in_box(self, center: Vector3, size: Vector3, params: dict[str, Any] = ..., /) -> list[Entity]:
        """Every entity and player character touching a box of this size (its full width, depth and height, at most 100 m on any side, kept square to the world's axes) centred on center, nearest first. Takes the same params as World:GetEntitiesInRadius."""
        ...
    def get_entities_in_radius(self, center: Vector3, radius: float, params: dict[str, Any] = ..., /) -> list[Entity]:
        """Every entity and player character touching a ball of this radius (at most 50 m) around center, nearest first. Overlap-only entities (triggers) count; entities with no collision never do. A character is listed as its Entity; Players:GetPlayerFromCharacter tells whose it is. params: Include, Exclude, Tag, IgnorePlayers, Layers, ExcludeLayers (collision layers, server scripts only), and MaxResults (256 at most, and by default). Runs on this machine only; a script may make 256 spatial queries a frame."""
        ...
    def on_tagged(self, tag: str, handler: Callable[..., Any], /) -> Connection:
        """Call handler for every entity that has the tag now, and for each one that gets it later."""
        ...
    def on_untagged(self, tag: str, handler: Callable[..., Any], /) -> Connection:
        """Call handler when an entity loses the tag."""
        ...
    def raycast(self, origin: Vector3, direction: Vector3, params: dict[str, Any] = ..., /) -> Optional[dict[str, Any]]:
        """Cast a ray from origin along direction and return the first thing it hits: a table of Entity, Player, Position, Normal and Distance, or nil. Player is set when the hit is a player's character. The ray is as long as direction, or params.MaxDistance when given, and at most 1 km. It passes through overlap-only entities (triggers) and never hits entities with no collision. params: MaxDistance, Include (only these entities or players count), Exclude, Tag (only entities with this tag count), IgnorePlayers, Layers (only entities on these collision layers count; server scripts only) and ExcludeLayers (look through entities on these layers). Runs on this machine only; a script may make 256 spatial queries a frame."""
        ...
    def spherecast(self, origin: Vector3, radius: float, direction: Vector3, params: dict[str, Any] = ..., /) -> Optional[dict[str, Any]]:
        """Sweep a ball of this radius (at most 50 m) from origin along direction and return the first thing it touches, in the same table as World:Raycast, or nil. Distance is how far the ball's centre travelled. Takes the same params as World:Raycast."""
        ...

World: _WorldService

class _Task:
    def wait(self, seconds: float = 0) -> WaitToken: ...
    def spawn(self, fn: Callable[..., Any], *args: Any) -> int: ...
    def delay(self, seconds: float, fn: Callable[..., Any], *args: Any) -> int: ...
    def defer(self, fn: Callable[..., Any], *args: Any) -> int: ...
    def cancel(self, id: int) -> None: ...

task: _Task

def warn(*args: Any) -> None: ...
def require(path: str) -> Any: ...
