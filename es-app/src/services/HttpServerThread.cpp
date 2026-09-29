#include "HttpServerThread.h"
#include "httplib.h"
#include "Log.h"

#ifdef WIN32
#include <Windows.h>
#endif

#include "utils/Platform.h"
#include "Gamelist.h"
#include "SystemData.h"
#include "FileData.h"
#include "views/ViewController.h"
#include <unordered_map>
#include "CollectionSystemManager.h"
#include "guis/GuiMenu.h"
#include "guis/GuiMsgBox.h"
#include "utils/FileSystemUtil.h"
#include "HttpApi.h"
#include "Settings.h"
#include "ApiSystem.h"

#include "ThreadedHasher.h"
#include "scrapers/ThreadedScraper.h"
#include "guis/GuiUpdate.h"
#include "ContentInstaller.h"

#ifdef _ENABLETURBORAMA
#include "scrapers/Scraper.h"
#endif

/*

Misc APIS
-----------------
GET  /caps                                                      -> capability info
GET  /restart
GET  /quit
GET  /emukill
GET  /reloadgames
POST /messagebox												-> body must contain the message text as text/plain
POST /notify													-> body must contain the message text as text/plain
POST /launch													-> body must contain the exact file path as text/plain
GET  /runningGame
GET  /isIdle

System/Games APIS
-----------------
GET  /systems
GET  /systems/{systemName}
GET  /systems/{systemName}/logo
GET  /systems/{systemName}/games/{gameId}
POST /systems/{systemName}/games/{gameId}						-> body must contain the game metadata to save as application/json
GET  /systems/{systemName}/games/{gameId}/media/{mediaType}
POST /systems/{systemName}/games/{gameId}/media/{mediaType}		-> body must contain the file bytes to save. Content-type must be valid.

Store APIs
----------
POST /addgames/{systemName}										-> body must contain partial gamelist.xml file as application/xml
POST /removegames/{systemName}									-> body must contains partial gamelist.xml file as application/xml

File APIs
---------
GET /resources/{path relative to resources}"					-> any file in resources
GET /{path relative to resources/services}"						-> any other file in resources/services

*/
HttpServerThread::HttpServerThread(Window* window) : mWindow(window)
{
	LOG(LogDebug) << "HttpServerThread : Starting";

	mHttpServer = nullptr;

	// creer le thread
	mFirstRun = true;
	mRunning = true;
	mThread = new std::thread(&HttpServerThread::run, this);
}

HttpServerThread::~HttpServerThread()
{
	LOG(LogDebug) << "HttpServerThread : Exit";

	if (mHttpServer != nullptr)
	{
		mHttpServer->stop();
		delete mHttpServer;
		mHttpServer = nullptr;
	}

	mRunning = false;
	mThread->join();
	delete mThread;
}

static std::map<std::string, std::string> mimeTypes =
{
	{ "txt", "text/plain" },
	{ "html", "text/html" },
	{ "htm", "text/html" },
	{ "css", "text/css" },
	{ "jpeg", "image/jpg" },
	{ "jpg", "image/jpg" },
	{ "png", "image/png" },
	{ "gif", "image/gif" },
	{ "webp", "image/webp" },
	{ "svg", "image/svg+xml" },
	{ "ico", "image/x-icon" },
	{ "json", "application/json" },
	{ "pdf", "application/pdf" },
	{ "js", "application/javascript" },
	{ "wasm", "application/wasm" },
	{ "xml", "application/xml" },
	{ "xhtml", "application/xhtml+xml" }
};

std::string HttpServerThread::getMimeType(const std::string &path)
{
	auto ext = Utils::String::toLower(Utils::FileSystem::getExtension(path));
	if (ext[0] == '.')
		ext = ext.substr(1);

	auto it = mimeTypes.find(ext);
	if (it != mimeTypes.cend())
		return it->second;

	return "text/plain";
}

#ifdef _ENABLETURBORAMA

static bool isTextFile(const std::string& filename)
{
	// List of common text file extensions
	static std::vector<std::string> textExtensions = {
		".conf", ".cfg", ".txt", ".ini", ".xml", ".json",
		".sh", ".py", ".js", ".css", ".html", ".htm",
		".log", ".yaml", ".yml", ".properties", ".start", ".service",
		".rules", ".list", ".d", ".rc", ".config", ".toml"
	};

	std::string lower = Utils::String::toLower(filename);

	// Check if file has an extension
	bool hasExtension = lower.find('.') != std::string::npos;

	// If it has a text extension, it's a text file
	if (hasExtension) {
		for (auto& ext : textExtensions) {
			if (Utils::String::endsWith(lower, ext))
				return true;
		}
		// Has extension but not in our text list - probably binary
		return false;
	}

	// No extension - check if it's executable using stat
	struct stat st;
	if (stat(filename.c_str(), &st) == 0) {
		// If executable bit is set (user, group, or other), likely a binary
		if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
			return false; // Executable with no extension = binary
		}
	}

	// No extension and not executable - assume it's a text config file
	return true;
}
#endif

static bool isAllowed(const httplib::Request& req, httplib::Response& res)
{
	if (req.remote_addr != "127.0.0.1" && !Settings::getInstance()->getBool("PublicWebAccess"))
	{
		LOG(LogWarning) << "HttpServerThread : Access disabled for " + req.remote_addr;

		res.set_content("403 - Forbidden", "text/html");
		res.status = 403;
		return false;
	}

	return true;
}

