# A LandSandBoat test server on macOS

Built on 2026-09-02 so login could be tested without using anyone's real
account. It works: login, character creation and zone-in all succeed. What does
not work is the client decoding the zone reply, and the reason is a version
mismatch rather than anything in the setup - see the end.

## What it took

Four separate problems, none of them documented anywhere obvious.

**1. Apple's clang has no `std::jthread`.** `src/common/zmq/zmq_service.h` uses
it, so the Command Line Tools compiler cannot build the server at all:

    error: no type named 'jthread' in namespace 'std'

**2. Homebrew's current LLVM is too new.** LLVM 23 emits diagnostics the code
has never seen, and `-Werror` makes them fatal. `CMAKE_CXX_FLAGS` cannot fix
this: the build appends `-Wall -Wextra -Werror` *after* whatever you pass, so
`-Wno-...` is re-enabled and then promoted. There is a proper knob -
`-DWARNINGS_AS_ERRORS=OFF` - and that is the answer rather than patching source.

It also breaks on transitive includes that older libc++ supplied by accident:
`logging_context.h` uses `std::vector` without including `<vector>`, and the
bundled `fmt` uses `std::atomic_flag` without `<atomic>`. Those are genuine
upstream portability bugs and worth reporting.

**3. LLVM 20 is the version that works** - which the top of `CMakeLists.txt`
hints at, mentioning "Homebrew LLVM 20" by name.

**4. Its libc++ has to be linked, not just its headers used.** With LLVM 20's
headers and Apple's system libc++ the map server fails to link:

    Undefined symbols: std::__1::__from_chars_floating_point<double>(...)

`std::from_chars` for floating point is declared in the newer headers and
absent from the shipped dylib. Pointing the linker at LLVM 20's own libc++ is
the fix.

Together:

    brew install mariadb zeromq luajit openssl@3 zstd llvm@20
    /Library/Frameworks/Python.framework/Versions/3.13/bin/python3.13 \
        -m pip install -r tools/requirements.txt

    LIBCXX=/opt/homebrew/opt/llvm@20/lib/c++
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@20/bin/clang \
      -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@20/bin/clang++ \
      -DWARNINGS_AS_ERRORS=OFF \
      -DCMAKE_EXE_LINKER_FLAGS="-L$LIBCXX -Wl,-rpath,$LIBCXX"
    cmake --build build

## Database

Homebrew's MariaDB authenticates root through a unix socket, so the shipped
`root`/`root` cannot connect over TCP. A dedicated user can:

    brew services start mariadb
    mysql -e "CREATE DATABASE xidb CHARACTER SET utf8mb4;
              CREATE USER 'xiuser'@'127.0.0.1' IDENTIFIED BY 'xipass';
              GRANT ALL ON xidb.* TO 'xiuser'@'127.0.0.1';"

and `settings/network.lua` overrides the defaults. **Write it as a table
literal**, the same shape as `settings/default/network.lua`: `tools/dbtool.py`
scrapes `KEY = value` lines rather than executing the Lua, so a dotted
assignment parses as nothing and the tool silently falls back to `root`/`root`.

`dbtool.py setup` is not a command - the menu options are `e`, `1`-`4`, `r`,
`t`, `p`, `l`, `s`, `q` - and an unrecognised argument exits 0 having done
nothing, which is a confusing way to be told. Importing the SQL directly is
simpler and worked first time:

    for f in sql/*.sql; do mysql -u xiuser -pxipass -h 127.0.0.1 xidb < "$f"; done

118 tables, no failures.

An account, with a bcrypt hash the login server will accept:

    python3 -c "import bcrypt; print(bcrypt.hashpw(b'PASSWORD', bcrypt.gensalt()).decode())"
    -- then INSERT INTO accounts (id, login, password, ...) VALUES (...)

**`navmeshes` is required by the map server.** It is a submodule, a plain clone
leaves it empty, and the server exits on it:

    [map][critical] ./navmeshes/ directory isn't present or is empty!

    git submodule update --init --depth 1 navmeshes    # 421MB

Not to be confused with the note in `docs/macos-handoff.md` about navmeshes
being optional - that is about the *client's* flat map, not the server.

## Where it got to

Servers start clean: `xi_world`, `xi_connect`, `xi_map`, the last reporting
"ready to work after 9.25 seconds".

Against it, `tools/loginzone`:

    logged in - 16 character(s)
    no characters on this account - creating Testy...
    created Testy
    entering the world as Testy...
    Zone server did not answer, or its reply could not be decoded.

and on the server, at the same moment:

    [connect] char <Testy> was successfully created on account 1000
    [connect] data_session: zoneid: 234, zoneipp: 127.0.0.1:54230, for charid: 1
    [map] Player <Testy> logging in to zone <234>
    [map] CZone:: Bastok_Mines IncreaseZoneCounter <1> Testy

**The server accepted the zone-in and put the character in Bastok Mines. Only
the client's decode failed.** That is the version mismatch this project warns
about in every README: the client wants the August 2026 patch, and this clone
is at `f49d5650`, dated 2026-09-01 - a month of protocol changes newer.

So this setup is sound and the remaining gap is a version pin. To finish it,
`git fetch --unshallow` and check out a commit from around the August 2026
update before rebuilding.

**A server already on the right version does not have this problem**, which is
why `tools/loginzone` is worth running against ffxi.cc first: everything up to
the decode is already proven, and that last step is the only untested one.

## Stopping it

    pkill -f 'xi_map|xi_world|xi_connect'
    brew services stop mariadb
