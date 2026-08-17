package com.onart.digitiz;

import android.os.Bundle;
import android.os.SystemClock;
import android.view.KeyEvent;
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

    private long lastBackPressAt = 0;
    private Toast exitToast;

    static {
        System.loadLibrary("digitiz_guest");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // The session dies with the screen, so do not let it sleep while the
        // phone is being used as a tablet.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
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