void HttpServerThread::run()
{
	mHttpServer = new httplib::Server();

	mHttpServer->Get("/", [=](const httplib::Request & req, httplib::Response &res)
	{
		if (!isAllowed(req, res))
			return;

		res.set_redirect("/index.html");
	});

	mHttpServer->Get("/favicon.png", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		auto data = ResourceManager::getInstance()->getFileData(":/window_icon_256.png");
		if (data.ptr)
			res.set_content((char*)data.ptr.get(), data.length, "image/png");
	});

	mHttpServer->Get("/index.html", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		auto data = ResourceManager::getInstance()->getFileData(":/services/index.html");
		if (data.ptr)
		{
			res.set_content((char*)data.ptr.get(), data.length, "text/html");
			return;
		}

		res.set_content(
			"<!DOCTYPE html>\r\n"
		    "<html lang='fr'>\r\n"
			"<head>\r\n"
			"<title>EmulationStation services</title>\r\n"
			"<link rel=\"shortcut icon\" href=\"favicon.png\">\r\n"
			"</head>\r\n"
			"<body style='font-family: Open Sans, sans-serif;'>\r\n"
			"<p>EmulationStation services</p>\r\n"

			"<script type='text/javascript'>\r\n"

			"function quitES() {\r\n"
			"var xhr = new XMLHttpRequest();\r\n"
			"xhr.open('GET', '/quit');\r\n"
			"xhr.send(); }\r\n"

			"function reloadGamelists() {\r\n"
			"var xhr = new XMLHttpRequest();\r\n"
			"xhr.open('GET', '/reloadgames');\r\n"
			"xhr.send(); }\r\n"

			"function emuKill() {\r\n"
			"var xhr = new XMLHttpRequest();\r\n"
			"xhr.open('GET', '/emukill');\r\n"
			"xhr.send(); }\r\n"

			"</script>\r\n"

			"<img src='vid.jpg'/>\r\n"

			"<input type='button' value='Reload games' onClick='reloadGamelists()'/>\r\n"
			"<br/>"
			"<input type='button' value='Quit' onClick='quitES()'/>\r\n"
			"<br/>"
			"<input type='button' value='Kill emulator' onClick='emuKill()'/>\r\n"

			"</body>\r\n</html>", "text/html");
	});

	mHttpServer->Get("/quit", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

#if BATOCERA
		// http://127.0.0.1/quit?confirm=switchscreen
		if (req.has_param("confirm") && req.get_param_value("confirm") == "switchscreen")
		{
			Window* win = mWindow;
			mWindow->postToUiThread([win]() { win->pushGui(new GuiMsgBox(win, _("DO YOU WANT TO SWITCH THE SCREEN ?"), _("YES"), [] { Utils::Platform::quitES(); }, _("NO"), nullptr)); });
			return;
		}
#endif

		// http://127.0.0.1/quit?confirm=menu
		if (req.has_param("confirm") && req.get_param_value("confirm") == "menu")
		{
			GuiMenu::openQuitMenu_static(mWindow);
			return;
		}

		Utils::Platform::quitES();
	});

	mHttpServer->Get("/restart", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		Utils::Platform::quitES(Utils::Platform::QuitMode::REBOOT);
	});

	mHttpServer->Get("/emukill", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		ApiSystem::getInstance()->emuKill();
	});

	mHttpServer->Get("/caps", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		res.set_content(HttpApi::getCaps(), "application/json");
	});

	mHttpServer->Get("/systems", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		res.set_content(HttpApi::getSystemList(), "application/json");
	});

	mHttpServer->Get("/runningGame", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		std::string ret = HttpApi::getRunnningGameInfo();
		if (ret.empty())
		{
			res.set_content("{\"msg\":\"NO GAME RUNNING\"}", "application/json");
			res.status = 201;
		}
		else
			res.set_content(ret, "application/json");
	});

	mHttpServer->Get("/isIdle", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		bool idle =
			HttpApi::getRunnningGameInfo().empty() &&
			!ThreadedScraper::isRunning() &&
			!ContentInstaller::isRunning() &&
			!ThreadedHasher::isRunning() &&
			GuiUpdate::state != GuiUpdateState::UPDATER_RUNNING;

		if (idle)
		{
			res.set_content("[ true ]", "application/json");
			res.status = 200;
		}
		else
		{
			res.set_content("[ false ]", "application/json");
			res.status = 201;
		}
	});

	mHttpServer->Get(R"(/systems/(/?.*)/logo)", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		std::string systemName = req.matches[1];
		SystemData* system = SystemData::getSystem(systemName);
		if (system != nullptr)
		{
			auto theme = system->getTheme();
			if (theme != nullptr)
			{
				const ThemeData::ThemeElement* elem = theme->getElement("system", "logo", "image");
				if (elem && elem->has("path"))
				{
					std::string logo = elem->get<std::string>("path");
					auto data = ResourceManager::getInstance()->getFileData(logo);
					if (data.ptr)
					{
						res.set_content((char*)data.ptr.get(), data.length, getMimeType(logo).c_str());
						return;
					}
				}
			}
		}

		res.set_content("404 not found", "text/html");
		res.status = 404;
	});

	mHttpServer->Get(R"(/systems/(/?.*)/games)", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		std::string systemName = req.matches[1];
		SystemData* system = SystemData::getSystem(systemName);
		if (system != nullptr)
		{
			res.set_content(HttpApi::getSystemGames(system), "application/json");
			return;
		}

		res.set_content("404 system not found", "text/html");
		res.status = 404;
	});

	mHttpServer->Get(R"(/systems/(/?.*)/games/(/?.*)/media/(/?.*))", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		std::string systemName = req.matches[1];
		SystemData* system = SystemData::getSystem(systemName);
		if (system != nullptr)
		{
			std::string gameId = req.matches[2];
			auto game = HttpApi::findFileData(system, gameId);
			if (game != nullptr)
			{
				std::string metadataName = req.matches[3];

				if (game->getMetadata().getType(metadataName) == MD_PATH)
				{
					std::string path = game->getMetadata().get(metadataName);
					if (!path.empty())
					{
						auto data = ResourceManager::getInstance()->getFileData(path);
						if (data.ptr)
						{
							res.set_content((char*)data.ptr.get(), data.length, getMimeType(path).c_str());
							return;
						}

						return;
					}
				}
			}
		}

		res.set_content("404 media not found", "text/html");
		res.status = 404;
	});

	mHttpServer->Post(R"(/systems/(/?.*)/games/(/?.*)/media/(/?.*))", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		if (req.body.empty())
		{
			res.set_content("400 bad request - body is missing", "text/html");
			res.status = 400;
			return;
		}

		if (!req.has_header("Content-Type"))
		{
			res.set_content("400 missing content-type", "text/html");
			res.status = 400;
			return;
		}

		std::string contentType = req.get_header_value("Content-Type");

		std::string systemName = req.matches[1];
		SystemData* system = SystemData::getSystem(systemName);
		if (system != nullptr)
		{
			std::string gameId = req.matches[2];
			auto game = HttpApi::findFileData(system, gameId);
			if (game != nullptr)
			{
				std::string metadataName = req.matches[3];

				if (game->getMetadata().getType(metadataName) == MD_PATH)
				{
					if (HttpApi::ImportMedia(game, metadataName, contentType, req.body))
					{
						if (ViewController::hasInstance())
							mWindow->postToUiThread([game]() { ViewController::get()->onFileChanged(game, FileChangeType::FILE_METADATA_CHANGED); });

						return;
					}
				}
			}
		}

		res.set_content("404 media not found", "text/html");
		res.status = 404;
	});


	mHttpServer->Post(R"(/systems/(/?.*)/games/(/?.*))", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		if (req.body.empty())
		{
			res.set_content("400 bad request - body is missing", "text/html");
			res.status = 400;
			return;
		}

		std::string systemName = req.matches[1];
		SystemData* system = SystemData::getSystem(systemName);
		if (system != nullptr)
		{
			std::string gameId = req.matches[2];
			auto game = HttpApi::findFileData(system, gameId);
			if (game != nullptr)
			{
				if (HttpApi::ImportFromJson(game, req.body))
				{
					if (ViewController::hasInstance())
						mWindow->postToUiThread([game]() { ViewController::get()->onFileChanged(game, FileChangeType::FILE_METADATA_CHANGED); });

					return;
				}
			}
		}

		res.set_content("404 game not found", "text/html");
		res.status = 404;
	});


	mHttpServer->Get(R"(/systems/(/?.*)/games/(/?.*))", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		std::string systemName = req.matches[1];
		SystemData* system = SystemData::getSystem(systemName);
		if (system != nullptr)
		{
			std::string gameId = req.matches[2];
			auto game = HttpApi::findFileData(system, gameId);
			if (game != nullptr)
			{
				bool localpaths = req.has_param("localpaths") && req.get_param_value("localpaths") == "true";
				res.set_content(HttpApi::ToJson(game, localpaths), "application/json");
				return;
			}
		}

		res.set_content("404 game not found", "text/html");
		res.status = 404;
	});

	mHttpServer->Get(R"(/systems/(/?.*))", [](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		std::string systemName = req.matches[1];
		SystemData* system = SystemData::getSystem(systemName);
		if (system != nullptr)
		{
			bool localpaths = req.has_param("localpaths") && req.get_param_value("localpaths") == "true";
			res.set_content(HttpApi::ToJson(system, localpaths), "application/json");
			return;
		}

		res.set_content("404 not found", "text/html");
		res.status = 404;
	});


	mHttpServer->Get("/reloadgames", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		Window* w = mWindow;
		mWindow->postToUiThread([w]()
		{
			GuiMenu::updateGameLists(w, false);
		});
	});

	mHttpServer->Post("/messagebox", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		if (req.body.empty())
		{
			res.set_content("400 bad request - body is missing", "text/html");
			res.status = 400;
			return;
		}

		auto msg = req.body;
		Window* w = mWindow;
		mWindow->postToUiThread([msg, w]() { w->pushGui(new GuiMsgBox(w, msg)); });
	});

	mHttpServer->Post("/notify", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		if (req.body.empty())
		{
			res.set_content("400 bad request - body is missing", "text/html");
			res.status = 400;
			return;
		}

		mWindow->displayNotificationMessage(req.body);
	});

	mHttpServer->Post("/launch", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		if (req.body.empty())
		{
			res.set_content("400 bad request - body is missing", "text/html");
			res.status = 400;
			return;
		}

		auto path = Utils::FileSystem::getAbsolutePath(req.body);

		for (auto system : SystemData::sSystemVector)
		{
			if (system->isCollection() || !system->isGameSystem())
				continue;

			for (auto file : system->getRootFolder()->getFilesRecursive(GAME))
			{
				if (file->getFullPath() == path || file->getPath() == path)
				{
					mWindow->postToUiThread([file]() { ViewController::get()->launch(file); });
					return;
				}
			}
		}
	});

	mHttpServer->Post(R"(/addgames/(/?.*))", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		if (req.body.empty())
		{
			res.set_content("400 bad request - body is missing", "text/html");
			res.status = 400;
			return;
		}

		std::string systemName = req.matches[1];

		bool deleteSystem = false;

		SystemData* system = SystemData::getSystem(systemName);
		if (system == nullptr)
		{
			system = SystemData::loadSystem(systemName, false);
			if (system == nullptr)
			{
				res.set_content("404 System not found", "text/html");
				res.status = 404;
				return;
			}

			deleteSystem = true;
		}

		std::unordered_map<std::string, FileData*> fileMap;
		for (auto file : system->getRootFolder()->getFilesRecursive(GAME))
			fileMap[file->getPath()] = file;

		auto fileList = loadGamelistFile(req.body, system, fileMap, SIZE_MAX, false);
		if (fileList.size() == 0)
		{
			res.set_content("204 No game added / updated", "text/html");
			res.status = 204;

			if (deleteSystem)
				delete system;

			return;
		}

		for (auto file : fileList)
			file->getMetadata().setDirty();

		for (auto file : system->getRootFolder()->getFilesRecursive(GAME))
			if (fileMap.find(file->getPath()) != fileMap.cend())
				file->getMetadata().setDirty();

		updateGamelist(system);

		if (deleteSystem)
		{
			delete system;

			res.set_content("201 Game added. System not updated", "text/html");
			res.status = 201;

			Window* w = mWindow;
			mWindow->postToUiThread([w]() { GuiMenu::updateGameLists(w, false); });
		}
		else if (ViewController::hasInstance())
		{
			mWindow->postToUiThread([system]()
			{
				ViewController::get()->onFileChanged(system->getRootFolder(), FILE_METADATA_CHANGED); // Update root folder
			});
		}

		res.set_content("OK", "text/html");
	});

	mHttpServer->Post(R"(/removegames/(/?.*))", [this](const httplib::Request& req, httplib::Response& res)
	{
		if (!isAllowed(req, res))
			return;

		if (req.body.empty())
		{
			res.set_content("400 bad request - body is missing", "text/html");
			res.status = 400;
			return;
		}

		auto systemName = req.matches[1];

		SystemData* system = SystemData::getSystem(systemName);
		if (system == nullptr)
		{
			res.set_content("404 not found", "text/html");
			res.status = 404;
			return;
		}

		std::unordered_map<std::string, FileData*> fileMap;
		for (auto file : system->getRootFolder()->getFilesRecursive(GAME))
			fileMap[file->getPath()] = file;

		auto fileList = loadGamelistFile(req.body, system, fileMap, SIZE_MAX, false);
		if (fileList.size() == 0)
		{
			res.set_content("204 No game removed", "text/html");
			res.status = 204;
			return;
		}

		std::vector<SystemData*> systems;
		systems.push_back(system);

		for (auto file : fileList)
		{
			removeFromGamelistRecovery(file);

			auto filePath = file->getPath();
			if (Utils::FileSystem::exists(filePath))
				Utils::FileSystem::removeFile(filePath);

			for (auto sys : SystemData::sSystemVector)
			{
				if (!sys->isCollection())
					continue;

				auto copy = sys->getRootFolder()->FindByPath(filePath);
				if (copy != nullptr)
				{
					sys->getRootFolder()->removeFromVirtualFolders(file);
					systems.push_back(sys);
				}
			}

			system->getRootFolder()->removeFromVirtualFolders(file);
			// delete file; intentionnal mem leak
		}

		if (ViewController::hasInstance())
		{
			mWindow->postToUiThread([systems]()
			{
				for (auto changedSystem : systems)
					ViewController::get()->onFileChanged(changedSystem->getRootFolder(), FILE_REMOVED); // Update root folder
			});
		}

		res.set_content("OK", "text/html");
	});

	mHttpServer->Get(R"(/resources/(/?.*))", [](const httplib::Request& req, httplib::Response& res)  // (.*)
	{
		if (!isAllowed(req, res))
			return;

		std::string url = req.matches[1];
		auto data = ResourceManager::getInstance()->getFileData(":/" + url);
		if (data.ptr)
			res.set_content((char*)data.ptr.get(), data.length, getMimeType(url).c_str());
		else
		{
			res.set_content("404 not found", "text/html");
			res.status = 404;
			return;
		}
	});


