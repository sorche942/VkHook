### vkintox's reshade fx compiler.
This directory holds the ReShade FX compiler that VKIntox uses, taken from [crosire/reshade](https://github.com/crosire/reshade) and cut down to what compiling an effect needs: lexer, preprocessor, parser, symbol table and the SPIR-V code generator. The file extensions are `.hh`/`.cc` to match VKIntox's build, and the parser is kept as a single translation unit.

It is tracked directly in VKIntox as a squashed Git subtree of
[`buwryme/reshade-vkintox`](https://github.com/buwryme/reshade-vkintox), initially
imported at `4e296c625414adbc3ff846a363492e977a86780f`. Local compiler patches
are ordinary VKIntox commits. See the repository's `docs/WORKFLOWS.md` for
subtree updates and patch export; no submodule initialization is required.

### license
ReShade is licensed under the BSD 3-Clause License, see [LICENSE.md](LICENSE.md). the modifications that this repository has are under that license too.
