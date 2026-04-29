/*
 * EMU7800Activity.java
 *
 * SDL2 Activity subclass for EMU7800 Android port.
 *
 * Responsibilities:
 *   - Subclass SDLActivity so SDL2's JNI shim can call SDL_main()
 *   - Pass app cache directory and data directory to native at startup
 *   - Gate startup behind MANAGE_EXTERNAL_STORAGE ("All files access") on
 *     Android 11+; show an in-app dialog so the user never has to find the
 *     permission manually.
 *   - Provide SAF document picker as a fallback for ROM selection
 *   - Deliver the chosen ROM file descriptor to native via JNI
 *
 * Copyright (c) 2024 EMU7800
 */
package com.emu7800.android;

import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.TextView;
import android.media.AudioManager;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;
import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

public class EMU7800Activity extends SDLActivity {

    private static final String TAG = "EMU7800";
    private static final int    ROM_PICKER_REQUEST     = 1001;
    private static final int    MANAGE_STORAGE_REQUEST = 1002;

    /** Static reference used by openRomPicker() which is called from native. */
    private static EMU7800Activity sInstance;

    /** Mirrors the native g_use_cutout so detectAndReportCutout() can restore the mode. */
    private static boolean sCutoutEnabled = false;

    /** Timestamp of the last back press at root, for double-back-to-exit. */
    private long mLastBackPressTime = 0;

    /** Toast shown on first back press at root; cancelled before finish() so it doesn't linger. */
    private Toast mExitToast = null;

    /** Full-screen overlay shown when the device is in portrait mode. */
    private View mRotateOverlay = null;

    /**
     * Listens for physical display rotation changes at the hardware level.
     * Samsung's compat layer often suppresses onConfigurationChanged when the
     * display rotates (the app window doesn't change, so Android sees no config
     * change), but DisplayManager fires regardless.
     */
    private DisplayManager.DisplayListener mDisplayListener = null;

    /* ---- Activity lifecycle ---- */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sInstance = this;
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onCreate(savedInstanceState);

