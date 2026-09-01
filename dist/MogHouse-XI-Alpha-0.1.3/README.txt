MogHouse XI - Alpha 0.1.3
============================

A from-scratch Final Fantasy XI client. This is an alpha: it is missing a
great deal, and the parts that are here are the parts that have been built so
far rather than the parts you would miss least.

What you need
-------------

  * A Final Fantasy XI installation. The client finds it and reads the game's
    own files - models, textures, zones, music. Nothing here replaces them and
    no game data is included.
  * A private server to connect to, and an account on it.

Running it
----------

  1. Unzip anywhere. There is no installer and nothing is written outside this
     folder.
  2. Run MogHouse.App.exe.
  3. Enter your server's address, then log in.

Settings live in moghouse-settings.json beside the exe, and the servers you
add live beside that. Deleting the folder removes the client completely.

Controls
--------

  WASD          walk                    Shift   run
  Mouse drag    look                    Space   jump
  R             auto-run                Tab     orbit
  M             hold the map north-up   U       back out if collision traps you
  + and -       music volume            P       print position to the log
  / and !       open chat, with the key already typed

Known missing, so you do not report what is already known
---------------------------------------------------------

  * Combat. You can walk, talk, zone and look at the world; you cannot fight.
  * Telepoint and Homepoint crystals are invisible.
  * Some creatures have no model and do not appear.
  * Hair colour is wrong for some faces.
  * There is no full-screen map yet.

If something is wrong
---------------------

There are two logs beside the exe: moghouse.log for the client and
moghouse.log.renderer for the world. Both are plain text, and between them
they usually say what happened. Attaching them to a bug report is the single
most useful thing you can do.

Report bugs from inside the game with the link in the top-left corner, or at
the GitHub issues page. There is a Discord link beside it.
