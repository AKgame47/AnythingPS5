# Slice E: keyword check that the docs do not overclaim platform support.
# Run: cmake -DREPO=<repo root> -P tests/docs_claims.cmake
# This only catches known phrases. It cannot prove the docs are correct.
if(NOT REPO)
  message(FATAL_ERROR "pass -DREPO=<repo root>")
endif()
file(GLOB docs ${REPO}/docs/*.md)
set(files ${REPO}/README.md ${docs})
set(errors "")
set(checked 0)

# 1. phrases that are always an overclaim (or a stale claim) anywhere
set(banned
  "windows x86-64 preview"
  "ci-verified"
  "runs windows games"
  "run windows games"
  "supports windows games"
  "windows games run"
  "windows games are supported"
  "playable on windows"
  "runs on windows"
  "wine-based"
  "proton-based"
  "ps5 games run"
  "runs ps5 games"
  "plays ps5 games"
  "macos support"
  "arm support")
# 2. words that may appear only in docs/platforms.md and the README Windows section (where they are stated as unsupported)
set(platforms_only "wine" "proton" "dxvk" "vkd3d")

foreach(f ${files})
  file(READ ${f} text)
  string(TOLOWER "${text}" low)
  get_filename_component(name ${f} NAME)
  math(EXPR checked "${checked}+1")
  foreach(b ${banned})
    string(FIND "${low}" "${b}" pos)
    if(NOT pos EQUAL -1)
      string(APPEND errors "${name}: banned phrase '${b}'\n")
    endif()
  endforeach()
  if(NOT name STREQUAL "platforms.md" AND NOT name STREQUAL "README.md")
    foreach(w ${platforms_only})
      if(low MATCHES "(^|[^a-z0-9])${w}([^a-z0-9]|$)")
        string(APPEND errors "${name}: '${w}' may only be mentioned in docs/platforms.md or README.md (as unsupported)\n")
      endif()
    endforeach()
  endif()
  # 3. every table row that mentions Windows must carry an honest qualifier
  file(STRINGS ${f} lines)
  foreach(l ${lines})
    string(TOLOWER "${l}" ll)
    if(ll MATCHES "^\\|" AND ll MATCHES "windows")
      if(NOT ll MATCHES "blocked|planned|not |untested|notes only|cannot|partial|refuses|unsupported|n/a|^\\| part \\|")
        string(APPEND errors "${name}: Windows table row without Blocked/Planned/Not qualifier: ${l}\n")
      endif()
    endif()
  endforeach()
endforeach()

# 4. the honest statements must still exist
file(READ ${REPO}/docs/platforms.md plat)
foreach(req "Blocked" "Not supported, not planned" "native x86-64 Linux" "no emulation")
  string(TOLOWER "${plat}" plow)
  string(TOLOWER "${req}" rlow)
  string(FIND "${plow}" "${rlow}" p)
  if(p EQUAL -1)
    string(APPEND errors "docs/platforms.md lost the required statement '${req}'\n")
  endif()
endforeach()
file(READ ${REPO}/README.md readme)
if(NOT readme MATCHES "Windows execution path \\| Blocked")
  string(APPEND errors "README.md lost the row 'Windows execution path | Blocked'\n")
endif()
if(NOT readme MATCHES "no emulation, no VMs")
  string(APPEND errors "README.md lost the 'no emulation, no VMs' rule\n")
endif()

if(errors)
  message(FATAL_ERROR "docs claims check FAILED (${checked} files):\n${errors}")
endif()
message(STATUS "docs claims check ok (${checked} files)")
