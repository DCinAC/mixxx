package org.mixxx;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;
import java.net.InetAddress;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

/// Zydek: ZyDecks on the same Wi-Fi find each other, like Bluetooth devices. Each one announces its server
/// (port 8766) as a "_zydeck._tcp" service under the device's name; Scan lists the others, and Connect opens
/// the other one's controller page here (the tablet controlling the phone's decks, say).
public class ZydekLink {
    private static final String TAG = "ZydekLink";
    private static final String TYPE = "_zydeck._tcp.";
    private static final int PORT = 8766;

    private static NsdManager s_nsd;
    private static String s_myName = "";
    private static final Map<String, JSONObject> s_found = new LinkedHashMap<>();
    private static final ArrayDeque<NsdServiceInfo> s_toResolve = new ArrayDeque<>();
    private static boolean s_resolving;
    private static boolean s_discovering;

    /// The name other devices see: the one set in Android's settings ("About phone"), or the model.
    static String deviceName(Context context) {
        String name = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            name = Settings.Global.getString(context.getContentResolver(), Settings.Global.DEVICE_NAME);
        }
        return name == null || name.isEmpty() ? Build.MODEL : name;
    }

    public static void init(Context context) {
        s_nsd = (NsdManager) context.getSystemService(Context.NSD_SERVICE);
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName(deviceName(context));
        info.setServiceType(TYPE);
        info.setPort(PORT);
        try {
            s_nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, new NsdManager.RegistrationListener() {
                @Override
                public void onServiceRegistered(NsdServiceInfo registered) {
                    s_myName = registered.getServiceName();   // Android may add " (2)" if the name is taken
                }

                @Override
                public void onRegistrationFailed(NsdServiceInfo i, int errorCode) {
                    Log.w(TAG, "can't announce this device: " + errorCode);
                }

                @Override
                public void onServiceUnregistered(NsdServiceInfo i) {}

                @Override
                public void onUnregistrationFailed(NsdServiceInfo i, int errorCode) {}
            });
        } catch (RuntimeException e) {
            Log.w(TAG, "can't announce this device", e);
        }
    }

    /// Starts looking (again) for 15 s; devices() has what turned up so far.
    public static synchronized void scan() {
        if (s_nsd == null) {
            return;
        }
        s_found.clear();
        if (s_discovering) {
            return;
        }
        s_discovering = true;
        final NsdManager.DiscoveryListener listener = new NsdManager.DiscoveryListener() {
            @Override
            public void onServiceFound(NsdServiceInfo service) {
                if (!service.getServiceName().equals(s_myName)) {
                    resolve(service);
                }
            }

            @Override
            public void onServiceLost(NsdServiceInfo service) {
                synchronized (ZydekLink.class) {
                    s_found.remove(service.getServiceName());
                }
            }

            @Override
            public void onDiscoveryStarted(String type) {}

            @Override
            public void onDiscoveryStopped(String type) {
                synchronized (ZydekLink.class) {
                    s_discovering = false;
                }
            }

            @Override
            public void onStartDiscoveryFailed(String type, int errorCode) {
                Log.w(TAG, "can't look for devices: " + errorCode);
                synchronized (ZydekLink.class) {
                    s_discovering = false;
                }
            }

            @Override
            public void onStopDiscoveryFailed(String type, int errorCode) {}
        };
        s_nsd.discoverServices(TYPE, NsdManager.PROTOCOL_DNS_SD, listener);
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            try {
                s_nsd.stopServiceDiscovery(listener);
            } catch (RuntimeException e) {
                // already stopped
            }
        }, 15000);
    }

    // NsdManager resolves one service at a time
    private static synchronized void resolve(NsdServiceInfo service) {
        s_toResolve.add(service);
        if (!s_resolving) {
            resolveNext();
        }
    }

    @SuppressWarnings("deprecation")
    private static synchronized void resolveNext() {
        NsdServiceInfo next = s_toResolve.poll();
        s_resolving = next != null;
        if (next == null) {
            return;
        }
        s_nsd.resolveService(next, new NsdManager.ResolveListener() {
            @Override
            public void onServiceResolved(NsdServiceInfo info) {
                InetAddress host = info.getHost();
                if (host != null) {
                    try {
                        JSONObject d = new JSONObject();
                        d.put("name", info.getServiceName());
                        d.put("host", host.getHostAddress());
                        d.put("port", info.getPort());
                        synchronized (ZydekLink.class) {
                            s_found.put(info.getServiceName(), d);
                        }
                    } catch (Exception e) {
                        // skip it
                    }
                }
                resolveNext();
            }

            @Override
            public void onResolveFailed(NsdServiceInfo info, int errorCode) {
                resolveNext();
            }
        });
    }

    /// [{name, host, port}], and whether it's still looking
    public static synchronized String devices() {
        try {
            JSONObject out = new JSONObject();
            out.put("scanning", s_discovering);
            out.put("me", s_myName);
            out.put("devices", new JSONArray(s_found.values()));
            return out.toString();
        } catch (Exception e) {
            return "{\"devices\":[]}";
        }
    }
}
