# U3SetBeforeText.uasset

A prefix set saved by the class before the text fields existed, for `SuperSLM.PrefixCache.Cpu.Text.Persistence`'s
old-asset leg (cell 9.12). Regenerating it with a later class would not test the upgrade, so it is committed.

It was made on 2026-10-31 by the plugin at c8eace01a (box/wt2dpx-c3's U3 tree, no `Text` field on
`FSuperSLMPrefixSetEntry`), built on the UE 5.8 fork: `-run=pythonscript`, two token entries (`old00`, `old01`, 16 ids
each), no model, no wrappers. The original old asset (2026-10-22) was lost with its box's scratch directory; it had
the example model and built wrappers, so this fixture does not exercise the cell's "serves as before" leg, which
runs only when the old asset has a model. Stored outside LFS (`.gitattributes`): 1,833 bytes.
