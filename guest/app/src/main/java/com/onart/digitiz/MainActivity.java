package com.onart.digitiz;

import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

import com.google.androidgamesdk.GameActivity;

/**
 * Everything that matters happens in C++. This exists because GameActivity
 * needs a Java entry point, and because keeping the screen awake and hiding the
 * system bars are Java-side concerns.
 */
public class MainActivity extends GameActivity {

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

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
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
