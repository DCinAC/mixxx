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
import android.webkit.CookieManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
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
    private LinearLayout m_loginView;   // Bilibili login (see "Bilibili accounts" below)
    private Runnable m_loginPoll;
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

        /// site: "cn" (bilibili.com) or "intl" (bilibili.tv)
        @JavascriptInterface
        public void bilibiliLogin(String site) {
            runOnUiThread(() -> openBilibiliLogin(site));
        }

        @JavascriptInterface
        public void bilibiliLogout(String site) {
            runOnUiThread(() -> logoutBilibili(site));
        }

        /// Cookies pasted from a browser ("name=value; ..." or a cookies.txt): "" or what's wrong.
        @JavascriptInterface
        public String bilibiliPaste(String site, String text) {
            return pasteBilibiliCookies(site, text);
        }

        /// {"cn": true/false, "intl": true/false}: whether Zydek has a session for each.
        @JavascriptInterface
        public String bilibiliState() {
            return "{\"cn\":" + hasSession("cn") + ",\"intl\":" + hasSession("intl") + "}";
        }
    }

    // ---- Bilibili accounts ------------------------------------------------------------------------
    // For the Web tab's downloads (res/zydek/python/zydek_web.py): with a session yt-dlp gets past Bilibili's
    // blocks, and premium accounts get Hi-Res (FLAC) audio. The user logs in on Bilibili's own page in a
    // WebView; Zydek keeps only the cookies, in its private files, as the cookies.txt yt-dlp reads.
    private static final String COOKIE_FILE = "bilibili-cookies.txt";
    private static final long COOKIE_LIFETIME_S = 180L * 24 * 3600;   // the WebView doesn't say; yt-dlp needs one

    /// {site id, cookie domain, a page the cookies are sent to, login page, name}
    private static final String[][] SITES = {
            {"cn", ".bilibili.com", "https://www.bilibili.com/",
                    "https://passport.bilibili.com/h5-app/passport/login?gourl=https%3A%2F%2Fm.bilibili.com%2F",
                    "Bilibili"},
            {"intl", ".bilibili.tv", "https://www.bilibili.tv/", "https://www.bilibili.tv/en/", "Bilibili International"},
    };

    private static String[] site(String id) {
        for (String[] s : SITES) {
            if (s[0].equals(id)) {
                return s;
            }
        }
        return SITES[0];
    }

    private File cookieFile() {
        return new File(getFilesDir(), COOKIE_FILE);
    }

    private List<String> cookieLines() {
        List<String> lines = new ArrayList<>();
        try {
            if (cookieFile().exists()) {
                for (String line : Files.readAllLines(cookieFile().toPath(), StandardCharsets.UTF_8)) {
                    if (!line.isEmpty() && !line.startsWith("# ")) {
                        lines.add(line);
                    }
                }
            }
        } catch (Exception e) {
            // a broken file counts as no sessions
        }
        return lines;
    }

    private static boolean forDomain(String line, String domain) {
        String host = line.split("\t", 2)[0].replace("#HttpOnly_", "");
        return host.equals(domain) || host.endsWith(domain) || ("." + host).endsWith(domain);
    }

    private boolean hasSession(String id) {
        String domain = site(id)[1];
        for (String line : cookieLines()) {
            String[] f = line.split("\t");
            if (f.length >= 7 && forDomain(line, domain) && f[5].equals("SESSDATA") && !f[6].isEmpty()) {
                return true;
            }
        }
        return false;
    }

    /// Replaces the site's cookies in the file with these (Netscape lines).
    private void writeSiteCookies(String id, List<String> lines) {
        String domain = site(id)[1];
        StringBuilder out = new StringBuilder("# Netscape HTTP Cookie File\n# Zydek: Bilibili sessions for yt-dlp\n");
        for (String line : cookieLines()) {
            if (!forDomain(line, domain)) {
                out.append(line).append('\n');
            }
        }
        for (String line : lines) {
            out.append(line).append('\n');
        }
        try (FileOutputStream f = new FileOutputStream(cookieFile())) {
            f.write(out.toString().getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            android.util.Log.w("mixxx", "Zydek: can't save Bilibili cookies: " + e);
        }
    }

    /// "a=b; c=d" (a Cookie header, or what the WebView has) as Netscape lines for the site's domain.
    private static List<String> headerToLines(String domain, String header) {
        long expires = System.currentTimeMillis() / 1000 + COOKIE_LIFETIME_S;
        List<String> lines = new ArrayList<>();
        for (String pair : header.replaceFirst("(?i)^\\s*cookie:\\s*", "").split(";")) {
            int eq = pair.indexOf('=');
            if (eq > 0) {
                String name = pair.substring(0, eq).trim(), value = pair.substring(eq + 1).trim();
                if (!name.isEmpty()) {
                    lines.add(domain + "\tTRUE\t/\tTRUE\t" + expires + "\t" + name + "\t" + value);
                }
            }
        }
        return lines;
    }

    private String pasteBilibiliCookies(String id, String text) {
        String domain = site(id)[1];
        List<String> lines = new ArrayList<>();
        if (text.contains("\t")) {   // a cookies.txt
            for (String line : text.split("\n")) {
                line = line.trim();
                if (!line.isEmpty() && !line.startsWith("# ") && line.split("\t").length >= 7 && forDomain(line, domain)) {
                    lines.add(line);
                }
            }
        } else {
            lines = headerToLines(domain, text);
        }
        boolean session = false;
        for (String line : lines) {
            String[] f = line.split("\t");
            session |= f.length >= 7 && f[5].equals("SESSDATA") && !f[6].isEmpty();
        }
        if (!session) {
            return "Those cookies have no SESSDATA for " + site(id)[4] + ", so they aren't a login";
        }
        writeSiteCookies(id, lines);
        return "";
    }

    private void logoutBilibili(String id) {
        String[] s = site(id);
        writeSiteCookies(id, new ArrayList<>());
        CookieManager cookies = CookieManager.getInstance();
        String current = cookies.getCookie(s[2]);
        if (current != null) {
            for (String pair : current.split(";")) {
                int eq = pair.indexOf('=');
                if (eq > 0) {
                    String name = pair.substring(0, eq).trim();
                    cookies.setCookie(s[2], name + "=; Max-Age=0; Path=/; Domain=" + s[1]);
                }
            }
        }
        cookies.flush();
        notifyPage("zydekBilibili('" + id + "', false)");
    }

    private void openBilibiliLogin(String id) {
        String[] s = site(id);
        closeBilibiliLogin();
        m_loginView = new LinearLayout(this);
        m_loginView.setOrientation(LinearLayout.VERTICAL);
        m_loginView.setBackgroundColor(0xFF0B0B0B);
        LinearLayout bar = new LinearLayout(this);
        bar.setPadding(24, 16, 16, 16);
        TextView title = new TextView(this);
        title.setText("Log in to " + s[4] + ": Zydek keeps only the session, for downloads");
        title.setTextColor(0xFFE8E8E8);
        bar.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button cancel = new Button(this);
        cancel.setText("Close");
        cancel.setOnClickListener(v -> closeBilibiliLogin());
        bar.addView(cancel);
        m_loginView.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        WebView web = new WebView(this);
        WebSettings settings = web.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        CookieManager.getInstance().setAcceptCookie(true);
        CookieManager.getInstance().setAcceptThirdPartyCookies(web, true);
        web.setWebViewClient(new WebViewClient());
        m_loginView.addView(web, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        addContentView(m_loginView, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        m_loginView.bringToFront();
        web.loadUrl(s[3]);
        // Logged in once the site has set its session cookie: keep the cookies and close.
        m_loginPoll = new Runnable() {
            @Override
            public void run() {
                String cookies = CookieManager.getInstance().getCookie(s[2]);
                if (cookies != null && cookies.matches("(^|.*;\\s*)SESSDATA=[^;]+.*")) {
                    writeSiteCookies(id, headerToLines(s[1], cookies));
                    CookieManager.getInstance().flush();
                    closeBilibiliLogin();
                    notifyPage("zydekBilibili('" + id + "', true)");
                    return;
                }
                m_handler.postDelayed(this, 1000);
            }
        };
        m_handler.postDelayed(m_loginPoll, 1500);
    }

    private void closeBilibiliLogin() {
        if (m_loginPoll != null) {
            m_handler.removeCallbacks(m_loginPoll);
            m_loginPoll = null;
        }
        if (m_loginView != null) {
            ((ViewGroup) m_loginView.getParent()).removeView(m_loginView);
            m_loginView = null;
        }
    }

    private void notifyPage(String js) {
        if (m_phoneView != null) {
            m_phoneView.evaluateJavascript("window.zydekBilibili && " + js, null);
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
        if (m_loginView != null) {   // Back closes the Bilibili login
            closeBilibiliLogin();
            return;
        }
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
