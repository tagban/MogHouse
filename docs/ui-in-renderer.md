# Moving the UI into the renderer, and removing Avalonia

The plan that came out of the macOS black screen. Written 2026-09-02, with the
architecture proven but the UI not yet built.

## Why

The client has two windowing systems in one process: Avalonia for the launcher
(login, character select, install picker, settings) and the renderer for the
world. That is what the black screen was: `LiveRadar` starts the renderer's
blocking loop on a background thread, and macOS requires window creation on the
main thread. Avalonia already owns the main thread, so the renderer cannot have
it.

Every workaround preserves the underlying problem - two event loops competing
for one main thread. Removing Avalonia dissolves it:

- The renderer owns the process's main thread, which is what macOS demands and
  what Windows and Linux are happy with. `moghouse-renderer` already runs this
  way, which is why the standalone binary renders correctly today.
- One window. No launcher to hide, no world window to lose, no swapping
  between them, and no state where the client is running, logged in and
  invisible - which the watchdog exists to catch and which has happened twice.
- No IPC, and no splitting `runViewer` into a per-frame pump. The loop that
  keeps the window alive across zone changes - rebuilt once because reloading
  it was a terrible experience - is not touched.

And it buys something: with the UI in the renderer, the login and config
screens can have the world drawn behind them, which is what the original
launcher felt like. Avalonia can never do that.

The strategic argument: **every UI feature still to come is in-world UI.**
Inventory, equipment, menus, quest logs, maps. Avalonia cannot draw any of
them. It is scaffolding for screens that have to be rebuilt in-engine anyway,
so removing it is doing the inevitable earlier and deleting a class of bug on
the way.

## What is already proven

Measured, not assumed. All of this ran on an M4 on 2026-09-02.

**The renderer runs on the main thread when called from C#.** A console
harness referencing `MogHouse.Core` created a `NativeViewer` and called `Run()`
directly from `Main`:

    main thread id=1
    viewer created; calling Run() ON THE MAIN THREAD
    [background] still alive, viewer running; thread id=4

and the renderer log showed a full zone: `adapter: Apple M4 (Metal)`, 130
models, 8161 triangles of water, 348429 of collision, 334 draws, 25 textures.
Background threads keep running while it does, which is where the session and
protocol code will live.

**The renderer runs with no zone loaded.** Passing an empty zone path gives
`no DAT given - clearing only`, a window, and `font: 640x240 atlas, 95 glyphs`.
That is the login screen's substrate: a window and working text with no game
data in sight.

**SDL3 provides a native folder picker.** `SDL_ShowOpenFolderDialog` is in
`SDL_dialog.h`. That was the last thing genuinely needing a native toolkit -
the "Where is Final Fantasy XI?" prompt.

## What already exists to build on

More than expected. The death box is an immediate-mode UI in miniature:

- `dialog_shader.h` draws a modal panel, its rows and its buttons in **one draw
  call**, dimming the world behind it, with the fragment shader deciding what
  each pixel belongs to.
- Buttons already have normal, hot (hover) and disabled colours
  (`kDialogButton`, `kDialogButtonHot`, `kDialogButtonOff`), and a `live[]`
  flag per button - so enable/disable is solved.
- Hit-testing exists: `float rects[kDialogRows][4]` and a `DialogButton` struct.
- Glyph positions are computed on the CPU because the font is proportional,
  the same as nameplates.
- Chat already accepts typed text, so text entry exists.

The gap is that all of it is death-specific and fixed-size: `kDialogRows = 5`,
`kDialogChars = 40`, `kDialogButtons = 2`, with the labels hardcoded at the
call site.

## The work, in an order that keeps a working client at every step

**1. Generalise the dialog into a screen.** Variable rows and buttons, a
caption, and a new row kind: an editable field with a caret. Everything else -
the shader, the hit-testing, the hover states - already does what is needed.
Testable standalone in shell mode with `MOGHOUSE_SCREENSHOT`, with no client
and no server.

**2. Build the screens.** Login, character select, create account, create
character, install picker, settings. Eight `.axaml` views and ~1650 lines of
view models become in-engine screens; the install picker calls
`SDL_ShowOpenFolderDialog`.

**3. Expose them through the interop.** The existing API is already the right
shape - `set_*` in, `take_*` out. Screens need the same: show a screen, take
what the user chose.

**4. Invert the entry point.** `Program.Main` runs the renderer on the main
thread; session and protocol move to background threads. Everything currently
posted through `Dispatcher.UIThread` becomes a direct call, because there is no
dispatcher left to post to. This is small - the hard part is the screens above.

**5. Delete Avalonia.** The package references, the eight views, the view
models, and `MogHouse.App`'s dependency on a UI framework.

## What to be careful about

**Zoning must keep working exactly as it does.** `load_zone` replaces the
geometry inside a window that stays open. Nothing in this plan touches it, and
nothing should.

**Do not lose text input.** Chat already handles typed input, but the login
screen needs a password field, a caret, backspace and paste. `SDL_StartTextInput`
is the right entry point, and non-Latin input via IME is worth a thought if it
matters.

**The window is now the whole application.** With no launcher behind it, a
renderer that fails to start means nothing on screen at all - the state the
watchdog was written to catch. Failure to open a window has to be reported
somewhere a person will find it, which now means the log and a plain error
before the window would have appeared.
