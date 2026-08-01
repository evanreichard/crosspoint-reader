#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "Logging.h"
#include "util/lua/LuaBindings.h"

extern "C" {
#include <lauxlib.h>
}

using luabindings::addFunction;

namespace {
constexpr size_t MAX_FILE_READ_SIZE = 50000;
constexpr size_t MAX_LINE_READ_SIZE = 192;

std::vector<std::string> listEntries(const char* path, bool directories) {
  std::vector<std::string> names;
  names.reserve(16);
  auto root = Storage.open(path);
  if (!root || !root.isDirectory()) return names;
  root.rewindDirectory();
  char name[192];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    if (entry.isDirectory() == directories) {
      entry.getName(name, sizeof(name));
      if (name[0] != '.') names.emplace_back(name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

int pushEntries(lua_State* state, bool directories) {
  const auto names = listEntries(luaL_checkstring(state, 1), directories);
  lua_createtable(state, names.size(), 0);
  for (size_t i = 0; i < names.size(); ++i) {
    lua_pushlstring(state, names[i].data(), names[i].size());
    lua_rawseti(state, -2, i + 1);
  }
  return 1;
}

// --- Lists sorted sub-directory names under a path (hidden entries excluded).
// -- @param path string Absolute path on the SD card
// -- @return string[] names
// -- @within fs
int fsListDirs(lua_State* state) { return pushEntries(state, true); }
// --- Lists sorted file names under a path (hidden entries excluded).
// -- @param path string Absolute path on the SD card
// -- @return string[] names
// -- @within fs
int fsListFiles(lua_State* state) { return pushEntries(state, false); }

// --- Returns true if a file or directory exists.
// -- @param path string Absolute path on the SD card
// -- @return bool
// -- @within fs
int fsExists(lua_State* state) {
  lua_pushboolean(state, Storage.exists(luaL_checkstring(state, 1)));
  return 1;
}

// --- Reads an entire file (capped at ~50KB).
// -- @param path string Absolute path on the SD card
// -- @return string|nil content Nil if the file is missing or unreadable
// -- @within fs
int fsReadFile(lua_State* state) {
  HalFile file;
  if (!Storage.openFileForRead("LUA", luaL_checkstring(state, 1), file) || file.isDirectory()) {
    lua_pushnil(state);
    return 1;
  }
  const size_t size = std::min(file.size(), MAX_FILE_READ_SIZE);
  if (size == 0) {
    file.close();
    lua_pushliteral(state, "");
    return 1;
  }
  auto buffer = makeUniqueNoThrow<char[]>(size);
  if (!buffer) {
    LOG_ERR("LUA", "OOM: %u byte file read", static_cast<unsigned>(size));
    file.close();
    lua_pushnil(state);
    return 1;
  }
  size_t total = 0;
  while (total < size) {
    const int read = file.read(buffer.get() + total, size - total);
    if (read <= 0) break;
    total += read;
  }
  file.close();
  lua_pushlstring(state, buffer.get(), total);
  return 1;
}

// --- Returns the size of a file in bytes.
// -- @param path string Absolute path on the SD card
// -- @return int|nil size Nil if the file is missing or a directory
// -- @within fs
int fsFileSize(lua_State* state) {
  HalFile file;
  if (!Storage.openFileForRead("LUA", luaL_checkstring(state, 1), file) || file.isDirectory()) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(file.size()));
  return 1;
}

// --- Reads the first full line beginning at or after a byte offset, for paging large files.
// -- @param path string Absolute path on the SD card
// -- @param offset int Byte offset (0-based); snaps forward to the next line boundary if mid-line
// -- @return string|nil line Nil past end of file, or if the line exceeds 192 bytes
// -- @return int|nil nextOffset Byte offset of the following line
// -- @within fs
int fsReadLineAt(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  const lua_Integer requestedOffset = luaL_checkinteger(state, 2);
  luaL_argcheck(state, requestedOffset >= 0, 2, "offset must be non-negative");

  HalFile file;
  if (!Storage.openFileForRead("LUA", path, file) || file.isDirectory()) {
    lua_pushnil(state);
    lua_pushnil(state);
    return 2;
  }

  const size_t fileSize = file.size();
  const size_t offset = static_cast<size_t>(requestedOffset);
  if (offset >= fileSize) {
    lua_pushnil(state);
    lua_pushnil(state);
    return 2;
  }

  if (offset > 0) {
    if (!file.seek(offset - 1)) {
      lua_pushnil(state);
      lua_pushnil(state);
      return 2;
    }
    if (file.read() != '\n') {
      file.seek(offset);
      int value;
      do {
        value = file.read();
      } while (value >= 0 && value != '\n');
      if (value < 0 || file.position() >= fileSize) {
        lua_pushnil(state);
        lua_pushnil(state);
        return 2;
      }
    }
  }

  char line[MAX_LINE_READ_SIZE];
  size_t length = 0;
  bool overflow = false;
  for (int value = file.read(); value >= 0 && value != '\n'; value = file.read()) {
    if (value == '\r') continue;
    if (length < sizeof(line)) {
      line[length++] = static_cast<char>(value);
    } else {
      overflow = true;
    }
  }

  if (overflow) {
    lua_pushnil(state);
  } else {
    lua_pushlstring(state, line, length);
  }
  lua_pushinteger(state, static_cast<lua_Integer>(file.position()));
  return 2;
}

bool isSafeMutationPath(const char* path, bool allowProtectedRoot = false) {
  if (!path || path[0] != '/' || path[1] == '\0') return false;

  const char* component = path + 1;
  for (const char* cursor = component;; ++cursor) {
    if (*cursor == '\\') return false;
    if (*cursor != '/' && *cursor != '\0') continue;
    const size_t length = cursor - component;
    if ((length == 1 && component[0] == '.') || (length == 2 && component[0] == '.' && component[1] == '.')) {
      return false;
    }
    if (*cursor == '\0') break;
    component = cursor + 1;
  }

  size_t length = strlen(path);
  while (length > 1 && path[length - 1] == '/') --length;
  const bool protectedRoot = (length == 6 && strncmp(path, "/.apps", length) == 0) ||
                             (length == 12 && strncmp(path, "/.crosspoint", length) == 0);
  return allowProtectedRoot || !protectedRoot;
}

int pushFsResult(lua_State* state, bool ok, const char* error) {
  if (ok) {
    lua_pushboolean(state, true);
    return 1;
  }
  LOG_ERR("LUA", "%s", error);
  lua_pushnil(state);
  lua_pushstring(state, error);
  return 2;
}

// --- Creates a directory.
// -- @param path string Absolute path on the SD card
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsMkdir(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path, true)) return pushFsResult(state, false, "Unsafe directory path");
  if (Storage.exists(path)) {
    auto existing = Storage.open(path);
    return pushFsResult(state, existing && existing.isDirectory(), "Path exists and is not a directory");
  }
  return pushFsResult(state, Storage.mkdir(path), "Failed to create directory");
}

// --- Renames (moves) a file or directory.
// -- @param source string
// -- @param destination string Must not already exist
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsRename(lua_State* state) {
  const char* source = luaL_checkstring(state, 1);
  const char* destination = luaL_checkstring(state, 2);
  if (!isSafeMutationPath(source) || !isSafeMutationPath(destination)) {
    return pushFsResult(state, false, "Unsafe rename path");
  }
  if (Storage.exists(destination)) return pushFsResult(state, false, "Rename destination exists");
  return pushFsResult(state, Storage.rename(source, destination), "Failed to rename path");
}

// --- Deletes a file. /.apps and /.crosspoint are protected.
// -- @param path string Absolute path on the SD card
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsRemove(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path)) return pushFsResult(state, false, "Unsafe remove path");
  return pushFsResult(state, Storage.remove(path), "Failed to remove file");
}

