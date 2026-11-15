package org.mixxx;

import android.content.Context;
import android.hardware.usb.UsbDevice;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.util.Log;
import java.io.IOException;
import java.util.concurrent.ConcurrentHashMap;

/// MIDI controllers plugged into this phone or tablet (a DJ controller on a USB OTG adapter): each one Android's MIDI
/// service offers becomes a MIDI controller in Mixxx (src/controllers/zydek/androidmidi.cpp), as PortMidi makes them
/// on a computer. Android's MIDI service owns the USB device itself, so no USB permission is asked.
///
/// The computer's port (the phone as a USB MIDI device, ZydekMidi) is not one of them: it has no USB device behind it.
///
/// What arrives goes to nativeReceive() with two clocks for the session profiler: when Android's MIDI service took it
/// from USB (its timestamp) and when it reached this app, both System.nanoTime() (CLOCK_MONOTONIC).
public class ZydekMidiHost {
    private static final String TAG = "ZydekMidiHost";
    // MidiInputPort.send() takes at most this much at once (Android's packet size, less its header)
    private static final int MAX_SEND = 1015;

    static native void nativeAdded(int id, String name, String vendor, String product, int vendorId, int productId,
            String serial);
    static native void nativeRemoved(int id);
    static native void nativeReceive(int id, byte[] data, long midiNanos, long appNanos);

    private static final class Open {
        MidiDevice device;
        MidiInputPort toDevice;     // (LEDs and the like: what Mixxx's mapping sends)
        MidiOutputPort fromDevice;  // (the controls)
        // sending, for the profiler: how many, and how long Android took to take them
        long sends, sendNanos, sendMaxNanos;
    }

    private static MidiManager s_manager;
    private static Handler s_handler;   // (opening and sending: the MIDI service can be slow to answer)
    // (the profiler reads it from Mixxx's thread)
    private static final ConcurrentHashMap<Integer, Open> s_open = new ConcurrentHashMap<>();
    private static final ConcurrentHashMap<Integer, Boolean> s_opening = new ConcurrentHashMap<>();

    /// The controller manager's enumerator, once at start-up (after it registered the natives).
    public static void start(Context context) {
        if (s_handler != null) {
            return;
        }
        s_manager = (MidiManager) context.getApplicationContext().getSystemService(Context.MIDI_SERVICE);
        if (s_manager == null) {
            Log.i(TAG, "no MIDI on this device");
            return;
        }
        HandlerThread thread = new HandlerThread("ZydekMidiHost", Process.THREAD_PRIORITY_URGENT_AUDIO);
        thread.start();
        s_handler = new Handler(thread.getLooper());
        s_handler.post(() -> {
            s_manager.registerDeviceCallback(s_callback, s_handler);
            for (MidiDeviceInfo info : s_manager.getDevices()) {
                if (isController(info)) {
                    open(info);
                }
            }
        });
    }

    public static void stop() {
        if (s_handler == null) {
            return;
        }
        s_handler.post(() -> {
            s_manager.unregisterDeviceCallback(s_callback);
            for (Open o : s_open.values()) {
                close(o);
            }
            s_open.clear();
        });
    }

    /// A controller plugged into this device: USB with a USB device behind it (or Bluetooth MIDI), sending something
    private static boolean isController(MidiDeviceInfo info) {
        if (info.getOutputPortCount() == 0) {
            return false;
        }
        if (info.getType() == MidiDeviceInfo.TYPE_USB) {
            return info.getProperties().getParcelable(MidiDeviceInfo.PROPERTY_USB_DEVICE) != null;
        }
        return info.getType() == MidiDeviceInfo.TYPE_BLUETOOTH;
    }

