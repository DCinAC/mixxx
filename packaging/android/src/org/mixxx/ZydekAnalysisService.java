package org.mixxx;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/// Zydek: keeps Mixxx running while it analyzes the library ("Analyze all" on the phone page), so it carries
/// on with the screen off or another app in front, and shows the progress in a notification with a Stop
/// button. It asks Zydek's server for the progress every 2 s and stops itself when the analysis does.
public class ZydekAnalysisService extends Service {
    private static final String CHANNEL = "zydek-analysis";
    private static final int NOTIFICATION = 7101;
    private static final int DONE_NOTIFICATION = 7102;
    private static final String SERVER = "http://127.0.0.1:8766";
    private static final String ACTION_STOP = "org.mixxx.ZYDEK_ANALYSIS_STOP";
    private static final long MAX_WAKE_MS = 8L * 3600 * 1000;

    private Thread m_poller;
    private volatile boolean m_running;
    private PowerManager.WakeLock m_wakeLock;

    public static void start(Context context) {
        context.startForegroundService(new Intent(context, ZydekAnalysisService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        createChannel();
        startInForeground(progressNotification("Starting…", 0, 0));
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            new Thread(() -> request("/api/analyze/stop")).start();   // the poller sees it end
        }
        if (m_poller == null) {
            PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
            m_wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "zydek:analysis");
            m_wakeLock.acquire(MAX_WAKE_MS);
            m_running = true;
            m_poller = new Thread(this::poll, "zydek-analysis");
            m_poller.start();
        }
        return START_NOT_STICKY;
    }

    private void poll() {
        int idle = 0;
        boolean seenActive = false;
        int lastDone = 0;
        int lastTotal = 0;
        while (m_running) {
            try {
                Thread.sleep(2000);
            } catch (InterruptedException e) {
                return;
            }
            String body = request("/api/analyze/status");
            if (body == null) {
                continue;   // the server is busy or restarting: try again
            }
            try {
                JSONObject s = new JSONObject(body);
                if (s.optBoolean("active")) {
                    seenActive = true;
                    idle = 0;
                    lastDone = s.optInt("done");
                    lastTotal = s.optInt("total");
                    double seconds = s.optDouble("seconds", 0);
                    double perTrack = lastDone > 4 ? seconds / lastDone : 2.5;
                    long minutes = Math.max(1, Math.round((lastTotal - lastDone) * perTrack / 60));
                    notify(NOTIFICATION, progressNotification(
                            lastDone + " of " + lastTotal + " · about " + minutes + " min left", lastDone, lastTotal));
                } else if (seenActive || ++idle >= 5) {
                    finish(seenActive ? s.optInt("done", lastDone) : 0, seenActive ? s.optInt("total", lastTotal) : 0);
                    return;
                }
            } catch (Exception e) {
                // not JSON: ignore this round
            }
        }
    }

    private void finish(int done, int total) {
        if (total > 0) {
            String text = done >= total ? "All " + total + " tracks have a BPM and key now"
                                        : "Stopped after " + done + " of " + total + ": Analyze all carries on from there";
            notify(DONE_NOTIFICATION, builder().setContentTitle("Library analyzed").setContentText(text)
                    .setAutoCancel(true).build());
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override
    public void onTimeout(int startId, int fgsType) {   // Android 15 limits background data work to 6 h a day
        request("/api/analyze/stop");
        stopSelf();
    }

    @Override
    public void onDestroy() {
        m_running = false;
        if (m_poller != null) {
            m_poller.interrupt();
        }
        if (m_wakeLock != null && m_wakeLock.isHeld()) {
            m_wakeLock.release();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ---- notifications ----

    private void createChannel() {
        NotificationChannel channel = new NotificationChannel(CHANNEL, "Library analysis", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Progress while Zydek analyzes your library");
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
    }

    private Notification.Builder builder() {
        Intent open = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(smallIcon())
                .setContentIntent(PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE));
    }

    /// ZyDeck's own white reel-deck icon (res/drawable/ic_stat_zydeck.xml), Android's sync arrows if it's missing
    private int smallIcon() {
        int id = getResources().getIdentifier("ic_stat_zydeck", "drawable", getPackageName());
        return id != 0 ? id : android.R.drawable.stat_notify_sync;
    }

    private Notification progressNotification(String text, int done, int total) {
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, ZydekAnalysisService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
        return builder()
                .setContentTitle("Analyzing your library")
                .setContentText(text)
                .setProgress(total, done, total == 0)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .addAction(new Notification.Action.Builder(null, "Stop", stop).build())
                .build();
    }

    private void startInForeground(Notification notification) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFICATION, notification);
        }
    }

    private void notify(int id, Notification notification) {
        getSystemService(NotificationManager.class).notify(id, notification);
    }

    // ---- Zydek's server ----

    private static String request(String path) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(SERVER + path).openConnection();
            c.setConnectTimeout(3000);
            c.setReadTimeout(5000);
            if (c.getResponseCode() != 200) {
                return null;
            }
            try (InputStream in = c.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[4096];
                for (int n; (n = in.read(buf)) > 0;) {
                    out.write(buf, 0, n);
                }
                return out.toString(StandardCharsets.UTF_8.name());
            }
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) {
                c.disconnect();
            }
        }
    }
}
