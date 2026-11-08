# Dependencies

Submodules in this directory reference commits published in each dependency's
official repository. Integration code stays outside submodule directories;
the referenced source is not edited in place.

libdsm has an approved exception for correctness fixes. The build copies the
[pinned revision](https://github.com/videolabs/libdsm/tree/0f3edc6df6ab5950f2550d453035351440f71e31)
into its own directory and applies the [patch](../patches/libdsm-correctness.patch).
The fixes cover character conversion, listing error propagation, and builds
with current toolchains. Distribution packages include the license, patch,
and exact corresponding patched source.