    private static void open(MidiDeviceInfo info) {
        final int id = info.getId();
        if (s_open.get(id) != null || s_opening.get(id) != null) {
            return;
        }
        s_opening.put(id, true);
        s_manager.openDevice(info, device -> {
            s_opening.remove(id);
            if (device == null) {
                Log.w(TAG, "couldn't open " + info);
                return;
            }
            Open o = new Open();
            o.device = device;
            o.fromDevice = device.openOutputPort(0);
            o.toDevice = info.getInputPortCount() > 0 ? device.openInputPort(0) : null;
            if (o.fromDevice == null) {   // (held by another app)
                Log.w(TAG, "its port is busy: " + info);
                close(o);
                return;
            }
            o.fromDevice.connect(new MidiReceiver() {
                @Override
                public void onSend(byte[] msg, int offset, int count, long timestamp) {
                    final long now = System.nanoTime();
                    byte[] copy = new byte[count];
                    System.arraycopy(msg, offset, copy, 0, count);
                    nativeReceive(id, copy, timestamp, now);
                }
            });
            s_open.put(id, o);
            Bundle p = info.getProperties();
            UsbDevice usb = p.getParcelable(MidiDeviceInfo.PROPERTY_USB_DEVICE);
            String vendor = str(p.getString(MidiDeviceInfo.PROPERTY_MANUFACTURER));
            String product = str(p.getString(MidiDeviceInfo.PROPERTY_PRODUCT));
            String name = str(p.getString(MidiDeviceInfo.PROPERTY_NAME));
            if (name.isEmpty()) {
                name = (vendor + " " + product).trim();
            }
            Log.i(TAG, "controller plugged in: " + name);
            nativeAdded(id, name, vendor, product, usb != null ? usb.getVendorId() : -1,
                    usb != null ? usb.getProductId() : -1, str(p.getString(MidiDeviceInfo.PROPERTY_SERIAL_NUMBER)));
        }, s_handler);
    }

    private static String str(String s) {
        return s == null ? "" : s.trim();
    }

    private static void close(Open o) {
        try {
            if (o.fromDevice != null) {
                o.fromDevice.close();
            }
            if (o.toDevice != null) {
                o.toDevice.close();
            }
            if (o.device != null) {
                o.device.close();
            }
        } catch (IOException e) {
            // closing anyway
        }
    }

    /// Mixxx's mapping, any thread: bytes for the controller (its lights). Sent from this class's thread, as a
    /// write can wait on the USB.
    public static void send(int id, byte[] data) {
        if (s_handler == null) {
            return;
        }
        s_handler.post(() -> {
            Open o = s_open.get(id);
            if (o == null || o.toDevice == null) {
                return;
            }
            final long t0 = System.nanoTime();
            try {
                for (int at = 0; at < data.length; at += MAX_SEND) {
                    o.toDevice.send(data, at, Math.min(MAX_SEND, data.length - at));
                }
            } catch (IOException e) {
                Log.w(TAG, "send failed", e);
            }
            final long took = System.nanoTime() - t0;
            synchronized (o) {
                o.sends++;
                o.sendNanos += took;
                o.sendMaxNanos = Math.max(o.sendMaxNanos, took);
            }
        });
    }

    /// The profiler, once a second: {sends, total ns, worst ns} since the last time, for a controller
    public static long[] takeSendStats(int id) {
        Open o = s_open.get(id);   // (read from another thread: a stale answer at worst)
        if (o == null) {
            return new long[] {0, 0, 0};
        }
        synchronized (o) {
            long[] out = {o.sends, o.sendNanos, o.sendMaxNanos};
            o.sends = 0;
            o.sendNanos = 0;
            o.sendMaxNanos = 0;
            return out;
        }
    }

    private static final MidiManager.DeviceCallback s_callback = new MidiManager.DeviceCallback() {
        @Override
        public void onDeviceAdded(MidiDeviceInfo info) {
            if (isController(info)) {
                open(info);
            }
        }

        @Override
        public void onDeviceRemoved(MidiDeviceInfo info) {
            Open o = s_open.get(info.getId());
            if (o != null) {
                s_open.remove(info.getId());
                close(o);
                Log.i(TAG, "controller unplugged: " + info.getId());
                nativeRemoved(info.getId());
            }
        }
    };
}