#ifdef _ENABLETURBORAMA

// Config File APIs
// GET /config - List files/folders in config directory or subdirectory
// GET /config - List files/folders in config directory
mHttpServer->Get("/config", [](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string configPath = "/storage/.config";

	if (!Utils::FileSystem::exists(configPath))
	{
		res.set_content("404 config directory not found", "text/html");
		res.status = 404;
		return;
	}

	// List root directory contents
	auto dirContent = Utils::FileSystem::getDirContent(configPath, false);

	std::string json = "[";
	bool first = true;

	for (auto item : dirContent)
	{
		std::string filename = Utils::FileSystem::getFileName(item);
		bool isDir = Utils::FileSystem::isDirectory(item);

		// Skip binary files
		if (!isDir && !isTextFile(filename))
			continue;

		if (!first) json += ",";
		first = false;

		json += "{\"name\":\"" + filename + "\",";
		json += "\"path\":\"" + filename + "\",";
		json += "\"isDirectory\":" + std::string(isDir ? "true" : "false") + "}";
	}

	json += "]";

	res.set_content(json, "application/json");
});

mHttpServer->Get(R"(/config/(.+))", [](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string subpath = req.matches[1];

	// Sanitize path to prevent directory traversal
	if (subpath.find("..") != std::string::npos)
	{
		res.set_content("400 bad request - invalid path", "text/html");
		res.status = 400;
		return;
	}

	std::string configPath = "/storage/.config/" + subpath;

	if (!Utils::FileSystem::exists(configPath))
	{
		res.set_content("404 path not found", "text/html");
		res.status = 404;
		return;
	}

	// If it's a file, check if it's a text file before reading
	if (!Utils::FileSystem::isDirectory(configPath))
	{
		if (!isTextFile(configPath))
		{
			res.set_content("400 binary files not supported", "text/html");
			res.status = 400;
			return;
		}

		std::string content = Utils::FileSystem::readAllText(configPath);
		res.set_content(content, "text/plain");
		return;
	}

	// If it's a directory, list its contents
	auto dirContent = Utils::FileSystem::getDirContent(configPath, false);

	std::string json = "[";
	bool first = true;

	for (auto item : dirContent)
	{
		std::string filename = Utils::FileSystem::getFileName(item);
		bool isDir = Utils::FileSystem::isDirectory(item);

		// Skip binary files
		if (!isDir && !isTextFile(filename))
			continue;

		if (!first) json += ",";
		first = false;

		std::string relativePath = subpath + "/" + filename;

		json += "{\"name\":\"" + filename + "\",";
		json += "\"path\":\"" + relativePath + "\",";
		json += "\"isDirectory\":" + std::string(isDir ? "true" : "false") + "}";
	}

	json += "]";

	res.set_content(json, "application/json");
});

