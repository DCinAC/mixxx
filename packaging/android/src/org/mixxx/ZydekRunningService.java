package org.mixxx;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.drawable.Icon;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import java.net.HttpURLConnection;
import java.net.URL;
import java.util.List;

/// Zydek: keeps ZyDeck awake while it's open, as a music app does while it plays (a media playback foreground
/// service with a quiet notification). Without it Android freezes an app in the background once nothing plays:
/// its server stops answering, and a tablet linked to the phone (or ZyDeck Link on a computer) loses it until
/// ZyDeck is in front again. The notification's "Background off" stops just this, if you'd rather let it sleep.
public class ZydekRunningService extends Service {
    private static final String TAG = "ZydekRunning";
    private static final String CHANNEL = "zydek-running";
    private static final int NOTIFICATION = 7201;
    private static final String ACTION_STOP = "org.mixxx.ZYDEK_RUNNING_STOP";
    private static final String NOTICES = "zydek-notices";
    private static final int NOTICE = 7202;
    private static Context sContext;

    // Player mode (ZyDeck's smart shuffle: Mixxx's Auto DJ through the library): while it's on, the notification is a
    // media player (title, artist, play / pause, next, stop) with a media session (the lock screen, Bluetooth
    // buttons). The hub says what plays (player()); the buttons go to the hub's /api/player.
    private static final String ACTION_PLAYER = "org.mixxx.ZYDEK_PLAYER";
    private static ZydekRunningService sService;
    private static boolean sPlayerOn, sPlaying;
    private static String sTitle = "", sArtist = "";
    private static long sPosition, sDuration;
    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private MediaSession mSession;

    /// MainActivity, when ZyDeck opens. Quietly does nothing if Android won't allow it.
    public static void start(Context context) {
        sContext = context.getApplicationContext();
        try {
            context.startForegroundService(new Intent(context, ZydekRunningService.class));
        } catch (RuntimeException e) {
            Log.w(TAG, "can't keep running in the background", e);
        }
    }

    public static void stop(Context context) {
        context.stopService(new Intent(context, ZydekRunningService.class));
    }

    /// The hub (any thread), as player mode's state changes: on, what plays, playing or paused, where (ms)
    public static void player(boolean on, String title, String artist, boolean playing, long positionMs, long durationMs) {
        sMain.post(() -> {
            sPlayerOn = on;
            sTitle = title != null ? title : "";
            sArtist = artist != null ? artist : "";
            sPlaying = playing;
            sPosition = positionMs;
            sDuration = durationMs;
            if (sService != null) {
                sService.show();
            } else if (on && sContext != null) {
                start(sContext);   // (background off earlier: the player brings it back)
            }
        });
    }

    // A button: to the hub (off the main thread)
    private static void ask(String what) {
        new Thread(() -> {
            try {
                HttpURLConnection c = (HttpURLConnection) new URL("http://127.0.0.1:8766/api/player?do=" + what).openConnection();
                c.setConnectTimeout(2000);
                c.setReadTimeout(10000);
                c.getResponseCode();
                c.disconnect();
            } catch (Exception e) {
                Log.w(TAG, "player: the hub didn't answer", e);
            }
        }).start();
    }

    /// The hub, at start: why ZyDeck's last run ended (Android's record): "crash", "anr", "low memory" (killed for
    /// memory), "user" (swiped away, force-stopped), "exit", "other", or "" when Android doesn't say (before 11).
    public static String lastExitReason() {
        Context context = sContext;
        if (context == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return "";
        }
        try {
            List<ApplicationExitInfo> exits = context.getSystemService(ActivityManager.class)
                    .getHistoricalProcessExitReasons(context.getPackageName(), 0, 1);
            if (exits.isEmpty()) {
                return "";
            }
            switch (exits.get(0).getReason()) {
                case ApplicationExitInfo.REASON_CRASH:
                case ApplicationExitInfo.REASON_CRASH_NATIVE:
                    return "crash";
                case ApplicationExitInfo.REASON_ANR:
                    return "anr";
                case ApplicationExitInfo.REASON_LOW_MEMORY:
                    return "low memory";
                case ApplicationExitInfo.REASON_USER_REQUESTED:
                    return "user";
                case ApplicationExitInfo.REASON_EXIT_SELF:
                    return "exit";
                default:
                    return "other";
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "can't tell how the last run ended", e);
            return "";
        }
    }

