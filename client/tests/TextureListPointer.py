#!/usr/bin/env python3
"""Texturas de interfaz (Content/interface/textures.toml) con el TextureList real.

Compila TextureList.cpp con OpenJPG / OpenTGA registrando cada llamada y carga
cada sección que el código pide con TextureList_Load.  Comprueba que el lector
no rechace líneas, que todas las secciones del archivo se usen y que cada
textura exista en Content (con la extensión que lee el loader: .jpg -> .OZJ,
.tga -> .OZT).

Con --reference-ref REV compila además las llamadas OpenJPG/OpenTGA de las
funciones tal como estaban en REV y exige la misma secuencia, salvo la carga
repetida de Interface/level_02.jpg en el slot 289, que la tabla no repite.
"""
import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from MonsterSpawnPointer import build

ROOT = Path(__file__).resolve().parents[1]

# (archivo, función, sección)
GROUPS = [("src/scenes/Scene_AssetLoad.cpp", "Scene_LoadGameAssets", "basic"),
          ("src/assets/model/InterfaceTextures.cpp", "UI_LoadInterfaceTextures", "images"),
          ("src/scenes/Scene_Intro.cpp", "Scene_Intro(HDC", "intro"),
          ("src/scenes/Scene_Loading.cpp", "Scene_Loading(HDC", "loading"),
          ("src/scenes/Scene_Resources.cpp", "Scene_LoadAccountResources", "account"),
          ("src/scenes/Scene_Resources.cpp", "Scene_LoadCharSelectResources", "char_select")]

STUBS = r'''
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
static const char* g_group = "";
static std::string Norm(const char* p) { std::string s(p); for (char& c : s) if (c == '\\') c = '/'; return s; }
static int Log(const char* k, const char* p, int id, int f, int w, int a, char b) {
    std::printf("%s|%s|%s|%d|%d|%d|%d|%d\n", g_group, k, Norm(p).c_str(), id, f, w, a, (int)b);
    return 0;
}
static int OpenJPG(const char* p, int id, int f, int w, int a, char b) { return Log("J", p, id, f, w, a, b); }
static int OpenTGA(const char* p, int id, int f, int w, int a, char b) { return Log("G", p, id, f, w, a, b); }
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
    table = (ROOT / "src/assets/data/TextureList.cpp").read_text(encoding="utf-8")
    for include in ('#include "stdafx.h"', '#include "functions.h"'):
        table = table.replace(include, "")
    table = table.replace('#include "assets/data/TextureList.h"', header("assets/data/TextureList.h"))
    table = table.replace('#include "assets/data/TomlLines.h"', header("assets/data/TomlLines.h"))
    table = table.replace('extern "C" void DbgLogPublic(const char* msg);', "")
    return STUBS + table + main_for([(g, f'TextureList_Load("{g}");') for _, _, g in GROUPS])


def reference_harness(revision):
    functions = []
    for path, name, group in GROUPS:
        text = subprocess.check_output(["git", "show", f"{revision}:client/{path}"], cwd=ROOT).decode("utf-8")
        stmts = [l.strip() for l in body(text, name).split("\n") if re.match(r"\s*(OpenJPG|OpenTGA)\(", l)]
        functions.append(f"static void Old_{group}() {{\n" + "\n".join(stmts) + "\n}\n")
    return STUBS + "".join(functions) + main_for([(g, f"Old_{g}();") for _, _, g in GROUPS])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--architecture", choices=("Win32", "x64"), default="x64")
    parser.add_argument("--generator")
    parser.add_argument("--reference-ref")
    args = parser.parse_args()
    toml = ROOT / "bin/Client/Content/interface/textures.toml"
    sections = re.findall(r"^\[(\w+)\]", toml.read_text(encoding="utf-8"), re.M)
    used = {g for _, _, g in GROUPS}
    for path, name, group in GROUPS:
        if f'TextureList_Load("{group}")' not in body((ROOT / path).read_text(encoding="utf-8"), name):
            raise RuntimeError(f"{name} no carga [{group}]")
    if set(sections) != used:
        raise RuntimeError(f"secciones sin usar o faltantes: {sorted(set(sections) ^ used)}")
    with tempfile.TemporaryDirectory(prefix="mu-texture-list-") as temp:
        data = Path(temp) / "data"
        (data / "Content/interface").mkdir(parents=True)
        shutil.copyfile(toml, data / "Content/interface/textures.toml")
        rows = build(Path(temp) / "current", current_harness(), args, cwd=data)
        warnings = [r for r in rows if r.startswith("WARN|")]
        if warnings:
            raise RuntimeError("el lector rechazó líneas:\n" + "\n".join(warnings))
        files = {p.name.lower() for p in (ROOT / "bin/Client/Content").rglob("*") if p.is_file()}
        missing = sorted({r.split("|")[2] for r in rows
                          if re.sub(r"\.jpg$", ".ozj", re.sub(r"\.tga$", ".ozt", r.split("|")[2].lower()))
                          .rsplit("/", 1)[-1] not in files})
        if missing:
            raise RuntimeError(f"texturas que no están: {missing}")
        print(f"TextureList: {len(sections)} secciones, {len(rows)} texturas presentes")
        if args.reference_ref:
            reference = build(Path(temp) / "reference", reference_harness(args.reference_ref), args)
            reference.remove("images|J|Interface/level_02.jpg|289|9728|10496|0|1")
            if reference != rows:
                for a, b in zip(reference, rows):
                    if a != b:
                        raise RuntimeError(f"difiere:\n  código {a}\n  tabla  {b}")
                raise RuntimeError(f"cantidad distinta: código {len(reference)} / tabla {len(rows)}")
            print(f"Equivalencia con {args.reference_ref}: OK ({len(rows)} llamadas)")


if __name__ == "__main__":
    main()
