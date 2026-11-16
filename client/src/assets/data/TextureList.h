#pragma once

// Texturas que se cargan en un momento fijo: al arrancar, en el login, en la
// selección de personaje... (Content/interface/textures.toml).  Cada sección
// del archivo es un momento; sus texturas se cargan en el orden del archivo.

// Carga las texturas de la sección `group`.
void TextureList_Load(const char* group);