    /// The hub (any thread): something to tell even when ZyDeck isn't in front, e.g. the Discord bot kicked
    /// from its voice channel. Tapping it opens ZyDeck. Needs Android's notification permission for ZyDeck.
    public static void notice(String title, String text) {
        Context context = sContext;
        if (context == null) {
            return;
        }
        try {
            NotificationManager manager = context.getSystemService(NotificationManager.class);
            manager.createNotificationChannel(
                    new NotificationChannel(NOTICES, "ZyDeck notices", NotificationManager.IMPORTANCE_DEFAULT));
            Intent open = new Intent(context, MainActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_NEW_TASK);
            int icon = context.getResources().getIdentifier("ic_stat_zydeck", "drawable", context.getPackageName());
            manager.notify(NOTICE, new Notification.Builder(context, NOTICES)
                    .setSmallIcon(icon != 0 ? icon : android.R.drawable.stat_notify_error)
                    .setContentTitle(title)
                    .setContentText(text)
                    .setStyle(new Notification.BigTextStyle().bigText(text))
                    .setContentIntent(PendingIntent.getActivity(context, 2, open, PendingIntent.FLAG_IMMUTABLE))
                    .setAutoCancel(true)
                    .build());
        } catch (RuntimeException e) {
            Log.w(TAG, "can't show a notice", e);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        sService = this;
        mSession = new MediaSession(this, "ZyDeck");
        mSession.setCallback(new MediaSession.Callback() {
            @Override public void onPlay() { ask("play"); }
            @Override public void onPause() { ask("pause"); }
            @Override public void onSkipToNext() { ask("next"); }
            @Override public void onStop() { ask("stop"); }
        });
    }

    @Override
    public void onDestroy() {
        sService = null;
        if (mSession != null) {
            mSession.release();
        }
        super.onDestroy();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return START_NOT_STICKY;
        }
        if (intent != null && ACTION_PLAYER.equals(intent.getAction())) {   // a player button
            ask(intent.getStringExtra("do"));
            return START_NOT_STICKY;
        }
        NotificationChannel channel = new NotificationChannel(CHANNEL, "ZyDeck running", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Keeps ZyDeck answering linked devices while it's in the background; the player in player mode");
        channel.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
        Notification notification = build();
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
            } else {
                startForeground(NOTIFICATION, notification);
            }
        } catch (RuntimeException e) {   // not allowed right now (started from the background, say)
            Log.w(TAG, "can't keep running in the background", e);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    private PendingIntent playerIntent(String what, int code) {
        return PendingIntent.getService(this, code, new Intent(this, ZydekRunningService.class)
                .setAction(ACTION_PLAYER).putExtra("do", what), PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    // The notification as things are: ZyDeck running, or player mode's player
    private Notification build() {
        Intent open = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent content = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE);
        int icon = getResources().getIdentifier("ic_stat_zydeck", "drawable", getPackageName());
        Notification.Builder b = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(icon != 0 ? icon : android.R.drawable.stat_notify_sync)
                .setContentIntent(content)
                .setOngoing(true);
        if (!sPlayerOn) {
            mSession.setActive(false);
            PendingIntent stop = PendingIntent.getService(this, 1,
                    new Intent(this, ZydekRunningService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
            return b.setContentTitle("ZyDeck is running")
                    .setContentText("Linked devices and controllers keep working in the background")
                    .addAction(new Notification.Action.Builder(null, "Background off", stop).build())
                    .setCategory(Notification.CATEGORY_SERVICE)
                    .build();
        }
        // the player: the session first (the lock screen and the system's media controls read it)
        mSession.setMetadata(new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, sTitle.isEmpty() ? "ZyDeck player" : sTitle)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, sArtist)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, sDuration)
                .build());
        mSession.setPlaybackState(new PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
                        | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_STOP)
                .setState(sPlaying ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED, sPosition, sPlaying ? 1f : 0f)
                .build());
        mSession.setActive(true);
        Notification.Action toggle = sPlaying
                ? new Notification.Action.Builder(Icon.createWithResource(this, android.R.drawable.ic_media_pause), "Pause", playerIntent("pause", 11)).build()
                : new Notification.Action.Builder(Icon.createWithResource(this, android.R.drawable.ic_media_play), "Play", playerIntent("play", 12)).build();
        return b.setContentTitle(sTitle.isEmpty() ? "ZyDeck player" : sTitle)
                .setContentText(sArtist.isEmpty() ? "Player mode" : sArtist)
                .setSubText("Player mode")
                .addAction(toggle)
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, android.R.drawable.ic_media_next), "Next", playerIntent("next", 13)).build())
                .addAction(new Notification.Action.Builder(Icon.createWithResource(this, android.R.drawable.ic_menu_close_clear_cancel), "Stop", playerIntent("stop", 14)).build())
                .setStyle(new Notification.MediaStyle().setMediaSession(mSession.getSessionToken()).setShowActionsInCompactView(0, 1))
                .setCategory(Notification.CATEGORY_TRANSPORT)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .build();
    }

    // The notification again (the player's state changed)
    private void show() {
        try {
            getSystemService(NotificationManager.class).notify(NOTIFICATION, build());
        } catch (RuntimeException e) {
            Log.w(TAG, "can't update the notification", e);
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
