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
import android.util.Log;

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
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return START_NOT_STICKY;
        }
        NotificationChannel channel = new NotificationChannel(CHANNEL, "ZyDeck running", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Keeps ZyDeck answering linked devices while it's in the background");
        channel.setShowBadge(false);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
        Intent open = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, ZydekRunningService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
        int icon = getResources().getIdentifier("ic_stat_zydeck", "drawable", getPackageName());
        Notification notification = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(icon != 0 ? icon : android.R.drawable.stat_notify_sync)
                .setContentTitle("ZyDeck is running")
                .setContentText("Linked devices and controllers keep working in the background")
                .setContentIntent(PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE))
                .addAction(new Notification.Action.Builder(null, "Background off", stop).build())
                .setOngoing(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .build();
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

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
