package org.mixxx;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTimestamp;
import android.media.AudioTrack;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.util.ArrayList;
import java.util.Collections;

/// Zydek: how late the sound actually comes out, so the pages can draw the waveforms where the music you
/// hear is (Bluetooth adds a lot: 150-300 ms), like Serato's output latency compensation.
///
/// It plays silence through a low-latency AudioTrack for about a second and compares how many frames went
/// in with the frame Android says is at the speaker right now (AudioTrack.getTimestamp: that includes the
/// Bluetooth link). Measured at start-up and whenever an output appears or goes; nativeLatency() hands the
/// hub the result and the output's name.
public class ZydekAudio {
    private static final String TAG = "ZydekAudio";
    private static final int RATE = 48000;
    private static Context s_context;
    private static final Handler s_handler = new Handler(Looper.getMainLooper());
    private static volatile boolean s_measuring;

    static native void nativeLatency(int ms, String route);

    public static void init(Context context) {
        s_context = context.getApplicationContext();
        AudioManager am = (AudioManager) s_context.getSystemService(Context.AUDIO_SERVICE);
        am.registerAudioDeviceCallback(new AudioDeviceCallback() {
            @Override
            public void onAudioDevicesAdded(AudioDeviceInfo[] added) {
                later();
            }

            @Override
            public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
                later();
            }
        }, s_handler);
        later();
    }

    /// The hub's "measure again".
    public static void measureNow() {
        s_handler.post(ZydekAudio::start);
    }

    private static final Runnable s_start = ZydekAudio::start;

    private static void later() {   // devices come in bursts, and a new route takes a moment to settle
        s_handler.removeCallbacks(s_start);
        s_handler.postDelayed(s_start, 2500);
    }

    private static void start() {
        if (s_measuring) {
            return;
        }
        s_measuring = true;
        new Thread(() -> {
            try {
                measure();
            } catch (Throwable e) {
                Log.w(TAG, "couldn't measure the output latency", e);
            } finally {
                s_measuring = false;
            }
        }, "zydek-latency").start();
    }

    private static void measure() {
        int minBytes = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack track = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build())
                .setAudioFormat(new AudioFormat.Builder()
                        .setSampleRate(RATE)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .build())
                .setBufferSizeInBytes(minBytes)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
        short[] silence = new short[Math.max(256, minBytes / 8) * 2];   // a quarter of the buffer per write
        AudioTimestamp ts = new AudioTimestamp();
        ArrayList<Double> samples = new ArrayList<>();
        long written = 0;
        track.play();
        long until = System.nanoTime() + 1_400_000_000L;
        while (System.nanoTime() < until) {
            int n = track.write(silence, 0, silence.length);
            if (n <= 0) {
                break;
            }
            written += n / 2;
            if (track.getTimestamp(ts) && ts.framePosition > 0) {
                // frames not yet heard, less the time since the timestamp was taken
                double ms = (written - ts.framePosition) * 1000.0 / RATE - (System.nanoTime() - ts.nanoTime) / 1e6;
                if (ms > 0 && ms < 1000) {
                    samples.add(ms);
                }
            }
        }
        AudioDeviceInfo device = track.getRoutedDevice();
        String route = device == null ? "" : device.getProductName() + " · " + typeName(device.getType());
        track.stop();
        track.release();
        if (samples.size() < 5) {
            return;
        }
        // the later half, once the route has settled; the median of those
        ArrayList<Double> late = new ArrayList<>(samples.subList(samples.size() / 2, samples.size()));
        Collections.sort(late);
        // That includes this track's own buffer (about full, as the writes block): take it off, so the hub
        // can add Mixxx's buffer instead and get what Mixxx's sound goes through
        double ownBufferMs = (minBytes / 4.0 - silence.length / 4.0) * 1000.0 / RATE;
        int ms = (int) Math.round(Math.max(0, late.get(late.size() / 2) - ownBufferMs));
        Log.i(TAG, "output path latency " + ms + " ms on " + route + " (measured " + Math.round(late.get(late.size() / 2))
                + " ms, own buffer " + Math.round(ownBufferMs) + " ms)");
        nativeLatency(ms, route);
    }

    private static String typeName(int type) {
        switch (type) {
        case AudioDeviceInfo.TYPE_BLUETOOTH_A2DP:
        case AudioDeviceInfo.TYPE_BLE_HEADSET:
        case AudioDeviceInfo.TYPE_BLE_SPEAKER:
            return "Bluetooth";
        case AudioDeviceInfo.TYPE_USB_HEADSET:
        case AudioDeviceInfo.TYPE_USB_DEVICE:
        case AudioDeviceInfo.TYPE_USB_ACCESSORY:
            return "USB";
        case AudioDeviceInfo.TYPE_WIRED_HEADPHONES:
        case AudioDeviceInfo.TYPE_WIRED_HEADSET:
            return "wired";
        case AudioDeviceInfo.TYPE_BUILTIN_SPEAKER:
            return "speaker";
        default:
            return "output";
        }
    }
}
