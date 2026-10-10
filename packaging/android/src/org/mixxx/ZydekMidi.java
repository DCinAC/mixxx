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
import android.os.Looper;
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
    private static final Handler s_handler = new Handler(Looper.getMainLooper());
    private static boolean s_enabled;
    private static MidiDevice s_device;
    private static MidiInputPort s_toComputer;
    private static MidiOutputPort s_fromComputer;
    private static int s_state = -1;
    private static boolean s_usbPlugged;

    static native void nativeReceive(byte[] data);
    static native void nativeState(int state);

    /// MainActivity, at start-up.
    public static void init(Context context) {
        s_context = context.getApplicationContext();
        s_manager = (MidiManager) s_context.getSystemService(Context.MIDI_SERVICE);
    }

    /// The hub, when controller mode is turned on or off.
    public static void setEnabled(boolean on) {
        s_handler.post(() -> {
            if (s_manager == null || on == s_enabled) {
                return;
            }
            s_enabled = on;
            if (on) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    s_context.registerReceiver(s_usbReceiver, new IntentFilter(USB_STATE), Context.RECEIVER_NOT_EXPORTED);
                } else {
                    s_context.registerReceiver(s_usbReceiver, new IntentFilter(USB_STATE));
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

    /// The hub: MIDI bytes for the computer (any thread). false when there's no USB MIDI link.
    public static boolean send(byte[] data) {
        MidiInputPort port = s_toComputer;
        if (port == null) {
            return false;
        }
        try {
            port.send(data, 0, data.length);
            return true;
        } catch (IOException e) {
            Log.w(TAG, "send failed", e);
            return false;
        }
    }

    /// The computer's port shows as a USB device without a USB device behind it (a USB host's keyboard
    /// or controller plugged into the phone has one).
    private static boolean isPeripheralPort(MidiDeviceInfo info) {
        return info.getType() == MidiDeviceInfo.TYPE_USB
                && info.getProperties().getParcelable(MidiDeviceInfo.PROPERTY_USB_DEVICE) == null
                && info.getInputPortCount() > 0 && info.getOutputPortCount() > 0;
    }

    private static void findPort() {
        if (!s_enabled || s_device != null) {
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
        s_manager.openDevice(info, device -> {
            if (device == null || !s_enabled) {
                report();
                return;
            }
            s_device = device;
            s_toComputer = device.openInputPort(0);
            s_fromComputer = device.openOutputPort(0);
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
            }
            findPort();
        }
    };
}
