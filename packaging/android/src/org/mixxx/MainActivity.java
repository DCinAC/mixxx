package org.mixxx;

import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
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
