# mod-bag-sorter

Sort the items in your **carried bags** (backpack + the 4 equipped bag containers) by talking
to any **innkeeper**, or with a `.sortbags` chat command.

## Usage

### Innkeeper gossip
Talk to any innkeeper and pick **"Organize my bags"**. The innkeeper's normal options
(make this your home, etc.) are untouched — the sort option is simply appended. Choose a mode:

- **By type, then quality** — groups items by class/subclass/type, then quality, then item level.
- **By quality** — highest rarity first.
- **By item level** — highest item level first.
- **By name (A–Z)** — alphabetical.
- **Type & quality, quest items to last bag** — sorts the same as "by type, then quality", then
  sweeps all quest-class items into the tail of your bags (the last bag), clear of everything else.

The **Hearthstone** (item 6948) is always pinned to the backpack's first slot after any sort
(toggle with `BagSorter.PinHearthstone`).

### Command
```
.sortbags            # defaults to "by type, then quality"
.sortbags type
.sortbags quality
.sortbags ilvl
.sortbags name
.sortbags questlast  # type & quality, then quest items to the last bag
```

## What it does

1. (Optional) **Consolidates partial stacks** of the same item.
2. **Reorders** items into the chosen order, packing them toward the start of your bags.

Items are only ever **moved**, never destroyed. All moves go through the core's validated
`Player::SwapItem`, so they obey every normal inventory rule.

### Bag families
Specialized profession bags (herb bag, enchanting bag, …) are sorted **internally** only; the
backpack and general-purpose bags are sorted together as one pool. Items are never moved between
those pools, which guarantees every move is valid. A consequence: a loose herb sitting in your
backpack will **not** be relocated into your herb bag (a possible future enhancement). For the
same reason, "quest items to last bag" targets the last **general** bag — quest items can't be
placed into a profession bag, so if your bottom bag is one, they go to the bag above it.

## Configuration

`conf/mod_bag_sorter.conf.dist` → copy to your active worldserver config as
`mod_bag_sorter.conf`:

| Key | Default | Meaning |
| --- | --- | --- |
| `BagSorter.Enable` | `1` | Master on/off for both the gossip option and the command. |
| `BagSorter.MergeStacks` | `1` | Consolidate partial stacks before sorting. With `0`, reordering may still incidentally merge two adjacent identical stacks. |
| `BagSorter.Announce` | `1` | Whisper a one-line confirmation after sorting. |
| `BagSorter.PinHearthstone` | `1` | Always move the Hearthstone (6948) to the backpack's first slot. |

> **Docker note:** add these keys to the *active* worldserver config the container loads, not
> only the `.dist` file, or boot logs a missing-config warning.

## Notes / limitations

- **Scope:** carried bags only. Bank sorting is intentionally out of scope for this version.
- **Command permission:** `.sortbags` reuses `RBAC_PERM_COMMAND_DISMOUNT` (held by the default
  player role) so any player may run it without an auth-DB change. Swap in a dedicated RBAC
  permission if you prefer.
- **Scripted innkeepers:** the module takes over `OnGossipHello` for innkeepers, which would skip
  a *custom C++* gossip script on an innkeeper (extremely rare — innkeepers are DB-gossip driven).
