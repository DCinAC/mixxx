#pragma once

#ifdef __BROADCAST__

#include "preferences/broadcastsettings.h"
#include "preferences/usersettings.h"

namespace zydeck {

// ZyDeck: a Live Broadcasting connection to the ZyDeck Discord bot, "ZyDeck Discord", set up from ZyDeck Connect
// (its menu's Discord Bot..., where the stream key is typed) rather than Mixxx's preferences. Connect hands it over
// once in zydeck-broadcast.json in the settings folder ({host, port, key}); this reads it and deletes it at once,
// and makes (or updates) the profile as the bot takes a stream: Icecast 2, mount /zydeck, Ogg Opus, 48 kHz stereo,
// the key as its password, kept in the system's keychain (Mixxx's secure credential storage), switched on. Mixxx's
// Live Broadcasting switch then streams to the bot. The answer, without the key: zydeck-broadcast-done.json.
void watchDiscordBroadcast(BroadcastSettingsPointer pBroadcast, UserSettingsPointer pConfig);

} // namespace zydeck

#endif // __BROADCAST__
