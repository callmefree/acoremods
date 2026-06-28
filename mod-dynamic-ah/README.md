# mod-dynamic-ah

**Dynamic Auction House Module for AzerothCore**

---

## Overview

**mod-dynamic-ah** enhances the player experience on low-population AzerothCore servers by dynamically managing Auction House (AH) listings and buying operations. This module ensures a lively economy by:

-   Automatically posting items players need based on their professions.
-   Intelligently purchasing undervalued items to maintain market health.
-   Controlling auction saturation through configurable limits.

## Key Features

-   **Dynamic Selling:** Posts profession-specific materials (herbs, ores, cloth, enchanting materials, etc.) based on the skill levels of active players.
-   **Intelligent Buying:** Buys undervalued items for later resale, governed by a configurable margin and budget.
-   **Configurable Caps:** Controls market flooding through limits on total items per cycle, items per Auction House, and specific item families.
-   **Profession Context Awareness:** Detects player professions and skill levels to determine relevant materials to list.
-   **Dry-run Mode:** Safely test configurations without real money transactions.

---

## Installation

1. Clone this repository into your AzerothCore modules directory:

```bash
git clone https://github.com/yourusername/mod-dynamic-ah.git modules/mod-dynamic-ah
```

2. Build AzerothCore with the module enabled:

```bash
cd build
cmake .. -DMODULES=mod-dynamic-ah
make -j$(nproc)
```

3. No SQL modifications needed (for now).

4. Copy the default configuration:

```bash
cp modules/mod-dynamic-ah/conf/mod_dynamic_ah.conf.dist modules/mod-dynamic-ah/conf/mod_dynamic_ah.conf
```

5. Adjust configuration settings to suit your server.

---

## Configuration

Configuration options are located in:

```
modules/mod-dynamic-ah/conf/mod_dynamic_ah.conf
```

Key configuration options:

-   `ModDynamicAH.Enabled` (toggle module functionality)
-   `ModDynamicAH.DryRun` (simulation mode without actual transactions)
-   `ModDynamicAH.Interval.Minutes` (frequency of AH cycles)
-   `ModDynamicAH.Buy.PerCycleBudgetGold` (budget for buying operations)
-   `ModDynamicAH.Cap.TotalPerCycle` (limit auctions per cycle)

---

## Usage

The module offers convenient GM commands in-game:

-   `.dah setup`: Initializes AH bot characters.
-   `.dah plan`: Preview upcoming buy/sell actions.
-   `.dah run`: Immediately execute AH operations.
-   `.dah budget`: Fund AH bot characters.
-   `.dah caps`: View or adjust runtime caps.
-   `.dah dryrun`: Toggle simulation mode.

---

## Logging

Logs appear in server logs and are prefixed clearly for easy debugging:

-   `[BUY]`: Buying decisions.
-   `[POST]`: Selling decisions.
-   `[SKIP]`: Decisions to skip buying/selling with reasons.
-   `[CTX]`: Profession context and material calculations.

---

## Contributing

To contribute:

1. Fork this repository.
2. Create your feature branch (`git checkout -b feature/my-feature`).
3. Commit your changes (`git commit -am 'Add my feature'`).
4. Push to the branch (`git push origin feature/my-feature`).
5. Open a pull request.

---

## License

Distributed under the AGPL v3.0 License. See `LICENSE` for more information.

---

## Support

For support, issues, or questions, open an issue on GitHub.

---

**Enjoy your thriving Auction House!**
