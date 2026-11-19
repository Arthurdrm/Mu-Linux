// Opciones de items del catálogo (F3:FD): ItemOptionTable_Find tiene que dar lo
// mismo que CItemOption::GetItemOption del server.  Las filas son las del
// ItemOption.txt que trae el server.
#include "game/item/ItemOptionTable.h"
#include "net/Protocol/GameServerProtocol.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FALLA linea %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

static int Item(int section, int index) { return section * 32 + index; }

static Proto::CATALOG_ITEM_OPTION Row(int slot, int option, int value, int itemMin, int itemMax,
                                      int skill, int luck, int add, int exc)
{
    Proto::CATALOG_ITEM_OPTION row = {};
    row.Index = (std::uint8_t)slot;
    row.Option = (std::uint8_t)option;
    row.Value = (std::uint8_t)value;
    row.SkillOption = (std::uint8_t)(skill < 0 ? 0xFF : skill);
    row.LuckOption = (std::uint8_t)(luck < 0 ? 0xFF : luck);
    row.AddOption = (std::uint8_t)(add < 0 ? 0xFF : add);
    row.ExceOption = (std::uint8_t)(exc < 0 ? 0xFF : exc);
    row.ItemMin = (std::uint16_t)(itemMin < 0 ? 0xFFFF : itemMin);
    row.ItemMax = (std::uint16_t)(itemMax < 0 ? 0xFFFF : itemMax);
    return row;
}

static bool Load(const std::vector<Proto::CATALOG_ITEM_OPTION>& rows)
{
    return ItemOptionTable_SetCatalog(rows.empty() ? nullptr : (const std::uint8_t*)rows.data(),
                                      (int)sizeof(Proto::CATALOG_ITEM_OPTION), (int)rows.size());
}

int main()
{
    std::uint8_t opt = 0, val = 0;

    // Sin tabla: ItemConvert usa las reglas del 0.97k.
    ItemOptionTable_ResetSession();
    CHECK(!ItemOptionTable_Loaded());
    CHECK(!ItemOptionTable_Find(ITEM_OPTION_SLOT_LUCK, Item(0, 19), 0, 1, 0, 0, &opt, &val));

    const std::vector<Proto::CATALOG_ITEM_OPTION> rows = {
        Row(0, 22, 0, Item(0, 19), Item(0, 19), 1, -1, -1, -1),          // Archangel: Cyclone
        Row(1, 64, 7, Item(0, 0), Item(0, 31), -1, 1, -1, -1),           // suerte de espadas (+7%)
        Row(2, 60, 4, Item(0, 0), Item(0, 31), -1, -1, 1, -1),           // adicional de espadas
        Row(2, 60, 4, Item(12, 3), Item(12, 3), -1, -1, 1, -1),          // ala de elfa 2: daño...
        Row(2, 65, 1, Item(12, 3), Item(12, 3), -1, -1, 1, 32),          // ...o vida si trae el bit 32
        Row(4, 73, 20, Item(0, 0), Item(0, 31), -1, -1, -1, 2),          // excelente 2: nivel/20
    };
    CHECK(Load(rows));
    CHECK(ItemOptionTable_Loaded());

    // Suerte: el valor es el del server, no un 5 fijo.
    CHECK(ItemOptionTable_Find(ITEM_OPTION_SLOT_LUCK, Item(0, 19), 0, 1, 0, 0, &opt, &val));
    CHECK(opt == 64 && val == 7);
    // Sin suerte en el item no hay opción.
    CHECK(!ItemOptionTable_Find(ITEM_OPTION_SLOT_LUCK, Item(0, 19), 0, 0, 0, 0, &opt, &val));
    // Fuera del rango de la fila (un escudo) tampoco.
    CHECK(!ItemOptionTable_Find(ITEM_OPTION_SLOT_LUCK, Item(6, 0), 0, 1, 0, 0, &opt, &val));

    // Adicional: exige al menos nivel 1; el valor por nivel lo multiplica ItemConvert.
    CHECK(!ItemOptionTable_Find(ITEM_OPTION_SLOT_ADDITIONAL, Item(0, 19), 0, 0, 0, 0, &opt, &val));
    CHECK(ItemOptionTable_Find(ITEM_OPTION_SLOT_ADDITIONAL, Item(0, 19), 0, 0, 3, 0, &opt, &val));
    CHECK(opt == 60 && val == 4);

    // Gana la última fila que coincide: el bit 32 cambia la opción del ala.
    CHECK(ItemOptionTable_Find(ITEM_OPTION_SLOT_ADDITIONAL, Item(12, 3), 0, 0, 1, 0, &opt, &val));
    CHECK(opt == 60 && val == 4);
    CHECK(ItemOptionTable_Find(ITEM_OPTION_SLOT_ADDITIONAL, Item(12, 3), 0, 0, 1, 32, &opt, &val));
    CHECK(opt == 65 && val == 1);

    // Excelente: la máscara se compara contra los bits del item.
    CHECK(!ItemOptionTable_Find(4, Item(0, 19), 0, 0, 0, 1, &opt, &val));
    CHECK(ItemOptionTable_Find(4, Item(0, 19), 0, 0, 0, 2 | 1, &opt, &val));
    CHECK(opt == 73 && val == 20);

    // Skill: exige el bit de skill del item.
    CHECK(!ItemOptionTable_Find(ITEM_OPTION_SLOT_SKILL, Item(0, 19), 0, 0, 0, 0, &opt, &val));
    CHECK(ItemOptionTable_Find(ITEM_OPTION_SLOT_SKILL, Item(0, 19), 1, 0, 0, 0, &opt, &val));
    CHECK(opt == 22);

    // Un registro fuera de rango no se aplica y queda la tabla anterior.
    std::vector<Proto::CATALOG_ITEM_OPTION> bad = rows;
    bad.push_back(Row(1, 64, 9, 9000, 9000, -1, 1, -1, -1));
    CHECK(!Load(bad));
    CHECK(ItemOptionTable_Find(ITEM_OPTION_SLOT_LUCK, Item(0, 19), 0, 1, 0, 0, &opt, &val));
    CHECK(val == 7);
    // Tamaño de registro distinto (otra versión del protocolo): idem.
    CHECK(!ItemOptionTable_SetCatalog((const std::uint8_t*)rows.data(), 8, (int)rows.size()));

    // Una tabla vacía cuenta como cargada: el server no da opciones.
    CHECK(Load({}));
    CHECK(ItemOptionTable_Loaded());
    CHECK(!ItemOptionTable_Find(ITEM_OPTION_SLOT_LUCK, Item(0, 19), 0, 1, 0, 0, &opt, &val));

    if (failures) return 1;
    std::printf("item_option_table: OK\n");
    return 0;
}
