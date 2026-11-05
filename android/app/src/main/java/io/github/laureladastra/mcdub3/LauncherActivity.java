package io.github.laureladastra.mcdub3;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;

/* The start screen (ADR 0010): which game folder to play, before SDL and the
 * game start (MainActivity). The first time it is the onboarding -- storage
 * access, then the folder -- and afterwards the list of folders with Play,
 * Add folder and Remove, and the on-screen gamepad's setting, all on the
 * one screen.
 *
 * A game folder is the extracted disc: the folder holding default.xbe, any
 * edition (the game checks the disc itself at boot). Found by themselves:
 * game*\/ in the app's own folder and in <sd_dir>/ and switch/<sd_dir>/ on
 * internal storage and every SD card (the Switch build's place: one card for
 * both under switchroot). Added by hand through Android's folder picker; a
 * folder holding game folders adds those. Reading outside the app's folder
 * takes "All files access", asked for here first. */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 1;
    private static final String PREFS = "launcher", FOLDERS = "folders", LAST = "last",
                                TOUCH_PAD = "touchPad";

    private SharedPreferences prefs;
    private LinearLayout root;
    private RadioGroup list;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(16, 16, 22));
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(28);
        root.setPadding(pad, dp(20), pad, pad);
        scroll.addView(root);
        /* Android 15 draws apps under the status and navigation bars: keep
         * the screen clear of them */
        scroll.setOnApplyWindowInsetsListener((v, in) -> {
            android.graphics.Insets b = in.getInsets(android.view.WindowInsets.Type.systemBars()
                                                     | android.view.WindowInsets.Type.displayCutout());
            v.setPadding(b.left, b.top, b.right, b.bottom);
            return android.view.WindowInsets.CONSUMED;
        });
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        build();                    // back from the permission page or the picker
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private TextView text(String s, int sp, boolean bold, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold)
            t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(0, dp(4), 0, dp(4));
        return t;
    }

    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    private LinearLayout row() {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(8), 0, dp(8));
        return r;
    }

    /* ── The screen ──────────────────────────────────────────────────── */

    private void build() {
        boolean access = Environment.isExternalStorageManager();
        List<String> folders = folders();
        String last = prefs.getString(LAST, null);
        root.removeAllViews();

        root.addView(text(getString(R.string.app_name), 26, true, Color.WHITE));

        if (folders.isEmpty()) {
            /* onboarding */
            root.addView(text("Welcome. The game runs from your own copy of the disc: "
                + "the extracted folder that holds default.xbe (US or European disc).",
                16, false, Color.LTGRAY));
            root.addView(text((access ? "✓ " : "1. ") + "Allow storage access, so the game can "
                + "read a folder anywhere on this device or its SD card.", 16, false,
                access ? Color.rgb(120, 200, 120) : Color.WHITE));
            if (!access)
                root.addView(button("Allow storage access", v -> openAccess()));
            root.addView(text((access ? "1. " : "2. ") + "Choose your game folder.", 16, false,
                Color.WHITE));
            Button add = button("Choose game folder", v -> pick());
            add.setEnabled(access);
            root.addView(add);
            root.addView(text("Already on the SD card for the Switch build (switch/"
                + getString(R.string.sd_dir) + "/game-usa or game-eur)? It is found by itself "
                + "once access is allowed.", 13, false, Color.GRAY));
            touchPadSetting();
            return;
        }

        if (!access) {
            root.addView(text("Storage access is off: folders outside the app's own folder "
                + "cannot be read.", 14, false, Color.rgb(240, 180, 80)));
            root.addView(button("Allow storage access", v -> openAccess()));
        }
        root.addView(text("Game folder", 15, true, Color.LTGRAY));
        list = new RadioGroup(this);
        int checked = -1;
        for (int i = 0; i < folders.size(); i++) {
            String f = folders.get(i);
            boolean ok = isGame(new File(f));
            RadioButton rb = new RadioButton(this);
            rb.setId(1000 + i);
            rb.setTag(f);
            rb.setText(f + "\n" + (ok ? "✓ default.xbe found" : "✗ no default.xbe here"));
            rb.setTextColor(ok ? Color.WHITE : Color.GRAY);
            rb.setPadding(dp(8), dp(10), 0, dp(10));
            list.addView(rb);
            if (ok && (checked < 0 || f.equals(last)))
                checked = rb.getId();
        }
        if (checked >= 0)
            list.check(checked);
        root.addView(list);

        LinearLayout actions = row();
        Button play = button("▶  Play", v -> play());
        play.setTextSize(18);
        actions.addView(play);
        actions.addView(button("Add folder", v -> pick()));
        actions.addView(button("Remove", v -> remove()));
        root.addView(actions);
        root.addView(text("Add folder: Android's folder picker; a folder that holds game "
            + "folders adds each of them. Folders found by themselves come back after "
            + "Remove while they hold a disc.", 13, false, Color.GRAY));
        touchPadSetting();
    }

    /* The on-screen gamepad: shown when no controller is connected, always
     * (Joy-Cons on the rails count as connected), or never. */
    private void touchPadSetting() {
        String mode = prefs.getString(TOUCH_PAD, "auto");
        List<String> pads = controllers();
        root.addView(text("On-screen gamepad", 15, true, Color.LTGRAY));
        RadioGroup g = new RadioGroup(this);
        String[][] opts = {
            { "auto", "Automatic: hidden while a controller is connected" },
            { "1", "Always show (touch and controllers both work)" },
            { "0", "Never: controllers or keyboard only" },
        };
        for (int i = 0; i < opts.length; i++) {
            RadioButton rb = new RadioButton(this);
            rb.setId(2000 + i);
            rb.setText(opts[i][1]);
            rb.setTextColor(Color.WHITE);
            rb.setPadding(dp(8), dp(6), 0, dp(6));
            g.addView(rb);
            if (opts[i][0].equals(mode))
                g.check(rb.getId());
        }
        g.setOnCheckedChangeListener((grp, id) ->
            prefs.edit().putString(TOUCH_PAD, opts[id - 2000][0]).apply());
        root.addView(g);
        root.addView(text(pads.isEmpty() ? "Controllers connected now: none"
            : "Controllers connected now: " + String.join(", ", pads), 13, false, Color.GRAY));
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

    /* ── Folders ─────────────────────────────────────────────────────── */

    private static boolean isGame(File dir) {
        return new File(dir, "default.xbe").canRead();
    }

    /* The game folders in dir: itself, or its game*-style subfolders. */
    private static List<String> expand(File dir) {
        List<String> out = new ArrayList<>();
        if (isGame(dir)) {
            out.add(dir.getAbsolutePath());
            return out;
        }
        File[] kids = dir.listFiles();
        if (kids != null) {
            Arrays.sort(kids);
            for (File k : kids)
                if (k.isDirectory() && isGame(k))
                    out.add(k.getAbsolutePath());
        }
        return out;
    }

    private List<String> found() {
        List<String> out = new ArrayList<>();
        List<File> roots = new ArrayList<>();
        roots.add(Environment.getExternalStorageDirectory());
        File[] vols = new File("/storage").listFiles();
        if (vols != null)
            for (File v : vols)
                if (!v.getName().equals("self") && !v.getName().equals("emulated"))
                    roots.add(v);
        File own = getExternalFilesDir(null);
        if (own != null)
            out.addAll(expand(own));
        String sd = getString(R.string.sd_dir);
        for (File r : roots)
            for (String sub : new String[] { sd, "switch/" + sd })
                out.addAll(expand(new File(r, sub)));
        return out;
    }

    private List<String> folders() {
        LinkedHashSet<String> all = new LinkedHashSet<>();
        String saved = prefs.getString(FOLDERS, "");
        for (String s : saved.split("\n"))
            if (!s.isEmpty())
                all.add(s);
        String removed = prefs.getString("removed", "");
        for (String f : found())
            if (!("\n" + removed + "\n").contains("\n" + f + "\n"))
                all.add(f);
        return new ArrayList<>(all);
    }

    private void save(List<String> folders) {
        prefs.edit().putString(FOLDERS, String.join("\n", folders)).apply();
    }

    /* A tree from the folder picker as a file path: primary:<path> is
     * internal storage, <volume id>:<path> an SD card under /storage. */
    private static String treePath(Uri uri) {
        String id = DocumentsContract.getTreeDocumentId(uri);
        int c = id.indexOf(':');
        String vol = c < 0 ? id : id.substring(0, c), rest = c < 0 ? "" : id.substring(c + 1);
        String base = vol.equalsIgnoreCase("primary")
            ? Environment.getExternalStorageDirectory().getAbsolutePath() : "/storage/" + vol;
        return rest.isEmpty() ? base : base + "/" + rest;
    }

    /* ── Actions ─────────────────────────────────────────────────────── */

    private void openAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void pick() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null)
            return;
        File dir = new File(treePath(data.getData()));
        List<String> games = expand(dir);
        List<String> folders = folders();
        if (games.isEmpty()) {
            new android.app.AlertDialog.Builder(this)
                .setTitle("No game here")
                .setMessage(dir + "\n\nholds no default.xbe, nor do its folders. Choose the "
                    + "extracted disc's folder (the one with default.xbe in it).")
                .setPositiveButton("OK", null).show();
            return;
        }
        String removed = prefs.getString("removed", "");
        for (String g : games) {
            if (!folders.contains(g))
                folders.add(g);
            removed = ("\n" + removed + "\n").replace("\n" + g + "\n", "\n").trim();
        }
        prefs.edit().putString("removed", removed).putString(LAST, games.get(0)).apply();
        save(folders);
        build();
    }

    private String selected() {
        if (list == null || list.getCheckedRadioButtonId() < 0)
            return null;
        return (String) list.findViewById(list.getCheckedRadioButtonId()).getTag();
    }

    private void remove() {
        String f = selected();
        if (f == null)
            return;
        List<String> folders = folders();
        folders.remove(f);
        save(folders);
        String removed = prefs.getString("removed", "");
        prefs.edit().putString("removed", removed.isEmpty() ? f : removed + "\n" + f).apply();
        build();
    }

    private void play() {
        String f = selected();
        if (f == null || !isGame(new File(f))) {
            new android.app.AlertDialog.Builder(this)
                .setMessage("Choose a folder with default.xbe first.")
                .setPositiveButton("OK", null).show();
            return;
        }
        prefs.edit().putString(LAST, f).apply();
        Intent i = new Intent(this, MainActivity.class);
        i.putExtra(MainActivity.EXTRA_GAME_DIR, f);
        i.putExtra(MainActivity.EXTRA_TOUCH_PAD, prefs.getString(TOUCH_PAD, "auto"));
        startActivity(i);
        finish();
    }
}