// POST /config/{path} - Write a config file
mHttpServer->Post(R"(/config/(.+))", [](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	if (req.body.empty())
	{
		res.set_content("400 bad request - body is missing", "text/html");
		res.status = 400;
		return;
	}

	std::string filepath = req.matches[1];

	// Sanitize path to prevent directory traversal
	if (filepath.find("..") != std::string::npos)
	{
		res.set_content("400 bad request - invalid path", "text/html");
		res.status = 400;
		return;
	}

	std::string configPath = "/storage/.config/" + filepath;

	// Create parent directories if they don't exist
	std::string parentDir = Utils::FileSystem::getParent(configPath);
	if (!Utils::FileSystem::exists(parentDir))
	{
		Utils::FileSystem::createDirectory(parentDir);
	}

	// Create backup before writing
	if (Utils::FileSystem::exists(configPath))
	{
		std::string backupPath = configPath + ".backup";
		Utils::FileSystem::copyFile(configPath, backupPath);
	}

	Utils::FileSystem::writeAllText(configPath, req.body);
	res.set_content("OK", "text/plain");
});

// DELETE /config/{path} - Delete a config file
mHttpServer->Delete(R"(/config/(.+))", [](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string filepath = req.matches[1];

	// Sanitize path to prevent directory traversal
	if (filepath.find("..") != std::string::npos)
	{
		res.set_content("400 bad request - invalid path", "text/html");
		res.status = 400;
		return;
	}

	std::string configPath = "/storage/.config/" + filepath;

	if (!Utils::FileSystem::exists(configPath))
	{
		res.set_content("404 file not found", "text/html");
		res.status = 404;
		return;
	}

	// Create backup before deleting
	std::string backupPath = configPath + ".deleted";
	Utils::FileSystem::copyFile(configPath, backupPath);

	if (Utils::FileSystem::removeFile(configPath))
	{
		res.set_content("OK", "text/plain");
	}
	else
	{
		res.set_content("500 failed to delete file", "text/html");
		res.status = 500;
	}
});


