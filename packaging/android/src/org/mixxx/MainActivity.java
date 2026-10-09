package org.mixxx;

import android.content.Context;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.media.MediaScannerConnection;
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
import android.util.DisplayMetrics;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import org.qtproject.qt.android.QtActivityBase;

public class MainActivity extends QtActivityBase {
    // Zydek: the phone page (served by Zydek itself on port 8766, see src/zydek/zydekhub.cpp) covers Mixxx's
    // own interface: the library in portrait, the decks in landscape (res/zydek/web/phone.html).
    private static final String PHONE_ORIGIN = "http://127.0.0.1:8766";
    private static final String PHONE_PAGE = PHONE_ORIGIN + "/phone";
    // Shown until Zydek's server answers (Mixxx takes a few seconds to start); the phone page opens with the
    // same picture, so it carries on seamlessly.
    private static final String BOOT_PAGE = "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
            + "<style>html,body{margin:0;height:100%;background:#0b0b0b}"
            + "body{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:18px;font-family:system-ui,sans-serif}"
            + ".rec{width:128px;height:128px;border-radius:50%;display:grid;place-items:center;animation:s 1.8s linear infinite;"
            + "background:repeating-radial-gradient(circle,#141414 0 1.5px,#1e1e1e 1.5px 3px);box-shadow:0 0 0 1px #2a2a2a}"
            + ".rec b{width:44px;height:44px;border-radius:50%;background:#ff8a1e;color:#1b0f02;display:grid;place-items:center;font:800 22px system-ui}"
            + ".w{font:700 20px system-ui;letter-spacing:.35em;text-indent:.35em;color:#ececec}.st{font-size:13px;color:#8a8a8a}"
            + "@keyframes s{to{transform:rotate(360deg)}}</style></head><body>"
            + "<div class=rec><b>Z</b></div><div class=w>ZYDEK</div><div class=st>Starting Mixxx…</div><script>"
            + "(function t(){fetch('/phone',{cache:'no-store'}).then(function(r){if(r.ok)location.replace('/phone');"
            + "else setTimeout(t,400)}).catch(function(){setTimeout(t,400)})})()</script></body></html>";
    private WebView m_phoneView;
    private LinearLayout m_loginView;   // Bilibili login (see "Bilibili accounts" below)
    private WebView m_loginWeb;
    private TextView m_loginStatus;
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

        /// Opens a QR login link in the Bilibili app for that site: "" or why it couldn't.
        @JavascriptInterface
        public String bilibiliOpenApp(String site, String link) {
            return openInBilibiliApp(site, link);
        }

        /// Lets the gallery (and the Bilibili app's scan-from-photos) see a picture Zydek saved.
        @JavascriptInterface
        public void addToGallery(String path) {
            MediaScannerConnection.scanFile(MainActivity.this, new String[] {path}, new String[] {"image/png"}, null);
        }

