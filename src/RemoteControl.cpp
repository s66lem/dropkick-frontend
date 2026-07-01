#include "RemoteControl.h"

#include "AudioCapture.h"
#include "ProjectMSDLApplication.h"
#include "notifications/PlaybackControlNotification.h"

#include <httplib.h>

#include <Poco/NotificationCenter.h>
#include <Poco/Path.h>
#include <Poco/Util/Application.h>

#include <dirent.h>
#include <sstream>
#include <sys/stat.h>

namespace
{
std::string JsonEscape(const std::string& in)
{
    std::string out;
    for (char c : in)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else { out += c; }
    }
    return out;
}
} // namespace

RemoteControl::RemoteControl() = default;
RemoteControl::~RemoteControl() = default;

const char* RemoteControl::name() const
{
    return "RemoteControl";
}

void RemoteControl::initialize(Poco::Util::Application& app)
{
    auto config = app.config().createView("remote");
    _port = static_cast<uint16_t>(config->getInt("port", 8080));
    _token = config->getString("token", "");
    _presetRoot = config->getString("presetRoot",
                     Poco::Path::expand("~/.local/share/dropkick/presets"));
    _webRoot = config->getString("webRoot",
                     Poco::Path::expand("~/.local/share/dropkick/remote"));

    _server = std::make_unique<httplib::Server>();
    RegisterRoutes();

    _running = true;
    _serverThread = std::thread([this]() {
        poco_information_f1(_logger, "Remote control starting on port %u.", static_cast<unsigned>(_port));
        if (!_server->listen("0.0.0.0", _port))
        {
            poco_error_f1(_logger, "Remote control failed to bind port %u (already in use?).", static_cast<unsigned>(_port));
        }
    });
}

void RemoteControl::uninitialize()
{
    _running = false;
    if (_server) { _server->stop(); }
    if (_serverThread.joinable()) { _serverThread.join(); }
    _server.reset();
}

bool RemoteControl::Authorized(const std::string& token) const
{
    return _token.empty() || token == _token;
}

void RemoteControl::Enqueue(const Command& command)
{
    std::lock_guard<std::mutex> lock(_queueMutex);
    _queue.push_back(command);
}

void RemoteControl::RegisterRoutes()
{
    auto guard = [this](const httplib::Request& req, httplib::Response& res) -> bool {
        std::string token = req.get_header_value("X-Dropkick-Token");
        if (token.empty()) { token = req.get_param_value("token"); }
        if (!Authorized(token))
        {
            res.status = 401;
            res.set_content("{\"error\":\"unauthorized\"}", "application/json");
            return false;
        }
        return true;
    };

    _server->set_mount_point("/", _webRoot); // serves index.html, app.js, style.css

    _server->Get("/api/status", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        res.set_content(StatusJson(), "application/json");
    });

    _server->Get("/api/packs", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        res.set_content(PacksJson(), "application/json");
    });

    auto post = [this, guard](const char* path, CommandType type) {
        _server->Post(path, [this, guard, type](const httplib::Request& req, httplib::Response& res) {
            if (!guard(req, res)) { return; }
            Enqueue(Command{type, ""});
            res.set_content("{\"ok\":true}", "application/json");
        });
    };

    post("/api/next", CommandType::Next);
    post("/api/prev", CommandType::Previous);
    post("/api/random", CommandType::Random);
    post("/api/shuffle", CommandType::ToggleShuffle);
    post("/api/lock", CommandType::ToggleLock);
    post("/api/audio/next", CommandType::NextAudio);

    _server->Post("/api/pack", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        std::string pack = req.get_param_value("name");
        if (pack.empty() || pack.find("..") != std::string::npos || pack.find('/') != std::string::npos)
        {
            res.status = 400;
            res.set_content("{\"error\":\"invalid pack name\"}", "application/json");
            return;
        }
        Enqueue(Command{CommandType::LoadPack, pack});
        res.set_content("{\"ok\":true}", "application/json");
    });
}

void RemoteControl::DrainCommands()
{
    std::deque<Command> pending;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        pending.swap(_queue);
    }

    auto& center = Poco::NotificationCenter::defaultCenter();
    auto& app = ProjectMSDLApplication::instance();

    for (const auto& command : pending)
    {
        switch (command.type)
        {
            case CommandType::Next:
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::NextPreset));
                break;
            case CommandType::Previous:
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::PreviousPreset));
                break;
            case CommandType::Random:
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::RandomPreset));
                break;
            case CommandType::ToggleShuffle:
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::ToggleShuffle));
                break;
            case CommandType::ToggleLock:
                center.postNotification(new PlaybackControlNotification(PlaybackControlNotification::Action::TogglePresetLocked));
                break;
            case CommandType::NextAudio:
                app.getSubsystem<AudioCapture>().NextAudioDevice();
                break;
            case CommandType::LoadPack:
            {
                std::string path = _presetRoot + "/" + command.arg;
                app.getSubsystem<ProjectMWrapper>().LoadPresetPack(path);
                break;
            }
        }
    }
}

void RemoteControl::PublishStatus(const ProjectMWrapper::PlaybackStatus& status, const std::string& audioDevice)
{
    std::ostringstream json;
    json << "{"
         << "\"preset\":\"" << JsonEscape(status.presetName) << "\","
         << "\"position\":" << status.position << ","
         << "\"size\":" << status.playlistSize << ","
         << "\"shuffle\":" << (status.shuffle ? "true" : "false") << ","
         << "\"locked\":" << (status.locked ? "true" : "false") << ","
         << "\"audio\":\"" << JsonEscape(audioDevice) << "\""
         << "}";
    std::lock_guard<std::mutex> lock(_statusMutex);
    _statusJson = json.str();
}

std::string RemoteControl::StatusJson() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    return _statusJson;
}

std::string RemoteControl::PacksJson() const
{
    std::ostringstream json;
    json << "[";
    DIR* dir = opendir(_presetRoot.c_str());
    bool first = true;
    if (dir)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            std::string entryName = entry->d_name;
            if (entryName == "." || entryName == "..") { continue; }
            std::string full = _presetRoot + "/" + entryName;
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            {
                if (!first) { json << ","; }
                json << "\"" << JsonEscape(entryName) << "\"";
                first = false;
            }
        }
        closedir(dir);
    }
    json << "]";
    return json.str();
}
