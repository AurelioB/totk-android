package com.aure.totk;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.provider.Settings;
import android.system.Os;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicBoolean;

/** The game's software keyboard (nn::swkbd, runtime/swkbd_host.c) on Android: a dialog over the game with one text
 * field and the system keyboard. It starts with the game's initial text and its prompt as the title; the field takes only
 * what TextRules allows (length limit, disabled keys, number pad); OK sends the text once it is long enough, Cancel or Back
 * cancels. A gamepad's A / B confirm / cancel (by label, or by position with TOTK_FACE_BUTTONS=position as in the game);
 * Android's own key fallback does not: A only opens the keyboard and the field swallows B. A controller that counts as a
 * hardware keyboard makes Android hold the on-screen keyboard back (Retroid Pocket Duo: show_ime_with_hard_keyboard=0):
 * the dialog asks for it as a tap on the field does, and if it still stays hidden says so, with a button to Android's
 * "Show on-screen keyboard" setting. The answer goes back exactly once. */
final class TextEntry {
    interface Answer { void done(String text); }   // null: cancelled

    private TextEntry() {}

    /** UI thread. */
    static void show(Activity activity, String prompt, String initial, TextRules rules, boolean secure, Answer answer) {
        AtomicBoolean answered = new AtomicBoolean();
        EditText edit = new EditText(activity);
        edit.setSingleLine(true);
        edit.setInputType(rules.digitsOnly
                ? InputType.TYPE_CLASS_NUMBER | (secure ? InputType.TYPE_NUMBER_VARIATION_PASSWORD : 0)
                : InputType.TYPE_CLASS_TEXT | (secure ? InputType.TYPE_TEXT_VARIATION_PASSWORD : InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS));
        edit.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        edit.setFilters(new InputFilter[]{(source, start, end, dest, dstart, dend) -> {
            CharSequence typed = source.subSequence(start, end);
            int room = rules.maxUnits - (dest.length() - (dend - dstart));
            String kept = rules.clean(typed, Math.max(0, room));
            return kept.contentEquals(typed) ? null : kept;
        }});
        String first = rules.clean(initial == null ? "" : initial);
        edit.setText(first);
        edit.setSelection(first.length());
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(edit);
        TextView hint = new TextView(activity);
        hint.setText("No on-screen keyboard: Android hides it while a controller counts as a hardware keyboard. Tap the field, or turn on \"Show on-screen keyboard\".");
        hint.setVisibility(android.view.View.GONE);
        box.addView(hint);
        AlertDialog dialog = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(prompt == null || prompt.isEmpty() ? null : prompt)
                .setView(box)
                .setPositiveButton(android.R.string.ok, (d, which) -> {
                    String text = edit.getText().toString();
                    if (answered.compareAndSet(false, true)) answer.done(rules.acceptable(text) ? text : null);
                })
                .setNegativeButton(android.R.string.cancel, null)
                .setNeutralButton("Keyboard settings", null)
                .create();
        dialog.setCanceledOnTouchOutside(false);
        boolean byPosition = "position".equals(Os.getenv("TOTK_FACE_BUTTONS"));   // the game's layout (frontend_android.c switch_bit)
        int confirmKey = byPosition ? KeyEvent.KEYCODE_BUTTON_B : KeyEvent.KEYCODE_BUTTON_A;
        int cancelKey = byPosition ? KeyEvent.KEYCODE_BUTTON_A : KeyEvent.KEYCODE_BUTTON_B;
        dialog.setOnKeyListener((d, code, event) -> {   // before the focused field and the IME's unhandled-key path
            if (code != confirmKey && code != cancelKey) return false;
            if (event.getAction() != KeyEvent.ACTION_UP || event.isCanceled()) return true;
            if (code == cancelKey) dialog.cancel();
            else {
                Button ok = dialog.getButton(DialogInterface.BUTTON_POSITIVE);
                if (ok.isEnabled()) ok.performClick();
            }
            return true;
        });
        dialog.setOnDismissListener(d -> { if (answered.compareAndSet(false, true)) answer.done(null); });
        dialog.setOnShowListener(d -> {
            Button ok = dialog.getButton(DialogInterface.BUTTON_POSITIVE);
            ok.setEnabled(rules.acceptable(edit.getText()));
            edit.addTextChangedListener(new TextWatcher() {
                @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
                @Override public void afterTextChanged(Editable s) { ok.setEnabled(rules.acceptable(s)); }
            });
            edit.setOnEditorActionListener((v, action, event) -> {
                if (action != EditorInfo.IME_ACTION_DONE) return false;
                if (ok.isEnabled()) ok.performClick();
                return true;
            });
            Button settings = dialog.getButton(DialogInterface.BUTTON_NEUTRAL);
            settings.setVisibility(android.view.View.GONE);
            settings.setOnClickListener(v -> {   // keeps the dialog open
                try { activity.startActivity(new Intent(Settings.ACTION_HARD_KEYBOARD_SETTINGS)); }
                catch (RuntimeException e) { activity.startActivity(new Intent(Settings.ACTION_INPUT_METHOD_SETTINGS)); }
            });
            edit.requestFocus();
            edit.post(() -> {   // what a tap on the field does; SOFT_INPUT_STATE_ALWAYS_VISIBLE alone is ignored then
                InputMethodManager imm = activity.getSystemService(InputMethodManager.class);
                if (imm != null) imm.showSoftInput(edit, 0);
            });
            edit.postDelayed(() -> {
                WindowInsets insets = edit.getRootWindowInsets();
                if (!dialog.isShowing() || insets != null && insets.isVisible(WindowInsets.Type.ime())) return;
                hint.setVisibility(android.view.View.VISIBLE);
                settings.setVisibility(android.view.View.VISIBLE);
            }, 1000);
        });
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        dialog.show();
    }
}