        /// CSS pixels per millimetre of this screen (the deck view sizes its controls in mm).
        @JavascriptInterface
        public double screenPxPerMm() {
            DisplayMetrics m = getResources().getDisplayMetrics();
            return (m.xdpi + m.ydpi) / 2 / 25.4 / m.density;
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
    private static final String DESKTOP_USER_AGENT =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36";
    private static final long COOKIE_LIFETIME_S = 180L * 24 * 3600;   // the WebView doesn't say; yt-dlp needs one

    /// {site id, cookie domain, a page the cookies are sent to, login page, name}
    private static final String[][] SITES = {
            {"cn", ".bilibili.com", "https://www.bilibili.com/",
                    "https://passport.bilibili.com/login",
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
        Button app = new Button(this);
        app.setText(id.equals("intl") ? "Use the BiliBili app" : "Use the Bilibili app");
        app.setOnClickListener(v -> startAppLogin(id));
        bar.addView(app);
        Button cancel = new Button(this);
        cancel.setText("Close");
        cancel.setOnClickListener(v -> closeBilibiliLogin());
        bar.addView(cancel);
        m_loginView.addView(bar, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        m_loginStatus = new TextView(this);
        m_loginStatus.setPadding(24, 0, 24, 12);
        m_loginStatus.setTextColor(0xFF9A9A9A);
        m_loginStatus.setText("Log in on the page, or with the app on this phone: \"" + app.getText() + "\" opens it to confirm.");
        m_loginView.addView(m_loginStatus, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        WebView web = new WebView(this);
        WebSettings settings = web.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        // The desktop sites: the mobile ones keep handing over to Bilibili's app (bstar://, bilibili://).
        // Fitted to the screen, with pinch zoom.
        settings.setUserAgentString(DESKTOP_USER_AGENT);
        settings.setUseWideViewPort(true);
        settings.setLoadWithOverviewMode(true);
        settings.setBuiltInZoomControls(true);
        settings.setDisplayZoomControls(false);
        CookieManager.getInstance().setAcceptCookie(true);
        CookieManager.getInstance().setAcceptThirdPartyCookies(web, true);
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                String scheme = request.getUrl().getScheme();
                return !"https".equals(scheme) && !"http".equals(scheme);   // app links: stay on the page
            }
        });
        web.addJavascriptInterface(new LoginBridge(id), "ZydekLogin");
        m_loginWeb = web;
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
            m_loginWeb.destroy();
            m_loginWeb = null;
        }
    }

    // Logging in with the app on the same phone, instead of scanning the page's QR code with another device:
    // the login page asks the site for a QR login itself, the link inside it opens in the app, and the page
    // waits for the confirmation there. The site then sets its session cookies in the WebView, as after a
    // scan (openBilibiliLogin's poll picks them up).
    //   bilibili.com: the Bilibili app (Google Play's com.bilibili.app.in, or the mainland tv.danmaku.bili)
    //                 opens the link in its own browser: bilibili://browser?url=...
    //   bilibili.tv:  the BiliBili app (com.bstar.intl) opens the www.biliintl.com link; accounts there are
    //                 separate from bilibili.com's, so only that app can confirm.
    private static final String[] CN_APPS = {"com.bilibili.app.in", "tv.danmaku.bili", "com.bilibili.app.blue"};
    private static final String INTL_APP = "com.bstar.intl";

    private static final String CN_APP_LOGIN_JS =
            "(async () => {"
            + " const get = u => fetch(u, {credentials: 'include'}).then(r => r.json());"
            + " const g = await get('https://passport.bilibili.com/x/passport-login/web/qrcode/generate?source=main-fe-header');"
            + " if (g.code !== 0) return ZydekLogin.status('Bilibili gave no login code: ' + g.message);"
            + " ZydekLogin.open(g.data.url);"
            + " for (let i = 0; i < 90; i++) {"
            + "  await new Promise(r => setTimeout(r, 2000));"
            + "  const p = await get('https://passport.bilibili.com/x/passport-login/web/qrcode/poll?source=main-fe-header&qrcode_key=' + g.data.qrcode_key);"
            + "  const c = p.data ? p.data.code : p.code;"
            + "  if (c === 0) { ZydekLogin.status('Confirmed: finishing the login…'); if (p.data.url) location.href = p.data.url; return; }"
            + "  if (c === 86090) ZydekLogin.status('Now tap Confirm in the Bilibili app, then come back');"
            + "  if (c === 86038) return ZydekLogin.status('The login code ran out: tap the button again');"
            + " }"
            + "})().catch(e => ZydekLogin.status('Login failed: ' + e));";

