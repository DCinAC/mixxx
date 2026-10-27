package org.mixxx;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;
import java.io.IOException;

/// Zydek controller mode: the phone or tablet as a USB MIDI device for desktop Mixxx.
///
/// When the USB cable is set to "MIDI" (Android's USB notification), Android offers the computer as a MIDI
/// device of its own here: the "USB MIDI peripheral" port. Bytes sent to its input port go to the computer,
/// and what the computer sends arrives on its output port. Zydek's hub (C++) turns the controller page into
/// the Zydek mapping's MIDI/SysEx, exactly as for Mixxx on the phone, and hands it to send(); what comes back
/// goes to nativeReceive(). nativeState() reports the cable: 0 unplugged, 1 plugged in but not set to MIDI,
/// 2 ready.
public class ZydekMidi {
    private static final String TAG = "ZydekMidi";
    // Android's sticky broadcast about the USB cable (a hidden API, stable since Android 4)
    private static final String USB_STATE = "android.hardware.usb.action.USB_STATE";

    private static Context s_context;
    private static MidiManager s_manager;
    // All of it runs on its own thread: opening the port is a call into Android's MIDI service, which can hang
    // (it did when the app was replaced while the old one still had the port open), and on the main thread that
    // froze the whole app at start-up.
    private static Handler s_handler;
    private static boolean s_enabled;
    private static MidiDevice s_device;
    private static volatile MidiInputPort s_toComputer;
    private static MidiOutputPort s_fromComputer;
    private static int s_state = -1;
    private static boolean s_usbPlugged;
    // An open on its way (Android answers later): plugging in brings two calls at once (the USB state and the MIDI
    // device), and a second open of the port gets nothing (only one may hold it), which then stood for the cable
    private static boolean s_opening;
    private static int s_retries;

    static native void nativeReceive(byte[] data);
    static native void nativeState(int state);

    /// MainActivity, at start-up.
    public static void init(Context context) {
        if (s_handler != null) {
            return;   // once per app, not per activity
        }
        s_context = context.getApplicationContext();
        s_manager = (MidiManager) s_context.getSystemService(Context.MIDI_SERVICE);
        HandlerThread thread = new HandlerThread("ZydekMidi");
        thread.start();
        s_handler = new Handler(thread.getLooper());
    }

    /// The hub, when controller mode is turned on or off.
    public static void setEnabled(boolean on) {
        if (s_handler == null) {
            return;
        }
        s_handler.post(() -> {
            if (s_manager == null || on == s_enabled) {
                return;
            }
            s_enabled = on;
            if (on) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    s_context.registerReceiver(s_usbReceiver, new IntentFilter(USB_STATE), null, s_handler,
                            Context.RECEIVER_NOT_EXPORTED);
                } else {
                    s_context.registerReceiver(s_usbReceiver, new IntentFilter(USB_STATE), null, s_handler);
                }
                s_manager.registerDeviceCallback(s_deviceCallback, s_handler);
                findPort();
            } else {
                try {
                    s_context.unregisterReceiver(s_usbReceiver);
                } catch (IllegalArgumentException e) {
                    // wasn't registered
                }
                s_manager.unregisterDeviceCallback(s_deviceCallback);
                close();
                s_state = -1;
            }
        });
    }

    /// The hub: MIDI bytes for the computer (any thread). false when there's no USB MIDI link, or the computer
    /// has stopped taking them. Sent from the MIDI thread: a write blocks when the computer doesn't read, and
    /// the hub's thread (the pages' controls) mustn't wait for that.
    public static boolean send(byte[] data) {
        if (s_toComputer == null || s_pending.get() > 4000) {
            return false;
        }
        s_pending.incrementAndGet();
        s_handler.post(() -> {
            s_pending.decrementAndGet();
            MidiInputPort port = s_toComputer;
            if (port == null) {
                return;
            }
            try {
                port.send(data, 0, data.length);
            } catch (IOException e) {
                Log.w(TAG, "send failed", e);
            }
        });
        return true;
    }

    private static final java.util.concurrent.atomic.AtomicInteger s_pending = new java.util.concurrent.atomic.AtomicInteger();

    /// The computer's port shows as a USB device without a USB device behind it (a USB host's keyboard
    /// or controller plugged into the phone has one).
    private static boolean isPeripheralPort(MidiDeviceInfo info) {
        return info.getType() == MidiDeviceInfo.TYPE_USB
                && info.getProperties().getParcelable(MidiDeviceInfo.PROPERTY_USB_DEVICE) == null
                && info.getInputPortCount() > 0 && info.getOutputPortCount() > 0;
    }

    private static void findPort() {
        if (!s_enabled || s_device != null || s_opening) {
            report();
            return;
        }
        MidiDeviceInfo[] infos = s_manager.getDevices();
        for (MidiDeviceInfo info : infos) {
            if (isPeripheralPort(info)) {
                open(info);
                return;
            }
        }
        report();
    }

    private static void open(MidiDeviceInfo info) {
        s_opening = true;
        s_manager.openDevice(info, device -> {
            s_opening = false;
            if (device == null || !s_enabled) {
                report();
                return;
            }
            MidiInputPort in = device.openInputPort(0);
            MidiOutputPort out = device.openOutputPort(0);
            if (in == null || out == null) {   // held (an open that hasn't let go yet): again in a moment, a few times
                try {
                    if (in != null) {
                        in.close();
                    }
                    if (out != null) {
                        out.close();
                    }
                    device.close();
                } catch (IOException e) {
                    // closing anyway
                }
                Log.w(TAG, "USB MIDI port busy" + (s_retries < 5 ? ", trying again" : ""));
                if (s_retries++ < 5) {
                    s_handler.postDelayed(ZydekMidi::findPort, 1000);
                }
                report();
                return;
            }
            s_retries = 0;
            s_device = device;
            s_toComputer = in;
            s_fromComputer = out;
            if (s_fromComputer != null) {
                s_fromComputer.connect(new MidiReceiver() {
                    @Override
                    public void onSend(byte[] msg, int offset, int count, long timestamp) {
                        byte[] copy = new byte[count];
                        System.arraycopy(msg, offset, copy, 0, count);
                        nativeReceive(copy);
                    }
                });
            }
            Log.i(TAG, "USB MIDI link to the computer is open");
            report();
        }, s_handler);
    }

    private static void close() {
        try {
            if (s_fromComputer != null) {
                s_fromComputer.close();
            }
            if (s_toComputer != null) {
                s_toComputer.close();
            }
            if (s_device != null) {
                s_device.close();
            }
        } catch (IOException e) {
            // closing anyway
        }
        s_fromComputer = null;
        s_toComputer = null;
        s_device = null;
    }

    private static void report() {
        int state = s_toComputer != null ? 2 : s_usbPlugged ? 1 : 0;
        if (state != s_state && s_enabled) {
            s_state = state;
            nativeState(state);
        }
    }

    private static final MidiManager.DeviceCallback s_deviceCallback = new MidiManager.DeviceCallback() {
        @Override
        public void onDeviceAdded(MidiDeviceInfo info) {
            if (isPeripheralPort(info)) {
                findPort();
            }
        }

        @Override
        public void onDeviceRemoved(MidiDeviceInfo info) {
            if (s_device != null && s_device.getInfo().getId() == info.getId()) {
                close();
                report();
            }
        }
    };

    private static final BroadcastReceiver s_usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            s_usbPlugged = intent.getBooleanExtra("connected", false);
            if (!s_usbPlugged) {
                close();
                s_retries = 0;
            }
            findPort();
        }
    };
}
