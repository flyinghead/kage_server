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
#include <dcserver/discord.hpp>
#include <chrono>

const char *getDCNetGameId(Game game)
{
	switch (game)
	{
	case Game::Bomberman: return "bomberman";
	case Game::Outtrigger: return "outtrigger";
	case Game::PropellerA: return "propeller";
	default: return nullptr;
	}
}

void discordLobbyJoined(Game gameId, const std::string& username, const std::vector<std::string>& playerList)
{
	using the_clock = std::chrono::steady_clock;
	static the_clock::time_point last_notif;
	the_clock::time_point now = the_clock::now();
	if (last_notif != the_clock::time_point() && now - last_notif < std::chrono::minutes(5))
		return;
	last_notif = now;
	Notif notif;
	notif.content = "Player **" + discordEscape(username) + "** joined the lobby";
	notif.embed.title = "Lobby Players";
	for (const auto& player : playerList)
		notif.embed.text += discordEscape(player) + "\n";
	discordNotif(getDCNetGameId(gameId), notif);
}

void discordGameCreated(Game gameId, const std::string& username, const std::string& gameName, const std::vector<std::string>& playerList)
{
	Notif notif;
	notif.content = "Player **" + discordEscape(username) + "** created game room **" + discordEscape(gameName) + "**";
	notif.embed.title = "Lobby Players";
	for (const auto& player : playerList)
		notif.embed.text += discordEscape(player) + "\n";

	discordNotif(getDCNetGameId(gameId), notif);
}

