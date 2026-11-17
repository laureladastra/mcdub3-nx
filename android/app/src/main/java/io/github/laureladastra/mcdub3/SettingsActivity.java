package io.github.laureladastra.mcdub3;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;

/* The settings screen, from the start screen (LauncherActivity): every
 * setting of GameSettings.ALL as a group of choices, saved as soon as one
 * is picked. Nothing here knows a particular setting. */
public class SettingsActivity extends Activity {
    private SharedPreferences prefs;
    private LinearLayout root;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = GameSettings.prefs(this);
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(16, 16, 22));
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(28);
        root.setPadding(pad, dp(20), pad, pad);
        scroll.addView(root);
        /* clear of the status and navigation bars, as the start screen */
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
        build();                    // notes follow what changed (a controller connected)
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

    private void build() {
        root.removeAllViews();
        root.addView(text("Settings", 26, true, Color.WHITE));
        root.addView(text("Applied when the game starts.", 13, false, Color.GRAY));
        for (int i = 0; i < GameSettings.ALL.length; i++)
            addSetting(GameSettings.ALL[i], 2000 + 100 * i);
        Button done = new Button(this);
        done.setText("Done");
        done.setAllCaps(false);
        done.setOnClickListener(v -> finish());
        root.addView(done);
    }

    /* One setting: its title, a radio button per choice (ids from base),
     * its note. */
    private void addSetting(GameSettings.Setting s, int base) {
        String value = s.value(prefs);
        TextView title = text(s.title, 15, true, Color.LTGRAY);
        title.setPadding(0, dp(16), 0, dp(4));
        root.addView(title);
        RadioGroup g = new RadioGroup(this);
        for (int i = 0; i < s.choices.length; i++) {
            RadioButton rb = new RadioButton(this);
            rb.setId(base + i);
            rb.setText(s.choices[i][1]);
            rb.setTextColor(Color.WHITE);
            rb.setPadding(dp(8), dp(6), 0, dp(6));
            g.addView(rb);
            if (s.choices[i][0].equals(value))
                g.check(rb.getId());
        }
        g.setOnCheckedChangeListener((grp, id) ->
            prefs.edit().putString(s.key, s.choices[id - base][0]).apply());
        root.addView(g);
        if (s.note != null)
            root.addView(text(s.note.apply(this), 13, false, Color.GRAY));
    }
}