// --- Recursively deletes a directory tree. /.apps and /.crosspoint are protected.
// -- @param path string Absolute path on the SD card
// -- @return bool|nil ok
// -- @return string|nil error Present when ok is nil
// -- @within fs
int fsRemoveTree(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  if (!isSafeMutationPath(path)) return pushFsResult(state, false, "Unsafe remove tree path");
  return pushFsResult(state, Storage.removeDir(path), "Failed to remove directory tree");
}

// --- Writes a file (creating or overwriting).
// -- @param path string Absolute path on the SD card
// -- @param content string
// -- @return bool ok
// -- @within fs
int fsWriteFile(lua_State* state) {
  const char* path = luaL_checkstring(state, 1);
  size_t size = 0;
  const char* content = luaL_checklstring(state, 2, &size);
  HalFile file;
  if (!Storage.openFileForWrite("LUA", path, file)) {
    lua_pushboolean(state, false);
    return 1;
  }
  lua_pushboolean(state, file.write(content, size) == size);
  return 1;
}
}  // namespace

void luabindings::registerFs(lua_State* state) {
  lua_newtable(state);
  addFunction(state, "listDirs", fsListDirs);
  addFunction(state, "listFiles", fsListFiles);
  addFunction(state, "exists", fsExists);
  addFunction(state, "readFile", fsReadFile);
  addFunction(state, "fileSize", fsFileSize);
  addFunction(state, "readLineAt", fsReadLineAt);
  addFunction(state, "mkdir", fsMkdir);
  addFunction(state, "rename", fsRename);
  addFunction(state, "remove", fsRemove);
  addFunction(state, "removeTree", fsRemoveTree);
  addFunction(state, "writeFile", fsWriteFile);
  lua_setglobal(state, "fs");
}
