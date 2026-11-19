// ItemOptionTable.cpp — opciones de los items que manda el server (F3:FD).

#include "game/item/ItemOptionTable.h"
#include "game/item/ItemDefines.h"
#include "net/Protocol/GameServerProtocol.h"
#include <cstring>
#include <vector>

namespace {

using ItemOptionRow = Proto::CATALOG_ITEM_OPTION;

std::vector<ItemOptionRow> s_Rows;
bool s_Loaded = false;

const std::uint8_t kAny = 0xFF;
const std::uint16_t kNoLimit = 0xFFFF;

bool ValidRow(const ItemOptionRow& row)
{
    return (row.ItemMin == kNoLimit || row.ItemMin < ITEM_MAX_EX) &&
           (row.ItemMax == kNoLimit || row.ItemMax < ITEM_MAX_EX);
}

} // namespace

void ItemOptionTable_ResetSession()
{
    s_Rows.clear();
    s_Loaded = false;
}

bool ItemOptionTable_SetCatalog(const std::uint8_t* records, int recordSize, int count)
{
    if (count < 0 || recordSize != (int)sizeof(ItemOptionRow) || (count > 0 && !records)) return false;
    std::vector<ItemOptionRow> rows((size_t)count);
    for (int n = 0; n < count; ++n) {
        memcpy(&rows[n], records + sizeof(ItemOptionRow) * n, sizeof(ItemOptionRow));
        if (!ValidRow(rows[n])) return false;
    }
    s_Rows.swap(rows);
    s_Loaded = true;
    return true;
}

bool ItemOptionTable_Loaded()
{
    return s_Loaded;
}

bool ItemOptionTable_Find(int slot, int behavior, int skill, int luck, int add, int exc,
                          std::uint8_t* option, std::uint8_t* value)
{
    bool found = false;
    for (const ItemOptionRow& row : s_Rows) {
        if (row.Index != slot) continue;
        if (row.ItemMin != kNoLimit && row.ItemMin > behavior) continue;
        if (row.ItemMax != kNoLimit && row.ItemMax < behavior) continue;
        if (row.SkillOption != kAny && row.SkillOption > skill) continue;
        if (row.LuckOption != kAny && row.LuckOption > luck) continue;
        if (row.AddOption != kAny && row.AddOption > add) continue;
        if (row.ExceOption != kAny && (exc & row.ExceOption) == 0) continue;
        found = true;
        *option = row.Option;
        *value = row.Value;
    }
    return found;
}
