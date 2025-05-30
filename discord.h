/*
	Kage game server.
    Copyright (C) 2025 Flyinghead <flyinghead.github@gmail.com>

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
#pragma once
#include "kage.h"
#include <string>
#include <vector>

void setDiscordWebhook(const std::string& url);
void discordLobbyJoined(Game gameId, const std::string& username, const std::vector<std::string>& playerList);
void discordGameCreated(Game gameId, const std::string& username, const std::string& gameName, const std::vector<std::string>& playerList);
