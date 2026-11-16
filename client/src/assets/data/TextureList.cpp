#include "stdafx.h"
#include "assets/data/TextureList.h"
#include "assets/data/TomlLines.h"
#include "functions.h"

#include <cctype>
#include <map>
#include <string>
#include <vector>

extern "C" void DbgLogPublic(const char* msg);

namespace {

struct Texture {
    int         Slot = 0;
    std::string File;     // .tga -> OpenTGA, el resto -> OpenJPG
    int         Filter = 0;
    int         Wrap = 0;
};

void Warn(const std::string& message)
{
    DbgLogPublic(("TextureList: " + message).c_str());
}

// slot = ["archivo", "nearest" | "linear", "clamp" | "repeat"]
bool ParseTexture(const std::string& key, const std::string& value, Texture& texture)
{
    std::vector<std::string> items;
    std::string filter, wrap;
    if (!TomlLines::ParseInt(key, texture.Slot) || texture.Slot < 0
        || !TomlLines::SplitArray(value, items) || items.size() != 3
        || !TomlLines::ParseString(items[0], texture.File)
        || !TomlLines::ParseString(items[1], filter) || !TomlLines::ParseString(items[2], wrap))
        return false;
    if (filter == "nearest") texture.Filter = 0x2600;
    else if (filter == "linear") texture.Filter = 0x2601;
    else return false;
    if (wrap == "clamp") texture.Wrap = 0x2900;
    else if (wrap == "repeat") texture.Wrap = 0x2901;
    else return false;
    return true;
}

std::map<std::string, std::vector<Texture>> Read()
{
    std::map<std::string, std::vector<Texture>> groups;
    std::vector<TomlLines::Line> lines, bad;
    if (!TomlLines::Read("Content/interface/textures.toml", lines, &bad)) {
        Warn("no se encontró Content/interface/textures.toml; no se cargan las texturas de la interfaz");
        return groups;
    }
    for (const auto& line : bad)
        Warn("línea " + std::to_string(line.number) + " inválida: " + line.value);
    for (const auto& line : lines) {
        Texture texture;
        if (line.section.empty() || !ParseTexture(line.key, line.value, texture))
            Warn("línea " + std::to_string(line.number) + " inválida: " + line.key + " = " + line.value);
        else
            groups[line.section].push_back(texture);
    }
    return groups;
}

bool IsTga(const std::string& file)
{
    const auto dot = file.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = file.substr(dot);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".tga";
}

} // namespace

void TextureList_Load(const char* group)
{
    static const std::map<std::string, std::vector<Texture>> groups = Read();
    const auto found = groups.find(group);
    if (found == groups.end()) {
        Warn(std::string("sección [") + group + "] vacía o inexistente");
        return;
    }
    for (const Texture& texture : found->second) {
        if (IsTga(texture.File))
            OpenTGA(texture.File.c_str(), texture.Slot, texture.Filter, texture.Wrap, 0, '\x01');
        else
            OpenJPG(texture.File.c_str(), texture.Slot, texture.Filter, texture.Wrap, 0, '\x01');
    }
}
