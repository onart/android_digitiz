package com.onart.digitiz;

import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.InputType;
import android.util.TypedValue;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.Toast;

import com.google.androidgamesdk.GameActivity;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Consumer;

/**
 * Everything that matters happens in C++. This exists because GameActivity
 * needs a Java entry point, and for the few things that are Java-only:
 * keeping the screen awake, hiding the system bars, and showing a Toast.
 */
public class MainActivity extends GameActivity {

    /** How long the second back press has to arrive to count as a confirmation. */
    private static final long EXIT_CONFIRM_WINDOW_MS = 2000;

    private static final String PREFS = "digitiz";
    /** Absent until the user first flips; until then the manifest's sensorLandscape rules. */
    private static final String KEY_REVERSED = "orientation_reversed";

    private long lastBackPressAt = 0;
    private Toast exitToast;

    static {
        System.loadLibrary("digitiz_guest");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before super.onCreate(), which is where GameActivity builds the
        // window: requesting the orientation first means it comes up the right
        // way round instead of rotating once the user can already see it.
        applyStoredOrientation();

        super.onCreate(savedInstanceState);

        // The session dies with the screen, so do not let it sleep while the
        // phone is being used as a tablet.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    // --- orientation -------------------------------------------------------
    //
    // The manifest asks for sensorLandscape, which is right while the phone is
    // being held: turn it over and the picture follows. It is useless once the
    // phone is lying flat on a desk, though — the accelerometer cannot tell
    // which way round a horizontal phone is, so whichever way it came up is
    // the way it stays. That is exactly the position this app is used in, and
    // the USB cable decides which edge is reachable, so there has to be a way
    // to say "the other way round" by hand.
    //
    // Doing it through the OS rather than by turning our own rendering keeps
    // the system bars, the cutout and the touch mapping on the correct side;
    // rotating only what we draw would leave all three where they were.

    private void applyStoredOrientation() {
        final SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        if (!prefs.contains(KEY_REVERSED)) {
            return;
        }
        setRequestedOrientation(prefs.getBoolean(KEY_REVERSED, false)
                ? ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
                : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    /**
     * Turns the display the other way round and remembers it. Note this also
     * ends auto-rotation: a half turn is only meaningful as a fixed choice,
     * and the sensor would be free to undo it the moment the phone is picked
     * up. Called from native, off the render thread.
     */
    @SuppressWarnings("unused")
    public void flipOrientation() {
        runOnUiThread(() -> {
            // Derived from the rotation the display actually has rather than
            // from the stored flag, so the first press flips no matter which
            // way the sensor happened to leave things.
            final int rotation = getWindowManager().getDefaultDisplay().getRotation();
            final boolean reversed = rotation != Surface.ROTATION_270;

            getSharedPreferences(PREFS, MODE_PRIVATE)
                    .edit()
                    .putBoolean(KEY_REVERSED, reversed)
                    .apply();
            setRequestedOrientation(reversed
                    ? ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
                    : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        });
    }

    // --- custom buttons ----------------------------------------------------
    //
    // Entering a shortcut or a pair of desktop coordinates needs a keyboard,
    // and the native side draws its own UI in GL with no text field in it.
    // Rather than build one, the two moments that need typing borrow a real
    // Android dialog: the platform IME, the validation, and the back button
    // all come for free, and the native UI stays a drawing surface.

    /** Must match ButtonKind in ButtonStore.hpp. */
    private static final int KIND_POINT = 0;
    private static final int KIND_REGION = 1;
    private static final int KIND_SHORTCUT = 2;

    /**
     * Region buttons are retired: a postage stamp standing in for a toolbar
     * reads well on paper and is fiddly in the hand. The kind still loads and
     * still works for anything already saved on a device -- it just cannot be
     * created any more. Put it back in this array to offer it again.
     */
    private static final int[] OFFERED_KINDS = {KIND_POINT, KIND_SHORTCUT};

    /** Must match proto::kMod* in messages.hpp. */
    private static final int MOD_CTRL = 1;
    private static final int MOD_SHIFT = 2;
    private static final int MOD_ALT = 4;
    private static final int MOD_META = 8;

    /** Must match ButtonCommand in ActivityBridge.hpp. */
    private static final int COMMAND_EDIT = -1;
    private static final int COMMAND_DELETE = 0;
    private static final int COMMAND_EARLIER = 1;
    private static final int COMMAND_LATER = 2;

    private static native void nativeButtonSaved(int index, int kind, String label, int x, int y,
                                                 int w, int h, int modifiers, String key);

    private static native void nativeButtonCommand(int index, int command);

    /** Must match PresetCommandKind in ActivityBridge.hpp. */
    private static final int PRESET_SELECT = 0;
    private static final int PRESET_CREATE = 1;
    private static final int PRESET_RENAME = 2;
    private static final int PRESET_BIND = 3;
    private static final int PRESET_UNBIND = 4;
    private static final int PRESET_DELETE = 5;

    private static native void nativePresetCommand(int command, int index, String text);

    /**
     * Called from native when the strip's tab is held. The presets come in
     * already formatted -- which one is current, and what each is bound to, is
     * decided on the native side, so this only has to show a list.
     */
    @SuppressWarnings("unused")
    public void showPresetMenu(final String[] names, final int current,
                               final String activeWindow) {
        runOnUiThread(() -> {
            final boolean haveWindow = activeWindow != null && !activeWindow.isEmpty();
            final List<String> items = new ArrayList<>(Arrays.asList(names));
            final int firstAction = items.size();

            items.add(getString(R.string.preset_new));
            items.add(getString(R.string.preset_rename));
            items.add(haveWindow ? getString(R.string.preset_bind, activeWindow)
                    : getString(R.string.preset_bind_none));
            items.add(getString(R.string.preset_unbind));
            items.add(getString(R.string.preset_delete));

            new AlertDialog.Builder(this)
                    .setTitle(R.string.preset_title)
                    .setItems(items.toArray(new String[0]), (dialog, which) -> {
                        if (which < firstAction) {
                            nativePresetCommand(PRESET_SELECT, which, null);
                            return;
                        }
                        switch (which - firstAction) {
                            case 0:
                                promptForName(R.string.preset_new, "",
                                        name -> nativePresetCommand(PRESET_CREATE, -1, name));
                                break;
                            case 1:
                                promptForName(R.string.preset_rename, names[current],
                                        name -> nativePresetCommand(PRESET_RENAME, current, name));
                                break;
                            case 2:
                                if (haveWindow) {
                                    nativePresetCommand(PRESET_BIND, current, activeWindow);
                                }
                                break;
                            case 3:
                                nativePresetCommand(PRESET_UNBIND, current, null);
                                break;
                            default:
                                new AlertDialog.Builder(this)
                                        .setTitle(R.string.preset_delete_title)
                                        .setMessage(getString(R.string.preset_delete_message,
                                                names[current]))
                                        .setNegativeButton(android.R.string.cancel, null)
                                        .setPositiveButton(R.string.preset_delete,
                                                (d, w) -> nativePresetCommand(PRESET_DELETE,
                                                        current, null))
                                        .show();
                                break;
                        }
                    })
                    .show();
        });
    }

    /** One text field and an OK, refusing to return an empty name. */
    private void promptForName(int titleRes, String initial, Consumer<String> onName) {
        final EditText field = new EditText(this);
        field.setSingleLine(true);
        field.setText(initial);
        field.setSelection(field.getText().length());
        field.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_DONE);

        final int pad = dp(20);
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, dp(8), pad, 0);
        root.addView(field);

        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(titleRes)
                .setView(root)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(android.R.string.ok, null)
                .create();
        dialog.show();
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).setOnClickListener(v -> {
            final String name = field.getText().toString().trim();
            if (name.isEmpty()) {
                Toast.makeText(this, R.string.preset_error_name, Toast.LENGTH_SHORT).show();
                return;
            }
            onName.accept(name);
            dialog.dismiss();
        });
    }

    /** Called from native. An {@code index} below zero creates a new button. */
    @SuppressWarnings("unused")
    public void showButtonEditor(final int index, final int kind, final String label, final int x,
                                 final int y, final int w, final int h, final int modifiers,
                                 final String key) {
        runOnUiThread(() -> buildEditor(index, kind, label, x, y, w, h, modifiers, key));
    }

    /** Called from native when a button is held down. */
    @SuppressWarnings("unused")
    public void showButtonMenu(final int index, final String label) {
        runOnUiThread(() -> {
            final String[] items = {
                    getString(R.string.button_menu_edit),
                    getString(R.string.button_menu_earlier),
                    getString(R.string.button_menu_later),
                    getString(R.string.button_menu_delete),
            };
            new AlertDialog.Builder(this)
                    .setTitle(label == null || label.isEmpty()
                            ? getString(R.string.button_menu_title) : label)
                    .setItems(items, (dialog, which) -> {
                        switch (which) {
                            case 0:
                                // Native holds the button's fields, so it
                                // answers by calling showButtonEditor back.
                                nativeButtonCommand(index, COMMAND_EDIT);
                                break;
                            case 1:
                                nativeButtonCommand(index, COMMAND_EARLIER);
                                break;
                            case 2:
                                nativeButtonCommand(index, COMMAND_LATER);
                                break;
                            default:
                                confirmDelete(index, label);
                                break;
                        }
                    })
                    .show();
        });
    }

    private void confirmDelete(final int index, final String label) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.button_delete_title)
                .setMessage(getString(R.string.button_delete_message, label))
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.button_menu_delete,
                        (dialog, which) -> nativeButtonCommand(index, COMMAND_DELETE))
                .show();
    }

    private void buildEditor(final int index, final int kind, final String label, final int x,
                             final int y, final int w, final int h, final int modifiers,
                             final String key) {
        final int pad = dp(20);

        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, dp(8), pad, 0);

        final RadioGroup kinds = new RadioGroup(this);
        kinds.setOrientation(RadioGroup.HORIZONTAL);
        for (final int offered : OFFERED_KINDS) {
            final RadioButton option = new RadioButton(this);
            // Keyed by kind rather than by position, so retiring one does not
            // renumber the others. RadioGroup reads 0 as "nothing checked".
            option.setId(offered + 1);
            option.setText(labelForKind(offered));
            kinds.addView(option);
        }
        // Editing a retired kind would otherwise leave nothing checked.
        kinds.check(isOffered(kind) ? kind + 1 : KIND_POINT + 1);
        root.addView(kinds);

        // The app is locked to landscape, and in landscape an IME defaults to
        // its fullscreen extract editor -- which covers the very dialog the
        // text is being typed into. NO_EXTRACT_UI keeps the keyboard docked so
        // the fields, the hint and the buttons all stay visible.
        final EditText nameField = new EditText(this);
        nameField.setHint(R.string.button_field_name);
        nameField.setSingleLine(true);
        nameField.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_NEXT);
        nameField.setText(label);
        root.addView(nameField);

        final TextView hint = new TextView(this);
        hint.setPadding(0, dp(12), 0, 0);
        root.addView(hint);

        final EditText valueField = new EditText(this);
        valueField.setSingleLine(true);
        valueField.setInputType(InputType.TYPE_CLASS_TEXT);
        valueField.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_DONE);
        root.addView(valueField);

        // Seeded from whichever fields this kind of button actually uses, so
        // reopening the editor shows what is already there.
        if (kind == KIND_SHORTCUT) {
            valueField.setText(formatShortcut(modifiers, key));
        } else if (kind == KIND_REGION) {
            valueField.setText(x + ", " + y + ", " + w + ", " + h);
        } else if (index >= 0) {
            valueField.setText(x + ", " + y);
        }
        applyKindHints(kind, hint, valueField);
        kinds.setOnCheckedChangeListener(
                (group, checked) -> applyKindHints(checked - 1, hint, valueField));

        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(index < 0 ? R.string.button_new_title : R.string.button_edit_title)
                .setView(root)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(android.R.string.ok, null)
                .create();
        dialog.show();

        // Wired after show() so that a rejected value can leave the dialog
        // open; a listener passed to the builder always dismisses.
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).setOnClickListener(
                v -> saveFromEditor(dialog, index, kinds.getCheckedRadioButtonId() - 1,
                        nameField, valueField));
    }

    private void saveFromEditor(AlertDialog dialog, int index, int kind, EditText nameField,
                                EditText valueField) {
        final String value = valueField.getText().toString().trim();
        String name = nameField.getText().toString().trim();

        if (kind == KIND_SHORTCUT) {
            final int mods = parseModifiers(value);
            final String keyName = parseKeyName(value);
            if (keyName.isEmpty()) {
                Toast.makeText(this, R.string.button_error_shortcut, Toast.LENGTH_SHORT).show();
                return;
            }
            if (name.isEmpty()) {
                name = formatShortcut(mods, keyName);
            }
            nativeButtonSaved(index, kind, name, 0, 0, 0, 0, mods, keyName);
            dialog.dismiss();
            return;
        }

        final boolean region = kind == KIND_REGION;
        final int[] numbers = parseNumbers(value, region ? 4 : 2);
        if (numbers == null) {
            Toast.makeText(this, region ? R.string.button_error_region
                    : R.string.button_error_point, Toast.LENGTH_SHORT).show();
            return;
        }
        if (region && (numbers[2] <= 0 || numbers[3] <= 0)) {
            Toast.makeText(this, R.string.button_error_region_size, Toast.LENGTH_SHORT).show();
            return;
        }
        if (name.isEmpty()) {
            name = numbers[0] + "," + numbers[1];
        }
        nativeButtonSaved(index, kind, name, numbers[0], numbers[1], region ? numbers[2] : 0,
                region ? numbers[3] : 0, 0, "");
        dialog.dismiss();
    }

    private static boolean isOffered(int kind) {
        for (final int offered : OFFERED_KINDS) {
            if (offered == kind) {
                return true;
            }
        }
        return false;
    }

    private static int labelForKind(int kind) {
        if (kind == KIND_SHORTCUT) {
            return R.string.button_kind_shortcut;
        }
        return kind == KIND_REGION ? R.string.button_kind_region : R.string.button_kind_point;
    }

    private void applyKindHints(int kind, TextView hint, EditText valueField) {
        if (kind == KIND_SHORTCUT) {
            hint.setText(R.string.button_hint_shortcut);
            valueField.setHint(R.string.button_field_shortcut);
        } else if (kind == KIND_REGION) {
            hint.setText(R.string.button_hint_region);
            valueField.setHint(R.string.button_field_region);
        } else {
            hint.setText(R.string.button_hint_point);
            valueField.setHint(R.string.button_field_point);
        }
    }

    /**
     * Exactly {@code wanted} integers, separated by commas or spaces. Null when
     * the text does not hold that many, or holds more, or holds anything else.
     * Kept deliberately strict: a coordinate that silently parses as something
     * other than what was typed puts the click somewhere unexplained.
     */
    private static int[] parseNumbers(String text, int wanted) {
        final int[] out = new int[wanted];
        int found = 0;
        for (final String part : text.split("[,\\s]+")) {
            if (part.isEmpty()) {
                continue;
            }
            if (found == wanted) {
                return null;
            }
            try {
                out[found++] = Integer.parseInt(part);
            } catch (NumberFormatException e) {
                return null;
            }
        }
        return found == wanted ? out : null;
    }

    private static boolean isModifierName(String piece) {
        switch (piece) {
            case "ctrl": case "control": case "shift": case "alt": case "option":
            case "win": case "meta": case "cmd": case "super":
                return true;
            default:
                return false;
        }
    }

    private static int parseModifiers(String text) {
        int mods = 0;
        for (final String part : text.toLowerCase().split("\\+")) {
            switch (part.trim()) {
                case "ctrl": case "control": mods |= MOD_CTRL; break;
                case "shift": mods |= MOD_SHIFT; break;
                case "alt": case "option": mods |= MOD_ALT; break;
                case "win": case "meta": case "cmd": case "super": mods |= MOD_META; break;
                default: break;
            }
        }
        return mods;
    }

    /** The one segment that is not a modifier. Empty when there is not exactly one. */
    private static String parseKeyName(String text) {
        String key = "";
        for (final String part : text.toLowerCase().split("\\+")) {
            final String piece = part.trim();
            if (piece.isEmpty() || isModifierName(piece)) {
                continue;
            }
            if (!key.isEmpty()) {
                return ""; // two real keys is not a shortcut we know how to send
            }
            key = piece;
        }
        return key;
    }

    private static String formatShortcut(int modifiers, String key) {
        final StringBuilder out = new StringBuilder();
        if ((modifiers & MOD_CTRL) != 0) {
            out.append("Ctrl+");
        }
        if ((modifiers & MOD_SHIFT) != 0) {
            out.append("Shift+");
        }
        if ((modifiers & MOD_ALT) != 0) {
            out.append("Alt+");
        }
        if ((modifiers & MOD_META) != 0) {
            out.append("Win+");
        }
        return out.append(key == null ? "" : key).toString();
    }

    private int dp(int value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                getResources().getDisplayMetrics()));
    }

    /**
     * GameActivity feeds key events straight into the native input buffer, so
     * back never reaches onBackPressed or the OnBackPressedDispatcher — it is
     * simply swallowed. Intercepting here, at the activity's own entry point,
     * catches it before the view hierarchy does.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            // Act on the release only, so a single press does not count twice.
            if (event.getAction() == KeyEvent.ACTION_UP) {
                onBackRequested();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    /**
     * A stray back press should not drop the connection mid-drawing, so the
     * first one only warns and a second one within the window exits.
     */
    private void onBackRequested() {
        final long now = SystemClock.uptimeMillis();

        if (lastBackPressAt != 0 && now - lastBackPressAt <= EXIT_CONFIRM_WINDOW_MS) {
            if (exitToast != null) {
                exitToast.cancel();
                exitToast = null;
            }
            finish();
            return;
        }

        lastBackPressAt = now;
        exitToast = Toast.makeText(this, R.string.press_back_again, Toast.LENGTH_SHORT);
        exitToast.show();
    }

    // --- called from native ------------------------------------------------
    //
    // The native side draws its own UI in GL and has no font of its own.
    // Rasterizing through Paint means the platform's font stack does the work,
    // so Korean and anything else render correctly without shipping a CJK font
    // that would add megabytes to the APK.

    /**
     * Rasterizes one line of text, white on transparent.
     *
     * <p>Packed into a single array to keep the JNI call trivial:
     * {@code [0]} width, {@code [1]} height, {@code [2]} baseline from the top,
     * then {@code width * height} ARGB_8888 pixels. Only the alpha channel is
     * used — the caller tints it.
     */
    @SuppressWarnings("unused")
    public static int[] rasterizeText(String text, float sizePx, boolean bold) {
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        paint.setTextSize(sizePx);
        paint.setColor(Color.WHITE);
        paint.setTypeface(bold ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);

        final Paint.FontMetrics fm = paint.getFontMetrics();
        final int ascent = (int) Math.ceil(-fm.ascent);
        final int descent = (int) Math.ceil(fm.descent);

        final int width = Math.max(1, (int) Math.ceil(paint.measureText(text)));
        final int height = Math.max(1, ascent + descent);

        final Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        final Canvas canvas = new Canvas(bitmap);
        canvas.drawText(text, 0, ascent, paint);

        final int[] out = new int[width * height + 3];
        out[0] = width;
        out[1] = height;
        out[2] = ascent;
        bitmap.getPixels(out, 3, width, 0, 0, width, height);
        bitmap.recycle();
        return out;
    }

    /**
     * Looks up a string resource by name, so labels drawn by the native UI come
     * from the same strings.xml as everything else and pick up translations.
     */
    @SuppressWarnings("unused")
    public String localizedString(String resourceName) {
        final int id = getResources().getIdentifier(resourceName, "string", getPackageName());
        return id != 0 ? getString(id) : resourceName;
    }

    // -----------------------------------------------------------------------

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
    }

    @Override
    protected void onPause() {
        // Leaving and coming back should not land the user one press from
        // quitting.
        lastBackPressAt = 0;
        if (exitToast != null) {
            exitToast.cancel();
            exitToast = null;
        }
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
    }

    private void hideSystemBars() {
        final View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
}
