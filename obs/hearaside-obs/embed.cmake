# Embeds a file as a C++ byte array: cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR bytes "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" arr "${arr}")
file(WRITE "${OUT}.tmp" "// generated from ${IN}\n#pragma once\n#include <cstddef>\nnamespace hearaside {\ninline constexpr unsigned char ${NAME}[] = {\n${arr}0x00 };\ninline constexpr std::size_t ${NAME}Size = ${bytes};\n}\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