// System Management APIs
// POST /addsystem - Add or update a system
mHttpServer->Post("/addsystem", [this](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	if (req.body.empty())
	{
		res.set_content("400 bad request - body is missing", "text/html");
		res.status = 400;
		return;
	}

	// The body should contain a complete <system>...</system> XML block
	std::string systemXml = req.body;

	// Path to es_systems.cfg
	std::string systemsPath = "/storage/.emulationstation/es_systems.cfg";

	if (!Utils::FileSystem::exists(systemsPath))
	{
		res.set_content("404 es_systems.cfg not found", "text/html");
		res.status = 404;
		return;
	}

	// Create backup
	std::string backupPath = systemsPath + ".backup";
	Utils::FileSystem::copyFile(systemsPath, backupPath);

	// Read current systems file
	std::string content = Utils::FileSystem::readAllText(systemsPath);

	// Parse to extract system name from incoming XML
	size_t nameStart = systemXml.find("<name>");
	size_t nameEnd = systemXml.find("</name>");

	if (nameStart == std::string::npos || nameEnd == std::string::npos)
	{
		res.set_content("400 invalid system XML - missing <name>", "text/html");
		res.status = 400;
		return;
	}

	std::string systemName = systemXml.substr(nameStart + 6, nameEnd - nameStart - 6);

	// Check if system already exists
	std::string searchTag = "<name>" + systemName + "</name>";
	size_t existingPos = content.find(searchTag);

	if (existingPos != std::string::npos)
	{
		// Find the complete <system>...</system> block
		size_t systemStart = content.rfind("<system>", existingPos);
		size_t systemEnd = content.find("</system>", existingPos);

		if (systemStart != std::string::npos && systemEnd != std::string::npos)
		{
			// Replace existing system
			content.replace(systemStart, systemEnd - systemStart + 9, systemXml);
		}
	}
	else
	{
		// Add new system before </systemList>
		size_t insertPos = content.find("</systemList>");
		if (insertPos != std::string::npos)
		{
			content.insert(insertPos, "  " + systemXml + "\n");
		}
		else
		{
			res.set_content("400 malformed es_systems.cfg", "text/html");
			res.status = 400;
			return;
		}
	}

	// Write back
	Utils::FileSystem::writeAllText(systemsPath, content);

	// Reload systems
	Window* w = mWindow;
	mWindow->postToUiThread([w]() { GuiMenu::updateGameLists(w, false); });

	res.set_content("OK - System added/updated", "text/plain");
});

