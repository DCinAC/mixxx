package org.mixxx;

import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import org.qtproject.qt.android.QtActivityBase;

public class MainActivity extends QtActivityBase {
    // Zydek: in portrait the phone shows the library page (served by Zydek itself on port 8766, see
    // src/zydek/zydekhub.cpp) on top of Mixxx; in landscape it hides, leaving Mixxx's own interface.
    private static final String PHONE_PAGE = "http://127.0.0.1:8766/phone";
    private WebView m_phoneView;
    private final Handler m_handler = new Handler(Looper.getMainLooper());

    /// Zydek: Mixxx reads music folders by path, which on Android 11+ needs "All files access".
    public static boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    /// Opens Android's "All files access" switch for this app (one tap to grant).
    public static void openAllFilesAccess(Context context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return;
        }
        Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + context.getPackageName()));
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            context.startActivity(intent);
        } catch (Exception e) {
            // Some devices only have the list of all apps
            Intent all = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
            all.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(all);
        }
    }

    /// What the library page can ask Android for (window.ZydekAndroid in its JavaScript).
    private class PageBridge {
        @JavascriptInterface
        public boolean hasAllFilesAccess() {
            return MainActivity.hasAllFilesAccess();
        }

        @JavascriptInterface
        public void openAllFilesAccess() {
            MainActivity.openAllFilesAccess(MainActivity.this);
        }
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Disable drawing over cutout - isn't working
        WindowManager.LayoutParams lp = this.getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER;

        // Disable system and navigation bar to prevent accidental back or app switch
        WindowInsetsControllerCompat windowInsetsController =
            WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
        windowInsetsController.setSystemBarsBehavior(
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        windowInsetsController.hide(WindowInsetsCompat.Type.navigationBars());

        createPhoneView();
        updateMode(getResources().getConfiguration().orientation);
        applyOrientationExtra(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        applyOrientationExtra(intent);
    }

    /// For testing over adb without touching the device's rotation settings:
    ///   adb shell am start -n org.mixxx/.MainActivity --es zydek_orientation landscape|portrait|auto
    private void applyOrientationExtra(Intent intent) {
        String orientation = intent == null ? null : intent.getStringExtra("zydek_orientation");
        if (orientation == null) {
            return;
        }
        switch (orientation) {
            case "landscape":
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
                break;
            case "portrait":
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT);
                break;
            default:
                setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_USER);   // follows the rotation lock
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        updateMode(newConfig.orientation);
    }

    @Override
    public void onBackPressed() {
        // In the library page, Back goes back within it rather than leaving Mixxx.
        if (m_phoneView != null && m_phoneView.getVisibility() == View.VISIBLE && m_phoneView.canGoBack()) {
            m_phoneView.goBack();
            return;
        }
        super.onBackPressed();
    }

    private void createPhoneView() {
        m_phoneView = new WebView(this);
        m_phoneView.setBackgroundColor(0xFF0B0B0B);
        WebSettings settings = m_phoneView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setMediaPlaybackRequiresUserGesture(false);
        m_phoneView.addJavascriptInterface(new PageBridge(), "ZydekAndroid");
        m_phoneView.setWebViewClient(new WebViewClient() {
            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                // Zydek's server starts a few seconds after the app: keep trying until it answers.
                if (request.isForMainFrame()) {
                    m_handler.postDelayed(() -> view.loadUrl(PHONE_PAGE), 1000);
                }
            }
        });
        addContentView(m_phoneView, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        m_phoneView.loadUrl(PHONE_PAGE);
    }

    private void updateMode(int orientation) {
        if (m_phoneView == null) {
            return;
        }
        boolean portrait = orientation == Configuration.ORIENTATION_PORTRAIT;
        m_phoneView.setVisibility(portrait ? View.VISIBLE : View.GONE);
        if (portrait) {
            m_phoneView.bringToFront();
            m_phoneView.requestFocus();
        }
    }
}