        // On Android 11+ (API 30+) the FUSE storage layer filters out non-media
        // files (.a78, .a26, .bin) for apps that don't hold MANAGE_EXTERNAL_STORAGE
        // ("All files access").  requestLegacyExternalStorage is silently ignored
        // on Android 11+, so this is the only way to see ROM files via opendir().
        //
        // super.onCreate() must be called first (it loads the native library).
        // SDL is now running behind any dialog we show, which is fine — the
        // filepicker shows an empty list when permission is absent, but the dialog
        // sits on top and guides the user.
        registerDisplayListener();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                && !Environment.isExternalStorageManager()) {
            showStoragePermissionDialog();
            return;
        }

        initNativeDirs();
    }

    @Override
    protected void onResume() {
        super.onResume();
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        // Post the check so SDL's view hierarchy is fully attached before we
        // try to add or remove the overlay.
        getWindow().getDecorView().post(this::syncRotateOverlay);
    }

    /**
     * Called when this window gains or loses focus.  On book-style foldables,
     * closing the device moves the app to the outer screen; by the time the
     * window regains focus the display transition is complete and
     * getRootWindowInsets() reflects the outer screen's actual cutout geometry.
     * requestApplyInsets() triggers the WindowInsetsListener with fresh data.
     */
    // ---- SDL controller button indices (must match nativeControllerButton in input.c) ----
    private static final int SDL_BTN_A         =  0;
    private static final int SDL_BTN_B         =  1;
    private static final int SDL_BTN_X         =  2;
    private static final int SDL_BTN_Y         =  3;
    private static final int SDL_BTN_BACK      =  4;  // View / Select
    private static final int SDL_BTN_START     =  6;
    private static final int SDL_BTN_LSTICK    =  7;
    private static final int SDL_BTN_RSTICK    =  8;
    private static final int SDL_BTN_LSHOULDER =  9;
    private static final int SDL_BTN_RSHOULDER = 10;
    private static final int SDL_BTN_DPAD_UP   = 11;
    private static final int SDL_BTN_DPAD_DOWN = 12;
    private static final int SDL_BTN_DPAD_LEFT = 13;
    private static final int SDL_BTN_DPAD_RIGHT= 14;

    // ---- Axis indices passed to nativeControllerAxis ----
    private static final int SDL_AXIS_LEFT_X   = 0;
    private static final int SDL_AXIS_LEFT_Y   = 1;
    private static final int SDL_AXIS_TRIGGER_L = 4;  // Select
    private static final int SDL_AXIS_TRIGGER_R = 5;  // Reset
    private static final int SDL_AXIS_HAT_X    = 6;
    private static final int SDL_AXIS_HAT_Y    = 7;

    private static int keycodeToSdlButton(int kc) {
        switch (kc) {
            case KeyEvent.KEYCODE_BUTTON_A:      return SDL_BTN_A;
            case KeyEvent.KEYCODE_BUTTON_B:      return SDL_BTN_B;
            case KeyEvent.KEYCODE_BUTTON_X:      return SDL_BTN_X;
            case KeyEvent.KEYCODE_BUTTON_Y:      return SDL_BTN_Y;
            case KeyEvent.KEYCODE_BUTTON_START:  return SDL_BTN_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return SDL_BTN_BACK;
            case KeyEvent.KEYCODE_BUTTON_L1:     return SDL_BTN_LSHOULDER;
            case KeyEvent.KEYCODE_BUTTON_R1:     return SDL_BTN_RSHOULDER;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return SDL_BTN_LSTICK;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return SDL_BTN_RSTICK;
            case KeyEvent.KEYCODE_DPAD_UP:       return SDL_BTN_DPAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:     return SDL_BTN_DPAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:     return SDL_BTN_DPAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:    return SDL_BTN_DPAD_RIGHT;
            case KeyEvent.KEYCODE_BACK:          return SDL_BTN_BACK;
            default:                             return -1;
        }
    }

    /**
     * Intercept all key events from gamepad/joystick sources and route them
     * directly to native via nativeControllerButton.  Returning true prevents
     * Android from applying system defaults (B→Back, A→media-play, etc.).
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int keyCode = event.getKeyCode();
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
                if (am != null) {
                    am.adjustStreamVolume(
                        AudioManager.STREAM_MUSIC,
                        keyCode == KeyEvent.KEYCODE_VOLUME_UP
                            ? AudioManager.ADJUST_RAISE : AudioManager.ADJUST_LOWER,
                        AudioManager.FLAG_SHOW_UI);
                }
            }
            return true;
        }
        int src = event.getSource();
        if ((src & InputDevice.SOURCE_GAMEPAD)  != 0 ||
            (src & InputDevice.SOURCE_JOYSTICK) != 0) {
            int btn = keycodeToSdlButton(event.getKeyCode());
            if (btn >= 0) {
                boolean pressed = (event.getAction() == KeyEvent.ACTION_DOWN);
                nativeControllerButton(btn, pressed);
            }
            return true;  // consume all gamepad key events regardless
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * Intercept generic motion events (analog sticks, HAT/D-pad axes) from
     * joystick sources and forward each axis to native.
     */
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        int src = event.getSource();
        if ((src & InputDevice.SOURCE_JOYSTICK) != 0 ||
            (src & InputDevice.SOURCE_GAMEPAD)  != 0) {
            nativeControllerAxis(SDL_AXIS_LEFT_X,    event.getAxisValue(MotionEvent.AXIS_X));
            nativeControllerAxis(SDL_AXIS_LEFT_Y,    event.getAxisValue(MotionEvent.AXIS_Y));
            nativeControllerAxis(SDL_AXIS_TRIGGER_L, event.getAxisValue(MotionEvent.AXIS_LTRIGGER));
            nativeControllerAxis(SDL_AXIS_TRIGGER_R, event.getAxisValue(MotionEvent.AXIS_RTRIGGER));
            nativeControllerAxis(SDL_AXIS_HAT_X,     event.getAxisValue(MotionEvent.AXIS_HAT_X));
            nativeControllerAxis(SDL_AXIS_HAT_Y,     event.getAxisValue(MotionEvent.AXIS_HAT_Y));
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            // Display transition (e.g. foldable inner→outer screen) is fully
            // complete by the time focus is granted.  Re-detect the cutout so
            // the "Use Notch Area" toggle reflects the new screen's geometry.
            detectAndReportCutout();
        }
    }

    /**
     * Returns true when the device is physically held in portrait.
     *
     * Configuration.orientation, DisplayMetrics, getCurrentWindowMetrics(), and
     * getMaximumWindowMetrics() are all unreliable on Samsung tablets: the tablet
     * has a landscape-native display, so Samsung's app-compatibility layer reports
     * a landscape window (e.g. 1200x720) to the app regardless of how the device
     * is physically held.
     *
     * Display.getRotation() is a hardware sensor value that Samsung cannot fake.
     * On a landscape-native device (all known Samsung tablets), the natural
     * orientation is ROTATION_0 (landscape).  ROTATION_90 / ROTATION_270 means
     * the hardware has been physically rotated into portrait.
     *
     * This is safe on phones because requestedOrientation works on phones — the
     * overlay only needs to fire on tablets that ignore the orientation lock.
     */
    private boolean isPhysicallyPortrait() {
        android.view.Display display = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
                ? getDisplay()
                : getWindowManager().getDefaultDisplay();

        int rotation = display.getRotation();

        // Samsung tablets fake every dimension API (DisplayMetrics, WindowMetrics,
        // getRealMetrics) to always return landscape dimensions regardless of how
        // the device is physically held.  Display.getRotation() is a hardware
        // sensor value that cannot be faked.
        //
        // On portrait-native tablets (all known Samsung Galaxy Tab models),
        // ROTATION_0 / ROTATION_180 = device physically in portrait.
        // ROTATION_90 / ROTATION_270 = device physically in landscape.
        //
        // This is the inverse of what you'd see on a landscape-native tablet,
        // but Samsung Galaxy Tab (the device causing this issue) is portrait-native.
        Log.i(TAG, "isPhysicallyPortrait: rotation=" + rotation);
        return rotation == android.view.Surface.ROTATION_0
                || rotation == android.view.Surface.ROTATION_180;
    }

    /** Shows or hides the rotate overlay based on physical device rotation. */
    private void syncRotateOverlay() {
        if (isPhysicallyPortrait()) {
            showRotateOverlay();
        } else {
            hideRotateOverlay();
        }
    }

    /**
     * Shows a full-screen "please rotate" overlay above the SDL surface.
     * Called when the display is portrait despite our orientation lock — this
     * happens on tablets whose OEM firmware overrides requestedOrientation.
     */
    private void showRotateOverlay() {
        if (mRotateOverlay != null) return;
        TextView tv = new TextView(this);
        tv.setText("Please rotate your device to landscape mode");
        tv.setTextColor(Color.WHITE);
        tv.setBackgroundColor(Color.BLACK);
        tv.setGravity(Gravity.CENTER);
        tv.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 22);
        tv.setPadding(48, 48, 48, 48);
        addContentView(tv, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        tv.bringToFront();
        mRotateOverlay = tv;
        Log.i(TAG, "showRotateOverlay: portrait detected (" +
                getResources().getDisplayMetrics().widthPixels + "x" +
                getResources().getDisplayMetrics().heightPixels + ")");
    }

    /** Removes the rotate overlay once the device returns to landscape. */
    private void hideRotateOverlay() {
        if (mRotateOverlay == null) return;
        ViewGroup parent = (ViewGroup) mRotateOverlay.getParent();
        if (parent != null) parent.removeView(mRotateOverlay);
        mRotateOverlay = null;
        Log.i(TAG, "hideRotateOverlay: landscape restored");
    }

    @Override
    protected void onDestroy() {
        unregisterDisplayListener();
        sInstance = null;
        super.onDestroy();
    }

    private void registerDisplayListener() {
        DisplayManager dm = (DisplayManager) getSystemService(DISPLAY_SERVICE);
        if (dm == null) return;
        mDisplayListener = new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int displayId) {}
            @Override public void onDisplayRemoved(int displayId) {}
            @Override
            public void onDisplayChanged(int displayId) {
                // Fires on physical rotation even when Samsung suppresses
                // onConfigurationChanged. Post to UI thread so view ops are safe.
                runOnUiThread(() -> getWindow().getDecorView().post(() -> {
                    syncRotateOverlay();
                    detectAndReportCutout();
                }));
            }
        };
        dm.registerDisplayListener(mDisplayListener, null);
    }

    private void unregisterDisplayListener() {
        if (mDisplayListener == null) return;
        DisplayManager dm = (DisplayManager) getSystemService(DISPLAY_SERVICE);
        if (dm != null) dm.unregisterDisplayListener(mDisplayListener);
        mDisplayListener = null;
    }

    /**
     * Called when the device folds/unfolds or rotates without restarting the
     * Activity (because configChanges includes orientation|screenSize|density).
     * Re-locks to landscape if the system tries to rotate us, then re-pushes
     * display density so dp-based button sizes stay consistent.
     */
    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        detectAndReportCutout();
    }

    /**
     * Detects whether the current display has a cutout and reports it to native.
     *
     * getDisplayCutout() via WindowInsets returns null when layoutInDisplayCutoutMode
     * is DEFAULT or NEVER — the system only includes cutout geometry when the window
     * is actually extending into the cutout region.  To detect the cutout without
     * requiring the user to have enabled it first, we:
     *
     *   API 31+: use Display.getCutout() which is display-level and unaffected by
     *            window layout mode.
     *   API 28–30: temporarily switch the window to SHORT_EDGES, read the insets
     *              after a layout pass, then restore the user's actual mode.
     */
    private void detectAndReportCutout() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            nativeSetHasCutout(false);
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // API 31+: display-level API, independent of window layout mode.
            android.view.Display display = getDisplay();
            boolean hasCutout = display != null && display.getCutout() != null;
            Log.i(TAG, "detectCutout (Display.getCutout): " + hasCutout);
            nativeSetHasCutout(hasCutout);
            return;
        }

        // API 28–30: temporarily set SHORT_EDGES so getRootWindowInsets() includes
        // the actual cutout geometry, then restore the user's preferred mode.
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);

        getWindow().getDecorView().post(() -> {
            boolean hasCutout = false;
            android.view.WindowInsets insets =
                    getWindow().getDecorView().getRootWindowInsets();
            if (insets != null) {
                android.view.DisplayCutout cutout = insets.getDisplayCutout();
                hasCutout = cutout != null && !cutout.getBoundingRects().isEmpty();
            }
            Log.i(TAG, "detectCutout (SHORT_EDGES probe): " + hasCutout);
            nativeSetHasCutout(hasCutout);
            // Restore the user's actual cutout preference.
            setCutoutMode(sCutoutEnabled);
        });
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (newConfig.orientation != Configuration.ORIENTATION_LANDSCAPE) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        }
        // Keep overlay in sync as a fallback for devices that ignore the lock.
        syncRotateOverlay();
        detectAndReportCutout();
        float density = getResources().getDisplayMetrics().density;
        nativeSetDisplayDensity(density);
        Log.i(TAG, "onConfigurationChanged: orientation=" + newConfig.orientation
                + " density=" + density);
    }

    /**
     * Shows a blocking dialog explaining why "All files access" is needed.
     * Grant  → opens EMU7800's specific entry in Settings > Special app access >
     *           All files access (not the full list).
     * Cancel → closes the app.
     */
    private void showStoragePermissionDialog() {
        new AlertDialog.Builder(this)
                .setTitle("Storage Permission Required")
                .setMessage(
                        "EMU7800 needs \"All files access\" to browse your ROM files.\n\n"
                        + "Tap Grant, then enable the toggle next to EMU7800.\n\n"
                        + "Tap Cancel to close the app.")
                .setCancelable(false)
                .setPositiveButton("Grant",  (d, w) -> openAllFilesPermission())
                .setNegativeButton("Cancel", (d, w) -> finish())
                .show();
    }

    /**
     * Opens the app-specific "All files access" toggle in system Settings.
     * Falls back to the full list if the device doesn't support the targeted intent.
     */
    private void openAllFilesPermission() {
        try {
            Intent intent = new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivityForResult(intent, MANAGE_STORAGE_REQUEST);
        } catch (Exception e) {
            Log.w(TAG, "Targeted all-files intent failed, using general list: " + e.getMessage());
            Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
            startActivityForResult(intent, MANAGE_STORAGE_REQUEST);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == MANAGE_STORAGE_REQUEST) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    && Environment.isExternalStorageManager()) {
                // Permission granted.  SDL is already running — no restart needed.
                // Set the native dirs (skipped earlier) then rescan so the
                // filepicker immediately shows ROM files.
                Log.i(TAG, "All files access granted — initialising dirs and rescanning");
                initNativeDirs();
                nativeRescan();
            } else {
                // User returned without granting.  Show dialog again so they can
                // retry or exit cleanly rather than seeing a broken empty list.
                Log.i(TAG, "All files access still not granted");
                showStoragePermissionDialog();
            }
            return;
        }

        if (requestCode != ROM_PICKER_REQUEST) return;
        if (resultCode != RESULT_OK || data == null) {
            Log.i(TAG, "ROM picker cancelled");
            nativeOnRomSelected(-1, "");
            return;
        }

        Uri uri = data.getData();
        if (uri == null) {
            Log.w(TAG, "onActivityResult: null URI");
            nativeOnRomSelected(-1, "");
            return;
        }

        try {
            getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) { }

        String filename = getDisplayName(uri);
        Log.i(TAG, "ROM picked: " + filename + " uri=" + uri);

        try (ParcelFileDescriptor pfd =
                     getContentResolver().openFileDescriptor(uri, "r")) {
            if (pfd == null) {
                Log.e(TAG, "openFileDescriptor returned null");
                nativeOnRomSelected(-1, filename);
                return;
            }
            nativeOnRomSelected(pfd.getFd(), filename);
        } catch (Exception e) {
            Log.e(TAG, "Failed to open ROM fd: " + e.getMessage());
            nativeOnRomSelected(-1, filename);
        }
    }

    /** Resolve the human-readable filename from a SAF URI. */
    private String getDisplayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(
                uri,
                new String[]{OpenableColumns.DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                String name = cursor.getString(0);
                if (name != null && !name.isEmpty()) return name;
            }
        } catch (Exception ignored) { }

        String seg = uri.getLastPathSegment();
        return (seg != null) ? seg : "rom.bin";
    }

    /** Pass directory paths and display metrics to native. */
    private void initNativeDirs() {
        nativeSetCacheDir(getCacheDir().getAbsolutePath());
        Log.i(TAG, "cache dir: " + getCacheDir().getAbsolutePath());

        nativeSetDataDir(getFilesDir().getAbsolutePath());
        Log.i(TAG, "data dir: " + getFilesDir().getAbsolutePath());

        // Pass display density so the filepicker can size its bitmap font to
        // match Android's 16sp body text (the same size as AlertDialog body text).
        float density = getResources().getDisplayMetrics().density;
        nativeSetDisplayDensity(density);
        Log.i(TAG, "display density: " + density);
    }

    /* ---- Library loading ---- */

    @Override
    protected String[] getLibraries() {
        return new String[]{"main"};
    }

    /* ---- Native method declarations ---- */

    public static native void nativeSetCacheDir(String path);
    public static native void nativeSetDataDir(String path);
    public static native void nativeOnRomSelected(int fd, String filename);

    /** Triggers a filepicker directory rescan on the SDL thread side. */
    public static native void nativeRescan();

    /** Sets the display density so the filepicker can match the system's 16sp body text. */
    public static native void nativeSetDisplayDensity(float density);

    /** Tells native whether the current display has a cutout (notch). */
    public static native void nativeSetHasCutout(boolean hasCutout);

    /** Delivers a gamepad button press/release to the emulator (bypasses SDL event queue). */
    public static native void nativeControllerButton(int button, boolean pressed);

    /** Delivers a gamepad axis value to the emulator (bypasses SDL event queue). */
    public static native void nativeControllerAxis(int axis, float value);

    /**
     * Called from Java's update-check thread when a newer GitHub release is confirmed.
     * version: e.g. "1.0.2", note: release body text, url: html_url of the release.
     */
    public static native void nativeUpdateResult(String version, String note, String url);

    /**
     * Called from native when the user presses Back while the filepicker is
     * already at /storage/emulated/0/.  Implements the standard Android
     * double-back-to-exit pattern: Toast on first press, finish() on second
     * press within 2 seconds.
     */
    public static void onFilepickerAtRoot() {
        EMU7800Activity a = sInstance;
        if (a == null) return;
        a.runOnUiThread(() -> {
            long now = System.currentTimeMillis();
            if (now - a.mLastBackPressTime < 2000) {
                if (a.mExitToast != null) { a.mExitToast.cancel(); a.mExitToast = null; }
                a.finish();
            } else {
                a.mLastBackPressTime = now;
                a.mExitToast = Toast.makeText(a, "Press back again to exit", Toast.LENGTH_SHORT);
                a.mExitToast.show();
            }
        });
    }

    /**
     * Called from native to enable or disable drawing into the display cutout
     * ("notch") area.  Uses SHORT_EDGES mode so the game extends into the
     * camera cutout when the device is in landscape.  Requires API 28+;
     * silently ignored on older devices.
     */
    public static void setCutoutMode(boolean enabled) {
        sCutoutEnabled = enabled;
        EMU7800Activity a = sInstance;
        if (a == null) return;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return;
        a.runOnUiThread(() -> {
            WindowManager.LayoutParams lp = a.getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = enabled
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER;
            a.getWindow().setAttributes(lp);
            Log.i(TAG, "setCutoutMode: " + enabled);
        });
    }

    /* ---- Update check ---- */

    private static final String GITHUB_RELEASES_URL =
            "https://api.github.com/repos/alan-morford/emu7800forandroid/releases/latest";

    /**
     * Called from native (updater.c → jni_start_update_check) to kick off the
     * background HTTPS fetch against the GitHub Releases API.
     * Runs the network call on a daemon thread so it never blocks the UI.
     */
    public static void startUpdateCheck() {
        Thread t = new Thread(() -> {
            try {
                URL url = new URL(GITHUB_RELEASES_URL);
                HttpURLConnection conn = (HttpURLConnection) url.openConnection();
                conn.setConnectTimeout(10000);
                conn.setReadTimeout(10000);
                conn.setRequestProperty("Accept", "application/vnd.github+json");
                conn.setRequestProperty("User-Agent", "EMU7800-Android");

                int code = conn.getResponseCode();
                if (code != 200) {
                    Log.w(TAG, "Update check: HTTP " + code);
                    return;
                }

                StringBuilder sb = new StringBuilder();
                try (BufferedReader br = new BufferedReader(
                        new InputStreamReader(conn.getInputStream()))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line);
                }

                JSONObject json    = new JSONObject(sb.toString());
                String tagName     = json.optString("tag_name", "");
                String body        = json.optString("body", "");
                String htmlUrl     = json.optString("html_url", "");

                /* Find the .apk asset download URL; fall back to html_url */
                String apkUrl = "";
                JSONArray assets = json.optJSONArray("assets");
                if (assets != null) {
                    for (int i = 0; i < assets.length(); i++) {
                        JSONObject asset = assets.getJSONObject(i);
                        String dlUrl = asset.optString("browser_download_url", "");
                        if (dlUrl.toLowerCase().endsWith(".apk")) {
                            apkUrl = dlUrl;
                            break;
                        }
                    }
                }
                if (apkUrl.isEmpty()) apkUrl = htmlUrl;

                /* Strip leading 'v' prefix from tag name */
                String remoteVer = tagName.startsWith("v") ? tagName.substring(1) : tagName;
                String localVer  = "";
                try {
                    EMU7800Activity inst = sInstance;
                    if (inst != null)
                        localVer = inst.getPackageManager()
                                       .getPackageInfo(inst.getPackageName(), 0).versionName;
                } catch (Exception ignored) {}

                Log.i(TAG, "Update check: local=" + localVer + " remote=" + remoteVer
                        + " apkUrl=" + apkUrl);

                if (!remoteVer.isEmpty() && isNewerVersion(localVer, remoteVer)) {
                    /* Truncate long release notes for the popup */
                    if (body.length() > 800) body = body.substring(0, 800) + "...";
                    nativeUpdateResult(remoteVer, body, apkUrl);
                }
            } catch (Exception e) {
                Log.w(TAG, "Update check failed: " + e.getMessage());
            }
        });
        t.setDaemon(true);
        t.start();
    }

    /** Returns true if remoteVersion is strictly newer than localVersion (major.minor.patch). */
    private static boolean isNewerVersion(String local, String remote) {
        try {
            String[] l = local.split("\\.");
            String[] r = remote.split("\\.");
            int len = Math.max(l.length, r.length);
            for (int i = 0; i < len; i++) {
                int lv = i < l.length ? Integer.parseInt(l[i]) : 0;
                int rv = i < r.length ? Integer.parseInt(r[i]) : 0;
                if (rv > lv) return true;
                if (rv < lv) return false;
            }
        } catch (NumberFormatException ignored) {}
        return false;
    }

    /**
     * Called from filepicker.c (via jni_bridge.c) when the user taps "UPDATE"
     * in the in-app update popup.  Uses DownloadManager so the system provides
     * a content:// URI — no FileProvider needed and no FileUriExposedException.
     */
    public static void downloadAndInstallApk(final String url) {
        EMU7800Activity a = sInstance;
        if (a == null || url == null || url.isEmpty()) return;
        Log.i(TAG, "downloadAndInstallApk: " + url);
        a.runOnUiThread(() -> {
            try {
                DownloadManager dm = (DownloadManager) a.getSystemService(Context.DOWNLOAD_SERVICE);
                DownloadManager.Request req = new DownloadManager.Request(Uri.parse(url))
                        .setTitle("EMU7800 Update")
                        .setDescription("Downloading update...")
                        .setNotificationVisibility(
                                DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                        .setMimeType("application/vnd.android.package-archive")
                        .setDestinationInExternalPublicDir(
                                Environment.DIRECTORY_DOWNLOADS, "EMU7800-update.apk");
                final long downloadId = dm.enqueue(req);
                final BroadcastReceiver[] holder = {null};
                holder[0] = new BroadcastReceiver() {
                    @Override
                    public void onReceive(Context ctx, Intent intent) {
                        long id = intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1);
                        if (id != downloadId) return;
                        a.unregisterReceiver(holder[0]);
                        Uri apkUri = dm.getUriForDownloadedFile(id);
                        if (apkUri == null) {
                            Log.e(TAG, "downloadAndInstallApk: no URI for download");
                            return;
                        }
                        Intent install = new Intent(Intent.ACTION_INSTALL_PACKAGE)
                                .setDataAndType(apkUri,
                                        "application/vnd.android.package-archive")
                                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                        try { a.startActivity(install); }
                        catch (Exception e) {
                            Log.e(TAG, "downloadAndInstallApk: install failed: " + e.getMessage());
                        }
                    }
                };
                a.registerReceiver(holder[0],
                        new IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE));
            } catch (Exception e) {
                Log.w(TAG, "downloadAndInstallApk: " + e.getMessage());
            }
        });
    }

    /** Opens a URL in the device's default browser. */
    public static void openUrl(String url) {
        EMU7800Activity a = sInstance;
        if (a == null || url == null || url.isEmpty()) return;
        try {
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            a.startActivity(intent);
        } catch (Exception e) {
            Log.w(TAG, "openUrl: " + e.getMessage());
        }
    }

    /** Opens a mailto: intent for the EMU7800 bug report email. */
    public static void sendBugReportEmail() {
        EMU7800Activity a = sInstance;
        if (a == null) return;
        Intent intent = new Intent(Intent.ACTION_SENDTO,
                android.net.Uri.parse("mailto:alanmorford@gmail.com?subject=EMU7800%20bug%20report"));
        intent.putExtra(Intent.EXTRA_SUBJECT, "EMU7800 bug report");
        try {
            a.startActivity(Intent.createChooser(intent, "Send bug report"));
        } catch (Exception e) {
            Log.w(TAG, "sendBugReportEmail: no email app found: " + e.getMessage());
        }
    }
}
