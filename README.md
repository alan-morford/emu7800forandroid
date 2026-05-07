# emu7800forandroid
This 100% Claude Code vibe-coded project is a port of EMU7800 for webOS which is itself a port from EMU7800. EMU7800 was developed by Atari enthusiast Mike Murphy. Originally released around 2003, the emulator is an open-source project (GNU GPLv2) designed for Windows and later adapted for other platforms, with active development continuing on GitHub. Portions of this port of EMU7800 also utilized open-source code from Stella version 7.0 (released in Oct 2024). Stella was originally developed for Linux by Bradford W. Mott and is now maintained and developed by Stephen Anthony and the Stella Team.

**Current version: 1.2.1**

## Features
- Atari 2600 and 7800 ProSystem support
- On-screen touch controls with multitouch D-pad and fire buttons
- Atari 2600 paddle controller support — touch slider and DPad modes with auto-detection
- Keyboard support
- Built-in file picker for browsing and launching ROMs (.a26, .a78, .bin, .zip)
- Save, load, and delete state support
- Resume last played ROM from file picker
- Multiple video modes (Original 4:3 Aspect Ratio and Fullscreen stretch)
- Starpath Supercharger ROM support
- Asteroids 7800 included just like the European release of the Atari 7800!
- Recently played list for quick resume
- Contact me link for bugs
- Launch game straight to Save State
- Set default directory, change in settings
- Settings for scanlines, 7800 color palette intensity, auto-save, control overlay brightness, paddle controls, in-game options menu
- In-app update checker — flashing UPDATE button in the file picker downloads and installs the latest release
- Resizable controls
- Bluetooth controller support (including left analog stick for paddle games)
- Auto-resize for book-style foldable support

## Changelog

### v1.2.1 (2026-05-07)
- Added Atari 2600 paddle controller support with auto-detection by ROM name
- Touch Slider mode: bottom-anchored pill-shaped track with orange knob
- DPad mode: tap left/right on the virtual d-pad for incremental paddle movement
- Bluetooth controller: left analog stick maps to paddle in Slider mode; d-pad taps nudge in DPad mode
- Paddle Controls option in Settings (SLIDER/DPAD); greyed out for non-paddle games
- DPad Size option greyed when Paddle Slider mode is active
- Paddle entry added to Bluetooth Controls Map
- Fixed Atari 2600 Original zoom mode to display at true 4:3 aspect ratio scaled to screen height
- ROM name next to RESUME button now truncated to 20 characters with ellipsis
- Fixed RECENT button position when no last-played ROM exists

### v1.2.0 (2026-05-03)
- Audio pop fix: eliminated random popping/clicking during gameplay
- Fixed update popup touch detection on all screen sizes
- Fixed in-app APK install on Android 14+
- ZIP files shown in blue in the file picker
- Files listed in alphabetical order (directories first)

### v1.1.1 (2026-04-28)
- Fixed: tapping LATER in the update popup no longer removes the flashing UPDATE indicator
- Fixed: APK download and install now works via DownloadManager (system download notification + auto-launch installer)
- Added: release notes scroll inside the update popup
- Added: markdown stripped from release notes, long lines word-wrapped
- Scroll resets to top each time the update popup is opened

### v1.1.0 (2026-04-28)
- ZIP ROM loading (.zip archives containing a single ROM are now supported)
- Audio pop fix on game launch
- GitHub update checker with in-app update popup styled to match the settings UI
- Flashing UPDATE label in the file picker top bar when a new release is available
- Version bump and release infrastructure improvements

### v1.0.1
- Bug fixes and stability improvements

### v1.0.0
- Initial release
