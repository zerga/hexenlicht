# Hexenlicht — effects checklist

Epic E6's goal ([#7](https://github.com/zerga/hexenlicht/issues/7)): this checklist of both games fully ticked. Written in story 6.1 ([DECISIONS.md](DECISIONS.md) X1); it was the epic's body until 6.6 moved it here (the owner, 2026-10-04). A story's PR ticks its lines after a look in game and updates their **now**.

Every effect the Hexen II and Portal of Praevus clients draw (HexenWorld is out of scope), one line per engine path, grouped by the story that owns it. Every `CE_*` of `cl_effect.c`, `TE_*` of `cl_tent.c`, trail of `r_part.c`, entity effect of `cl_main.c`, model draw flag of `gl_rmain.c` and view blend of `view.c` is on a line or in "Unused". A line gives the engine ids, what they draw and how GL draws it, where the game shows them (from the QuakeC in `gamecode/hc`; maps from `tools/hexenlicht/pak_entities.ps1`), and **now**: Hexenlicht's state.

**Now:**
- *drawn*: seen in 6.1's run, as GL draws it.
- *differs*: seen in the run, not as GL draws it (how).
- *not drawn*: GL draws it, Hexenlicht doesn't.
- *not looked at*: not seen in the run (not triggered, out of view, or too small to judge). Drawn by a path the run saw working (sprites, models, particles, beams, dynamic lights).

Run steps are named `c<class>_<near|far|item>_<step>`. "Run 5" is the second run, for the melee hits.

**6.1's run** (`tools/hexenlicht/effects_run.ps1`, 2026-09-30, TESTING.md "Effects (6.1)"):
- Every class fires its four weapons, normal and with the tome, and uses ten artifacts.
- Both engines start from the same saves at demo1's start and are shot paused at the same game time.
- Hexenlicht left out no effects and no instances; `Vulkan validation: 0 errors, 0 warnings`.

**Ticking.** A line is ticked by its story's PR after a look in game in Hexenlicht, with GL as a sanity reference (R107) and `Vulkan validation: 0 errors, 0 warnings` (DECISIONS X1).

**Particles.** The 29 particle types differ in how they move (`r_part.c`, shared with GL). Hexenlicht draws them as GL's triangles, with its dot texture and colors (G10). Two exceptions, as GL: snow has its own textures and sizes, and particles within 20 units are drawn at the base size.

**Light.** GL gives an effect light only where a line says so: the dynamic lights of `TE_EXPLOSION`, of the projectiles' model flags and of the `EF_*` effects. Explosions, fire sprites and beams have none; whether they should emit light is for 6.2 and 6.3 to decide (6.2: the fire, explosion, flash and spark sprites and the glowing projectiles do, DECISIONS X5–X8; 6.3: the beams of light do, a line light along each, X10–X11). glh2 adds dynamic lights to surfaces and models, but without overbright: faint on dark stone and clipped at the texture's color (R83). Where a line says the light is faint in glh2, that is this.

**Brightness.** Particles and sprites share one exposure factor (R41); effects brightness is an open question for 6.2 and 6.3.

## 6.2 Effects that glow emit light (#65), and the lines for 7.1

6.2 (narrowed 2026-09-30, DECISIONS X4) takes the light of the emitting sprites and of the glowing projectiles:
- the explosion, flash and spark sprites below;
- Praevus's fire sprites (under 6.8);
- the projectiles that own a dynamic light (trails, weapons, spells).

6.2 ticks those lines in its PR. The other lines here aren't a story's: 7.1's playthroughs look at them and tick or log them.

6.2's look (2026-09-30): `effects_run.ps1` with `r_effect_lights` 1 and 0, paused and `-NoPause`, and scripts of their own around the projectiles and hits (TESTING.md). **A paused game draws no client effects** (X9: `CE_*` sprites and models), so the run's paused shots can't show them, 6.1's neither: "not looked at" or "not caught" on a client effect's line can mean that. The lines 6.2 saw emitting are ticked; the rest say "emits (6.2)" or not.

### Weather and the world
- [ ] **Rain**: `CE_RAIN`.
  - Draws: `R_RainEffect`, every frame in the entity's box; splats are `particle4` GRAV, colors 408–412.
  - Where: `weather_rain` (castle4, eidolon; Praevus: thomas).
  - Now: not looked at.
- [ ] **Fountain**: `CE_FOUNTAIN`.
  - Draws: `R_RunParticleEffect2`, every frame.
  - Where: `misc_fountain` (romeric3, village4; Praevus: keep1).
  - Now: not looked at.
