# Building the lotus engine with MSVC

`lotus-engine` and `lotus-ffxi` are written against Clang. Getting them through
MSVC 14.51 took a set of changes that are not obvious from the error messages,
so they are recorded here - both to explain the diffs in the two forks and so
the same ground does not have to be re-covered.

`build-engine.bat` configures and builds everything. `build-one.bat <target>`
rebuilds a single object without reconfiguring, which is worth using when
bisecting one translation unit.

## The C++ standard library module

The Vulkan SDK's `vulkan.cppm` does `export import std;`. MSVC's `std` module
and the standard headers cannot both be in play, and the engine includes
standard headers everywhere, so `vendor/vulkan-module/vulkan.cppm` is a patched
copy with the standard headers moved into the global module fragment and the
`export import std;` removed. See `vendor/vulkan-module/README.md`.

A consequence: anything whose interface mentions a standard type has to include
that header itself rather than relying on Vulkan-Hpp to have re-exported `std`.
Most of the diff in both forks is exactly that - adding the includes that were
previously arriving for free.

`/std:c++23` is **not** a valid MSVC flag. It is accepted silently, falls back,
and produces a cascade of unrelated-looking errors. Use `/std:c++latest`.

## Declarations that do not merge with their definitions

This was the single largest cause of errors, and it wore several disguises.

Within one module, a forward declaration in one partition and the definition in
another are meant to be the same entity. Under MSVC they are not, unless the
declaration is exported too. An unexported declaration in an interface partition
is module-internal, and a consumer that reaches it first will use *that* - and
see an incomplete type, or a type that fails to match its own definition.

It shows up as, in rough order of how obvious it is:

- `error C2027: use of undefined type 'lotus::Renderer'` - the consumer resolved
  to the unexported forward declaration in `engine.cppm`.
- `error C2665: no overloaded function could convert all the argument types`,
  with a note saying "Types pointed to are unrelated" for two spellings of the
  same type. Two `lotus::Entity`, not one.
- `error C2371: redefinition; different basic types` on a member function whose
  declaration and definition are textually identical, because a type in the
  signature is two entities.
- `error LNK2019: unresolved external symbol` where the mangled name contains
  `TransformEntry[lotus]` on one side and `TransformEntry[!lotus]` on the other.
  `$$_A` is exported module linkage and `$$_B` is module-internal, so this is
  the same problem surviving all the way to the linker.

Two fixes, depending on the partition:

- In an **interface** partition (`export module lotus:x;`), export the forward
  declaration. Exporting a declaration of an already-exported type is a no-op,
  so this is safe to apply broadly, and it is applied to every such declaration
  in both forks.
- In an **implementation** partition (`module lotus:x;`), `export` is ill-formed.
  Either import the partition that defines the type instead of forward-declaring
  it, or promote the partition to an interface partition.

Related: `static` at namespace scope in an implementation partition gives
internal linkage, so no importer can ever see it, however the partition is
imported. `key_tables.cppm` and `actor_data.cppm` both did this with lookup
tables that other partitions read; both became interface partitions exporting
`inline` variables.

## MSVC loses std::basic_istream::sentry

`ffxi/dat/read_file.cpp` exists because of this:

```
istream(698): error C2079: '_Ok' uses undefined class
              'std::basic_istream<char,std::char_traits<char>>::sentry'
```

`Dat::Dat` read a file with `<fstream>` the same way `DatLoader::read_file`
does. The latter compiles; the former does not. Bisecting `dat.cpp`'s imports
showed that removing *any single one* of them makes the error go away, so it is
the size of the merged import set that tips it over rather than any one module.

Moving the code into a header does not help - it is still instantiated in the
module's context. It has to be a translation unit that belongs to no module, so
that is what `read_file.cpp` is.

## The Vulkan dynamic dispatcher

Under MSVC the Vulkan-Hpp module emits the storage for
`vk::detail::defaultDispatchLoaderDynamic` itself, so also compiling
`vk_dispatch.cpp` gives `LNK2005`. That file is now conditional on `NOT MSVC`,
since other toolchains still need it.

## Still open

`ffxi/CMakeLists.txt` builds an executable and hardcodes `target_sources(ffxi ...)`
against it, so there is no library for MogHouse's interop layer to link against.
Wiring the engine to the C# client needs that restructured first.
