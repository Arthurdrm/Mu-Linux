#pragma once

#include <cstdint>

// Opciones de los items (skill, suerte, adicional y excelentes) que manda el
// server en el catálogo (F3:FD, su ItemOption.txt).  Con la tabla cargada,
// ItemConvert arma Special/SpecialValue con la misma resolución que
// CItem::Convert del server; sin ella usa las reglas del 0.97k.

enum ItemOptionSlot
{
    ITEM_OPTION_SLOT_SKILL = 0,
    ITEM_OPTION_SLOT_LUCK = 1,
    ITEM_OPTION_SLOT_ADDITIONAL = 2,
    ITEM_OPTION_SLOT_EXCELLENT1 = 3,
    ITEM_OPTION_SLOT_EXCELLENT6 = 8,
};

// La tabla pertenece a la conexión con el GameServer.
void ItemOptionTable_ResetSession();

// Sección F3:FD.  Un registro inválido no se aplica y queda la tabla anterior.
bool ItemOptionTable_SetCatalog(const std::uint8_t* records, int recordSize, int count);

// true si el server mandó la tabla (aunque venga vacía).
bool ItemOptionTable_Loaded();

// CItemOption::GetItemOption del server: la última fila del slot que coincide
// gana.  `behavior` es ItemBehaviorType del item; skill/luck/add/exc son las
// opciones que trae el item.
bool ItemOptionTable_Find(int slot, int behavior, int skill, int luck, int add, int exc,
                          std::uint8_t* option, std::uint8_t* value);

// Valor de la tabla para la opción `optionCode` de ese item (recorre los slots
// como ItemConvert).  false si la tabla no la da.
bool ItemOptionTable_FindValue(int optionCode, int behavior, int skill, int luck, int add, int exc,
                               std::uint8_t* value);
