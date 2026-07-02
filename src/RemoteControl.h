#pragma once

#include "ProjectMWrapper.h"

#include <Poco/Logger.h>
#include <Poco/Util/Subsystem.h>
#include <Poco/Util/AbstractConfiguration.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

namespace httplib { class Server; }

/**
 * @brief In-process HTTP remote control subsystem.
 *
 * Serves a mobile web page and a REST API on a background thread. HTTP handlers
 * only enqueue commands and read mutex-guarded snapshots; all projectM/SDL work
 * happens on the render thread via DrainCommands(), called once per frame by
 * RenderLoop. Favorites are HTTP-thread-safe (own mutex, no projectM access).
 */
class RemoteControl : public Poco::Util::Subsystem
{
public:
    enum class CommandType
    {
        Next, Previous, Random, ToggleShuffle, ToggleLock, NextAudio, LoadPack,
        SetPosition, SetSetting
    };

    struct Command
    {
        CommandType type;
        std::string arg;  // pack name / preset index / setting key
        std::string arg2; // setting value
    };

    RemoteControl();
    ~RemoteControl() override; // out-of-line: unique_ptr<httplib::Server> is incomplete here

    const char* name() const override;
    void initialize(Poco::Util::Application& app) override;
    void uninitialize() override;

    /** Executes all queued commands. MUST be called on the render thread. */
    void DrainCommands();

    /** Refreshes status + settings snapshots served to HTTP clients. Render thread only. */
    void PublishStatus(const ProjectMWrapper::PlaybackStatus& status, const std::string& audioDevice);

private:
    void RegisterRoutes();
    void Enqueue(const Command& command);
    bool Authorized(const std::string& token) const;
    std::string StatusJson() const;
    std::string PacksJson() const;
    std::string PresetsJson() const;
    std::string SettingsJson() const;
    std::string FavoritesJson() const;
    bool ToggleFavorite(const std::string& path);
    void LoadFavorites();
    void SaveFavorites();
    void RebuildPresetCache();       //!< Render thread only.
    void ApplySetting(const std::string& key, const std::string& value); //!< Render thread only.
    void JumpToFavorite(bool nextInOrder); //!< Render thread only.

    std::unique_ptr<httplib::Server> _server;
    std::thread _serverThread;

    mutable std::mutex _queueMutex;
    std::deque<Command> _queue;

    mutable std::mutex _statusMutex;
    std::string _statusJson{"{}"};
    std::string _settingsJson{"{}"};

    mutable std::mutex _dataMutex;
    std::string _presetsJson{"[]"};

    mutable std::mutex _favMutex;
    std::set<std::string> _favorites;
    std::string _favoritesFile;

    std::unordered_map<std::string, uint32_t> _pathToIndex; //!< Render thread only.
    std::atomic<bool> _presetsDirty{true};
    std::atomic<bool> _favShuffle{false};

    std::string _token;
    std::string _presetRoot;
    std::string _webRoot;
    uint16_t _port{8080};
    std::atomic<bool> _running{false};

    Poco::Logger& _logger{Poco::Logger::get("RemoteControl")};
};
