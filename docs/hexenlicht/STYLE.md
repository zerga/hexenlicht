# Hexenlicht — art direction of the texture pack (E9)

What E9's redrawn textures should look like, hub by hub: the rules the labels in
`tools/hexenlicht/texpack/materials.csv` follow (story 9.3; PLAN E9, DECISIONS M57–), and
what 9.5's reviews judge the pack against. The labels are the per-texture form of this
document; `families.csv` lists the families named here.

## 1. Common rules

**The theme is Raven's.** A redraw keeps each texture's arrangement, colors and motifs and
draws them as a real material: the stone Raven painted as ochre blocks becomes ochre
limestone with chisel marks, not a different wall. The palette is the theme (DECISIONS M48:
the original's broad colors are put back after the redraw); the descriptions name the
originals' hues.

**Real scale.** A world unit is about 3.1 cm (the player, 56 units, is 1.75 m), a texel is a
unit times the census's `scale`: a 64-texel texture at scale 1 is 2 m across. Descriptions
give sizes in metres (blocks, planks, panels), so that a redraw draws a brick as a brick.

**Condition.** Every hub is old, abandoned or haunted: worn edges, grime in the joints, damp
where the hub is damp, dust and sand where it is dry, frost where it is cold. Blood stays
where Raven painted it (`rtex412~ca21`), as dried blood. Nothing is new or polished unless
it is metal or marble meant to shine.

**No light in the description.** The light is the path tracer's; the redraw's painted light
is taken out by numbers (M47). Descriptions say what a surface is, not how it is lit ("dark"
is a color here).

**No readable text.** Lettering and inscriptions are illegible (M51: else the model writes
readable blackletter); runes and glyphs keep Raven's shapes.

### Tiers (how far a redraw may depart)

| Tier | For | |
|---|---|---|
| `reimagine` | fill: a wall's, floor's or ceiling's surface of one material and its plain variants | a new surface at the same arrangement |
| `layout` | trims, pillars, panels, doors, grates, carvings and reliefs (ornament and figures) | every shape stays where it is |
| `faithful` | painted pictures, signs and inscriptions, symbols (runes, switch faces), painted props (books, bottles) | restored, no new content |
| `glass`, `liquid`, `lava` | windows; water, slime and other liquids; lava | their own prompts (M51) |
| `skip` | tool textures (`clip`, `origin`, `trigger`), blank fills, the skies (5.8's rule) | left as they are |

A texture between two tiers takes the stricter one. Skins (`skin`, `fx`, `skip`) are 9.6's (§9).
Carvings are `layout`; an intricate painted relief (Mazaera's polychrome panels, the sun
stone) is `faithful`. Windows are tier and class `glass` (see-through: the class writes the
`.mat`'s `kind glass`) only where 5.8 made them glass (`rtex018`, `rtex083` and its copy
`rtex467`); the painted windows found in 9.3 stay opaque (`layout`, class `prop`; owner,
2026-10-10); 6.20's reflective `rtex199` stays as it is (`skip`).

### The labels

`purpose`, `tier`, `class`, `family`, `regions`, `description` per texture (README "Labels";
the words in `classes.toml`'s `[labels]` and `[materials]`):

- **class** is the pipeline's (roughness range, specular, prompt noun): the main material.
- **regions** say which of the palette's ramps is which material (`labels.py`). Raven shaded
  a material over several ramps (an oak door's planks in orange, amber, brown and tan, its
  iron in grey and taupe), so a region is a set of ramps; `*` is the rest (`*=stone` for a
  texture of one material, `*=stone;yellow=gold`). A ramp that carries two materials takes
  `a/b`, the larger first. The saturated row (`special`: glowing eyes, gems) is kept as drawn.
- **description**: 15–40 words: the material and its arrangement (how many blocks, planks,
  panels and their size in metres), the colors, the motif, the condition; every part of a
  texture of several materials named (M51: a part the description leaves out is reinvented).
- **family**: textures that should read as one material in the hub, typically a wall's
  stone with its trims, cracked, mossy and bloody variants and the same stone in other
  shapes; each gets one hero in 9.5, whose redraw is the others' reference (9.4). Pictures,
  signs and one-off panels stand alone (no family). A family belongs to the hub of its lead;
  a texture of another hub joins it when it is that material.
- **Copies** (the same pixels under two names: about 30, mostly the mission pack's renames of
  the original game's textures, `ttex022` is `rtex041`) carry one label, the one of the copy
  that covers the most area; `labels check` holds them to it. Animations aside: two can share
  a first frame and differ after it. Near-copies (`ttex020` keeps 99.98 % of `rtex028`'s
  texels) are aligned by hand.

## 2. Blackmarsh (demo1–3, village1–5, rider1a)

A damp castle and its village in a northern marsh: Norman and Celtic, the Four Horsemen's
first seal. Grey, brown and black, with heraldic reds, blues and gold in the cloth.

- **Stone:** dark grey rounded fieldstone in black mortar, roots and vines growing through
  it; brown-tan rough-hewn ashlar for the castle; Celtic knotwork carved into slabs and inlaid
  in red and white.
- **Wood and iron:** dark oak planks and boards, half-timbered frames, heavy doors with
  wrought-iron straps, studs and knot bosses; crates and barrels.
- **Cloth:** woven rugs with knotwork borders, heraldic banners (lions, griffins, dragons).
- **Pictures:** the Four Horsemen in grey stone relief (`rtex013`, `rtex014`, `rtex062`,
  `rtex063`), carved figures (`rtex081`, `rtex091`).
- **Props:** the shops' shelves: bottles, tools, meat and fish, jars (painted, faithful).
- **Condition:** wet, mossy, grimy; mud and grass outside.

Families: `bm-fieldstone` (grey fieldstone walls and their root and vine variants),
`bm-ashlar` (the castle's brown ashlar and its cracked and carved variants), `bm-oak`
(planks, boards, crates), `bm-doors` (oak and iron doors and shutters), `bm-timber`
(half-timbered frames), `bm-celtic` (knotwork slabs and inlays), `bm-horsemen` (the four
reliefs), `bm-rugs` (woven rugs and tapestries), `bm-banners` (heraldic banners),
`bm-drapes` (curtains and hangings), `bm-ground` (mud, earth, grass, thatch), `bm-shelves`
(the painted props on dark shelves, the Keep's and Tulku's books and bottles too).

## 3. Mazaera (meso1–9, rider2c)

A Mesoamerican temple city in the jungle: Maya and Aztec, the seal of Famine. Ochre and tan
limestone, faded red pigment, jade green, gold; lava below.

- **Stone:** ochre and tan limestone blocks and slabs, worn and cracked; carved step-frets,
  meanders and glyph panels; jade-green carved stone; red-brown cracked rock and earth.
- **Carvings:** idol masks and faces, totem pillars of stacked figures, feathered serpents,
  the sun stone (`mtex472`, `mtex478`) and red-painted sun stars.
- **Nature:** jungle vines, moss and leaves, roots.
- **Condition:** weathered, moss in the joints, red paint faded and flaking, old blood.

Families: `mz-limestone` (the ochre blocks and slabs), `mz-frets` (carved step-frets,
meanders and glyph panels), `mz-idols` (masks, faces and totem figures), `mz-sun` (the sun
stars and calendar stones), `mz-jade` (green carved stone), `mz-redrock` (red-brown rock
and cracked earth, Tulku's earth floors too), `mz-jungle` (foliage, vines, moss),
`mz-darkstone` (dark rough speckled stone), `mz-webs` (cobwebs over darkness; Septimus uses
the same image, which may be drawn translucent).

## 4. Thysis (egypt1–7)

Ancient Egypt, tombs and temples of the New Kingdom; the seal of Pestilence. Tan sandstone,
painted plaster in ochre, lapis blue, malachite green, red and white, gold.

- **Stone:** tan sandstone blocks in courses, sand-blasted, fine joints; incised and raised
  hieroglyph reliefs; colossal statues' parts (heads, hands, feet).
- **Painted:** wall paintings on plaster (figures in profile, offerings, columns of
  hieroglyphs, cartouches), winged scarabs and falcons, starry blue ceilings, striped
  headdress cloth.
- **Gold:** coins and treasure, gilded details.
- **Condition:** dry, dusty, chipped paint, sand in the joints; blood where Raven put it.

Families: `th-sandstone` (the sandstone blocks and their cracked and bloody variants),
`th-glyphs` (carved hieroglyph reliefs and panels), `th-murals` (painted plaster, which
stand alone where they are scenes; the family keeps their plaster and paint alike),
`th-lapis` (blue-painted stone, starry ceilings, stripes), `th-winged` (winged scarabs and
falcons), `th-statues` (colossi's parts), `th-granite` (dark grey granite beams, slabs and
frames), `th-tiles` (patterned small-tile floors).

## 5. Septimus (romeric1–7)

A Roman city; the seal of War. White and grey marble, grey dressed stone, terracotta, bronze;
Pompeian red and ochre in the frescoes.

- **Stone:** veined white, grey and pink marble; grey dressed blocks and small bricks;
  classical moldings (egg-and-dart, dentils, meanders, beads, cornices).
- **Carvings:** reliefs of figures and processions, lion and gorgon masks, an equestrian
  statue, garlands.
- **Pictures:** Pompeian frescoes (`rtex407`, `rtex408`, `rtex498~be8a`), Latin
  inscriptions (illegible).
- **Other:** terracotta roof tiles and floor tiles, bronze-studded doors, drapes and banners,
  the elements' rune switches (air, fire, steam, water).
- **Condition:** aged, cracked marble, stains, ivy and grass in places.

Families: `sp-marble` (veined marble), `sp-greystone` (grey dressed blocks and bricks),
`sp-moldings` (trims and cornices), `sp-reliefs` (figurative reliefs and statues),
`sp-masks` (lion and gorgon masks, rosettes), `sp-terracotta` (tiles), `sp-drapes` (curtains
and banners), `sp-elements` (the rune switches), `sp-brownstone` (warm brown-taupe dressed
blocks and slabs), `sp-painted` (painted Pompeian wall plaster and its panels), `sp-ground`
(grass, soil, meadow, hedge).

## 6. The Eidolon finale (castle4–5, cath, tower, eidolon)

Eidolon's cathedral and fortress: Gothic, the darkest hub. Near-black grey stone, dark oak,
stained glass, blood.

- **Stone:** dark grey and black castle blocks, flagstones; Gothic vault ribs, columns,
  quatrefoil tracery.
- **Other:** crates, organ pipes, dark iron, stained-glass windows (`rtex018`, `rtex083`).
- **Condition:** soot, damp, blood.

Families: `ef-blackstone` (the dark castle blocks and their variants), `ef-gothic` (ribs,
columns, tracery), `ef-crates` (crates and boards), `ef-iron` (riveted and perforated rusted
iron plates), `ef-organ` (organ pipes). The mission pack's test map (`thomas`, hub `other`)
has `ot-brick` and `ot-mottled`; the deathmatch maps' textures join the families of the hubs
they resemble.

## 7. The Keep (keep1–5, the mission pack's finale)

Praevus's keep: a dark medieval castle in the snow, with Blackmarsh's and the finale's
textures beside its own. Grey and brown stone, dark timber, bone, snow and ice.

- **Stone:** big rough-cut grey-brown limestone blocks, grey castle stone and bricks, rough
  rock; carved demonic reliefs (skulls, bats); Celtic knots.
- **Other:** timber frames and beams, bone piles, snow and icicles over rock, a library's
  books and bottles, the red dragon's picture (`rtex493`).
- **Condition:** cold, frost, dust, bones.

Families: `kp-ashlar` (the big limestone blocks), `kp-greystone` (grey castle stone and
bricks), `kp-beams` (timber frames and beams), `kp-bones` (bone piles), `kp-snow` (snow and
ice over rock), `kp-ground` (earth and stony ground), `kp-redstone` (speckled deep red
stone), `kp-demons` (carved demonic reliefs); its books and bottles are `bm-shelves`, and many
of its textures join Blackmarsh's families (`bm-celtic`, `bm-oak`, `bm-timber`, `bm-doors`).

## 8. Tulku (tibet1–10, the mission pack)

A Himalayan monastery and its mountain: Tibetan. Ochre and red-painted timber, lacquered and
gilded carving, square stone flagstones, snow.

- **Stone:** square flagstones in grey, green, blue and ochre; rounded cobbles and
  fieldstone; ochre brick and blocks; rough mountain rock.
- **Timber:** red-brown painted beams, friezes and lattices, carved doors with dragons, dark
  planks.
- **Carvings:** wrathful deities and guardian lions, dragons, endless knots and other
  Buddhist ornaments, gilded and painted.
- **Other:** the switches' symbol discs, red lacquer and cloth, books, chains.
- **Condition:** cold and dry, snow on top of walls and floors, soot, worn paint.

Families: `tk-flagstone` (the square slabs), `tk-cobbles` (cobbles and fieldstone),
`tk-brick` (ochre brick and blocks), `tk-rock` (rough rock faces), `tk-snow` (snow and ice,
and the walls under it), `tk-timber` (red-brown painted beams, friezes, lattices and wooden
carvings), `tk-planks` (plain planks), `tk-ornament` (carved stone and gilded ornament),
`tk-deities` (the wrathful deities' and guardians' reliefs), `tk-gates` (carved doors),
`tk-switches` (the symbol discs), `tk-greystone` (dark grey wall stone with gold lines and
glyph panels), `tk-bandwall` (grey-green stone walls with red strips and a gold meander),
`tk-clay` (ochre clay plaster), `tk-marble` (veined dark marble), `tk-inlay` (patterned
inlaid stone floors and ceilings), `tk-rugs` (woven rugs and hangings), `tk-redplaster`
(red-painted plaster walls).

## 9. Models (the skins)

The 555 skins of both games' models, which 9.6 redraws. §1 holds for them (Raven's theme,
real scale, condition, no light, no readable text); a hub's object or puzzle item follows that
hub's section (Septimus's statues are Roman, Thysis's jars Egyptian). A skin is an atlas: the
model's pieces laid out flat, often a front half and a back half, on black that the model
doesn't use.

- **purpose**: `creature` (monsters and bosses, their mounts, heads and limbs), `player` (the
  five classes and their heads), `weapon` (in the hand, as a pickup, thrown), `item` (pickups:
  artifacts, armor, mana, the puzzle items), `object` (statues, furniture, chests, pots, books,
  plants, corpses, flags, snow), `debris` (chunks of broken things and bodies), `fx` (lights,
  flames, torches, glowing missiles and spells, webs, the crosshair).
- **tier**: `skin` (redrawn: every solid model), `fx` (upscaled only, M45: its look is glow,
  flame or a spell's colors; the torches and burners drawn with their flame, whose glow is the
  engine's), `skip` (placeholders and test models, blank atlases). The torch holders without
  their flame are redrawn (`castrch`, `egtorch`, `mesotrch`, `rometrch`: owner, M65). A solid thing with a small glow (a staff's
  gem, a creature's eyes) is `skin`: the palette's saturated colors are kept as drawn (M49).
  Tiny flat slivers of debris (`shard1`–`shard5`) stay `fx`.
- **class**: `skin_metal` (metallic over the whole atlas) only where metal is at least 60 % of
  the model's texels by its regions: weapons, keys, rings, the iron and bronze golems; a
  creature or a player in armour stays `skin` (M45: leather and cloth around the steel).
- **regions**: as the world's; the atlas's black is `grey` texels, which take the material of
  the model's grey parts. Chitin and wing membranes are `hide`, fur and feathers `hair`, snow
  `ice`, fired clay `tile`.
- **description**: what the model is, then its parts' materials and colors (body, head,
  clothing, armour, weapon, base) and the condition; not the atlas's layout. 9.6 adds a
  description per piece of the atlas.
- **family**: a model group whose skins should read alike (hub `models`, prefix `md-`): a
  creature's body, head and limbs (`md-imp`, `md-golem`), a boss with its mount (`md-death`),
  a player class and its head (`md-paladin`), a class's weapons in the hand and as pickups
  (`md-w-crusader`), a set (`md-canopic`, `md-elements`, `md-armor`), objects of one kind and
  hub (`md-statues-septimus`, `md-pots`, `md-corpses`), debris of one material
  (`md-debris-stone`). A one-off object or puzzle item stands alone. The lead is the biggest
  atlas (`labels leads`).
- **Copies** carry one label (§1), whatever their models: the Demoness's head is the
  Assassin's (`h_suc`, `h_ass`), the Bones of Loric are the bone pile (`puzzle/keep1`,
  `bonepile`), the lava ball is the shard (`lavaball`, `shard`: ice, rock and ashes, as
  `precache.hc` names them), Eidolon's three models share one skin.

Found in the gamecode: the imp's third skin is the stone gargoyle that wakes (`imp.hc`),
`snout.mdl` is a polymorphed player's view of the sheep (`sheep.hc`), `scrbp*` the scarab on the
Assassin's staff chain. The owner's answers (M65): Septimus's statues of Caesar, Mars and Neptune
are dark stone, not bronze; the Cube of Force is brass; the grey skin that `puzzle/e2`, `m5`,
`s1` and data1's scepter share is a cloth backpack.
