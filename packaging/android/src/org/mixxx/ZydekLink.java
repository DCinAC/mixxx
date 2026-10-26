package org.mixxx;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.URL;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/// Zydek: ZyDecks on the same Wi-Fi find each other, like Bluetooth devices. Each one announces its server
/// (port 8766) as a "_zydeck._tcp" service under the device's name; Scan lists the others, and Connect opens
/// the other one's controller page here (the tablet controlling the phone's decks, say).
///
/// Android doesn't announce services on its own hotspot, though (nor do some routers pass the announcements
/// on), so Scan also asks every address of this device's Wi-Fi networks directly: GET /api/whoami on port
/// 8766, which only a ZyDeck answers.
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
    private static boolean s_probing;
    private static Context s_context;

    /// This device's name, for /api/whoami (the hub)
    public static String displayName() {
        return s_context == null ? Build.MODEL : deviceName(s_context);
    }

    /// The name other devices see: the one set in Android's settings ("About phone"), or the model.
    static String deviceName(Context context) {
        String name = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            name = Settings.Global.getString(context.getContentResolver(), Settings.Global.DEVICE_NAME);
        }
        return name == null || name.isEmpty() ? Build.MODEL : name;
    }

    public static void init(Context context) {
        if (s_nsd != null) {
            return;   // announced once per app, not per activity
        }
        s_context = context.getApplicationContext();
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
    // ---- controller mode over Wi-Fi: the radio kept awake ----
    // Android's Wi-Fi power saving holds what arrives until the radio wakes for the router's next beacon (every
    // ~100 ms): the computer's deck positions came in clumps 100 ms apart (up to 0.7 s), and a scratch's grab reached
    // Mixxx late. A low-latency lock (the one games and calls use) keeps it awake while ZyDeck is linked over Wi-Fi;
    // Android honours it only while the app is in front with the screen on. No permission beyond WAKE_LOCK.
    private static android.net.wifi.WifiManager.WifiLock s_wifiLock;

    public static synchronized void setLowLatency(boolean on) {
        if (s_context == null) {
            return;
        }
        if (on && s_wifiLock == null) {
            android.net.wifi.WifiManager wifi = (android.net.wifi.WifiManager) s_context.getSystemService(Context.WIFI_SERVICE);
            if (wifi == null) {
                return;
            }
            int mode = Build.VERSION.SDK_INT >= 29 ? android.net.wifi.WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                                                   : android.net.wifi.WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            s_wifiLock = wifi.createWifiLock(mode, "ZyDeck:controller");
            s_wifiLock.setReferenceCounted(false);
            s_wifiLock.acquire();
            Log.i(TAG, "Wi-Fi kept awake (low latency) while linked");
        } else if (!on && s_wifiLock != null) {
            s_wifiLock.release();
            s_wifiLock = null;
        }
    }

    /// Battery saver on: it overrides the lock (and slows the device), worth saying while linked over Wi-Fi
    public static boolean powerSave() {
        if (s_context == null) {
            return false;
        }
        android.os.PowerManager pm = (android.os.PowerManager) s_context.getSystemService(Context.POWER_SERVICE);
        return pm != null && pm.isPowerSaveMode();
    }

    /// For the session profiler (the hub, once a second while it records): {"rssi": dBm, "mbps": link speed,
    /// "mhz": band, "thermal": 0-6, "batteryC": temperature, "battery": %, "charging"}; what this Android
    /// version tells without asking for more (the Wi-Fi from the network's own report, Android 10 and 12 on).
    public static String deviceStats() {
        if (s_context == null) {
            return "{}";
        }
        StringBuilder out = new StringBuilder("{");
        try {
            android.net.ConnectivityManager cm =
                    (android.net.ConnectivityManager) s_context.getSystemService(Context.CONNECTIVITY_SERVICE);
            android.net.NetworkCapabilities caps = cm == null ? null : cm.getNetworkCapabilities(cm.getActiveNetwork());
            if (caps != null && caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)) {
                if (Build.VERSION.SDK_INT >= 29 && caps.getSignalStrength() != android.net.NetworkCapabilities.SIGNAL_STRENGTH_UNSPECIFIED) {
                    out.append("\"rssi\":").append(caps.getSignalStrength()).append(',');
                }
                if (Build.VERSION.SDK_INT >= 31 && caps.getTransportInfo() instanceof android.net.wifi.WifiInfo) {
                    android.net.wifi.WifiInfo info = (android.net.wifi.WifiInfo) caps.getTransportInfo();
                    out.append("\"mbps\":").append(info.getLinkSpeed()).append(",\"mhz\":").append(info.getFrequency()).append(',');
                }
            }
        } catch (RuntimeException e) {
            // (not on this device)
        }
        android.os.PowerManager pm = (android.os.PowerManager) s_context.getSystemService(Context.POWER_SERVICE);
        if (pm != null && Build.VERSION.SDK_INT >= 29) {
            out.append("\"thermal\":").append(pm.getCurrentThermalStatus()).append(',');
        }
        android.content.Intent battery = s_context.registerReceiver(null,
                new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
        if (battery != null) {
            int level = battery.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1);
            int scale = battery.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100);
            int status = battery.getIntExtra(android.os.BatteryManager.EXTRA_STATUS, -1);
            out.append("\"batteryC\":").append(battery.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, 0) / 10.0)
                    .append(",\"battery\":").append(scale > 0 ? 100 * level / scale : -1)
                    .append(",\"charging\":").append(status == android.os.BatteryManager.BATTERY_STATUS_CHARGING
                            || status == android.os.BatteryManager.BATTERY_STATUS_FULL);
        }
        if (out.charAt(out.length() - 1) == ',') {
            out.setLength(out.length() - 1);
        }
        return out.append('}').toString();
    }

    public static synchronized void scan() {
        if (s_nsd == null) {
            return;
        }
        s_found.clear();
        if (!s_probing) {
            s_probing = true;
            new Thread(ZydekLink::probeNetworks, "zydeck-scan").start();
        }
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

    /// Every address of this device's Wi-Fi networks (its own hotspot's too), at most 254 per network, asked
    /// at once in batches: about 10 s at worst.
    private static void probeNetworks() {
        try {
            Set<String> mine = new HashSet<>();
            List<String> hosts = new ArrayList<>();
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                String name = ni.getName();
                if (!ni.isUp() || ni.isLoopback() || !(name.startsWith("wlan") || name.startsWith("swlan")
                        || name.startsWith("ap") || name.startsWith("eth"))) {
                    continue;
                }
                for (InterfaceAddress a : ni.getInterfaceAddresses()) {
                    if (!(a.getAddress() instanceof Inet4Address)) {
                        continue;
                    }
                    mine.add(a.getAddress().getHostAddress());
                    byte[] b = a.getAddress().getAddress();
                    int ip = ((b[0] & 255) << 24) | ((b[1] & 255) << 16) | ((b[2] & 255) << 8) | (b[3] & 255);
                    int prefix = Math.max(a.getNetworkPrefixLength(), 24);
                    int net = ip & (-1 << (32 - prefix));
                    int count = (1 << (32 - prefix)) - 2;
                    for (int i = 1; i <= count; i++) {
                        int h = net + i;
                        hosts.add(((h >>> 24) & 255) + "." + ((h >>> 16) & 255) + "." + ((h >>> 8) & 255) + "." + (h & 255));
                    }
                }
            }
            hosts.removeAll(mine);
            ExecutorService pool = Executors.newFixedThreadPool(48);
            for (String host : hosts) {
                pool.execute(() -> probe(host));
            }
            pool.shutdown();
            pool.awaitTermination(20, TimeUnit.SECONDS);
            pool.shutdownNow();
        } catch (Exception e) {
            Log.w(TAG, "network scan failed", e);
        } finally {
            synchronized (ZydekLink.class) {
                s_probing = false;
            }
        }
    }

    private static void probe(String host) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL("http://" + host + ":" + PORT + "/api/whoami").openConnection();
            c.setConnectTimeout(600);
            c.setReadTimeout(1500);
            if (c.getResponseCode() != 200) {
                return;
            }
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[1024];
                for (int n; (n = in.read(buf)) > 0 && body.size() < 4096;) {
                    body.write(buf, 0, n);
                }
            }
            JSONObject j = new JSONObject(body.toString("UTF-8"));
            if (!j.optBoolean("zydeck")) {
                return;
            }
            String name = j.optString("name", host);
            JSONObject d = new JSONObject();
            d.put("name", name);
            d.put("host", host);
            d.put("port", PORT);
            synchronized (ZydekLink.class) {
                for (JSONObject other : s_found.values()) {
                    if (host.equals(other.optString("host"))) {
                        return;   // found by its announcement already
                    }
                }
                s_found.put(s_found.containsKey(name) ? name + " (" + host + ")" : name, d);
            }
        } catch (Exception e) {
            // nothing there, or not a ZyDeck
        } finally {
            if (c != null) {
                c.disconnect();
            }
        }
    }

    /// [{name, host, port}], and whether it's still looking
    public static synchronized String devices() {
        try {
            JSONObject out = new JSONObject();
            out.put("scanning", s_discovering || s_probing);
            out.put("me", s_myName);
            out.put("devices", new JSONArray(s_found.values()));
            return out.toString();
        } catch (Exception e) {
            return "{\"devices\":[]}";
        }
    }
}
