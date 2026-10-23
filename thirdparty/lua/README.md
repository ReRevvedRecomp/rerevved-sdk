# Lua 5.5.1

Source: https://www.lua.org/ftp/lua-5.5.1.tar.gz
SHA-256: 1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce

The src .c/.h files are unchanged. The standalone interpreter lua.c and
compiler luac.c, upstream Makefiles, documentation and other files are omitted.
LICENSE contains the license from lua.h. CMake compiles the library as C++
with Lua's exception error bridge; no upstream Makefile is executed.
