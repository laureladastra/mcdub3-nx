package io.github.laureladastra.mcdub3;

import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
import android.os.Bundle;
import android.provider.Settings;
import android.system.Os;
import android.view.InputDevice;
import android.view.KeyEvent;

import org.libsdl.app.SDLActivity;

/* SDL's activity, loading SDL2 and the game (libmain.so, android/app/jni). */
public class MainActivity extends SDLActivity {
    /* The game folder chosen on the start screen (LauncherActivity). */
    public static final String EXTRA_GAME_DIR = "gameDir";
    /* The on-screen gamepad: "auto", "1" always, "0" never (touch_pad.c). */
    public static final String EXTRA_TOUCH_PAD = "touchPad";

    @Override
    protected void onCreate(Bundle state) {
        /* For android_host.c, before the game starts: it wins over the
         * settings file and the search. */
        String dir = getIntent() != null ? getIntent().getStringExtra(EXTRA_GAME_DIR) : null;
        String pad = getIntent() != null ? getIntent().getStringExtra(EXTRA_TOUCH_PAD) : null;
        try {
            if (dir != null)
                Os.setenv("MC3_LAUNCHER_GAME_DIR", dir, true);
            if (pad != null)
                Os.setenv("MC3_LAUNCHER_TOUCH_PAD", pad, true);
        } catch (Exception e) {
            /* the native search and defaults then */
        }
        super.onCreate(state);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    /* "All files access", for a disc outside the app's folder (an SD card
     * shared with the Switch build); called from android_host.c. */
    public static boolean hasAllFilesAccess() {
        return Environment.isExternalStorageManager();
    }

    public static boolean openAllFilesAccess() {
        if (mSingleton == null)
            return false;
        try {
            Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                  Uri.parse("package:" + mSingleton.getPackageName()));
            mSingleton.startActivity(i);
        } catch (Exception e) {
            mSingleton.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
        return true;
    }

    /* A keyboard's keys go to SDL as keys. SDL takes any device that also
     * reports a d-pad for a joystick -- the emulator's keyboard and many real
     * ones do -- and its arrows then became a pad's d-pad, not the arrows
     * the keyboard layout drives with (input/sdl_keyboard.c). Gamepads and
     * the volume keys go the usual way. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        InputDevice dev = event.getDevice();
        if (dev != null && dev.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC
            && !dev.supportsSource(InputDevice.SOURCE_GAMEPAD)
            && !dev.supportsSource(InputDevice.SOURCE_JOYSTICK)
            && code != KeyEvent.KEYCODE_VOLUME_UP && code != KeyEvent.KEYCODE_VOLUME_DOWN
            && code != KeyEvent.KEYCODE_VOLUME_MUTE) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                if (event.getRepeatCount() == 0)
                    SDLActivity.onNativeKeyDown(code);
                return true;
            }
            if (event.getAction() == KeyEvent.ACTION_UP) {
                SDLActivity.onNativeKeyUp(code);
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }
}
