package com.onart.digitiz;

import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;

import com.google.androidgamesdk.GameActivity;

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