// POST /removesystem/{systemName} - Remove a system
mHttpServer->Post(R"(/removesystem/(.+))", [this](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string systemName = req.matches[1];

	// Path to es_systems.cfg
	std::string systemsPath = "/storage/.emulationstation/es_systems.cfg";

	if (!Utils::FileSystem::exists(systemsPath))
	{
		res.set_content("404 es_systems.cfg not found", "text/html");
		res.status = 404;
		return;
	}

	// Create backup
	std::string backupPath = systemsPath + ".backup";
	Utils::FileSystem::copyFile(systemsPath, backupPath);

	// Read current systems file
	std::string content = Utils::FileSystem::readAllText(systemsPath);

	// Find the system
	std::string searchTag = "<name>" + systemName + "</name>";
	size_t namePos = content.find(searchTag);

	if (namePos == std::string::npos)
	{
		res.set_content("404 system not found", "text/html");
		res.status = 404;
		return;
	}

	// Find the complete <system>...</system> block
	size_t systemStart = content.rfind("<system>", namePos);
	size_t systemEnd = content.find("</system>", namePos);

	if (systemStart == std::string::npos || systemEnd == std::string::npos)
	{
		res.set_content("400 malformed system entry", "text/html");
		res.status = 400;
		return;
	}

	// Remove the system block
	content.erase(systemStart, systemEnd - systemStart + 9);

	// Write back
	Utils::FileSystem::writeAllText(systemsPath, content);

	// Reload systems
	Window* w = mWindow;
	mWindow->postToUiThread([w]() { GuiMenu::updateGameLists(w, false); });

	res.set_content("OK - System removed", "text/plain");
});

// POST /system/{systemName}/addemulator - Add or update an emulator/core
mHttpServer->Post(R"(/system/(.+)/addemulator)", [this](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	if (req.body.empty())
	{
		res.set_content("400 bad request - body is missing", "text/html");
		res.status = 400;
		return;
	}

	std::string systemName = req.matches[1];
	std::string emulatorXml = req.body;

	// Path to es_systems.cfg
	std::string systemsPath = "/storage/.emulationstation/es_systems.cfg";

	if (!Utils::FileSystem::exists(systemsPath))
	{
		res.set_content("404 es_systems.cfg not found", "text/html");
		res.status = 404;
		return;
	}

	// Create backup
	std::string backupPath = systemsPath + ".backup";
	Utils::FileSystem::copyFile(systemsPath, backupPath);

	// Read current systems file
	std::string content = Utils::FileSystem::readAllText(systemsPath);

	// Find the system
	std::string searchTag = "<name>" + systemName + "</name>";
	size_t namePos = content.find(searchTag);

	if (namePos == std::string::npos)
	{
		res.set_content("404 system not found", "text/html");
		res.status = 404;
		return;
	}

	// Find the system block
	size_t systemStart = content.rfind("<system>", namePos);
	size_t systemEnd = content.find("</system>", namePos);

	if (systemStart == std::string::npos || systemEnd == std::string::npos)
	{
		res.set_content("400 malformed system entry", "text/html");
		res.status = 400;
		return;
	}

	// Extract emulator name from incoming XML
	size_t emulNameStart = emulatorXml.find("name=\"");
	if (emulNameStart == std::string::npos)
	{
		res.set_content("400 invalid emulator XML - missing name attribute", "text/html");
		res.status = 400;
		return;
	}

	emulNameStart += 6;
	size_t emulNameEnd = emulatorXml.find("\"", emulNameStart);
	std::string emulatorName = emulatorXml.substr(emulNameStart, emulNameEnd - emulNameStart);

	// Find <emulators> section
	size_t emulatorsStart = content.find("<emulators>", systemStart);
	size_t emulatorsEnd = content.find("</emulators>", systemStart);

	if (emulatorsStart == std::string::npos || emulatorsEnd == std::string::npos ||
	    emulatorsStart > systemEnd || emulatorsEnd > systemEnd)
	{
		// No emulators section, create one
		size_t insertPos = systemEnd;
		std::string emulatorsBlock = "\n    <emulators>\n      " + emulatorXml + "\n    </emulators>\n  ";
		content.insert(insertPos, emulatorsBlock);
	}
	else
	{
		// Check if emulator already exists
		std::string searchEmul = "name=\"" + emulatorName + "\"";
		size_t existingEmul = content.find(searchEmul, emulatorsStart);

		if (existingEmul != std::string::npos && existingEmul < emulatorsEnd)
		{
			// Replace existing emulator
			size_t emulStart = content.rfind("<emulator", existingEmul);
			size_t emulEnd = content.find("</emulator>", existingEmul);

			if (emulStart != std::string::npos && emulEnd != std::string::npos)
			{
				content.replace(emulStart, emulEnd - emulStart + 11, emulatorXml);
			}
		}
		else
		{
			// Add new emulator before </emulators>
			content.insert(emulatorsEnd, "      " + emulatorXml + "\n");
		}
	}

	// Write back
	Utils::FileSystem::writeAllText(systemsPath, content);

	// Reload systems
	Window* w = mWindow;
	mWindow->postToUiThread([w]() { GuiMenu::updateGameLists(w, false); });

	res.set_content("OK - Emulator added/updated", "text/plain");
});

