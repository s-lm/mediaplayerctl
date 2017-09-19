#include <giomm.h>
#include <glibmm.h>

#include <string>
#include <vector>
#include <set>
#include <map>
#include <iostream>

enum State {STOPPED, PAUSED, PLAYING};
using PlayerSet     = std::set<std::string>;
using PlayerStates  = std::map<std::string, State>;
using PlayerActions = std::map<std::string, std::string>;
using Connection    = Glib::RefPtr<Gio::DBus::Connection>;


static std::string progname = "(unset)";

auto usage()
-> void
{
    std::cout << "usage: " << progname
              << " <play|pause|playpause|stop|next|prev>" << std::endl;
}


auto eval_args(int argc, char** argv)
-> std::string
{
    progname = argv[0];
    if (argc != 2) {
    	usage();
    	exit (127);
    }

    return argv[1];
}


auto getMediaPlayerInstances(const Connection& connection)
-> PlayerSet
{
    PlayerSet result;

    // create proxy
    auto proxy = Gio::DBus::Proxy::create_sync(connection, "org.freedesktop.DBus",
	    "/org/freedesktop/DBus", "org.freedesktop.DBus");
    if (!proxy) {
        std::cerr << "The proxy to the user's session bus was not successfully "
                     "created." << std::endl;
        exit (2);
    }

    try {
        // query dbus, eval response
        const auto call_result = proxy->call_sync("ListNames");
        Glib::Variant<std::vector<Glib::ustring>> names_variant;
        call_result.get_child(names_variant);
        auto names = names_variant.get();

        // filter for org.mpris.MediaPlayer2 entries
        for (const auto& i : names) {
            static const std::string prefix = "org.mpris.MediaPlayer2.";
            if (i.compare(0, prefix.size(), prefix) == 0)
                result.insert(i);
        }
    } catch (const Glib::Error& error) {
        std::cerr << "Got an error: '" << error.what() << "'." << std::endl;
    }

    return std::move(result);
}


auto getMediaPlayerStates(const Connection& connection, const PlayerSet& players)
-> PlayerStates
{
    PlayerStates result;

    // create proxy
    for(const auto& player : players) {
        auto proxy = Gio::DBus::Proxy::create_sync(connection, player,
                "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties");
        if (!proxy) {
            std::cerr << "The proxy to the user's session bus was not "
		    "successfully created." << std::endl;
            exit (3);
        }

        try {
            // query state of players
            using ParamType = std::vector<Glib::VariantBase>;
            using GUString  = Glib::ustring;
            using VGUString = Glib::Variant<GUString>;
            ParamType parameters = {
                VGUString::create("org.mpris.MediaPlayer2.Player"),
                VGUString::create("PlaybackStatus") };
            auto parameter_variant =
                Glib::Variant<std::vector<GUString>>::create_tuple(parameters);

            const auto call_result = proxy->call_sync(
                    "Get",
                    parameter_variant);

            // eval response
            Glib::Variant<std::vector<GUString>> state_variant;
            call_result.get_child(state_variant);
            auto state = state_variant.get();
            if (state.size() != 1) {
                std::cerr << "Unable to determine state of " << player
                          << std::endl;
                continue;
            }
            if (state[0] == "Playing") result[player] = PLAYING;
            else if (state[0] == "Paused") result[player] = PAUSED;
            else if (state[0] == "Stopped") result[player] = STOPPED;
            else {
                std::cerr << "Unknown state " << state[0] << " of " << player
                          << std::endl;
                continue;
            }

        } catch (const Glib::Error& error) {
            std::cerr << "Got an error: '" << error.what() << "'." << std::endl;
        }
    }

    return std::move(result);
}


auto execMediaPlayerMethod(const Connection& connection,
		                   const std::string& player, const std::string& meth)
-> void
{
    // create proxy
    auto proxy = Gio::DBus::Proxy::create_sync(connection, player,
	    "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player");
    if (!proxy) {
        std::cerr << "The proxy to the user's session bus was not successfully "
            "created." << std::endl;
        exit (4);
    }

    try {
        // query state of players
        proxy->call_sync(meth);

    } catch (const Glib::Error& error) {
        std::cerr << "Got an error: '" << error.what() << "'." << std::endl;
    }
}


auto findPlayer(const PlayerStates& pstates, std::vector<State>&& states)
-> PlayerSet
{
    PlayerSet result;
    for (const auto& entry : pstates)
        for (const auto& state : states)
            if (entry.second == state)
                result.insert(entry.first);

    return std::move(result);
}


auto evalActions(const std::string& method, const PlayerStates& states)
-> PlayerActions
{
    PlayerActions result;
    auto players_playing = findPlayer(states, {PLAYING});

    // play|pause|playpause|stop|next|prev>
    if (method == "play") {
	    if (players_playing.empty()) {
            auto player = findPlayer(states, {PAUSED});
            if (player.empty())
                player = findPlayer(states, {STOPPED});
            if (!player.empty())
                result[*player.begin()] = "Play";
	    }
    } else if(method == "pause") {
	    for (const auto& player : players_playing) {
            result[player] = "Pause";
	    }
    } else if(method == "playpause") {
        if (players_playing.empty()) {
            auto player = findPlayer(states, {PAUSED});
            if (player.empty())
                player = findPlayer(states, {STOPPED});
            if (!player.empty())
                result[*player.begin()] = "Play";
        } else {
            for (const auto& player : players_playing) {
                result[player] = "Pause";
            }
        }
    } else if(method == "stop") {
        auto players = findPlayer(states, {PLAYING, PAUSED});
        for (const auto& player : players) {
            result[player] = "Stop";
        }
    } else if(method == "next") {
	    if (!players_playing.empty()) {
            result[*players_playing.begin()] = "Next";
	    }
    } else if(method == "prev") {
	    if (!players_playing.empty()) {
            result[*players_playing.begin()] = "Previous";
	    }
    } else {
        std::cerr << "Unknown method " << method << std::endl;
        usage();
        exit(127);
    }

    return std::move(result);
}


auto main(int argc, char** argv)
-> int
{
    std::locale::global(std::locale(""));
    Gio::init();

    // eval cli args
    std::string method = eval_args(argc, argv);


    // get the user session bus connection.
    auto connection = Gio::DBus::Connection::get_sync(
            Gio::DBus::BusType::BUS_TYPE_SESSION);
    if (!connection) {
        std::cerr << "The user's session bus is not available." << std::endl;
        return 1;
    }

    // get running players
    auto players = getMediaPlayerInstances(connection);
    if (players.empty()) {
        std::cout << "no player found." << std::endl;
        return 0;
    }

    // get state of running players
    auto states = getMediaPlayerStates(connection, players);

    // eval actions
    auto actions = evalActions(method, states);

    // exec command, if necessary
    for (const auto& action : actions) {
	    if (action.second.empty()) continue;
	    execMediaPlayerMethod(connection, action.first, action.second);
    }

    return 0;
}
