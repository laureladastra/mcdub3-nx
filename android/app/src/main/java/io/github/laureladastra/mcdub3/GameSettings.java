package io.github.laureladastra.mcdub3;

import android.content.Context;
import android.content.SharedPreferences;
import android.view.InputDevice;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;

/* The game's settings on Android (SettingsActivity shows them). Each is a
 * runtime switch the game already reads -- the RECOMP_* / MC3_* variables
 * of <app>_env.txt (README, Configuration) -- with a few choices, kept in
 * the launcher's preferences.
 *
 * A new setting is one entry in ALL: SettingsActivity draws it, and the
 * game gets it as its variable (env(), MainActivity, android_host.c), with
 * nothing to add on the native side. The first choice is the default; only
 * a choice other than the default is handed to the game, so a variable set
 * in <app>_env.txt still applies while its setting is left at the default,
 * and a choice made here wins over the file. Settings apply when the game
 * starts. */
final class GameSettings {
    static final String PREFS = "launcher";

    static final class Setting {
        final String key;              /* in the preferences */
        final String env;              /* the variable the game reads */
        final String title;
        final String[][] choices;      /* { value, label }; the first is the default */
        final Function<Context, String> note;   /* below the choices, or null */

        Setting(String key, String env, String title, String[][] choices,
                Function<Context, String> note) {
            this.key = key;
            this.env = env;
            this.title = title;
            this.choices = choices;
            this.note = note;
        }

        String fallback() {
            return choices[0][0];
        }

        /* The stored choice; the default for one not (or no longer) offered. */
        String value(SharedPreferences prefs) {
            String v = prefs.getString(key, fallback());
            for (String[] c : choices)
                if (c[0].equals(v))
                    return v;
            return fallback();
        }
    }

    static final Setting[] ALL = {
        /* touch_pad.c: shown when no controller is connected, always
         * (Joy-Cons on the rails count as connected), or never */
        new Setting("touchPad", "RECOMP_TOUCH_PAD", "On-screen gamepad", new String[][] {
            { "auto", "Automatic: hidden while a controller is connected" },
            { "1", "Always show (touch and controllers both work)" },
            { "0", "Never: controllers or keyboard only" },
        }, ctx -> {
            List<String> pads = controllers();
            return pads.isEmpty() ? "Controllers connected now: none"
                : "Controllers connected now: " + String.join(", ", pads);
        }),
        /* kernel_xbox.c: whether the game is told the TV is widescreen. At
         * 4:3 it renders its original framing, shown with black side bars,
         * and the renderer's 16:9 fill and square pixels are off */
        new Setting("aspect", "RECOMP_WIDESCREEN", "Aspect ratio", new String[][] {
            { "1", "16:9 widescreen" },
            { "0", "4:3: the original framing, black bars at the sides" },
        }, null),
    };

    private GameSettings() {
    }

    static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /* For the game (MainActivity -> MC3_LAUNCHER_ENV -> android_host.c):
     * VAR=value per line, the settings not at their default. */
    static String env(SharedPreferences prefs) {
        StringBuilder sb = new StringBuilder();
        for (Setting s : ALL) {
            String v = s.value(prefs);
            if (!v.equals(s.fallback()))
                sb.append(s.env).append('=').append(v).append('\n');
        }
        return sb.toString();
    }

    private static List<String> controllers() {
        List<String> out = new ArrayList<>();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual())
                continue;
            int src = d.getSources();
            if ((src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                if (!out.contains(d.getName()))
                    out.add(d.getName());
        }
        return out;
    }
}
