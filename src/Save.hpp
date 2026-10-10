#pragma once
#include "World.hpp"
#include <string>

std::string SavePath();
bool SaveExists();
bool SaveGame(const World& w);
bool LoadGame(World& w);
void DeleteSave();
