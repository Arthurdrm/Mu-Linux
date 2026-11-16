#!/usr/bin/env python3
"""Listas de carga (AssetList): texturas de la interfaz y modelos de items.

Compila AssetList.cpp con OpenJPG / OpenTGA / AccessModel /
AccessModelWithTextures / OpenTexture registrando cada llamada y carga cada
sección que pide el código:
  - Content/interface/textures.toml, una sección por momento (login, intro...);
  - Content/item/models.toml, [models] y [textures].
Comprueba que el lector no rechace líneas, que cada sección se use y que cada
textura y cada modelo exista en Content.

Con --reference-ref REV compila además el código de carga tal como estaba en
REV (las llamadas sueltas de cada función) y exige la misma secuencia, salvo:
  - la carga repetida de Interface/level_02.jpg en el slot 289;
  - en items, un slot que se carga dos veces: vale la primera carga, y la de
    AccessModelWithTextures reemplaza a la de AccessModel (ya trae las
    texturas, así que la OpenTexture que la seguía tampoco va);
  - la rama de .smd de Data2 y los DAT_0055a7c4 = 1, que no cambiaban nada.
REV tiene que ser anterior a las dos tandas (T10-C interfaz e items).
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from MonsterSpawnPointer import build, item_constants

ROOT = Path(__file__).resolve().parents[1]

# (archivo, función, toml, sección)
GROUPS = [("src/scenes/Scene_AssetLoad.cpp", "Scene_LoadGameAssets", "interface/textures.toml", "basic"),
          ("src/assets/model/InterfaceTextures.cpp", "UI_LoadInterfaceTextures", "interface/textures.toml", "images"),
          ("src/scenes/Scene_Intro.cpp", "Scene_Intro(HDC", "interface/textures.toml", "intro"),
          ("src/scenes/Scene_Loading.cpp", "Scene_Loading(HDC", "interface/textures.toml", "loading"),
          ("src/scenes/Scene_Resources.cpp", "Scene_LoadAccountResources", "interface/textures.toml", "account"),
          ("src/scenes/Scene_Resources.cpp", "Scene_LoadCharSelectResources", "interface/textures.toml",
           "char_select"),
          ("src/assets/model/ItemModels.cpp", "Model_LoadItemMeshes", "item/models.toml", "models"),
          ("src/assets/model/ItemTextures.cpp", "Model_AssignItemTexturePrefixes", "item/models.toml", "textures")]

STUBS = r'''
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
typedef unsigned char BYTE;
static const char* g_group = "";
static std::string N(const char* p) { std::string s(p); for (char& c : s) if (c == '\\') c = '/'; return s; }
static int Image(const char* k, const char* p, int id, int f, int w, int a, char b) {
    std::printf("%s|%s|%s|%d|%d|%d|%d|%d\n", g_group, k, N(p).c_str(), id, f, w, a, (int)b);
    return 0;
}
static int OpenJPG(const char* p, int id, int f, int w, int a, char b) { return Image("J", p, id, f, w, a, b); }
static int OpenTGA(const char* p, int id, int f, int w, int a, char b) { return Image("G", p, id, f, w, a, b); }
static void AccessModel(int id, const char* d, const char* f, int i)
{ std::printf("%s|M|%d|%s|%s|%d\n", g_group, id, N(d).c_str(), f, i); }
static void AccessModelWithTextures(int id, char* d, char* f, int i)
{ std::printf("%s|W|%d|%s|%s|%d\n", g_group, id, N(d).c_str(), f, i); }
static void OpenTexture(int id, const void* d, int fl, char l)
{ std::printf("%s|T|%d|%s|%d|%d\n", g_group, id, N((const char*)d).c_str(), fl, (int)l); }
static void OpenModels(int, const char*, int) { std::printf("%s|SMD\n", g_group); }
static void SetMaxTextures(int) {}
static char DAT_0055a7c4 = 1;
static void DbgLogPublic(const char* m) { std::printf("WARN|%s\n", m); }
'''


def body(text, name):
    i = text.rindex(name)
    k = text.index("{", i)
    depth = 0
    for j in range(k, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[k:j + 1]


def header(path):
    return (ROOT / "src" / path).read_text(encoding="utf-8").replace("#pragma once", "")


def main_for(calls):
    return "int main() {\n" + "".join(f'    g_group = "{g}"; {call}\n' for g, call in calls) + "}\n"


def current_harness():
    table = (ROOT / "src/assets/data/AssetList.cpp").read_text(encoding="utf-8")
    for include in ('#include "stdafx.h"', '#include "functions.h"'):
        table = table.replace(include, "")
    table = table.replace('#include "assets/data/AssetList.h"', header("assets/data/AssetList.h"))
    table = table.replace('#include "assets/data/TomlLines.h"', header("assets/data/TomlLines.h"))
    table = table.replace('extern "C" void DbgLogPublic(const char* msg);', "")
    return STUBS + table + main_for([(s, f'AssetList_Load("{t}", "{s}");') for _, _, t, s in GROUPS])


def reference_harness(revision):
    def show(path):
        return subprocess.check_output(["git", "show", f"{revision}:client/{path}"], cwd=ROOT).decode("utf-8")
    functions = []
    for path, name, _, section in GROUPS:
        code = body(show(path), name)
        if section in ("models", "textures"):
            if "    // NoneBlendMesh flags." in code:
                code = code[:code.index("    // NoneBlendMesh flags.")] + "}"
        else:
            code = "{\n" + "\n".join(l.strip() for l in code.split("\n")
                                    if re.match(r"\s*(OpenJPG|OpenTGA)\(", l)) + "\n}"
        functions.append(f"static void Old_{section}() {code}\n")
    defines = item_constants(show("src/game/item/ItemDefines.h"))
    return STUBS + defines + "".join(functions) + main_for([(s, f"Old_{s}();") for _, _, _, s in GROUPS])


def reduce_reference(rows):
    """Las diferencias documentadas: duplicados, la rama .smd y AccessModel reemplazada."""
    rows = [r for r in rows if r.split("|")[1] != "SMD"]
    rows.remove("images|J|Interface/level_02.jpg|289|9728|10496|0|1")
    with_textures = {r.split("|")[2] for r in rows if r.split("|")[1] == "W"}
    out, models, textures = [], set(), set()
    for r in rows:
        group, kind, slot = r.split("|")[:3]
        if kind == "M" and (slot in with_textures or slot in models):
            continue
        if kind in "MW":
            models.add(slot)
        if kind == "T":
            if (group == "models" and slot in with_textures) or (group, slot) in textures:
                continue
            textures.add((group, slot))
        out.append(r)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--architecture", choices=("Win32", "x64"), default="x64")
    parser.add_argument("--generator")
    parser.add_argument("--reference-ref")
    args = parser.parse_args()
    content = ROOT / "bin/Client/Content"
    for toml in sorted({t for _, _, t, _ in GROUPS}):
        sections = set(re.findall(r"^\[(\w+)\]", (content / toml).read_text(encoding="utf-8"), re.M))
        used = {s for _, _, t, s in GROUPS if t == toml}
        if sections != used:
            raise RuntimeError(f"{toml}: secciones sin usar o faltantes: {sorted(sections ^ used)}")
    for path, name, toml, section in GROUPS:
        if f'"{section}")' not in body((ROOT / path).read_text(encoding="utf-8"), name):
            raise RuntimeError(f"{name} no carga [{section}] de {toml}")
    with tempfile.TemporaryDirectory(prefix="mu-asset-list-") as temp:
        data = Path(temp) / "data"
        for toml in {t for _, _, t, _ in GROUPS}:
            (data / "Content" / toml).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(content / toml, data / "Content" / toml)
        rows = build(Path(temp) / "current", current_harness(), args, cwd=data)
        warnings = [r for r in rows if r.startswith("WARN|")]
        if warnings:
            raise RuntimeError("el lector rechazó líneas:\n" + "\n".join(warnings))
        # un archivo existe si está en Content o si alias.toml lo redirige a otro
        files = {p.name.lower() for p in content.rglob("*") if p.is_file()}
        files |= {k.rsplit("/", 1)[-1].lower() for k in
                  re.findall(r'^"([^"]+)"\s*=', (content / "alias.toml").read_text(encoding="utf-8"), re.M)}
        missing = []
        for r in rows:
            f = r.split("|")
            if f[1] in "JG":
                name = re.sub(r"\.jpg$", ".ozj", re.sub(r"\.tga$", ".ozt", f[2].lower())).rsplit("/", 1)[-1]
            elif f[1] in "MW":
                index = int(f[5])
                name = (f"{f[4]}0{index}.bmd" if index < 10 else f"{f[4]}{index}.bmd").lower()
            else:
                continue
            if name not in files:
                missing.append(name)
        if missing:
            raise RuntimeError(f"archivos que no están: {sorted(set(missing))}")
        count = {k: sum(1 for r in rows if r.split("|")[1] in k) for k in ("JG", "MW", "T")}
        print(f"AssetList: {count['JG']} texturas de interfaz, {count['MW']} modelos de items y "
              f"{count['T']} juegos de texturas, presentes")
        if args.reference_ref:
            reference = reduce_reference(build(Path(temp) / "reference", reference_harness(args.reference_ref), args))
            if reference != rows:
                for a, b in zip(reference, rows):
                    if a != b:
                        raise RuntimeError(f"difiere:\n  código {a}\n  tabla  {b}")
                raise RuntimeError(f"cantidad distinta: código {len(reference)} / tabla {len(rows)}")
            print(f"Equivalencia con {args.reference_ref}: OK ({len(rows)} llamadas)")


if __name__ == "__main__":
    main()
