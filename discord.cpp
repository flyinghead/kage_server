/*
	Kage game server.
    Copyright 2025 Flyinghead <flyinghead.github@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "discord.h"
#include <curl/curl.h>
#include "json.hpp"
#include <thread>
#include <atomic>
#include <chrono>

static std::string DiscordWebhook;
static std::atomic_int threadCount;

using namespace nlohmann;

struct {
	const char *name;
	const char *url;
} Games[] = {
	{ "Bomberman Online",	"https://dcnet.flyca.st/gamepic/bomberman.jpg" },
	{ "Outtrigger",			"https://dcnet.flyca.st/gamepic/outtrigger.jpg" },
	{ "Propeller Arena",	"https://dcnet.flyca.st/gamepic/propeller.jpg" },
};

class Notif
{
public:
	Notif(Game gameId) : gameId(gameId) {}

	std::string to_json() const
	{
		json embeds;
		embeds.push_back({
			{ "author",
				{
					{ "name", Games[(int)gameId].name },
					{ "icon_url", Games[(int)gameId].url }
				},
			},
			{ "title", embed.title },
			{ "description", embed.text },
			{ "color", 9118205 },
		});

		json j = {
			{ "content", content },
			{ "embeds", embeds },
		};
		return j.dump(4);
	}

	Game gameId;
	std::string content;
	struct {
		std::string title;
		std::string text;
	} embed;
};

static void postWebhook(Notif notif)
{
	CURL *curl = curl_easy_init();
	if (curl == nullptr)
	{
		fprintf(stderr, "Can't create curl handle\n");
		threadCount.fetch_sub(1);
		return;
	}
	CURLcode res;
	curl_easy_setopt(curl, CURLOPT_URL, DiscordWebhook.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "DCNet-DiscordWebhook");
	curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	std::string json = notif.to_json();
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		fprintf(stderr, "curl error: %d\n", res);
	}
	else
	{
		long code;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		if (code < 200 || code >= 300)
			fprintf(stderr, "Discord error: %ld\n", code);
	}
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	threadCount.fetch_sub(1);
}

static void discordNotif(const Notif& notif)
{
	if (DiscordWebhook.empty())
		return;
	if (threadCount.fetch_add(1) >= 5) {
		threadCount.fetch_sub(1);
		fprintf(stderr, "Discord max thread count reached\n");
		return;
	}
	std::thread thread(postWebhook, notif);
	thread.detach();
}

void setDiscordWebhook(const std::string& url) {
	DiscordWebhook = url;
}

void discordLobbyJoined(Game gameId, const std::string& username, const std::vector<std::string>& playerList)
{
	using the_clock = std::chrono::steady_clock;
	static the_clock::time_point last_notif;
	the_clock::time_point now = the_clock::now();
	if (last_notif != the_clock::time_point() && now - last_notif < std::chrono::minutes(5))
		return;
	last_notif = now;
	Notif notif(gameId);
	notif.content = "Player **" + username + "** joined the lobby";
	notif.embed.title = "Lobby Players";
	for (const auto& player : playerList)
		notif.embed.text += player + "\n";
	discordNotif(notif);
}

void discordGameCreated(Game gameId, const std::string& username, const std::string& gameName, const std::vector<std::string>& playerList)
{
	Notif notif(gameId);
	notif.content = "Player **" + username + "** created game room **" + gameName + "**";
	notif.embed.title = "Lobby Players";
	for (const auto& player : playerList)
		notif.embed.text += player + "\n";

	discordNotif(notif);
}

