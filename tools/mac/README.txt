Eternal Darkness Recomp - Mac (Apple silicon), beta
===================================================

This app holds no game. It turns your own copy of Eternal Darkness:
Sanity's Requiem into a native Mac game.

1. Move "Eternal Darkness.app" to Applications.
2. The app is not notarised: the first time, right-click it and choose Open,
   then Open again. (Or, in Terminal:
   xattr -dr com.apple.quarantine "/Applications/Eternal Darkness.app")
3. When it asks, drop your disc image on its window, or click Choose Disc
   Image: an .iso or .gcm of the European (GEDP01) GameCube disc. The USA
   disc is not supported.

The first start translates your disc and builds the game from it, which takes
a minute or two. After that the game starts straight away. Hold Option while
starting the app to choose a different disc.

Everything it makes, and your saves, settings and logs, stay in
~/Library/Application Support/Eternal Darkness. Nothing from your disc leaves
your Mac. An HD texture pack (Dolphin's tex1_... .png files) goes in the
textures folder there.

Needs macOS 13 or later on an Apple silicon Mac.