- [ ] **Teleport**: `CE_TELEPORTERBODY` and `CE_TELEPORTERPUFFS`.
  - Draws: `teleport.mdl`, translucent and scaled in XY, plus 8 `telesmk2.spr` puffs, translucent.
  - Where: `trigger_teleport`, the teleport artifact, co-op respawn, polymorph casting and wearing off.
  - Now: not looked at (the run's teleport was out of view).
- [ ] **Monster spawner**: `TE_TELEPORT`.
  - Draws: `R_TeleportSplash` particles.
  - Where: `func_monsterspawner`.
  - Now: not looked at.
- [ ] **Spike traps**: `TE_SPIKE`.
  - Draws: 10 particles of color 0 where `spike.mdl` hits.
  - Where: `trap_spikeshooter`, `trap_shooter`.
  - Now: not looked at.
- [ ] **Wall hits**: `TE_GUNSHOT`.
  - Draws: 20 particles of color 0.
  - Where: gauntlets, the axe's swing, punch dagger and sickle hitting a wall.
  - Now: drawn (run 5, `c4_near_w1t` at 12: the particles on the pedestal in both engines).
- [ ] **Hit puffs**: `svc_particle4` FASTGRAV (`SpawnPuff`).
  - Draws: the color of what is hit: blood, stone, metal sparks, wood, ice, spider blood, dust.
  - Where: every melee and missile hit on something that takes damage.
  - Now: not looked at (no monsters in the run).
- [ ] **Lightning and sunbeam hits**: `svc_particle` (`R_RunParticleEffect`).
  - Draws: color 225 on the target. A corpse killed this way smoulders for 7 s with color 272–288 and `CE_WHITE_SMOKE`.
  - Where: sunstaff, tomed warhammer, thunderstorms, `weather_lightning`, the fallen angel lord.
  - Now: not looked at.
- [ ] **Corpses and souls**: several effects.
  - Draws:
    - `CE_YELLOWRED_FLASH` (`yr_flsh.spr`, translucent) when a corpse fades.
    - Soul spheres: `soulskul`, `soulball` and `cross.mdl` (fullbright), `goodsphr.mdl`, with `svc_particle2` C_EXPLODE colors 144/176.
    - Bubbles when dying in water (`s_bubble.spr`, translucent, abslight).
  - Where: kills (a soul sphere at level 3+ for the Necromancer, 6+ for the Crusader).
  - Now: not looked at.

### Server particles
- [ ] **`svc_particle2`** (`R_RunParticleEffect2`, a box).
  - Where:
    - The hydra's ink (SPIT 256).
    - The mummy lord's and Death's ground flames (BLOB 168, on `mumshot.mdl`, abslight).
    - The fallen angel dying (SPELL 384).
    - The ice mace freezing flesh (ICE 145).
    - Soul spheres (C_EXPLODE).
  - Now: not looked at.
- [ ] **`svc_particle4`** (`R_RunParticleEffect4`, a radius).
  - Where:
    - The grenade trail (SLOWGRAV 284).
    - Bone shards' impacts (GRAV 368–384).
    - Golems: the iron golem's gem trail, the bronze golem's stomp and charge, their deaths.
    - The medusa's arrow trail; Death's falling bones.
    - `trap_fireball`'s trail (FIREBALL); the polymorph shot's trail (EXPLODE2).
    - The meteor tornado in water.
  - Now: drawn (grenade trails, bone shards' impacts, polymorph trails: `c4_*_w3`, `c3_*_w3`, `c*_item_polymorph`).
- [ ] **`svc_particle_explosion`** (`R_ColoredParticleExplosion`).
  - Where:
    - Ice and stone shatter deaths (colors 14, 10).
    - `obj_barrel_exploding` (384).
    - A sheep turning back.
    - The ice mace hitting flesh.
    - The medusa's arrow dying; the polymorph shot hitting a wall.
  - Now: not looked at.
- [ ] **`svc_raineffect`** (`R_RainEffect`).
  - Where: the tomed ice mace's blizzard (colors 400–415).
  - Now: drawn (`c2_*_w2t`).

### Explosions
- [x] **Rocket explosion**: `TE_EXPLOSION`.
  - Draws: `R_ParticleExplosion` (explode particles) and a dynamic light, radius 350, 0.5 s; red with `gl_colored_dynamic_lights`.
  - Where: meteors, tomed grenades, the Necromancer's and Assassin's glyphs, scarabs, War's axe, imps' fireballs, the cube's shots, `trap_fireball`.
  - Now: drawn, with its light on the pedestal (faint in glh2, R83; `c2_near_w3`). 6.2: unchanged, GL's own light.
- [x] **`CE_SM_EXPLOSION`**: `sm_expld.spr`.
  - Where: axe blade, raven staff, the small Eidolon.
  - Now: emits (6.2; `c3_*_w4` in the unpaused run, X9).
- [x] **`CE_LG_EXPLOSION`**: `bg_expld.spr`.
  - Where: explosions in the air (meteors, grenades, skull wizard), `obj_barrel_exploding`, the Assassin's tripwire.
  - Now: emits (6.2; `c2_*_w3`, `c4_*_w4t` unpaused).
- [x] **`CE_FLOOR_EXPLOSION`**: `fl_expld.spr`.
  - Where: explosions on the ground, the summoning stone's imp, Death's circle of fire.
  - Now: emits (6.2): the summoned imp stands in its light (`c*_item_summon` at 60, unpaused).
- [x] **`CE_NEW_EXPLOSION`**: `gen_expl.spr`.
  - Where: grenades, the Crusader's glyph, the Staff of Set.
  - Now: emits (6.2; `c4_*_w4`, `c2_item_glyph` unpaused: it lights the weapon at the player's feet).
- [ ] **`CE_XBOW_EXPLOSION`**: `xbowexpl.spr`.
  - Where: tomed crossbow.
  - Now: emits (6.2); not caught: the tomed arrows explode after the run's 50 frames.
- [ ] **`CE_BLUE_EXPLOSION`**: `xpspblue.spr`.
  - Where: tomed axe hits.
  - Now: emits (6.2); not caught in the run.
- [x] **`CE_MAGIC_MISSILE_EXPLOSION`**: `mm_expld.spr`.
  - Where: magic missile, the Eidolon's spell.
  - Now: emits (6.2): the hit lights the pedestal, strongly from near (`c3_near_w2`: the view up to 2.2 times brighter), grainy while the light changes every frame.
- [x] **`CE_BONE_EXPLOSION`**: `bonexpld.spr`.
  - Where: tomed bone shards.
  - Now: emits (6.2; `c3_*_w3t` unpaused).
- [x] **`CE_SM_CIRCLE_EXP`**: `fcircle.spr`.
  - Where: purifier, the mummy's arrows.
  - Now: emits (6.2; `c1_near_w4` unpaused: the view 1.3 times brighter).
- [x] **`CE_BG_CIRCLE_EXP`**: `xplod29.spr`.
  - Where: tomed purifier, the Eidolon's fireball.
  - Now: emits (6.2; `c1_*_w4t` unpaused).

### Flashes, sparks and smoke
All are unlit sprites.
- [ ] **`CE_SM_WHITE_FLASH`**: `sm_white.spr`.
  - Where: an axe blade ending, vorpal hits with mana, the ring of turning, soul spheres ending.
  - Now: emits (6.2); not told apart in the run.
- [x] **`CE_WHITE_FLASH`**: `gryspt.spr`, translucent.
  - Where: tomed gauntlets' hits; tomed punch dagger and sickle on hits and walls (instead of the yellow spark); the summoning stone with no room; the fallen angel deflecting.
  - Now: emits (6.2, at a third: translucent): the tomed dagger's and sickle's wall hits (`c4_near_w1t`, `c3_near_w1t` unpaused).
- [x] **`CE_BLUE_FLASH`**: `bluflash.spr`, translucent.
  - Where: the blast radius, one per entity it pushes.
  - Now: emits (6.2): seven flashes in the run (`c*_item_blast` unpaused).
- [ ] **`CE_SM_BLUE_FLASH`**: `sm_blue.spr`, translucent.
  - Where: a tomed axe blade ending, the raven staff's splits.
  - Now: emits (6.2); not caught in the run.
- [ ] **`CE_RED_FLASH`**: `redspt.spr`, translucent.
  - Where: archer launches, backstabs, Famine's volleys, the summoned imp vanishing, the mummy lord's shot, the tomed Set hook, the skull wizard blinking.
  - Now: emits (6.2); not looked at.
- [x] **`CE_BLUESPARK`**: `bspark.spr`.
  - Where: tomed axe bounces; Praevus: Praevus's stars, the ice archer's arrows.
  - Now: emits (6.2): the tomed axe's bounces (`c1_*_w3t` unpaused).
- [x] **`CE_YELLOWSPARK`**: `spark.spr`.
  - Where: axe bounces, punch dagger and sickle on walls, the archer lord's gold arrows, the werejaguar blocking.
  - Now: emits (6.2): axe bounces, the dagger and the sickle on walls (`c1_*_w3`, `c4_near_w1`, `c3_near_w1` unpaused); a spark in the wall's surface moves out, up to 4 units.
- [ ] **`CE_REDSPARK`**: `rspark.spr`.
  - Where: archers' red arrows, the fallen angel's spell, Famine's and Death's missiles, the snake's spit.
  - Now: emits (6.2); not looked at.
- [ ] **`CE_GREENSPARK`**: `gspark.spr`.
  - Where: archers' green arrows, Pestilence's missiles.
  - Now: emits (6.2); not looked at.
- [ ] **`CE_WHITE_SMOKE`**: `whtsmk1.spr`, translucent, drifting.
  - Where: vorpal, gauntlets and crossbow on walls; meteors' trails and fizzles; the tornado; ravens; bone shards; golems' and werejaguars' skids; the skull wizard's death; Pestilence; `obj_cauldron`; `fx_smoke_generator` (egypt1, egypt3, egypt5, tower, village2, village3); smouldering corpses.
  - Now: not looked at (in the run: `c2_*_w3`, `c3_*_w3`).
- [ ] **`CE_GREEN_SMOKE`**: `grnsmk1.spr`, translucent.
  - Where: the fallen angel's wing blades, the medusa's arrow trail; Praevus: the acid rune, the Demoness's glyph.
  - Now: not looked at.
- [ ] **`CE_GREY_SMOKE`**: `grysmk1.spr`, translucent.
  - Where: `fx_smoke_generator` thingtype 3, the snake waking (egypt5).
  - Now: not looked at.
- [ ] **`CE_RED_SMOKE`**: `redsmk1.spr`, translucent.
  - Where: `fx_smoke_generator` thingtypes 1 and 2 (the gamecode makes "green" red, as in GL); a raven ending.
  - Now: not looked at (in the run: `c3_*_w4t`).
- [ ] **`CE_TELESMK1`**: `telesmk1.spr`, translucent.
  - Where: the raven staff's muzzle.
  - Now: not looked at (in the run: `c3_*_w4`).
- [ ] **`CE_REDCLOUD`**: `rcloud.spr`, opaque.
  - Where: Famine blinking, Death's missile shot down (rider1a, meso9).
  - Now: not looked at.
- [ ] **`CE_GHOST`**: `ghost.spr`, translucent, abslight 127.
  - Where: tomed bone shards on a target.
  - Now: not looked at (needs a target).
- [ ] **`CE_ICEHIT`**: `icehit.spr`.
  - Where: the ice mace's snowball hitting anything; Praevus: the yakman's snowball.
  - Now: doesn't emit (6.2, X5: snow and ice); not looked at.
- [ ] **`CE_MEDUSA_HIT`**: `medhit.spr`.
  - Where: the medusa's arrow (romeric1–4, romeric7).
  - Now: emits (6.2); not looked at.
- [ ] **`CE_MEZZO_REFLECT`**: `mezzoref.spr`.
  - Where: the werepanther reflecting a missile (meso2, meso4–6, meso8).
  - Now: emits (6.2, faint); not looked at.

### Chunks and gibs
Chunks are server entities in Hexen II. Portal of Praevus's client-side `CE_CHUNK` is under 6.8.
- [ ] **Debris**: `chunk.hc` models.
  - Draws:
    - Wood splinters, metal, stone, clay, leaves, hay and cloth chunks.
    - Glass shards `shard1`–`5` (clear glass translucent, red glass skin 2, webs skin 3).
    - Ice `shard.mdl` (translucent, abslight 0.75).
    - Meteor pieces `tempmetr.mdl`.
  - Where: `breakable_brush` (1082 in the maps) and the breakable `obj_*`.
  - Now: not looked at.
- [ ] **Gibs**: flesh and heads.
  - Draws: `flesh1`–`3` (`EF_ZOMGIB` trail), `sflesh1`–`3` (`EF_ROCKET` trail, and its light), heads `h_*.mdl` (`EF_GIB` trail).
  - Where: gibbed monsters and players.
  - Now: not looked at.
- [ ] **Shatter**: frozen or stoned deaths.
  - Draws: `svc_particle_explosion` and shards.
  - Where: the ice mace, the medusa.
  - Now: not looked at.
- [ ] **Bone models**: `CE_BONESHARD` and `CE_BONESHRAPNEL`.
  - Draws: `boneshot.mdl` and `boneshrd.mdl`, spinning client models that replace the server's `EF_NODRAW` projectiles.
  - Where: bone shards normal (the shards) and tomed (20 pieces at impact).
  - Now: drawn (`c3_near_w3`).

### Trails (the model's flag, `R_RocketTrail`)
The first flag in `CL_RelinkEntities`' chain wins; `mdl_flags.ps1 -Trails` lists the models.
- [ ] **`EF_ROCKET`** (`rt_rocket_trail`), with a light of radius 200.
  - Models: `lavaball.mdl`, `pestshot.mdl`, `sflesh1`–`3`.
  - Where: `misc_fireball` lava balls, Pestilence (rider2c), gibs.
  - Now: not looked at.
- [ ] **`EF_GRENADE`** (`rt_smoke`).
  - Models: `shard1.mdl`, `tempmetr.mdl`.
  - Where: glass chunks, the meteor staff's meteors.
  - Now: not looked at (in the run: `c2_*_w3`).
- [ ] **`EF_GIB`** and **`EF_ZOMGIB`** (`rt_blood`, `rt_slight_blood`).
  - Models: heads `h_*.mdl`, `flesh1`–`3`.
  - Where: gibs.
  - Now: not looked at.
- [ ] **`EF_TRACER`** (`rt_tracer`).
  - Models: `goop.mdl`.
  - Where: the snake's spit (egypt5).
  - Now: not looked at.
- [ ] **`EF_TRACER3`** (`rt_voor_trail`).
  - Models: `booberry.mdl`.
  - Where: the ghosts of players Death kills (meso9).
  - Now: not looked at.
- [ ] **`EF_FIREBALL`** (`rt_fireball`), with a light of radius 100–120.
  - Models: `fireball.mdl`.
  - Where: fire imps, the cube of force's shots, Death's circle of fire.
  - Now: not looked at (in the run: `c*_item_cube`).
- [ ] **`EF_ICE`** (`rt_ice`).
  - Models: `shardice.mdl`; Praevus: `yakball.mdl`, `budd_star.mdl`.
  - Where: ice imps; Praevus: the yakman, Praevus.
  - Now: not looked at.
- [ ] **`EF_SPIT`** (`rt_spit`), with a negative-radius light that lights nothing in GL or Hexenlicht.
  - Models: `spit.mdl`.
  - Where: the hydra (demo2, cath, egypt4, meso3, romeric3–4).
  - Now: not looked at.
- [ ] **`EF_SPELL`** (`rt_spell`).
  - Models: `faspell.mdl`, `famshot.mdl`.
  - Where: the fallen angel, Famine, Death.
  - Now: not looked at.
- [ ] **`EF_VORP_MISSILE`** (`rt_vorpal`).
  - Models: `vorpshot`, `vorpshok`, `vorpshk2.mdl`.
  - Where: tomed vorpal sword.
  - Light: a light with `gl_extra_dynamic_lights`; Hexenlicht adds it to the scene only (R81).
  - Now: differs: the shock's black square (`c1_near_w2t`; see "Transparent models").
- [ ] **`EF_SET_STAFF`** (`rt_setstaff`).
  - Models: `scrbstp1.mdl`.
  - Where: Staff of Set.
  - Now: drawn (`c4_far_w4`).
- [x] **`EF_MAGICMISSILE`** (`rt_magicmissile`), light as `EF_VORP_MISSILE`'s.
  - Models: `ball.mdl`.
  - Where: magic missile, the Eidolon, the tomed ice mace's blizzard ball.
  - Now: the ball glows (6.2, X7) and isn't lit by its light (`c3_*_w2`, `w2t`; tomed: three glowing).
- [ ] **`EF_BONESHARD`** (`rt_boneshard`).
  - Models: `bonelump.mdl`.
  - Where: tomed bone shards.
  - Now: not looked at (in the run: `c3_*_w3t`).
- [x] **`EF_SCARAB`** (`rt_scarab`), light as `EF_VORP_MISSILE`'s.
  - Models: `scrbpwng.mdl`.
  - Where: tomed Staff of Set.
  - Now: the scarab's body glows (6.2: its translucent wings carry the light, X7) and isn't lit by it (`c4_*_w4t`).

### Lights and glows of effects
- [ ] **`EF_MUZZLEFLASH`**: a light of radius 200–231 for 0.1 s (the server clears the flag every frame; the light dies after 0.1 s).
  - Where: magic missile, bone shards, meteor staff, Staff of Set, tomed warhammer and ice mace, the fallen angel lord, the Eidolon, every Demoness weapon.
  - Now: drawn: it lights the hand (bone shards, `c3_near_w3`).
- [x] **`EF_BRIGHTLIGHT`**: a light of radius 400–431.
  - Where: sunstaff firing, tomed purifier missile, the Paladin's invincibility, the torch (with `EF_MUZZLEFLASH`: `EF_TORCHLIGHT`), `trap_lightning`, the tripwire, glowing trains, the Riders' deaths.
  - Now: drawn. 6.2: the tomed purifier's ball glows and isn't lit by its light (`c1_*_w4t`); players, the Riders and Praevus don't glow (X7). Its light on surfaces is faint in glh2 (R83; `c1_far_w4t`), and it lights the weapon (the Paladin's invincibility).
- [x] **`EF_DIMLIGHT`**: a light of radius 200–231.
  - Where: the torch (the torch at the feet is 4.19), the summoning stone, tomed magic missiles; Praevus: the fire storm, the tempest staff, burning victims.
  - Now: drawn (`c3_far_w2t`). 6.2: the summoning stone and the tomed magic missiles glow without being lit by their light (`c*_item_summon`, `c3_*_w2t`); the torch's player doesn't (X7).
- [ ] **`EF_DARKLIGHT`**: a dark light.
  - Where: the Necromancer's invincibility (R99).
  - Now: drawn (`c3_item_invincibility`, R99).
- [ ] **`EF_LIGHT`**: a light of radius 200.
  - Where: invisibility (with `EF_NODRAW`).
  - Now: not looked at (`c*_item_invisibility`).
- [ ] **`EF_DARKFIELD`**: `R_DarkFieldParticles` around the entity.
  - Where: haste.
  - Now: differs (see "Haste").
- [ ] **Model light modes**: `MLS_ABSLIGHT`, `MLS_POWERMODE`, `MLS_TORCH`, `MLS_FIREFLICKER`, `MLS_FULLBRIGHT`, `MLS_CRYSTALGOLEM`.
  - Draws: GL's fixed light levels on effect models (projectiles, flames, shock balls); Hexenlicht uses the instance's `light` (light styles 25–30).
  - Where: most projectiles, the summoning stone, tomed axe blades, the cube, souls.
  - Now: differs: models at a fixed light level are brighter than GL's: the summoning stone (power mode), the scarab (abslight 0.5), the cube (abslight 0.1–1); see the weapons and spells.
- [ ] **HoT's glows** (not planned, X4: HoT's patch, not Raven's; revisit in 7.1 if a missile is hard to see): coronas.
  - Draws: `gl_missile_glows` 1 by default (`XF_MISSILE_GLOW`: 23 names in `gl_model.c`, 21 of them models; `models/shard` wants exactly 12 characters and `models/scrbpbody` misspells `scrbpbdy`, so glass shards and the scarab have none; `fireball`, `drgnball`, `purfir1`, `iceshot`, `iceshot2`, `flaming`, `scrbstp1`, `spit`, `goop`, `snakearr`, `shardice`, `lavaball`, `eidoball`, `famshot`, `pestshot`, `mumshot`, `golemmis`, …); `gl_glows` 0 (torches); `gl_other_glows` 0 (mana).
  - Now: not drawn. Hexenlicht registers the cvars only. GL's blue and yellow balls on the ice mace's and purifier's missiles are missing (`c2_far_w2`, `c1_far_w4`), as are the red and green ones on the Demoness's blood rain and acid (`c5_far_w1`, `w2t`). Whether to draw them or leave them to bloom is for 6.2 to decide.

### Class weapons (Hexen II's four classes; the Demoness is under 6.8)
Each weapon is in the run, near (the pedestal about 40 units ahead) and far (246 units). A line is the weapon as a whole: its models, trails, impacts and lights. The full run's near steps were level, so the melee weapons missed; run 5 (the Paladin and the Assassin) looked down 20° so they reach the pedestal.
- [ ] **Paladin 1, gauntlets**.
  - Normal: `SpawnPuff` on hits; `TE_GUNSHOT` and `CE_WHITE_SMOKE` on walls.
  - Tome: adds `CE_WHITE_FLASH` on hits.
  - Now: the swing is drawn; no hit effect was caught in the melee run (run 5, `c1_near_w1`, `w1t`).
- [ ] **Paladin 2, vorpal sword**.
  - Normal: a swipe `vorpswip.mdl` (transparent, translucent); `CE_SM_WHITE_FLASH` and `CE_WHITE_SMOKE`.
  - Tome: adds a missile `vorpshot.mdl` (abslight 0.5, `EF_VORP_MISSILE`), then the shocks `vorpshok.mdl` and `vorpshk2.mdl` (transparent, facing the view).
  - Now: differs.
    - The swipe is an opaque bright slab; GL's is faint (`c1_far_w2`).
    - The tomed shock shows its black square, which GL doesn't (`c1_near_w2t`).
- [ ] **Paladin 3, axe**.
  - Normal: a swing and a blade `axblade.mdl` (transparent) with a tail `axtail.mdl` (translucent). Bounce `CE_YELLOWSPARK`, hit `CE_SM_EXPLOSION`, end `CE_SM_WHITE_FLASH`.
  - Tome: 3 blades, skin 1, power mode. Bounce `CE_BLUESPARK`, hit `CE_BLUE_EXPLOSION`, end `CE_SM_BLUE_FLASH`.
  - Now: differs: the blue tail is brighter than GL's (`c1_near_w3`, `w3t`).
- [x] **Paladin 4, purifier**.
  - Normal: a missile `purfir1.mdl` (abslight 1, a missile glow in GL), hit `CE_SM_CIRCLE_EXP`.
  - Tome: a ball `drgnball.mdl` (`EF_BRIGHTLIGHT`, a glow) with smoke rings `ring.mdl` (translucent), hit `CE_BG_CIRCLE_EXP`.
  - Now: differs (6.2 done): the missile's `CE_SM_CIRCLE_EXP` and the tomed ball's `CE_BG_CIRCLE_EXP` light the pedestal; the tomed ball glows (a flat, clipped yellow at `r_emissive_scale` 32) and isn't lit by its own light. GL's glows are left out (X4). The light on the pedestal is faint in glh2 (R83).
- [ ] **Crusader 1, warhammer**.
  - Normal: `SpawnPuff` on hits.
  - Tome: a thrown `hamthrow.mdl` (abslight 1); a hit casts 3 `TE_STREAM_LIGHTNING`, color 225 particles and a shock ball.
  - Now: the thrown hammer is drawn; its lightning was not looked at (`c2_*_w1t`).
- [ ] **Crusader 2, ice mace**.
  - Normal: a snowball `iceshot1.mdl` (abslight 0.5) with a corona `iceshot2.mdl` (translucent; both with missile glows in GL). Hits give `CE_ICEHIT`; on walls, shards `shard.mdl`; on flesh, freezing.
  - Tome: a `ball.mdl` (scale 0.1, `EF_MUZZLEFLASH`, its `EF_MAGICMISSILE` trail) that becomes a blizzard (`svc_raineffect` and 6 `TE_STREAM_ICECHUNKS` every 0.1 s).
  - Now: the tome's blizzard is drawn. Normal differs:
    - GL's blue glow is missing (`c2_far_w2`).
    - A white shape at the mace's head is Hexenlicht's only (`c2_near_w2` at 30).
- [x] **Crusader 3, meteor staff**.
  - Normal: meteors `tempmetr.mdl` (abslight 1, a `CE_WHITE_SMOKE` trail); impacts give `TE_EXPLOSION`, `CE_LG_EXPLOSION` or `CE_FLOOR_EXPLOSION`, and 3–10 mini-meteors.
  - Tome: a tornado `tornato.mdl` with a funnel `funnal.mdl` (transparent, holey, abslight 0.2) that flings meteors.
  - Now: drawn (`c2_*_w3`, `w3t`); 6.2: the explosion sprites emit beside `TE_EXPLOSION`'s light. The explosion's light on the pedestal is faint in glh2 (R83).
- [ ] **Crusader 4, sunstaff**.
  - Normal: `TE_STREAM_SUNSTAFF1` (1 beam and reflections), `EF_BRIGHTLIGHT`.
  - Tome: 3 beams.
  - Now: the beams are drawn. Differs: the glow at the staff is opaque and brighter than GL's translucent one (`c2_near_w4`, `w4t`).
- [ ] **Necromancer 1, sickle**.
  - Normal: `SpawnPuff`; `TE_GUNSHOT` and `CE_YELLOWSPARK` on walls.
  - Tome: `CE_WHITE_FLASH` on hits and walls, instead of the yellow spark.
  - Now: the swing is drawn (`c3_*_w1`). The hits were not looked at: the Necromancer wasn't in the melee run.
- [x] **Necromancer 2, magic missile**.
  - Normal: `ball.mdl` (faces the view, `EF_MAGICMISSILE`) with 2 `star.mdl` (holey, abslight) and a hand flash `handfx.mdl` (translucent, holey); hit `CE_MAGIC_MISSILE_EXPLOSION`; `EF_MUZZLEFLASH`.
  - Tome: 3 homing missiles with `EF_DIMLIGHT`.
  - Now: drawn (`c3_far_w2`, `w2t`); 6.2: the balls glow, the hit lights the pedestal (strongly from near, `c3_near_w2`). The tomed missiles' light on the pedestal is faint in glh2 (R83).
- [x] **Necromancer 3, bone shards**.
  - Normal: `CE_BONESHARD` models, hitscan puffs, `svc_particle4` GRAV, `CE_WHITE_SMOKE`, `EF_MUZZLEFLASH`.
  - Tome: `bonelump.mdl` (`EF_BONESHARD`), `CE_BONE_EXPLOSION`, 20 `CE_BONESHRAPNEL`, `CE_GHOST` on a target.
  - Now: drawn (`c3_*_w3`, `w3t`); 6.2: `CE_BONE_EXPLOSION` emits. The muzzle flash lights the hand far more than in glh2 (R83).
- [ ] **Necromancer 4, raven staff**.
  - Normal: `CE_TELESMK1` at the muzzle; `vindsht1.mdl` splits with `CE_SM_BLUE_FLASH`; hit `CE_SM_EXPLOSION`.
  - Tome: `birdmsl2.mdl`, then 3 ravens `ravproj.mdl`; `CE_WHITE_SMOKE`, `CE_RED_SMOKE`.
  - Now: the tomed raven is drawn (`c3_near_w4t`). The normal shots are too small to judge (`c3_*_w4`).
- [ ] **Assassin 1, punch dagger**.
  - Normal: `SpawnPuff`; `TE_GUNSHOT` and `CE_YELLOWSPARK` on walls; `CE_RED_FLASH` on a backstab.
  - Tome: `CE_WHITE_FLASH` on hits and walls, instead of the yellow spark.
  - Now: the wall hits' particles are drawn (run 5, `c4_near_w1t` at 12); the sparks and flashes weren't caught. Not an effect, not looked into: the dagger's pose at 30 frames differs (GL raised, Hexenlicht low).
- [ ] **Assassin 2, crossbow**.
  - Normal: 3 `arrow.mdl`; hits give the flash `arrowhit.mdl` (holey, abslight 0.5) and `SpawnPuff`; walls give `CE_WHITE_SMOKE` and wood chunks.
  - Tome: 5 `flaming.mdl` (fire flicker, a missile glow in GL) that stick and explode with `CE_XBOW_EXPLOSION`.
  - Now: drawn (`c4_near_w2`, `w2t`).
- [ ] **Assassin 3, grenades**.
  - Normal: `assgren.mdl` with a `svc_particle4` trail; `CE_NEW_EXPLOSION`, or `CE_LG_EXPLOSION` / `CE_FLOOR_EXPLOSION`.
  - Tome: a big grenade, `TE_EXPLOSION` and 3–6 sub-bombs.
  - Now: drawn (`c4_*_w3`, `w3t`).
- [x] **Assassin 4, Staff of Set**.
  - Normal: `scrbstp1.mdl` (abslight 0.5, `EF_SET_STAFF` trail) through its targets, `CE_NEW_EXPLOSION`; `EF_MUZZLEFLASH`.
  - Tome: a scarab `scrbpbdy.mdl` (abslight 0.5) with wings `scrbpwng.mdl` (translucent, `EF_SCARAB`). On a living target, 4 hooks with `TE_STREAM_CHAIN`; otherwise `TE_EXPLOSION`.
  - Now: the normal shot is drawn (`c4_far_w4`); 6.2: `CE_NEW_EXPLOSION` emits, and the scarab glows (skin times 16: abslight 0.5) without being lit by its wings' light (`c4_*_w4t`). It still lights the weapon.

### Spells (artifacts)
Each is used in the run from far (`c<class>_item_<name>`), shot at 10, 60 and 200 frames.
- [ ] **Torch**: `EF_DIMLIGHT` for 1 s, `EF_TORCHLIGHT` (`EF_MUZZLEFLASH` and `EF_BRIGHTLIGHT`) for 23 s, then `EF_DIMLIGHT`.
  - Now: nothing to see in the shots; the light at the player's feet is 4.19.
- [x] **Summoning stone**: `a_summon.mdl` (`EF_DIMLIGHT`, power mode) thrown; the imp lord appears with `CE_FLOOR_EXPLOSION`; with no room, `CE_WHITE_FLASH`.
  - Now: the imp is drawn. 6.2: the stone glows (power mode) without being lit by its own light, and the floor explosion lights the imp (`c*_item_summon` unpaused).
- [ ] **Invisibility**: `EF_NODRAW` and `EF_LIGHT`; the weapon is translucent.
  - Now: the weapon is drawn translucent. 6.6: GL's grey view tint drawn (`blend_run.ps1`'s `invisibility`).
- [ ] **Glyph of the ancients**: one per class.
  - Paladin: `blast.mdl`, a growing ball of fire (abslight). Now: drawn (`c1_item_glyph` at 60).
  - Crusader: a time bomb `glyphwir.mdl` (translucent, abslight) and `CE_NEW_EXPLOSION`. Now: not looked at (at the feet, below the view).
  - Necromancer: a proximity mine `glyphwir.mdl` (power mode), `TE_EXPLOSION`. Now: differs: Hexenlicht's view goes black (`c3_item_glyph`, all three shots). The gamecode spawns the mine at the player's `proj_ofs` (44), 6 units below the eye (50), so the eye is inside its model (radius about 9). GL's 4-unit near plane clips it; Hexenlicht's primary rays see its inside (6.12).
  - Assassin: a tripwire (`twspike.mdl`, `TE_STREAM_CHAIN`); tripped, `EF_BRIGHTLIGHT`, the explosion sprites and `TE_EXPLOSION`. Now: the thrown glyph is drawn (`c4_item_glyph`); the tripwire and its explosion were not looked at.
- [ ] **Haste**: `EF_DARKFIELD` (`R_DarkFieldParticles` around the player).
  - Now: differs: the particles near the camera are large soft purple blobs; GL's are dots (`c*_item_haste` at 60 and 200). They are inside GL's near plane: 6.12.
- [x] **Blast radius**: `CE_BLUE_FLASH` on each entity it pushes.
  - Now: 6.2: its flashes emit (seven in the run, `c*_item_blast` unpaused).
- [ ] **Polymorph**: 5 `polymrph.spr` (power mode) with `svc_particle4` EXPLODE2 trails; a hit gives the teleport effect (skin 1).
  - Now: drawn (`c*_item_polymorph` at 10). The near particles are larger, as haste's.
- [ ] **Cube of force**: `cube.mdl` (abslight 0.1, 1 when it fires) firing `fireball.mdl` (`TE_EXPLOSION`).
  - Now: differs: the cube is brighter than GL's dark one (`c*_item_cube`); where it flies is random.
- [ ] **Invincibility**: a view tint (drawn since 6.6) and, per class:
  - Paladin: `EF_BRIGHTLIGHT`. Now: it lights the weapon far more than in glh2 (R83).
  - Necromancer: `EF_DARKLIGHT`. Now: it darkens the surroundings (R99).
  - Crusader: the stone skin. Now: not looked at (the own model isn't drawn). 6.11: the own model in the stone skin shows in a mirror floor (`viewer_run.ps1 -Mirror`, `cath_mirror_stone`).
  - Assassin: colormap 140. Now: differs: GL's weapon turns grey-green, Hexenlicht's doesn't (`c4_item_invincibility`). 6.11: the own model shows it in a mirror floor (`cath_mirror_tint`); the weapon still doesn't.
  - Praevus: the Demoness is under 6.8.
- [ ] **Teleport**: `CE_TELEPORTERBODY` and `CE_TELEPORTERPUFFS` where the player leaves and arrives.
  - Now: not looked at.
- [ ] **Tome of power**: power mode on the player, and the weapons' tome modes above.
  - Now: see the weapons.
- [ ] **Ring of turning**: `CE_SM_WHITE_FLASH` on each missile it turns.
  - Now: not looked at.

### Monsters and bosses
- [ ] **The Riders' and the Eidolon's deaths**: `CE_RIDER_DEATH`.
  - Draws:
    - `RiderParticle` (`pt_rd`) in stages.
    - Beams `boss/circle.mdl` (special-trans, faces the view), `boss/shaft.mdl` (special-trans), `boss/star.mdl`.
    - `EF_BRIGHTLIGHT` and abslight 3 on the rider.
  - Where: rider1a (Famine), rider2c (Pestilence), romeric6 (War), meso9 (Death), eidolon; Praevus: `func_train_mp`'s soul skull, Praevus's death (beams only).
  - Now: not looked at.
- [ ] **The Eidolon**: several models and beams.
  - Draws:
    - Fireball `eidoball.mdl` (transparent, abslight) with `ring.mdl` smoke rings.
    - Flames `eidoflam.spr` with `EF_MUZZLEFLASH`.
    - The chaos orb `boss/chaosorb.mdl` (power mode) and its lightning (6.3).
    - Its spell: magic missiles with `glowball.mdl`, or polymorph.
  - Where: eidolon.
  - Now: not looked at.
- [ ] **Monsters' missiles**: models with abslight and trails.
  - Draws:
    - Archers' `akarrow.mdl`.
    - Imps' `shardice`, `fireball`.
    - The medusa's `snakearr.mdl` (homing, trail).
    - The mummy lord's invisible shot and its ground flames.
    - The skull wizard's `skulshot.mdl`.
    - Golems' `golemmis.mdl`.
    - The fallen angel's `faspell`, `fablade`.
    - The snake's `goop`.
    - Riders' `pestshot`, `famshot`, `boss/waraxe` (transparent), `boss/bone3` (transparent, translucent).
  - Where: see the trails and sparks above for maps.
  - Now: not looked at.

## 6.3 Beams and lightning (#66)
The client's stream entities (`cl_tent.c`) are beam segments made every frame, alias models at abslight 128. GL gives them no light; the sunstaff's player carries `EF_BRIGHTLIGHT`.

6.3 (DECISIONS X10–X14): the beams of light (sunstaff, lightning, color beam, Famine's, the gaze) glow by 6.2's rule for glowing projectiles (×16 their skin; the opaque ones cast no shadows), and each but the gaze is a line light along it, the sunstaff's hit a sphere, of the power its glowing surface shows. The chain and the ice chunks aren't light. Translucent parts (the sunstaff's sheath and hit glow, the lightning's last 0.25 s, the color beam) stay as 6.1 showed them until 6.4 blends them, now glowing: heavier. 6.3's look (2026-10-02): `effects_run.ps1` classes 2 and 5 unpaused with `r_effect_lights` 1 and 0, a paused A/B of the sunstaff, tower's lightning, rider1a's Famine against GL (TESTING.md "Beams (6.3)").
- [x] **Sunstaff**: `TE_STREAM_SUNSTAFF1`.
  - Draws: `stsunsf1`–`4.mdl` (the core, a translucent sheath, the ends) and `R_SunStaffTrail` particles.
  - Where: sunstaff (normal: 1 beam and up to 2 reflections; tome: 3 beams), `weather_sunbeam_start` (egypt2–5; Praevus: tibet1).
  - Now: glows and lights (6.3): a line light along each beam and reflection, a sphere at each hit (`c2_*_w4`, `w4t`; paused: the wall a beam hits 0.07 → 0.20, the dark wall by a far reflection 0.004 → 0.010). The tome's six beams fill GL's 128 segment entities. The translucent sheath and hit glow show nearly opaque and glow heavier than GL's translucent ones until 6.4. The weather sunbeams not looked at (triggered).
- [x] **Lightning**: `TE_STREAM_LIGHTNING`.
  - Draws: `stlghtng.mdl`; its last 0.25 s translucent, GL's level falling (abslight 128 + 192 × the time left, below 0).
  - Where: tomed warhammer (3 strikes on a hit); `light_thunderstorm` (rider1a, eidolon, tower); `weather_lightning_start` (egypt5, meso5–8, romeric1/3/5, eidolon; Praevus: tibet10, thomas); `trap_lightning` with TRACK; the Eidolon's chaos orb and death; Praevus: the tempest staff's ball, Praevus.
  - Now: glows white and lights (6.3): tower's always-running weather lightning (the pillar next to it 0.0087 → 0.0129), the tempest staff's bolts (`c5_*_w4`). The tomed warhammer's not caught (needs a hit, `c2_*_w1t`).
- [ ] **Chain**: `TE_STREAM_CHAIN`.
  - Draws: `stchain.mdl`.
  - Where: the tomed Staff of Set's four hooks (on a living target of at most 150 health), the Assassin's glyph (tripwire).
  - Now: not light (6.3, X10): lit by the world as in 6.1. Not looked at (needs a living target; the tripwire's is in `c4_item_glyph`).
- [ ] **Color beam**: `TE_STREAM_COLORBEAM`.
  - Draws: `stclrbm.mdl` (a transparent model), its skin the color.
  - Where: the bronze golem (cath, romeric1, romeric7, tower), the fallen angel lord (cath); Praevus: the cube of force.
  - Now: glows and lights by the same path (6.3; its five skins' powers made at map load). Not looked at: cath's golem didn't fire in 6.3's tries.
- [x] **Ice chunks**: `TE_STREAM_ICECHUNKS`.
  - Draws: `stice.mdl`.
  - Where: the tomed ice mace's blizzard (6 beams every 0.1 s).
  - Now: drawn as in 6.1, not light (6.3, X10) (`c2_*_w2t`).
- [ ] **Gaze**: `TE_STREAM_GAZE`.
  - Draws: `stmedgaz.mdl` (holey).
  - Where: the medusa (romeric1–4, romeric7).
  - Quirk (the same in GL): the gamecode writes the flags, the duration and then the entity; the client reads the entity first, so the beam takes a wrong entity, flags and duration.
  - Now: glows (6.3); no line light: a cutout shadows its own (X12, a masked light group is 6.13, #172); its light through bounces only. Not looked at (no `monster_medusa_*` in the maps' entity lumps).
- [x] **Famine's beam**: `TE_STREAM_FAMINE`.
  - Draws: `fambeam.mdl`, pulling `soulball.mdl`.
  - Where: Famine (rider1a).
  - Now: glows and lights (6.3): rider1a's start, Famine pulls the player in the first seconds after the map loads, against GL. Its fully red skin saturates to a flat bright red at ×16 (GL: a bright red ribbon with some shading; X10, a look for 7.1).
- [ ] **Shock ball**: a server entity.
  - Draws: `vorpshok.mdl` (a transparent model that faces the view), torch light mode, scale 2.5.
  - Where: tomed warhammer hits, lightning in water; the same model is the tomed vorpal's shock. Praevus: every lightning hit as `CE_LSHOCK` (6.8).
  - Now: differs: the model's black square (`c1_near_w2t`): transparent models' black texels are 6.4's (X4, its "transparent models" line). Not a stream: no light in 6.3.
- [x] **Light from beams**: none in GL. 6.3: a line light per beam of light, the sunstaff's hit a sphere (X11); the line checked against spheres in game (0.2367 against the analytic 0.2378); the tome's sunstaff 4.68 → 4.98 ms a frame (X14).

## 6.4 Translucency
GL draws `DRF_TRANSLUCENT` at `r_wateralpha` (0.33), as Hexenlicht does. Blending since 3.5b is R33.
6.4 (DECISIONS X15–X20): a translucent model's opacity is its entity's times its skin's (GL's per type: `EF_TRANSPARENT` clear at color 0 and 0.33 at odd colors, `EF_SPECIAL_TRANS`'s table, a translucent cutout's holes), a translucent surface glows at its opacity, a further translucent layer of another entity is passed by chance (shown at its opacity on average), a material file's glass replaces a window's blend. Kept: the blend in linear light (bright translucent things over dark backgrounds denser than GL's), windows lit (GL draws them unlit). 6.4's look (2026-10-02): `effects_run.ps1` classes 1–4 in both engines and `main`'s build, `translucency_run.ps1` (village1's windows, a window behind a window, shards, rings, the hand effect, the sheath) in all three, the test pack's glass on village1's panes (TESTING.md "Translucency (6.4)").
- [x] **Translucent models** (`DRF_TRANSLUCENT`).
  - Where:
    - Effect models: `axtail`, `vorpswip`, `ring.mdl` smoke rings, the ice mace's `iceshot2` corona, `handfx`, the scarab's wings.
    - Glass chunks, the teleport body.
    - Monsters: the fallen angel lord, the crystal golem (village2), frozen monsters.
    - Praevus: Praevus's shields and stars.
  - Now: blended (6.4): the vorpal swipe at a third without its black edges (`c1_far_w2`; its skin is 38 % clear), the smoke rings (`d1_rings`), the shards of a breaking window (`v1_shards`), the ice mace's shard (GL's purple halo is HoT's missile glow, X4). Denser than GL's over dark backgrounds (the blend in linear light: the swipe adds 35–70 levels, GL's 25; the axe's tail, `c1_*_w3t`). The hand effect is lit by its light, much brighter than GL's abslight 0.5 (X19). The crystal golem is dark in its room (GL: a fixed light level, X4). Not looked at: the teleport body, the fallen angel lords (not at the cathedral's spots without a trigger), Praevus's (6.8), frozen monsters (below). 6.14 (X25–X26): they shade the light through them at their opacity (`caustics_run.ps1`), the sunstaff's sheaths not their own line light (`d1_sheath`), the rings faintly (`d1_rings`).
- [x] **Transparent models** (`EF_TRANSPARENT`, texture alpha).
  - Models: `axblade`, `vorpswip`, `vorpshot`, `vorpshok` (faces the view), `vorpshk2`, `stclrbm`, `meteor`, `eidoball`, `boss/waraxe`, `boss/bone3`, `w_l3_c2`, `funnal` (also holey).
  - Now: blended by their skin (6.4): `vorpshok.mdl`'s black square and the swipe's black edges are gone (`c1_near_w2t`, `c1_far_w2`, against `main`'s); the shock is brighter than GL's, lit by the hit's light (GL: abslight 0.5, X19). The meteor staff's and the axe's in the run; the boss models not looked at (7.1). 6.14: they shade the light by their skin's opacity, not their whole mesh (X25).
- [ ] **Special-trans models** (`EF_SPECIAL_TRANS`, alpha through the particle table).
  - Models: `soulball`, `goodsphr`, `boss/circle`, `boss/shaft`.
  - Now: not looked at: 6.4 blends them by their table alone, two-sided, as GL (X15); no scripted way to them (the Riders' and the Eidolon's effects, the soul spheres): 7.1.
- [x] **Cutouts** (`EF_HOLEY`: 15 models in Hexen II; Praevus adds `snowleopard`, `succubus`, `sucwp4`, `lball`, and its `ball.mdl` is holey, not facing the view).
  - Models: `star`, `ring`, `arrowhit`, `handfx`, `glowball`, `booberry`, `stmedgaz`, `imp`, `webs`, `megaweb`, `icestaff`, `mezzoman`, …
  - Now: drawn since 2.4b (G7); the ice staff in the hand too. 6.4: a translucent cutout (`ring`, `handfx`) keeps its holes (it is in the transparent group, which now tests them).
- [x] **Translucent sprites** (`DRF_TRANSLUCENT`, `EF_TRANSPARENT`; alpha 0.33).
  - Where: smoke, flashes, teleport puffs, fire.
  - Now: drawn: blended by their alpha × 0.33 as effects (G10), unchanged by 6.4; seen in 6.2's unpaused runs (flashes, fire) and the crossbow bolt's hit (`v1_shards_20`).
- [x] **The invisible player** (`svc_set_view_flags` `DRF_TRANSLUCENT` on the weapon; `EF_NODRAW` and `EF_LIGHT` on the player).
  - Where: invisibility, the Assassin's cloak.
  - Now: drawn (`c*_item_invisibility`: the weapon is translucent; 6.4 the same as `main`'s; 6.6 draws GL's grey tint).
- [ ] **Frozen and stoned**.
  - Draws: frozen is skin 101 (ice), colormap 159 → 144, translucent, abslight; Praevus: the FROZEN spawnflag, waking 144 → 149. Stoned is skin 100 (the medusa, the Crusader's invincibility).
  - Now: 6.15 (#177, X33–X34): frozen is solid ice, after the freeze's 1.5 s tint (the blend): refraction in and out at 1.31, Fresnel, absorption along the path in the skin's hue (`r_ice 0`: 6.4's blend); the crystal golem too. `vk_freeze` freezes the monster in front (the ice mace freezes only a flesh monster it hits at 10 health or less); `ice_run.ps1`: demo1's archer from the front and the side, one behind village1's window, one in demo1's pool, the golem in its dark alcove (nearly invisible, as dark as GL's), `glh2` from the same saves. 6.19 (#191, X49): the cutout monsters too (the imp, the were-jaguar and were-panther, Portals' were-snow-leopard and were-tiger, the Demoness frozen in deathmatch), which 6.15 left at the blend; `ice_run.ps1`'s meso2 were-panther. Praevus's FROZEN spawnflag is used by none of its maps. Stoned not looked at: 7.1.
- [x] **Translucent brush entities** (`DRF_TRANSLUCENT` brushes, alpha 0.33), and a material file's glass on them (M33).
  - Where: breakable windows.
  - Now: blended and lit (`v1_bay_*`): GL draws them unlit, so its panes are darker (village1's in daylight 8-bit 45 against 75, X19, a look for 7.1); a window behind a window (`v1_side_l`, `_r`: the panes `*44`, `*46` on one ray) shows the far one at its opacity, the noise of a single frame 1.0–1.1 levels RMS (`main` 0.8), none seen while turning. With the test pack's `kind glass` on `rtex199` the panes are glass alone (X18; `main`: a third). 6.14 (X25): they let 0.67 of the light through (a test light behind the bay pane, `v1_bay_light`; the sun, `v1_bay_sun`); the test pack's glass tints it.

## 6.5 Water
6.5 (DECISIONS X21–X24): with `r_water 1` a liquid's horizontal surface against the air is physical water: Fresnel reflection, refraction (Snell's window and total internal reflection from below), GL's texture opacity kept as a layer (0.33 for `*rtex078` and `*lowlight`; opaque water stays its texture under the reflection), waves from Hexen II's turbulence; the liquids are a medium (extinction, GL's contents color lit as a model at the camera) instead of GL's tint and Quake II RTX's absorption. Vertical liquid faces stay walls (R31). 6.5's look (2026-10-02): `water_run.ps1`'s 21 views in GL and Hexenlicht with `r_water 0` and 1 (TESTING.md "Water (6.5)").
- [x] **Water and slime surfaces**: GL draws them opaque, warped and unlit (the translucent `*rtex078` and `*lowlight` at 0.33); before 6.5 Hexenlicht kept them so (R31).
  - Where: horizontal water on 33 of the 59 maps, 92 % of it translucent; no map has slime.
  - Now: physical water (6.5). From above, the texture layer over the pool's floor: romeric3's and demo1's blue water as GL's (before 6.5, lit at its own dark surface, it nearly vanished); demo2's deep pool's purple is dimmer than GL's (lit as its dark floor 240 units down). Reflections at grazing angles; torches glint in the waves (romeric1, romeric3). castle4's opaque water reflects. egypt's vertical water walls as before. 6.14 (X27): the light through the surface shows the waves' pattern (soft blobs, subtle: `d1_pool_light`, `d2_pool_light`, `r3_pool_light`); the texture layer passes the light. 6.16 (X29): the pattern is the water texture's, at `r_water_caustics` 3: visible under demo1's water (its walls, lit from above) and on romeric3's walls above the waterline (its lights in the pool); 6.14's sine pattern was invisible.
- [x] **Under water**: GL's liquid tints (`V_SetContentsColor`: water 130 80 50 at 128/255, slime 0 25 5 at 150/255, lava 255 80 0 at 150/255); the fog is new, not GL's.
  - Now: the medium (6.5, X22): a brown fog growing with distance, lit as a model at the camera; the surface above seen through Snell's window with its texture layer, the walls mirrored outside it. Darker than GL's tint in a dark pool (demo2), close to its mood in lit ones (demo1, romeric3). GL's tint is left out of 6.6's view blends. 6.14 (X27): the light dims by the medium on its path too (a pool's floor 0.91–0.92× in the direct light), darker than GL's light maps. 6.17 (X31): the medium's glow is steady along demo2's moat: GL's light level averaged around the eye in the liquid and eased over 0.5 s (before, the light map under the player made it come and go, 24–128). 6.18 (X43): lit where it is, from above too: GL's light level of the water itself (the liquids' light grid), so a lit stretch of the moat glows and a dark one stays dark, and the whole medium no longer flips as the player walks over light map patches (`main` up to 65 % between frames from above, now 9 %).
- [x] **Lava**: emissive (R82), its light polygons.
  - Where: meso8, the lava maps.
  - Now: unchanged by 6.5 (`m8_lava_above`). Just under its surface the lava's medium (GL's 255 80 0) is thin at the eye; the lava's underside glows as before (`m8_lava_under`).
- [x] **Bubbles**: `s_bubble.spr` (translucent, abslight).
  - Where: drowning and dying in water; Praevus: `air_bubbles`; the crossbow's bolts in water.
  - Now: drawn: the crossbow's bolts under demo1's pool raise bubbles as in GL (`d1_078_bubbles`), dimmed by the medium (the muzzle flash brightens the medium for a moment; since 6.18 around the flash, by GL's rule for a model's dynamic light, not the whole medium). Drowning and Praevus's `air_bubbles` not looked at (the same sprite).

## 6.6 View effects
GL's view blends (`V_CalcBlend` over `cl.cshifts`, drawn by `R_PolyBlend` over the 3D view, the weapon included, the 2D not). 6.6 (DECISIONS X37–X39) draws them as GL: its blend function on the 8-bit colors before `gamma`, in the composite (`gl_polyblend`). Since 6.5 the contents shift (under water, slime, lava) is the liquids' medium: left out of the blends (X22). 6.6's look (2026-10-04): `blend_run.ps1` in both engines (TESTING.md "View blends (6.6)"): every blend's numbers through `v_cshift` on a paused frame, each against the same frame without it: GL's blend function in both engines, within the noise between frames (the same blend in linear light would be 6–47 levels off); the flashes and power-ups in play.
- [x] **Damage flash** (`svc_damage`): 200 100 100 / 220 50 50 / 255 0 0 by armor, up to 150/255 (percent is out of 255).
  - Where: every hit on the player.
  - Now: drawn (6.6): the three colors as GL's (`dmg_armor_1`, `dmg_mixed_1`, `dmg_blood_1`); an archer's arrows flash the view red in both engines (`dmg_NN`).
- [x] **Bonus flash** (`bf`).
  - Where: pickups; Praevus: the tempest staff's shot, glyph poison.
  - Now: drawn (6.6): `bf_01`, `bf_10` as GL's; under demo1's water without the water's brown (`water_bf_01`).
- [x] **Dark flash and white flash** (`df`, `wf`).
  - Where: the hydra's blinding; Praevus's death.
  - Now: drawn (6.6): the view black or white, fading over 2.5 s as GL's (`df_*`, `wf_*`). The hydra's and Praevus's not looked at (the same commands; 7.1).
- [x] **Power-up tints**: invisibility 100 100 100 at 100/255; invincibility 255 255 0 at 30/255 (the Icon of the Defender's yellow-green); frozen 20 70 255 at 65/255; stoned 205 at 80/255; divine intervention (white).
  - Where: the artifacts; frozen by another player's ice mace; stoned by the medusa's gaze; divine intervention when the Crusader's god saves him and on the players in a tomed ice mace's blizzard (white for a frame, then fading as the bonus flash).
  - Now: drawn (6.6): the Icon of the Defender and invisibility in play (`invincibility`, `invisibility`), as GL's; frozen and stoned with their numbers through `v_cshift` (`frozen_1`, `stoned_1`), not triggered in play (7.1).
- [x] **Underwater warp**: GL (HoT) has none by default (`r_waterwarp 0`; Raven's GL client wobbled the vertices of the surfaces in water, HoT made it a cvar); the software renderer warps the screen.
  - Now: not drawn, the owner's choice (2026-10-04, DECISIONS X38): the look under water is 6.5's medium and refraction, closer to physically based.

## 6.7 Screens
GL's own screen code (`gl_screen.c`) over `vk_draw.c`'s port of `gl_draw.c`, and upstream's demo code (DECISIONS X40). 6.7's look (2026-10-04): the owner, from saves made right before each (TESTING.md "Screens (6.7)").
- [x] **Intermissions** (`svc_intermission`, `cl_inlude.c`: a 320x200 picture stretched over the screen, text typed out; no 3D view meanwhile).
  - Where: 1–4 after the riders (meso, egypt, roman, castle); the finale 6 → 7 → 8 after the Eidolon (end-1–3, white text after a delay); Praevus: 10 after Praevus (mpend), 11 into the Tibet hub (mpmid, keep5), 12 its opening from the menu (end-3, no server); 5 and 9 only in the demo and OEM versions.
  - Now: drawn (6.7): 11 into tibet1 and 12 into keep1 looked at by the owner, as GL's code draws them. The bosses' screens (1–4, 6–8, 10) not looked at: a boss kill each, the same code with other pictures and text flags (7.1).
- [x] **Cutscenes** (`camera_remote`: `svc_setview` to the camera for its `wait`, `svc_setangle` or `svc_setangle_interpolate`, the weapon hidden by the gamecode, the "bf" flash). `svc_finale` and `svc_cutscene` are Quake's, commented out in Hexen II's `cl_parse.c`; the finale is intermissions 6–8.
  - Where: 17 in Hexen II (castle4, romeric1–4), 48 in Praevus (keep1–5, tibet1–10, thomas).
  - Now: drawn (6.7): castle4's and keep1's looked at by the owner (the player's body in view, 6.11). The crosshair hidden while a camera shows the view (GL shows it; X41).
- [x] **Plaques** (`svc_plaque`, 2D).
  - Now: drawn (6.7): demo1's looked at by the owner.
- [x] **Demo playback** (`playdemo`, `timedemo`, `record`). Hexen II ships no demos (`startdemos` is commented out in `hexen.rc`); Praevus's intro is `t9.dem` (thomas).
  - Now: drawn (6.7): `t9.dem` and a recorded Hexen II demo play through (validation 0/0), looked at by the owner. The view's angles are interpolated over the demo's changes (X42): t9's cameras turned in 0.1 s steps (in GL too).

## 6.8 Portal of Praevus
What only the mission pack draws (its gamecode in `gamecode/hc/portals`). The shared effects above apply to it too.

### The Demoness (class 5, the run's `c5_*`)
- [ ] **Demoness 1, blood rain**.
  - Normal: `sucwp1p.mdl` (fire flicker, scale 1.3) shrinking away; hit `CE_BLDRN_EXPL`; `SpawnPuff`; `EF_MUZZLEFLASH`.
  - Tome: 3 bouncing missiles, the middle one spinning; bounce `CE_BRN_BOUNCE`.
  - Now: differs: GL's red missile glow is missing, and the missile shows as a small orange speck (`c5_far_w1`, `w1t`).
- [ ] **Demoness 2, acid rune**.
  - Normal: `sucwp2p.mdl` (`EF_ACIDBALL` trail and a light of radius 100–120, green with colored lights); hit `CE_ACID_HIT`.
  - Tome: a blob (scale 2.5) trailing `CE_ACID_MUZZFL`; bursts with `TE_EXPLOSION` and `CE_ACID_EXPL`, then 3–10 drops (`CE_ACID_SPLAT`, `CE_GREEN_SMOKE`); in DM and co-op, `CE_CHUNK` acid.
  - Now: drawn (`c5_*_w2`, `w2t`). The tomed blob's light on the pedestal is faint in glh2 (R83); GL's green missile glows are missing (`c5_far_w2t`).
- [ ] **Demoness 3, fire storm**.
  - Normal: an invisible missile (`null.spr`, `EF_DIMLIGHT`) trailing 2 `CE_FLAMESTREAM` and ground fire `CE_FIREWALL_*`. A target hit gives `CE_FBOOM` and may set it alight (`CE_LG_EXPLOSION`, then `CE_ONFIRE`); a wall gives `CE_BOMB`.
  - Tome: a spiral swarm; on a target, flame balls (`sucwp1p.mdl` frame 4, abslight 0.5) rain for 3 s.
  - Now: paused shots show nothing (X9: a paused game draws no client effects). 6.2, unpaused: the trails, the ground fire and the impacts are drawn and emit, up to 19 lights (`c5_far_w3`); the tome's swarm not looked at.
- [ ] **Demoness 4, tempest staff**.
  - Normal: a homing ball `lball.mdl` (`EF_DIMLIGHT`, scale 0.75), the "bf" flash; the impact gives `CE_LBALL_EXPL` and 3–4 `TE_STREAM_LIGHTNING` bolts (`CE_LSHOCK` on targets).
  - Tome: a chain of `TE_STREAM_LIGHTNING_SMALL` between targets within 1000 units; with no target, 3 branching arcs.
  - Now: drawn: the ball, the bolts and the tome's arcs (`c5_*_w4`, `w4t`). 6.3: the bolts and arcs glow and light (line lights).
- [ ] **The Demoness's glyph and invincibility**.
  - Glyph: a gas grenade `glyphwir.mdl`; `CE_ACID_EXPL`, then `CE_GREEN_SMOKE` every 0.1 s for up to 30 s; poisoned players get "bf" every second.
  - Invincibility and DM spawn protection: translucent and abslight, `svc_particle2` FIREBALL 416 and REDFIRE 135 every frame, `EF_BRIGHTFIELD` (draws nothing: its call is commented out in `cl_main.c`, in GL too).
  - Now: the invincibility's particles are drawn (`c5_item_invincibility`); the glyph's gas was not looked at (at the feet).

### Praevus effects
- [x] **`CE_FLAMESTREAM`**: `flamestr.spr` (translucent, abslight 255).
  - Where: the fire storm's trails, the flame balls.
  - Now: emits (6.2): the fire storm's trails (`c5_far_w3` unpaused, X9: paused shots draw no client effects).
- [x] **`CE_FIREWALL_SMALL`, `_MEDIUM`, `_LARGE`**: `firewal1`, `firewal5`, `firewal4.spr`.
  - Where: the fire storm's ground fire; Praevus's fire wave (medium, large).
  - Now: emits (6.2): the fire storm's ground fire, up to 19 lights in a frame (`c5_far_w3` unpaused).
- [ ] **`CE_FLAMEWALL`**: `firewal1.spr` (opaque).
  - Where: Praevus's fire wave and fire pillars (tibet10).
  - Now: emits (6.2); not looked at.
- [ ] **`CE_ONFIRE`**: `firewal1`–`3.spr` (translucent), puffs for 5–10 s.
  - Where: victims the fire storm sets alight.
  - Now: emits (6.2); not looked at (needs a target).
- [x] **`CE_FBOOM`**, **`CE_BOMB`**: `fboom.spr`, `pow.spr`.
  - Where: fire storm impacts.
  - Now: emit (6.2): the tomed fire storm's impacts (`c5_*_w3t` unpaused).
- [x] **`CE_ACID_HIT`**, **`CE_ACID_SPLAT`**, **`CE_ACID_EXPL`**, **`CE_ACID_MUZZFL`**: `axplsn_2`, `axplsn_1`, `axplsn_5.spr` (abslight 255), `muzzle1.spr` (translucent, abslight 51).
  - Where: the acid rune.
  - Now: emit (6.2; `c5_*_w2`, `w2t` unpaused: the hit, the burst).
- [x] **`CE_BLDRN_EXPL`**, **`CE_BRN_BOUNCE`**: `xplsn_1.spr`, `spark.spr`.
  - Where: blood rain, the pentacles' spit.
  - Now: emit (6.2; `c5_*_w1`, `w1t` unpaused: nine lights with the tome).
- [x] **`CE_LBALL_EXPL`**: `Bluexp3.spr`.
  - Where: the tempest staff's ball.
  - Now: emits (6.2; `c5_*_w4` unpaused).
- [ ] **`CE_LSHOCK`**: `vorpshok.mdl` (torch light mode, scale 255, rolled 90°).
  - Where: every lightning hit on a target (it replaces Hexen II's server shock ball), zaps under water.
  - Now: not looked at (needs a target).
- [ ] **`CE_FLOOR_EXPLOSION3`**: `biggy.spr`.
  - Where: Praevus shrinking and growing, his fire pillars.
  - Now: emits (6.2); not looked at.
- [ ] **`CE_GRAVITYWELL`**: `GravityWellParticle` (`pt_gravwell`).
  - Where: Praevus teleporting (colors 208–223, 0.8 s) and recharging (128, 3 s).
  - Now: not looked at.
- [ ] **`CE_SNOW`**: `R_SnowEffect` (`pt_snow`, GL's snow textures and sizes from the count).
  - Where: `weather_snow` (tibet1–4, tibet9, thomas; TESTING.md: tibet9 looking up under the ceiling's opening).
  - Now: not looked at in 6.1 (drawn since 2.5).
- [ ] **`CE_CHUNK`**: client chunks with their trails.
  - Draws: glass (translucent), wood, metal, flesh, stone, clay, leaves, hay, cloth; ice `shard.mdl` (translucent, abslight 127, `rt_ice`); acid `sucwp2p.mdl` (`rt_acidball`); meteor `tempmetr.mdl` (`rt_smoke`); blood (`rt_bloodshot`, `pt_darken`).
  - Where: every gib and debris in DM and co-op; ice and stone shatter always; the meteor and acid bursts in DM and co-op.
  - Now: not looked at.
- [ ] **`TE_STREAM_LIGHTNING_SMALL`**: `stltng2.mdl` (the end translucent and brighter).
  - Where: the tomed tempest staff.
  - Now: drawn (`c5_*_w4t`). 6.3: glows and lights by the lightning's path (DECISIONS X10–X11); this line is 6.8's.

### Praevus monsters and the world
- [ ] **Praevus** (`monster_buddha`, tibet10).
  - Draws:
    - Translucent while shrinking.
    - Shields `shield.mdl` (translucent, abslight).
    - Stars `budd_star.mdl` (translucent, power mode, `EF_ICE`).
    - The fire wave's wind-up (`svc_particle2` BLOB 424); reappearing (`svc_particle4` SLOWGRAV 498).
    - His thunderstorm (`TE_STREAM_LIGHTNING`).
    - His death: the rider death's beams, `EF_BRIGHTLIGHT`, the "wf" white flash.
  - Now: not looked at.
- [ ] **Yakman** (tibet2–9, keep5).
  - Draws: snowballs `yakball.mdl` (abslight 0.5, `EF_ICE`) with `CE_ICEHIT` and freezing; skid dust (particle 344) and `CE_WHITE_SMOKE`; `SpawnPuff`.
  - Now: not looked at.
- [ ] **Pentacles** (`monster_pentacles`, keep3–4, tibet1/3/4/7).
  - Draws: an `EF_NODRAW` entity with `pent.mdl` attached; spit `sucwp1p.mdl` (fire flicker) → `CE_BLDRN_EXPL`; exploding, `TE_EXPLOSION`, the explosion sprites and meteors.
  - Now: not looked at.
- [ ] **Ice archer, snow leopard, weretiger**.
  - Draws: blue arrows `akarrow2.mdl` → `CE_BLUESPARK`; the werecats' effects.
  - Now: not looked at.
- [ ] **Praevus lights and the world**.
  - Draws:
    - `light_newfire` (`newfire.mdl`, translucent, fire flicker).
    - `light_candle`, `light_burner`, `light_lantern`, `light_palace_torch` (abslight 0.75 with `flame2.mdl`); flame lights are no longer static outside DM.
    - `func_train_mp`'s soul skull (the rider death).
    - `weather_dust` (`svc_particle4` FASTGRAV), `air_bubbles`, `trigger_rubble`'s chunks, `func_obstacle` (translucent, abslight).
  - Now: not looked at.

## 6.12 GL's near plane (#169)
- [ ] **Particles at the eye**: GL's near plane clips particles within 4 units of the eye; Hexenlicht's effect rays start at the eye.
  - Where: haste's dark field around the player.
  - Now: differs: large soft blobs (`c*_item_haste`).
- [ ] **Models at the eye**: GL clips everything within its 4-unit near plane; Hexenlicht's primary rays start at the eye (only the weapon's ray starts at the near plane).
  - Where: the Necromancer's proximity mine, which the gamecode spawns 6 units below the eye, the eye inside its model.
  - Now: differs: the view goes black (c3_item_glyph).

## Unused (listed, not ticked)
- **`CE_QUAKE`** (`R_RunQuakeEffect`): War's arena quake returns before starting it.
- **Never started:** `CE_SLOW_WHITE_SMOKE`, `CE_TELESMK2`, `CE_FLOOR_EXPLOSION2` (`flrexpl2.spr`); Praevus: `CE_FLAMEWALL2`.
- **Commented out in the client:** `TE_TAREXPLOSION` (the gamecode defines it, never sends it), `TE_EXPLOSION2`; `svc_sellscreen` in `cl_parse.c`.
- **Never sent:** `TE_SUPERSPIKE`, `TE_WIZSPIKE`, `TE_KNIGHTSPIKE`, `TE_LAVASPLASH` (`R_LavaSplash`), `TE_LIGHTNING2`, `TE_LIGHTNING3`, `TE_STREAM_SUNSTAFF2` (`stsunsf5.mdl`); `svc_set_view_tint` (the weapon's `colorshade`: the client reads it, no server writes it; found in 6.6).
- **Sent, but draws nothing:** `TE_LIGHTNING1` (`trap_lightning` without TRACK); the client reads it and draws nothing, in GL too.
- **Trail flags on no model:** `EF_TRACER2` (`rt_tracer2`); `EF_BLOODSHOT` is used only by Praevus's `CE_CHUNK`.
- **`r_part.c` functions nothing calls:** `R_ParticleExplosion2`, `R_BlobExplosion`. `R_EntityParticles` for `EF_BRIGHTFIELD` is commented out in `cl_main.c`, so the Demoness's bright field draws nothing in GL either. `svc_particle3` has no gamecode caller.
- **Particle types the gamecode never sends:** STATIC, FIRE, EXPLODE, BLOB2, RAIN, C_EXPLODE2 (the engine's own effects use some).
- **`EF_TEX_STOPF`, `EF_TEX_STOPL`** (Praevus's `trigger_ani_event`): no map uses them. The server sends `effects` as a byte, which would cut them.
- **Gamecode dead code:** `T_MissileTouch`'s explosion, `launch_mumshot2`, the mezzo shield branch in `damage.hc`, the rider death's circle beam at `count == 3`, Hexen II's `fx_colorbeam_start`, `weather_dust` and `fx_particle_explosion`, the water splash `wsplash.spr`.