    private static final String INTL_APP_LOGIN_JS =
            "(async () => {"
            + " const base = 'https://passport.bilibili.tv/x/intl/passport-login/qrcode/auth/';"
            + " const get = u => fetch(u, {credentials: 'include'}).then(r => r.json());"
            + " const g = await get(base + 'url?s_locale=en_US&platform=web');"
            + " if (g.code !== 0) return ZydekLogin.status('bilibili.tv gave no login code: ' + g.message);"
            + " const ticket = new URL(g.data.qr_url).searchParams.get('ticket');"
            + " ZydekLogin.open(g.data.qr_url);"
            + " for (let i = 0; i < 90; i++) {"
            + "  await new Promise(r => setTimeout(r, 2000));"
            + "  const p = await get(base + 'fetch?s_locale=en_US&platform=web&ticket=' + ticket);"
            + "  if (p.code === 0) { ZydekLogin.status('Confirmed: finishing the login…');"
            + "   const u = p.data && (p.data.redirect_url || p.data.url); if (u) location.href = u; else location.reload(); return; }"
            + "  if (p.code === 10018100) return ZydekLogin.status('The login code ran out: tap the button again');"
            + " }"
            + "})().catch(e => ZydekLogin.status('Login failed: ' + e));";

    private class LoginBridge {
        private final String m_site;

        LoginBridge(String site) {
            m_site = site;
        }

        @JavascriptInterface
        public void open(String link) {
            String problem = openInBilibiliApp(m_site, link);
            status(problem.isEmpty() ? "Confirm the login in the app, then come back here" : problem);
        }

        @JavascriptInterface
        public void status(String message) {
            runOnUiThread(() -> {
                if (m_loginStatus != null) {
                    m_loginStatus.setText(message);
                }
            });
        }
    }

    private boolean installed(String pkg) {
        try {
            getPackageManager().getPackageInfo(pkg, 0);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private void startAppLogin(String id) {
        if (m_loginWeb == null) {
            return;
        }
        m_loginStatus.setText("Asking for a login code…");
        m_loginWeb.evaluateJavascript(id.equals("intl") ? INTL_APP_LOGIN_JS : CN_APP_LOGIN_JS, null);
    }

    /// "" when the app opened, otherwise what to tell the user.
    private String openInBilibiliApp(String id, String link) {
        Intent intent;
        String app = null;
        if (id.equals("intl")) {
            intent = new Intent(Intent.ACTION_VIEW, Uri.parse(link));
            if (installed(INTL_APP)) {
                app = INTL_APP;
            }
        } else {
            intent = new Intent(Intent.ACTION_VIEW, Uri.parse("bilibili://browser?url=" + Uri.encode(link)));
            for (String pkg : CN_APPS) {
                if (installed(pkg)) {
                    app = pkg;
                    break;
                }
            }
        }
        if (app == null) {
            return id.equals("intl")
                    ? "The BiliBili app for bilibili.tv isn't installed. Save the QR code and scan it from another device, or log in on the page"
                    : "No Bilibili app is installed. Save the QR code and scan it from another device, or log in on the page";
        }
        intent.setPackage(app);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            startActivity(intent);
            return "";
        } catch (ActivityNotFoundException e) {
            return "The app wouldn't open the login link: log in on the page instead";
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
                // Zydek's server isn't up (yet, or any more): the start-up page waits for it.
                if (request.isForMainFrame()) {
                    showBootPage();
                }
            }
        });
        addContentView(m_phoneView, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        showBootPage();
    }

    private void showBootPage() {
        // On the server's origin, so the page can ask it whether it's up.
        m_phoneView.loadDataWithBaseURL(PHONE_ORIGIN + "/", BOOT_PAGE, "text/html", "utf-8", null);
    }

    private void updateMode(int orientation) {
        if (m_phoneView == null) {
            return;
        }
        // Always on top: the page itself switches between the library (portrait) and the decks (landscape),
        // which it keeps loaded, so turning the phone is instant. The decks get the whole screen.
        boolean portrait = orientation == Configuration.ORIENTATION_PORTRAIT;
        WindowInsetsControllerCompat bars = WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
        if (portrait) {
            bars.show(WindowInsetsCompat.Type.statusBars());
        } else {
            bars.hide(WindowInsetsCompat.Type.statusBars());
        }
        m_phoneView.setVisibility(View.VISIBLE);
        m_phoneView.bringToFront();
        m_phoneView.requestFocus();
    }
}