// POST /system/{systemName}/removeemulator/{emulatorName} - Remove an emulator
mHttpServer->Post(R"(/system/(.+)/removeemulator/(.+))", [this](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string systemName = req.matches[1];
	std::string emulatorName = req.matches[2];

	// Path to es_systems.cfg
	std::string systemsPath = "/storage/.emulationstation/es_systems.cfg";

	if (!Utils::FileSystem::exists(systemsPath))
	{
		res.set_content("404 es_systems.cfg not found", "text/html");
		res.status = 404;
		return;
	}

	// Create backup
	std::string backupPath = systemsPath + ".backup";
	Utils::FileSystem::copyFile(systemsPath, backupPath);

	// Read current systems file
	std::string content = Utils::FileSystem::readAllText(systemsPath);

	// Find the system
	std::string searchTag = "<name>" + systemName + "</name>";
	size_t namePos = content.find(searchTag);

	if (namePos == std::string::npos)
	{
		res.set_content("404 system not found", "text/html");
		res.status = 404;
		return;
	}

	// Find the system block
	size_t systemStart = content.rfind("<system>", namePos);
	size_t systemEnd = content.find("</system>", namePos);

	// Find the emulator
	std::string searchEmul = "name=\"" + emulatorName + "\"";
	size_t emulPos = content.find(searchEmul, systemStart);

	if (emulPos == std::string::npos || emulPos > systemEnd)
	{
		res.set_content("404 emulator not found", "text/html");
		res.status = 404;
		return;
	}

	// Find the complete <emulator>...</emulator> block
	size_t emulStart = content.rfind("<emulator", emulPos);
	size_t emulEnd = content.find("</emulator>", emulPos);

	if (emulStart == std::string::npos || emulEnd == std::string::npos)
	{
		res.set_content("400 malformed emulator entry", "text/html");
		res.status = 400;
		return;
	}

	// Remove the emulator block
	content.erase(emulStart, emulEnd - emulStart + 11);

	// Write back
	Utils::FileSystem::writeAllText(systemsPath, content);

	// Reload systems
	Window* w = mWindow;
	mWindow->postToUiThread([w]() { GuiMenu::updateGameLists(w, false); });

	res.set_content("OK - Emulator removed", "text/plain");
});


// Scraping APIs
// POST /scrape/{systemName} - Scrape all games in a system OR a single game if body contains path
mHttpServer->Post(R"(/scrape/(.+))", [this](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string systemName = req.matches[1];

	SystemData* system = SystemData::getSystem(systemName);
	if (system == nullptr)
	{
		res.set_content("404 system not found", "text/html");
		res.status = 404;
		return;
	}

	// Check if scraper is already running
	if (ThreadedScraper::isRunning())
	{
		res.set_content("409 scraper already running", "text/html");
		res.status = 409;
		return;
	}

	Window* w = mWindow;

	// If body is provided, scrape single game by path
	if (!req.body.empty())
	{
		std::string gamePath = req.body;

		mWindow->postToUiThread([w, system, gamePath]()
		{
			// Find game by path
			FileData* foundGame = nullptr;
			auto games = system->getRootFolder()->getFilesRecursive(GAME);

			for (auto game : games)
			{
				if (game->getPath() == gamePath || game->getFullPath() == gamePath)
				{
					foundGame = game;
					break;
				}
			}

			if (foundGame != nullptr)
			{
				// Create scraper search queue with single game
				std::queue<ScraperSearchParams> searches;
				ScraperSearchParams search;
				search.game = foundGame;
				search.system = foundGame->getSystem();
				searches.push(search);

				// Start the threaded scraper
				ThreadedScraper::start(w, searches);
			}
		});

		res.set_content("OK - Game scraping started", "text/plain");
	}
	else
	{
		// No body = scrape entire system
		mWindow->postToUiThread([w, system]()
		{
			// Get all games in the system
			auto games = system->getRootFolder()->getFilesRecursive(GAME);

			if (games.size() > 0)
			{
				// Create scraper search queue
				std::queue<ScraperSearchParams> searches;
				for (auto game : games)
				{
					ScraperSearchParams search;
					search.game = game;
					search.system = game->getSystem();
					searches.push(search);
				}

				// Start the threaded scraper
				ThreadedScraper::start(w, searches);
			}
		});

		res.set_content("OK - System scraping started", "text/plain");
	}
});

// This adds file upload support for emulator cores
// POST /upload/core - Upload emulator core file
mHttpServer->Post("/upload/core", [this](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	if (!req.has_file("file"))
	{
		res.set_content("{\"error\":\"No file provided\"}", "application/json");
		res.status = 400;
		return;
	}

	const auto& file = req.get_file_value("file");

	// Check file size (100 MB limit)
	const size_t MAX_FILE_SIZE = 100 * 1024 * 1024;
	if (file.content.size() > MAX_FILE_SIZE)
	{
		res.set_content("{\"error\":\"File too large. Maximum size is 100 MB\"}", "application/json");
		res.status = 400;
		return;
	}

	// Sanitize filename - remove path components and dangerous characters
	std::string filename = file.filename;
	size_t lastSlash = filename.find_last_of("/\\");
	if (lastSlash != std::string::npos)
		filename = filename.substr(lastSlash + 1);

	// Remove null bytes and other dangerous characters
	filename.erase(std::remove(filename.begin(), filename.end(), '\0'), filename.end());

	if (filename.empty())
	{
		res.set_content("{\"error\":\"Invalid filename\"}", "application/json");
		res.status = 400;
		return;
	}

	// Create cores directory if it doesn't exist
	std::string coresDir = "/tmp/cores";
	if (!Utils::FileSystem::exists(coresDir))
	{
		Utils::FileSystem::createDirectory(coresDir);
	}

	// Full path to save file
	std::string filepath = coresDir + "/" + filename;

	// Write file
	std::ofstream outfile(filepath, std::ios::binary);
	if (!outfile)
	{
		res.set_content("{\"error\":\"Failed to create file\"}", "application/json");
		res.status = 500;
		return;
	}

	outfile.write(file.content.c_str(), file.content.size());
	outfile.close();

	// Set executable permissions (0755)
	chmod(filepath.c_str(), 0755);

	// Return success response
	std::string jsonResponse = "{\"success\":true,\"filename\":\"" + filename +
	                           "\",\"size\":" + std::to_string(file.content.size()) +
	                           ",\"path\":\"" + filepath + "\"}";
	res.set_content(jsonResponse, "application/json");
});

// GET /cores/list - List all uploaded cores
mHttpServer->Get("/cores/list", [](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string coresDir = "/tmp/cores";

	if (!Utils::FileSystem::exists(coresDir))
	{
		res.set_content("[]", "application/json");
		return;
	}

	auto dirContent = Utils::FileSystem::getDirContent(coresDir, false);

	std::string json = "[";
	bool first = true;

	for (const auto& item : dirContent)
	{
		if (Utils::FileSystem::isDirectory(item))
			continue;

		std::string filename = Utils::FileSystem::getFileName(item);

		// Only list .so files (RetroArch cores), not .info files
		if (filename.length() < 3 || filename.substr(filename.length() - 3) != ".so")
			continue;

		// Get file size and modified time
		struct stat st;
		if (stat(item.c_str(), &st) != 0)
			continue;

		if (!first) json += ",";
		first = false;

		json += "{\"name\":\"" + filename + "\",";
		json += "\"size\":" + std::to_string(st.st_size) + ",";
		json += "\"modified\":" + std::to_string(st.st_mtime) + "}";
	}

	json += "]";

	res.set_content(json, "application/json");
});

// DELETE /cores/{filename} - Delete a core file
mHttpServer->Delete(R"(/cores/(.+))", [](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string filename = req.matches[1];

	// Sanitize filename to prevent directory traversal
	if (filename.find("..") != std::string::npos ||
	    filename.find("/") != std::string::npos ||
	    filename.find("\\") != std::string::npos)
	{
		res.set_content("{\"error\":\"Invalid filename\"}", "application/json");
		res.status = 400;
		return;
	}

	std::string filepath = "/tmp/cores/" + filename;

	if (!Utils::FileSystem::exists(filepath))
	{
		res.set_content("{\"error\":\"File not found\"}", "application/json");
		res.status = 404;
		return;
	}

	if (Utils::FileSystem::isDirectory(filepath))
	{
		res.set_content("{\"error\":\"Not a file\"}", "application/json");
		res.status = 400;
		return;
	}

	// Delete the file
	if (remove(filepath.c_str()) != 0)
	{
		res.set_content("{\"error\":\"Failed to delete file\"}", "application/json");
		res.status = 500;
		return;
	}

	std::string jsonResponse = "{\"success\":true,\"message\":\"Deleted " + filename + "\"}";
	res.set_content(jsonResponse, "application/json");
});

// GET /cores/{filename} - Download a core file
mHttpServer->Get(R"(/cores/(.+))", [](const httplib::Request& req, httplib::Response& res)
{
	if (!isAllowed(req, res))
		return;

	std::string filename = req.matches[1];

	// Sanitize filename to prevent directory traversal
	if (filename.find("..") != std::string::npos ||
	    filename.find("/") != std::string::npos ||
	    filename.find("\\") != std::string::npos)
	{
		res.set_content("400 bad request - invalid filename", "text/html");
		res.status = 400;
		return;
	}

	std::string filepath = "/tmp/cores/" + filename;

	if (!Utils::FileSystem::exists(filepath))
	{
		res.set_content("404 file not found", "text/html");
		res.status = 404;
		return;
	}

	// Read file content
	std::ifstream file(filepath, std::ios::binary);
	if (!file)
	{
		res.set_content("500 failed to read file", "text/html");
		res.status = 500;
		return;
	}

	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	file.close();

	// Set appropriate headers for download
	res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
	res.set_content(content, "application/octet-stream");
});

#endif



	mHttpServer->Get(R"(/(/?.*))", [](const httplib::Request& req, httplib::Response& res)  // (.*)
	{
		if (!isAllowed(req, res))
			return;

		std::string url = req.matches[1];

		auto data = ResourceManager::getInstance()->getFileData(":/services/" + url);
		if (data.ptr)
			res.set_content((char*)data.ptr.get(), data.length, getMimeType(url).c_str());
		else
		{
			res.set_content("404 not found", "text/html");
			res.status = 404;
			return;
		}
	});

	try
	{
		std::string ip = "127.0.0.1";

		if (Settings::getInstance()->getBool("PublicWebAccess"))
			ip = "0.0.0.0";

		mHttpServer->listen(ip.c_str(), 1234);
	}
	catch (...)
	{

	}
}
